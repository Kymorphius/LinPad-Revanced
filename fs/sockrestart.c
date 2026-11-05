#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include "debug.h"
#include "fs/sockrestart.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/sock.h"
#include "kernel/fs.h"
#include "kernel/task.h"
#include "util/list.h"
extern const struct fd_ops socket_fdops;

static lock_t sockrestart_lock = LOCK_INITIALIZER;

// A healthy listener fails accept() only occasionally; this many non-routine
// failures within one second mean the guest is spinning on a dead listener.
#define SPIN_FAILURES 64
#define NS_PER_SEC 1000000000ull
#define FIRST_RETRY_NS NS_PER_SEC
#define MAX_RETRY_NS (60 * NS_PER_SEC)

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
}

void sockrestart_begin_listen(struct fd *sock, int backlog) {
    if (sock->ops != &socket_fdops)
        return;
    // Recorded while the listener is certainly healthy: right after listen().
    struct sockaddr_storage name;
    socklen_t name_len = sizeof(name);
    int type;
    socklen_t type_len = sizeof(type);
    bool known = getsockname(sock->real_fd, (struct sockaddr *) &name, &name_len) == 0 &&
        getsockopt(sock->real_fd, SOL_SOCKET, SO_TYPE, &type, &type_len) == 0 &&
        name_len <= sizeof(name);
    lock(&sockrestart_lock);
    struct fd_sockrestart *r = &sock->sockrestart;
    memset(r, 0, sizeof(*r));
    if (known) {
        r->listening = true;
        r->backlog = backlog;
        r->type = type;
        r->name = name;
        r->name_len = name_len;
    }
    unlock(&sockrestart_lock);
}

void sockrestart_end_listen(struct fd *sock) {
    if (sock->ops != &socket_fdops)
        return;
    lock(&sockrestart_lock);
    sock->sockrestart.listening = false;
    unlock(&sockrestart_lock);
}

static struct list listen_tasks = LIST_INITIALIZER(listen_tasks);

void sockrestart_begin_listen_wait(void) {
    lock(&sockrestart_lock);
    if (current->sockrestart.count++ == 0)
        list_add(&listen_tasks, &current->sockrestart.listen);
    unlock(&sockrestart_lock);
}

void sockrestart_end_listen_wait(void) {
    lock(&sockrestart_lock);
    if (current->sockrestart.count > 0 && --current->sockrestart.count == 0)
        list_remove(&current->sockrestart.listen);
    unlock(&sockrestart_lock);
}

void sockrestart_task_exit(struct task *task) {
    lock(&sockrestart_lock);
    if (task->sockrestart.count > 0)
        list_remove(&task->sockrestart.listen);
    task->sockrestart.count = 0;
    unlock(&sockrestart_lock);
}

bool sockrestart_should_restart_listen_wait(void) {
    lock(&sockrestart_lock);
    bool punt = current->sockrestart.punt;
    current->sockrestart.punt = false;
    unlock(&sockrestart_lock);
    return punt;
}

// Failures that say nothing about the listener itself.
static bool accept_error_is_routine(int err) {
    switch (err) {
        case EAGAIN:
#if EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
        case EINTR:
        case EMFILE:
        case ENFILE:
        case ENOBUFS:
        case ENOMEM:
            return true;
    }
    return false;
}

// Still the socket listen() recorded: same type and same bound address.
static bool host_listener_intact(int real_fd, const struct fd_sockrestart *r) {
    int type;
    socklen_t type_len = sizeof(type);
    if (getsockopt(real_fd, SOL_SOCKET, SO_TYPE, &type, &type_len) < 0 || type != r->type)
        return false;
    struct sockaddr_storage name;
    socklen_t name_len = sizeof(name);
    if (getsockname(real_fd, (struct sockaddr *) &name, &name_len) < 0)
        return false;
    return name_len == r->name_len && memcmp(&name, &r->name, name_len) == 0;
}

// Builds a listener like the recorded one and moves it onto sock->real_fd.
// Returns 0, or the errno of the failed step with *step naming it; on failure
// sock->real_fd is unchanged.
static int rebuild_host_listener(struct fd *sock, const struct fd_sockrestart *r, const char **step) {
    int family = r->name.ss_family;
    *step = "socket";
    int fresh = socket(family, r->type, 0);
    if (fresh < 0)
        return errno;
    if (family == AF_INET || family == AF_INET6) {
        int one = 1;
        setsockopt(fresh, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    } else if (family == AF_UNIX) {
        // Only the host path that backs a guest socket; it belongs to this app.
        const char *path = ((const struct sockaddr_un *) &r->name)->sun_path;
        if (strncmp(path, sock_tmp_prefix, strlen(sock_tmp_prefix)) == 0)
            unlink(path);
        int bufsize = 1024 * 1024; // as sock_fd_create
        setsockopt(fresh, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
        setsockopt(fresh, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    }
    *step = "bind";
    if (bind(fresh, (const struct sockaddr *) &r->name, r->name_len) < 0)
        goto fail;
    *step = "listen";
    if (listen(fresh, r->backlog) < 0)
        goto fail;
    // dup2 clears close-on-exec on the target; keep what the old one had.
    int fd_flags = fcntl(sock->real_fd, F_GETFD);
    if (fd_flags < 0)
        fd_flags = FD_CLOEXEC;
    *step = "dup2";
    if (dup2(fresh, sock->real_fd) < 0)
        goto fail;
    close(fresh);
    fcntl(sock->real_fd, F_SETFD, fd_flags);
    // Host O_NONBLOCK follows the guest's flag (realfs_setflags).
    int fl = fcntl(sock->real_fd, F_GETFL);
    if (fl >= 0) {
        fl = (sock->flags & O_NONBLOCK_) ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
        fcntl(sock->real_fd, F_SETFL, fl);
    }
    return 0;
fail:;
    int err = errno;
    close(fresh);
    return err;
}

static unsigned listener_port(const struct fd_sockrestart *r) {
    if (r->name.ss_family == AF_INET)
        return ntohs(((const struct sockaddr_in *) &r->name)->sin_port);
    if (r->name.ss_family == AF_INET6)
        return ntohs(((const struct sockaddr_in6 *) &r->name)->sin6_port);
    return 0;
}

bool sockrestart_repair_after_accept_error(struct fd *sock, int err) {
    if (sock->ops != &socket_fdops || accept_error_is_routine(err))
        return false;
    lock(&sockrestart_lock);
    struct fd_sockrestart *r = &sock->sockrestart;
    if (!r->listening || r->repairing) {
        unlock(&sockrestart_lock);
        return false;
    }
    uint64_t now = monotonic_ns();
    if (now - r->window_start_ns > NS_PER_SEC) {
        r->window_start_ns = now;
        r->failures = 0;
    }
    r->failures++;
    bool spinning = r->failures >= SPIN_FAILURES;
    bool intact = host_listener_intact(sock->real_fd, r);
    if ((intact && !spinning) || now < r->next_attempt_ns) {
        unlock(&sockrestart_lock);
        return false;
    }
    r->repairing = true;
    const char *step = "";
    int result = rebuild_host_listener(sock, r, &step);
    unsigned failures = r->failures;
    if (result == 0) {
        r->failures = 0;
        r->attempt_delay_ns = 0;
        r->next_attempt_ns = 0;
    } else {
        r->attempt_delay_ns = r->attempt_delay_ns ? r->attempt_delay_ns * 2 : FIRST_RETRY_NS;
        if (r->attempt_delay_ns > MAX_RETRY_NS)
            r->attempt_delay_ns = MAX_RETRY_NS;
        r->next_attempt_ns = now + r->attempt_delay_ns;
    }
    int family = r->name.ss_family;
    unsigned port = listener_port(r);
    uint64_t retry_s = r->attempt_delay_ns / NS_PER_SEC;
    unlock(&sockrestart_lock);

    if (result == 0) {
        printk("sockrestart: rebuilt host listener %d (family %d port %u) after accept errno %d, intact %d, %u failures/s\n",
                sock->real_fd, family, port, err, intact, failures);
        // Lock order elsewhere is poll lock, then the sockrestart lock.
        poll_rearm_fd(sock);
        lock(&sockrestart_lock);
        struct task *task;
        list_for_each_entry(&listen_tasks, task, sockrestart.listen) {
            task->sockrestart.punt = true;
            pthread_kill(task->thread, SIGUSR1);
        }
        unlock(&sockrestart_lock);
    } else {
        printk("sockrestart: cannot rebuild host listener %d (family %d port %u) after accept errno %d, intact %d: %s errno %d; retry in %llu s\n",
                sock->real_fd, family, port, err, intact, step, result,
                (unsigned long long) retry_s);
    }
    lock(&sockrestart_lock);
    r->repairing = false;
    unlock(&sockrestart_lock);
    return result == 0;
}
