/*
 * Shared memory client bridge for valkey-benchmark.
 *
 * Handshake: connect to server's shmem listener (Unix socket), receive
 * the shm segment name, mmap it, send ACK.
 *
 * Data path: a bridge thread shuttles data between a socketpair (used by
 * libvalkey) and the shmem ring buffers. The original Unix socket is kept
 * open as a notification channel — a 1-byte "nudge" is sent whenever new
 * data is written to a ring, so the server's epoll wakes up.
 */

#include "shmem_client.h"
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <poll.h>

#define SHMEM_RING_SIZE (1024 * 1024)

typedef struct {
    volatile size_t head;
    volatile size_t tail;
    volatile int closed;
    char data[SHMEM_RING_SIZE];
} shmemRing;

typedef struct {
    shmemRing tx; /* server -> client */
    shmemRing rx; /* client -> server */
} shmemShared;

typedef struct {
    int sock_fd;         /* our end of the socketpair (talks to libvalkey) */
    int notify_fd;       /* the Unix socket (notification channel to server) */
    shmemShared *shared;
    shmemRing *my_tx;    /* client tx = shm.rx (client -> server) */
    shmemRing *my_rx;    /* client rx = shm.tx (server -> client) */
    int shm_fd;
    volatile int stop;
} shmemBridge;

static size_t ring_available(shmemRing *ring) {
    size_t h = ring->head, t = ring->tail;
    return (t >= h) ? (t - h) : (SHMEM_RING_SIZE - h + t);
}

static size_t ring_read(shmemRing *ring, void *buf, size_t len) {
    size_t h = ring->head, t = ring->tail;
    size_t avail = (t >= h) ? (t - h) : (SHMEM_RING_SIZE - h + t);
    if (len > avail) len = avail;
    if (len == 0) return 0;
    size_t first = SHMEM_RING_SIZE - h;
    if (first >= len) {
        memcpy(buf, ring->data + h, len);
        ring->head = (h + len) % SHMEM_RING_SIZE;
    } else {
        memcpy(buf, ring->data + h, first);
        memcpy((char *)buf + first, ring->data, len - first);
        ring->head = len - first;
    }
    return len;
}

static size_t ring_write(shmemRing *ring, const void *buf, size_t len) {
    size_t h = ring->head, t = ring->tail;
    size_t space = SHMEM_RING_SIZE - ((t >= h) ? (t - h) : (SHMEM_RING_SIZE - h + t)) - 1;
    if (len > space) len = space;
    if (len == 0) return 0;
    size_t first = SHMEM_RING_SIZE - t;
    if (first >= len) {
        memcpy(ring->data + t, buf, len);
        ring->tail = (t + len) % SHMEM_RING_SIZE;
    } else {
        memcpy(ring->data + t, buf, first);
        memcpy(ring->data, (const char *)buf + first, len - first);
        ring->tail = len - first;
    }
    return len;
}

static void nudge(int fd) {
    char c = 'N';
    int ret = write(fd, &c, 1);
    (void)ret;
}

static void drain_notify(int fd) {
    char buf[64];
    while (read(fd, buf, sizeof(buf)) > 0) {}
}

/* Bridge thread */
static void *shmemBridgeThread(void *arg) {
    shmemBridge *b = arg;
    char buf[65536];

    while (!b->stop) {
        /* Poll both the socketpair (data from libvalkey) and the
         * notification socket (nudges from server). */
        struct pollfd pfds[2];
        pfds[0].fd = b->sock_fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = b->notify_fd;
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;

        int timeout = ring_available(b->my_rx) > 0 ? 0 : 1;
        poll(pfds, 2, timeout);

        /* Drain server nudges */
        if (pfds[1].revents & POLLIN) drain_notify(b->notify_fd);

        /* Socket -> shmem ring (client sends to server) */
        if (pfds[0].revents & POLLIN) {
            ssize_t n = read(b->sock_fd, buf, sizeof(buf));
            if (n <= 0) break;
            size_t written = 0;
            while (written < (size_t)n && !b->stop) {
                size_t w = ring_write(b->my_tx, buf + written, n - written);
                written += w;
                if (w > 0) nudge(b->notify_fd); /* wake server */
                if (written < (size_t)n) usleep(10);
            }
        }

        /* Shmem ring -> socket (server sends to client) */
        size_t avail = ring_available(b->my_rx);
        if (avail > 0) {
            size_t to_read = avail < sizeof(buf) ? avail : sizeof(buf);
            size_t got = ring_read(b->my_rx, buf, to_read);
            if (got > 0) {
                size_t sent = 0;
                while (sent < got && !b->stop) {
                    ssize_t w = write(b->sock_fd, buf + sent, got - sent);
                    if (w <= 0) { b->stop = 1; break; }
                    sent += w;
                }
            }
        }

        if (pfds[0].revents & (POLLERR | POLLHUP)) break;
    }
    return NULL;
}

static int shmemHandshake(const char *socket_path, shmemBridge *bridge) {
    int ufd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ufd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(ufd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(ufd);
        return -1;
    }

    /* Read shm name length + name */
    size_t namelen = 0;
    if (read(ufd, &namelen, sizeof(namelen)) != (ssize_t)sizeof(namelen) || namelen > 256) {
        close(ufd);
        return -1;
    }
    char shm_name[257];
    if (read(ufd, shm_name, namelen) != (ssize_t)namelen) {
        close(ufd);
        return -1;
    }
    shm_name[namelen] = '\0';

    /* Open shared memory */
    int shm_fd = shm_open(shm_name, O_RDWR, 0600);
    if (shm_fd < 0) {
        close(ufd);
        return -1;
    }

    shmemShared *shared = mmap(NULL, sizeof(shmemShared), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shared == MAP_FAILED) {
        close(shm_fd); close(ufd);
        return -1;
    }

    /* Send ACK */
    char ack = 'K';
    if (write(ufd, &ack, 1) != 1) {
        munmap(shared, sizeof(shmemShared));
        close(shm_fd); close(ufd);
        return -1;
    }

    /* Make Unix socket non-blocking for notification use */
    int flags = fcntl(ufd, F_GETFL);
    fcntl(ufd, F_SETFL, flags | O_NONBLOCK);

    bridge->notify_fd = ufd;
    bridge->shared = shared;
    bridge->shm_fd = shm_fd;
    /* Client: tx = shm.rx (client->server), rx = shm.tx (server->client) */
    bridge->my_tx = &shared->rx;
    bridge->my_rx = &shared->tx;
    bridge->stop = 0;
    return 0;
}

valkeyContext *shmemConnect(const char *socket_path, int nonblock) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return NULL;

    shmemBridge *bridge = malloc(sizeof(*bridge));
    if (!bridge) { close(sv[0]); close(sv[1]); return NULL; }
    bridge->sock_fd = sv[1];

    if (shmemHandshake(socket_path, bridge) < 0) {
        free(bridge);
        close(sv[0]); close(sv[1]);
        return NULL;
    }

    pthread_t tid;
    pthread_create(&tid, NULL, shmemBridgeThread, bridge);
    pthread_detach(tid);

    if (nonblock) {
        int flags = fcntl(sv[0], F_GETFL);
        fcntl(sv[0], F_SETFL, flags | O_NONBLOCK);
    }

    return valkeyConnectFd(sv[0]);
}
