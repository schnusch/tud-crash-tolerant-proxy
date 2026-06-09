#ifndef COMMON_IPC_H
#define COMMON_IPC_H

/**
 * Send an IPC datagram.
 */
int ipc_send(int ipc_fd, const char *action, size_t slot, int fd, const char *fmt, ...);

/**
 * Callback functions called by `ipc_process_incoming`.
 * \param action content of the JSON property `action`
 * \param slot   content of the JSON property `slot`
 * \param fd     file descriptor received by `recvmsg(2)`, `-1` if not file descriptor was received
 * \param json   literal JSON serialization, properties aside from `action` and `slot` are ignore and can be extracted later
 * \param ctx    pointer passed to `ipc_process_incoming`
 * \return -1 on error
 * \return 0  on success
 */
typedef int (*ipc_method_t)(const char *action, size_t slot, int fd, const char *tail, void *ctx);

/**
 * Map IPC actions to their respective callback methods.
 */
struct ipc_action_method {
    /** The IPC action, must not contain white-spaces. */
    const char *action;
    /** Associated callback */
    ipc_method_t method;
};

/**
 * Receive and process an IPC datagram.
 * \param methods Array of callback methods. The last item must have `action`
 *                set to `NULL` and may have a function pointer set, that will
 *                be called if no previous `action` matched.
 * \param ctx     passed to the callback methods verbatim
 */
int ipc_process_incoming(
    int ipc_fd,
    const struct ipc_action_method *methods,
    void *ctx
);

#endif
