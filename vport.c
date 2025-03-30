#include "tap_utils.h"
#include "sys_utils.h"
#include <stdbool.h>
#include <assert.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <net/ethernet.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

/**
 * VPort Instance
 */
typedef struct {
    int tapfd;                       // TAP device file descriptor
    int vport_sockfd;                // UDP socket for communicating with VSwitch
    struct sockaddr_in vswitch_addr; // VSwitch address
    volatile bool running;           // Flag to control thread execution
} vport_t;

// Forward declarations
void vport_init(vport_t *vport, const char *server_ip_str, int server_port);
void vport_cleanup(vport_t *vport);
void *forward_ether_data_to_vswitch(void *arg);
void *forward_ether_data_to_tap(void *arg);
void handle_signal(int sig);

// Global variable for signal handling
static volatile sig_atomic_t keep_running = 1;

void handle_signal(int sig) {
    (void)sig;
    keep_running = 0;
}

int main(int argc, char const *argv[]) {
    // Set up signal handlers for graceful shutdown
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // Parse arguments with better error checking
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <server_port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *server_ip_str = argv[1];
    int server_port = atoi(argv[2]);
    if (server_port <= 0 || server_port > 65535) {
        fprintf(stderr, "Invalid port number: %d\n", server_port);
        exit(EXIT_FAILURE);
    }

    // Initialize vport
    vport_t vport = {0};
    vport_init(&vport, server_ip_str, server_port);
    vport.running = true;

    // Create threads
    pthread_t up_forwarder, down_forwarder;
    if (pthread_create(&up_forwarder, NULL, forward_ether_data_to_vswitch, &vport) != 0 ||
        pthread_create(&down_forwarder, NULL, forward_ether_data_to_tap, &vport) != 0) {
        perror("Failed to create threads");
        vport_cleanup(&vport);
        exit(EXIT_FAILURE);
    }

    // Wait for signal to terminate
    while (keep_running) {
        sleep(1);
    }

    // Signal threads to stop
    vport.running = false;
    
    // Trigger threads to wake up (they might be blocked on I/O)
    shutdown(vport.vport_sockfd, SHUT_RDWR);
    close(vport.tapfd);  // This will make read() return in the threads

    // Wait for threads to finish
    pthread_join(up_forwarder, NULL);
    pthread_join(down_forwarder, NULL);

    // Cleanup resources
    vport_cleanup(&vport);
    return EXIT_SUCCESS;
}

void vport_init(vport_t *vport, const char *server_ip_str, int server_port) {
    // Validate input
    if (!vport || !server_ip_str) {
        fprintf(stderr, "Invalid arguments to vport_init\n");
        exit(EXIT_FAILURE);
    }

    // Allocate TAP device
    char ifname[IFNAMSIZ] = "tapyuan";
    vport->tapfd = tap_alloc(ifname);
    if (vport->tapfd < 0) {
        perror("Failed to allocate TAP device");
        exit(EXIT_FAILURE);
    }

    // Create UDP socket
    vport->vport_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (vport->vport_sockfd < 0) {
        perror("Failed to create socket");
        close(vport->tapfd);
        exit(EXIT_FAILURE);
    }

    // Set socket timeout to allow periodic checking of running flag
    struct timeval tv = {
        .tv_sec = 1,
        .tv_usec = 0
    };
    setsockopt(vport->vport_sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Setup VSwitch address
    memset(&vport->vswitch_addr, 0, sizeof(vport->vswitch_addr));
    vport->vswitch_addr.sin_family = AF_INET;
    vport->vswitch_addr.sin_port = htons(server_port);
    
    if (inet_pton(AF_INET, server_ip_str, &vport->vswitch_addr.sin_addr) != 1) {
        perror("Invalid server IP address");
        close(vport->tapfd);
        close(vport->vport_sockfd);
        exit(EXIT_FAILURE);
    }

    printf("[VPort] Initialized: TAP device '%s', VSwitch at %s:%d\n", 
           ifname, server_ip_str, server_port);
}

void vport_cleanup(vport_t *vport) {
    if (vport) {
        if (vport->tapfd >= 0) {
            close(vport->tapfd);
        }
        if (vport->vport_sockfd >= 0) {
            close(vport->vport_sockfd);
        }
    }
}

void log_ether_frame(const char *direction, const struct ether_header *hdr, int datasz) {
    printf("[VPort] %s: "
           "dhost<%02x:%02x:%02x:%02x:%02x:%02x> "
           "shost<%02x:%02x:%02x:%02x:%02x:%02x> "
           "type<%04x> "
           "size<%d>\n",
           direction,
           hdr->ether_dhost[0], hdr->ether_dhost[1], hdr->ether_dhost[2],
           hdr->ether_dhost[3], hdr->ether_dhost[4], hdr->ether_dhost[5],
           hdr->ether_shost[0], hdr->ether_shost[1], hdr->ether_shost[2],
           hdr->ether_shost[3], hdr->ether_shost[4], hdr->ether_shost[5],
           ntohs(hdr->ether_type),
           datasz);
}

void *forward_ether_data_to_vswitch(void *arg) {
    vport_t *vport = (vport_t *)arg;
    uint8_t ether_data[ETHER_MAX_LEN];

    while (vport->running) {
        ssize_t ether_datasz = read(vport->tapfd, ether_data, sizeof(ether_data));
        if (ether_datasz < 0) {
            if (errno == EINTR || !vport->running) break;
            perror("Read from TAP failed");
            continue;
        }

        if (ether_datasz >= (ssize_t)sizeof(struct ether_header)) {
            const struct ether_header *hdr = (const struct ether_header *)ether_data;
            log_ether_frame("Sent to VSwitch", hdr, ether_datasz);

            ssize_t sendsz = sendto(vport->vport_sockfd, ether_data, ether_datasz, 0,
                                   (struct sockaddr *)&vport->vswitch_addr, sizeof(vport->vswitch_addr));
            if (sendsz != ether_datasz) {
                fprintf(stderr, "Sendto size mismatch: expected=%zd, actual=%zd\n", 
                        ether_datasz, sendsz);
            }
        }
    }

    return NULL;
}

void *forward_ether_data_to_tap(void *arg) {
    vport_t *vport = (vport_t *)arg;
    uint8_t ether_data[ETHER_MAX_LEN];
    socklen_t addrlen = sizeof(vport->vswitch_addr);

    while (vport->running) {
        ssize_t ether_datasz = recvfrom(vport->vport_sockfd, ether_data, sizeof(ether_data), 0,
                                       (struct sockaddr *)&vport->vswitch_addr, &addrlen);
        if (ether_datasz < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (errno == EINTR || !vport->running) break;
            perror("Recvfrom failed");
            continue;
        }

        if (ether_datasz >= (ssize_t)sizeof(struct ether_header)) {
            const struct ether_header *hdr = (const struct ether_header *)ether_data;
            log_ether_frame("Forward to TAP", hdr, ether_datasz);

            ssize_t writesz = write(vport->tapfd, ether_data, ether_datasz);
            if (writesz != ether_datasz) {
                fprintf(stderr, "Write size mismatch: expected=%zd, actual=%zd\n", 
                        ether_datasz, writesz);
            }
        }
    }

    return NULL;
}
