#ifndef LISTENER_MAIN_PASSIVE_H
#define LISTENER_MAIN_PASSIVE_H

#include "cmdline.h"

/**
 * Main routine of the listening process pair's active/child process.
 */
int main_active(struct cmdline_opts *cmdline, struct shared_memory_mapping *map, int parent_pidfd);

#endif
