// iOS reclaims the kernel part of a suspended app's listening sockets: the
// descriptor stays open but stops being a usable listener. accept() then fails
// immediately, the host readiness filter keeps firing, and guest servers spin
// at a full core (Dropbear's select loop, tokio's accept loop) and become
// unreachable. See TN2277:
// https://developer.apple.com/library/archive/technotes/tn2277/_index.html
//
// Each guest listen() records the host listener's address and configuration.
// When a host accept() on it fails with a non-routine error and the host socket
// no longer matches that record (or the same failure repeats rapidly), the
// listener is rebuilt in place: a new host socket is bound and listening at the
// same address, moved onto the same host descriptor number with dup2, its
// readiness registrations are re-armed, and threads waiting on it restart.
// A rebuild that fails leaves the descriptor alone and backs off.
#ifndef FS_SOCKRESTART_H
#define FS_SOCKRESTART_H
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>
#include "util/list.h"
struct fd;

void sockrestart_begin_listen(struct fd *sock, int backlog);
void sockrestart_end_listen(struct fd *sock);
void sockrestart_begin_listen_wait(struct fd *sock);
void sockrestart_end_listen_wait(struct fd *sock);
bool sockrestart_should_restart_listen_wait(void);
// After a host accept() on sock failed with err. Returns true when the
// listener was rebuilt and accept should be tried again.
bool sockrestart_repair_after_accept_error(struct fd *sock, int err);

struct fd_sockrestart {
    // Recorded by listen(); protected by the sockrestart lock.
    bool listening;
    bool repairing;
    int backlog;
    int type;
    struct sockaddr_storage name;
    socklen_t name_len;
    // Non-routine accept failures in the current one-second window.
    unsigned failures;
    uint64_t window_start_ns;
    // Rebuild attempts back off after failures, up to a minute.
    uint64_t next_attempt_ns;
    uint64_t attempt_delay_ns;
};

struct task_sockrestart {
    int count;
    bool punt;
    struct list listen;
};

#endif
