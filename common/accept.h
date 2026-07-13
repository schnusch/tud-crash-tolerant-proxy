#ifndef LISTENER_ACCEPT_H
#define LISTENER_ACCEPT_H

#include "shared_memory.h"

struct connection *accept_connection(struct shared_memory_mapping *map, int listen_fd);

#endif
