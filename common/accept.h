#ifndef LISTENER_ACCEPT_H
#define LISTENER_ACCEPT_H

#include "shared_memory.h"

/**
 * Allocate a and initialize new `struct connection` in `map` and accept a new
 * connection on `listen_fd`.
 */
struct connection *accept_connection(struct shared_memory_mapping *map, int listen_fd);

#endif
