#ifndef SHMEM_CLIENT_H
#define SHMEM_CLIENT_H

#include <valkey/valkey.h>

/* Connect to a Valkey server via shared memory.
 * Returns a valkeyContext using a socketpair bridged to shmem ring buffers.
 * The bridge runs in a background thread. */
valkeyContext *shmemConnect(const char *shm_name, int nonblock);

#endif
