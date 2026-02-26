/*
 * Shared memory connection type for Valkey
 * Uses a Unix domain socket for accept/handshake AND as a notification
 * channel (for epoll wakeups). Actual data flows through shared memory
 * ring buffers. The Unix socket fd stays as conn->fd for event loop
 * integration — a 1-byte "ping" is sent over it whenever new data is
 * written to a ring buffer.
 */

#include "server.h"
#include "connection.h"
#include "anet.h"
#include "trace/trace.h"
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>

#define SHMEM_RING_SIZE (1024 * 1024) /* 1MB ring buffer per direction */

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
    connection conn;       /* conn.fd = the Unix socket (notification channel) */
    char *shm_name;
    int shm_fd;
    shmemShared *shared;
    shmemRing *my_tx;      /* this side's transmit ring */
    shmemRing *my_rx;      /* this side's receive ring */
} shmemConnection;

static ConnectionType CT_Shmem;
static _Atomic int shmem_conn_counter = 0;

/* ------------------------------------------------------------------ */
/* Ring buffer helpers                                                */
/* ------------------------------------------------------------------ */

static size_t ring_available(shmemRing *ring) {
    size_t h = ring->head, t = ring->tail;
    return (t >= h) ? (t - h) : (SHMEM_RING_SIZE - h + t);
}

static size_t ring_space(shmemRing *ring) {
    return SHMEM_RING_SIZE - ring_available(ring) - 1;
}

static size_t ring_read(shmemRing *ring, void *buf, size_t len) {
    size_t h = ring->head, t = ring->tail;
    size_t avail = (t >= h) ? (t - h) : (SHMEM_RING_SIZE - h + t);
    if (avail == 0) return 0;
    if (len > avail) len = avail;
    size_t first = SHMEM_RING_SIZE - h;
    if (len <= first) {
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
    if (space == 0) return 0;
    if (len > space) len = space;
    size_t first = SHMEM_RING_SIZE - t;
    if (len <= first) {
        memcpy(ring->data + t, buf, len);
        ring->tail = (t + len) % SHMEM_RING_SIZE;
    } else {
        memcpy(ring->data + t, buf, first);
        memcpy(ring->data, (const char *)buf + first, len - first);
        ring->tail = len - first;
    }
    return len;
}

/* Send a 1-byte nudge over the Unix socket so epoll wakes up the peer */
static void shmemNudge(int fd) {
    char c = 'N';
    /* Best-effort; ignore EAGAIN. */
    int ret = write(fd, &c, 1);
    UNUSED(ret);
}

/* Drain any pending nudge bytes so the fd stops being readable */
static void shmemDrain(int fd) {
    char buf[64];
    while (read(fd, buf, sizeof(buf)) > 0) {}
}

/* ------------------------------------------------------------------ */
/* Connection type callbacks                                          */
/* ------------------------------------------------------------------ */

static int connShmemGetType(void) { return CONN_TYPE_SHMEM; }
static void connShmemInit(void) {}
static void connShmemCleanup(void) {}

static connection *connShmemCreate(void) {
    shmemConnection *sc = zcalloc(sizeof(*sc));
    sc->conn.type = &CT_Shmem;
    sc->conn.state = CONN_STATE_NONE;
    sc->conn.fd = -1;
    sc->shm_fd = -1;
    return (connection *)sc;
}

static void connShmemClose(connection *conn_) {
    shmemConnection *sc = (shmemConnection *)conn_;
    valkey_shmem_trace(valkey_shmem, close, sc->shm_name ? sc->shm_name : "");
    if (sc->shared) {
        sc->my_tx->closed = 1;
        munmap(sc->shared, sizeof(shmemShared));
        sc->shared = NULL;
    }
    if (sc->shm_fd >= 0) { close(sc->shm_fd); sc->shm_fd = -1; }
    if (sc->shm_name) {
        shm_unlink(sc->shm_name);
        sdsfree(sc->shm_name);
        sc->shm_name = NULL;
    }
    if (conn_->fd >= 0) {
        aeDeleteFileEvent(server.el, conn_->fd, AE_READABLE | AE_WRITABLE);
        close(conn_->fd);
        conn_->fd = -1;
    }
    conn_->state = CONN_STATE_CLOSED;
}

static void connShmemShutdown(connection *conn) { connShmemClose(conn); }

/* Server-side handshake: create shm, tell client the name, wait for ACK.
 * The Unix socket fd (conn->fd) is kept for notifications. */
static int shmemSetupServer(shmemConnection *sc) {
    int id = atomic_fetch_add(&shmem_conn_counter, 1);
    sc->shm_name = sdscatprintf(sdsempty(), "/valkey_shm_%d_%d", (int)getpid(), id);

    sc->shm_fd = shm_open(sc->shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (sc->shm_fd < 0) return C_ERR;
    if (ftruncate(sc->shm_fd, sizeof(shmemShared)) < 0) goto err;

    sc->shared = mmap(NULL, sizeof(shmemShared), PROT_READ | PROT_WRITE, MAP_SHARED, sc->shm_fd, 0);
    if (sc->shared == MAP_FAILED) { sc->shared = NULL; goto err; }
    memset(sc->shared, 0, sizeof(shmemShared));

    /* Server writes to tx, reads from rx */
    sc->my_tx = &sc->shared->tx;
    sc->my_rx = &sc->shared->rx;

    int fd = sc->conn.fd; /* the accepted Unix socket */

    /* Send shm name to client */
    size_t namelen = sdslen(sc->shm_name);
    if (syncWrite(fd, (char *)&namelen, sizeof(namelen), 5000) < 0) goto err;
    if (syncWrite(fd, sc->shm_name, (ssize_t)namelen, 5000) < 0) goto err;

    /* Wait for 1-byte ACK */
    char ack = 0;
    if (syncRead(fd, &ack, 1, 5000) < 0 || ack != 'K') goto err;

    /* Make the Unix socket non-blocking for event loop use */
    anetNonBlock(NULL, fd);
    return C_OK;

err:
    if (sc->shared) { munmap(sc->shared, sizeof(shmemShared)); sc->shared = NULL; }
    if (sc->shm_fd >= 0) { close(sc->shm_fd); sc->shm_fd = -1; }
    if (sc->shm_name) { shm_unlink(sc->shm_name); sdsfree(sc->shm_name); sc->shm_name = NULL; }
    return C_ERR;
}

static int connShmemAccept(connection *conn_, ConnectionCallbackFunc accept_handler) {
    shmemConnection *sc = (shmemConnection *)conn_;
#ifdef USE_LTTNG
    ustime_t start = ustime();
#endif
    if (shmemSetupServer(sc) == C_ERR) {
        conn_->state = CONN_STATE_ERROR;
        if (accept_handler) accept_handler(conn_);
        return C_ERR;
    }
    conn_->state = CONN_STATE_CONNECTED;
#ifdef USE_LTTNG
    valkey_shmem_trace(valkey_shmem, accept, sc->shm_name, (uint64_t)(ustime() - start));
#endif
    if (accept_handler) accept_handler(conn_);
    return C_OK;
}

static int connShmemWrite(connection *conn_, const void *data, size_t len) {
    shmemConnection *sc = (shmemConnection *)conn_;
    if (!sc->shared || sc->my_tx->closed) return -1;
    size_t written = ring_write(sc->my_tx, data, len);
    if (written > 0) {
        shmemNudge(conn_->fd);
        valkey_shmem_trace(valkey_shmem, write, sc->shm_name ? sc->shm_name : "", (uint64_t)written);
    }
    return written > 0 ? (int)written : -1;
}

static int connShmemRead(connection *conn_, void *buf, size_t len) {
    shmemConnection *sc = (shmemConnection *)conn_;
    if (!sc->shared) return -1;
    if (sc->my_rx->closed && ring_available(sc->my_rx) == 0) return 0;
    size_t nread = ring_read(sc->my_rx, buf, len);
    if (nread > 0)
        valkey_shmem_trace(valkey_shmem, read, sc->shm_name ? sc->shm_name : "", (uint64_t)nread);
    return (int)nread;
}

static int connShmemSetWriteHandler(connection *conn, ConnectionCallbackFunc handler, int barrier) {
    conn->write_handler = handler;
    if (barrier) conn->flags |= CONN_FLAG_WRITE_BARRIER;
    else conn->flags &= ~CONN_FLAG_WRITE_BARRIER;
    if (handler && conn->fd >= 0)
        aeCreateFileEvent(server.el, conn->fd, AE_WRITABLE, conn->type->ae_handler, conn);
    else if (!handler && conn->fd >= 0)
        aeDeleteFileEvent(server.el, conn->fd, AE_WRITABLE);
    return C_OK;
}

static int connShmemSetReadHandler(connection *conn, ConnectionCallbackFunc handler) {
    conn->read_handler = handler;
    if (handler && conn->fd >= 0)
        aeCreateFileEvent(server.el, conn->fd, AE_READABLE, conn->type->ae_handler, conn);
    else if (!handler && conn->fd >= 0)
        aeDeleteFileEvent(server.el, conn->fd, AE_READABLE);
    return C_OK;
}

static void connShmemAeHandler(struct aeEventLoop *el, int fd, void *clientData, int mask) {
    UNUSED(el);
    connection *conn = clientData;
    shmemConnection *sc = (shmemConnection *)conn;
    if (!sc->shared) return;

    /* Drain notification bytes so we don't spin */
    shmemDrain(fd);

    int call_write = (mask & AE_WRITABLE) && conn->write_handler && ring_space(sc->my_tx) > 0;
    int call_read = conn->read_handler && ring_available(sc->my_rx) > 0;

    if (conn->flags & CONN_FLAG_WRITE_BARRIER) {
        if (call_write) conn->write_handler(conn);
        if (call_read) conn->read_handler(conn);
    } else {
        if (call_read) conn->read_handler(conn);
        if (call_write) conn->write_handler(conn);
    }
}

static int connShmemHasPendingData(void) { return 0; }
static int connShmemProcessPendingData(void) { return 0; }

static connection *connShmemCreateAccepted(int fd, void *priv) {
    UNUSED(priv);
    shmemConnection *sc = (shmemConnection *)connShmemCreate();
    sc->conn.fd = fd;
    sc->conn.state = CONN_STATE_ACCEPTING;
    return (connection *)sc;
}

static int connShmemAddr(connection *conn, char *ip, size_t ip_len, int *port, int remote) {
    UNUSED(remote);
    shmemConnection *sc = (shmemConnection *)conn;
    if (ip) snprintf(ip, ip_len, "shmem:%s", sc->shm_name ? sc->shm_name : "unknown");
    if (port) *port = 0;
    return C_OK;
}

static int connShmemIsLocal(connection *conn) { UNUSED(conn); return 1; }

/* ------------------------------------------------------------------ */
/* Listener                                                           */
/* ------------------------------------------------------------------ */

static int connShmemListen(connListener *listener) {
    if (listener->bindaddr_count == 0) return C_OK;
    for (int j = 0; j < listener->bindaddr_count; j++) {
        char *addr = listener->bindaddr[j];
        unlink(addr);
        int fd = anetUnixServer(server.neterr, addr, 0666, server.tcp_backlog, NULL);
        if (fd == ANET_ERR) {
            serverLog(LL_WARNING, "Failed opening shmem listener %s: %s", addr, server.neterr);
            exit(1);
        }
        anetNonBlock(NULL, fd);
        listener->fd[listener->count++] = fd;
    }
    return C_OK;
}

static void connShmemCloseListener(connListener *listener) {
    for (int j = 0; j < listener->count; j++) {
        if (listener->fd[j] != -1) {
            aeDeleteFileEvent(server.el, listener->fd[j], AE_READABLE);
            close(listener->fd[j]);
        }
    }
    for (int j = 0; j < listener->bindaddr_count; j++) unlink(listener->bindaddr[j]);
    listener->count = 0;
}

static void connShmemAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cfd, max = server.max_new_conns_per_cycle;
    struct ClientFlags flags = {0};
    UNUSED(el); UNUSED(mask); UNUSED(privdata);

    while (max--) {
        cfd = anetUnixAccept(server.neterr, fd);
        if (cfd == ANET_ERR) {
            if (anetRetryAcceptOnError(errno)) continue;
            if (errno != EWOULDBLOCK)
                serverLog(LL_WARNING, "Accepting shmem client: %s", server.neterr);
            return;
        }
        anetBlock(NULL, cfd); /* blocking for handshake */
        serverLog(LL_VERBOSE, "Accepted shmem connection");
        acceptCommonHandler(connShmemCreateAccepted(cfd, NULL), flags, NULL);
    }
}

static int connShmemConnect(connection *conn_, const char *addr, int port,
                            const char *src, int mp, ConnectionCallbackFunc handler) {
    UNUSED(addr); UNUSED(port); UNUSED(src); UNUSED(mp);
    conn_->state = CONN_STATE_ERROR;
    if (handler) handler(conn_);
    return C_ERR;
}

static ConnectionType CT_Shmem = {
    .get_type = connShmemGetType,
    .init = connShmemInit,
    .cleanup = connShmemCleanup,
    .ae_handler = connShmemAeHandler,
    .accept_handler = connShmemAcceptHandler,
    .addr = connShmemAddr,
    .is_local = connShmemIsLocal,
    .listen = connShmemListen,
    .closeListener = connShmemCloseListener,
    .conn_create = connShmemCreate,
    .conn_create_accepted = connShmemCreateAccepted,
    .close = connShmemClose,
    .shutdown = connShmemShutdown,
    .connect = connShmemConnect,
    .blocking_connect = NULL,
    .accept = connShmemAccept,
    .write = connShmemWrite,
    .writev = NULL,
    .read = connShmemRead,
    .set_write_handler = connShmemSetWriteHandler,
    .set_read_handler = connShmemSetReadHandler,
    .get_last_error = NULL,
    .sync_write = NULL,
    .sync_read = NULL,
    .sync_readline = NULL,
    .has_pending_data = connShmemHasPendingData,
    .process_pending_data = connShmemProcessPendingData,
    .postpone_update_state = NULL,
    .update_state = NULL,
    .get_peer_cert = NULL,
    .get_peer_username = NULL,
    .connIntegrityChecked = NULL,
};

int valkeyRegisterShmemConnectionType(void) {
    return connTypeRegister(&CT_Shmem);
}
