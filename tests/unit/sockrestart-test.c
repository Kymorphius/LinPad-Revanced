// Native tests for in-place listener repair (fs/sockrestart.c) with the
// production readiness code (fs/poll.c, fs/real.c). A reclaimed iOS listener
// is simulated by moving a pipe onto the listener's host descriptor: the
// descriptor stays open but is no longer the recorded socket, and the port
// becomes free, as after TN2277 reclamation. No guest, device or iOS needed.
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "kernel/task.h"
#include "kernel/fs.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/real.h"
#include "fs/sockrestart.h"

__thread struct task *current;
const struct fd_ops socket_fdops = {.poll = realfs_poll};
const char *sock_tmp_prefix;
int errno_map(void) { return -errno; }
bool is_adhoc_fd(struct fd *fd) { (void) fd; return false; }

static char log_buffer[8192];
void ish_printk(const char *format, ...) {
    size_t used = strlen(log_buffer);
    va_list args;
    va_start(args, format);
    vsnprintf(log_buffer + used, sizeof(log_buffer) - used, format, args);
    va_end(args);
}
static unsigned log_count(const char *needle) {
    unsigned n = 0;
    for (const char *p = log_buffer; (p = strstr(p, needle)) != NULL; p += strlen(needle))
        n++;
    return n;
}

#define CHECK(test) do { \
    if (!(test)) { \
        fprintf(stderr, "sockrestart-test:%d: %s (errno %d)\nlog: %s\n", \
                __LINE__, #test, errno, log_buffer); \
        exit(1); \
    } \
} while (0)

static void guest_socket(struct fd *fd, int host_fd, dword_t guest_flags) {
    memset(fd, 0, sizeof(*fd));
    fd->real_fd = host_fd;
    fd->type = S_IFSOCK;
    fd->ops = &socket_fdops;
    fd->flags = guest_flags;
    list_init(&fd->poll_fds);
    lock_init(&fd->poll_lock);
}

static int tcp_listener(unsigned *port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(s >= 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(bind(s, (struct sockaddr *) &a, sizeof(a)) == 0);
    socklen_t len = sizeof(a);
    CHECK(getsockname(s, (struct sockaddr *) &a, &len) == 0);
    CHECK(listen(s, 8) == 0);
    *port = ntohs(a.sin_port);
    return s;
}

static int tcp_connect(unsigned port) {
    int c = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(c >= 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(connect(c, (struct sockaddr *) &a, sizeof(a)) == 0);
    return c;
}

static unsigned bound_port(int fd) {
    struct sockaddr_in a = {0};
    socklen_t len = sizeof(a);
    if (getsockname(fd, (struct sockaddr *) &a, &len) < 0 || a.sin_family != AF_INET)
        return 0;
    return ntohs(a.sin_port);
}

// The listener stops being a socket and its port is released. Reclamation
// leaves descriptor flags alone; dup2 would clear close-on-exec, so restore it.
static void reclaim(int host_fd) {
    int flags = fcntl(host_fd, F_GETFD);
    CHECK(flags >= 0);
    int p[2];
    CHECK(pipe(p) == 0);
    CHECK(dup2(p[0], host_fd) == host_fd);
    CHECK(fcntl(host_fd, F_SETFD, flags) == 0);
    close(p[0]);
    close(p[1]);
}

static bool is_fifo(int fd) {
    struct stat st;
    return fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode);
}

static int note_events(void *context, int types, union poll_fd_info info) {
    (void) info;
    *(int *) context |= types;
    return 1;
}

static int wait_readable(struct poll *poll) {
    struct timespec timeout = {.tv_nsec = 900 * 1000000L};
    int events = 0;
    int n = poll_wait(poll, note_events, &events, &timeout);
    return n > 0 ? events : 0;
}

static void test_routine_and_intact(void) {
    unsigned port;
    struct fd fd;
    guest_socket(&fd, tcp_listener(&port), O_NONBLOCK_);
    CHECK(!sockrestart_repair_after_accept_error(&fd, EINVAL)); // never listened
    sockrestart_begin_listen(&fd, 8);
    CHECK(!sockrestart_repair_after_accept_error(&fd, EAGAIN));
    CHECK(!sockrestart_repair_after_accept_error(&fd, EMFILE));
    CHECK(!sockrestart_repair_after_accept_error(&fd, ECONNABORTED));
    CHECK(log_buffer[0] == '\0');
    int c = tcp_connect(port);
    int a = accept(fd.real_fd, NULL, NULL);
    CHECK(a >= 0);
    close(a);
    close(c);
    sockrestart_end_listen(&fd);
    close(fd.real_fd);
    printf("PASS routine errors and intact listener left alone\n");
}

static void test_tcp_reclaimed(void) {
    unsigned port;
    struct fd fd;
    int host = tcp_listener(&port);
    CHECK(fcntl(host, F_SETFD, FD_CLOEXEC) == 0);
    guest_socket(&fd, host, O_NONBLOCK_);
    sockrestart_begin_listen(&fd, 8);
    struct poll *poll = poll_create();
    CHECK(!IS_ERR(poll));
    CHECK(poll_add_fd(poll, &fd, POLL_READ | POLL_HUP | POLL_ERR, (union poll_fd_info) {0}) == 0);

    reclaim(host);
    CHECK(is_fifo(host));
    log_buffer[0] = '\0';
    CHECK(sockrestart_repair_after_accept_error(&fd, EINVAL));
    CHECK(fd.real_fd == host && !is_fifo(host));
    CHECK(bound_port(host) == port);
    CHECK(fcntl(host, F_GETFD) & FD_CLOEXEC);
    CHECK(fcntl(host, F_GETFL) & O_NONBLOCK);
    CHECK(log_count("rebuilt host listener") == 1);
    // The re-armed registration wakes the guest's wait for a new connection.
    CHECK(wait_readable(poll) == 0);
    int c = tcp_connect(port);
    CHECK(wait_readable(poll) & POLL_READ);
    int a = accept(host, NULL, NULL);
    CHECK(a >= 0);
    close(a);
    close(c);
    poll_del_fd(poll, &fd);
    poll_destroy(poll);
    sockrestart_end_listen(&fd);
    close(host);
    printf("PASS reclaimed TCP listener rebuilt on the same descriptor, port and flags\n");
}

static void test_port_taken(void) {
    unsigned port;
    struct fd fd;
    int host = tcp_listener(&port);
    guest_socket(&fd, host, O_NONBLOCK_);
    sockrestart_begin_listen(&fd, 8);
    reclaim(host);
    // Something else owns the port now: the rebuild must not disturb anything.
    int other = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(bind(other, (struct sockaddr *) &a, sizeof(a)) == 0);
    CHECK(listen(other, 8) == 0);
    log_buffer[0] = '\0';
    CHECK(!sockrestart_repair_after_accept_error(&fd, EINVAL));
    CHECK(fd.real_fd == host && is_fifo(host));
    CHECK(log_count("cannot rebuild") == 1 && strstr(log_buffer, "bind errno") != NULL);
    CHECK(!sockrestart_repair_after_accept_error(&fd, EINVAL)); // backing off
    CHECK(log_count("cannot rebuild") == 1);
    int c = tcp_connect(port); // the other listener still works
    int s = accept(other, NULL, NULL);
    CHECK(s >= 0);
    close(s);
    close(c);
    close(other);
    sockrestart_end_listen(&fd);
    close(host);
    printf("PASS occupied port: listener left unchanged, attempt logged, retry backed off\n");
}

static void test_spinning_but_healthy(void) {
    unsigned port;
    struct fd fd;
    guest_socket(&fd, tcp_listener(&port), O_NONBLOCK_);
    sockrestart_begin_listen(&fd, 8);
    log_buffer[0] = '\0';
    for (int i = 0; i < 100; i++)
        CHECK(!sockrestart_repair_after_accept_error(&fd, ECONNABORTED));
    // The spin trigger tried once; the live listener kept its port.
    CHECK(log_count("cannot rebuild") == 1);
    CHECK(bound_port(fd.real_fd) == port);
    int c = tcp_connect(port);
    int a = accept(fd.real_fd, NULL, NULL);
    CHECK(a >= 0);
    close(a);
    close(c);
    sockrestart_end_listen(&fd);
    close(fd.real_fd);
    printf("PASS repeated failures on a live listener never replace it\n");
}

static void test_unix_reclaimed(const char *dir) {
    char prefix[256], path[300];
    snprintf(prefix, sizeof(prefix), "%s/ishsock", dir);
    sock_tmp_prefix = prefix;
    snprintf(path, sizeof(path), "%s%d.1", prefix, (int) getpid());
    int host = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(host >= 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    CHECK(bind(host, (struct sockaddr *) &a, sizeof(a)) == 0);
    CHECK(listen(host, 8) == 0);
    CHECK(fcntl(host, F_SETFL, O_NONBLOCK) == 0); // guest asks for blocking below
    struct fd fd;
    guest_socket(&fd, host, 0);
    sockrestart_begin_listen(&fd, 8);
    reclaim(host); // the stale path stays behind, as on iOS
    log_buffer[0] = '\0';
    CHECK(sockrestart_repair_after_accept_error(&fd, ENOTSOCK));
    CHECK(!is_fifo(host));
    CHECK(!(fcntl(host, F_GETFL) & O_NONBLOCK));
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(connect(c, (struct sockaddr *) &a, sizeof(a)) == 0);
    int s = accept(host, NULL, NULL);
    CHECK(s >= 0);
    close(s);
    close(c);
    sockrestart_end_listen(&fd);
    close(host);
    unlink(path);
    printf("PASS reclaimed Unix listener rebuilt at its host path, guest blocking mode kept\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    struct tgroup group = {0};
    struct task task = {.group = &group};
    list_init(&task.sockrestart.listen);
    current = &task;
    char dir[] = "/tmp/sockrestart-test.XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    alarm(20);
    test_routine_and_intact();
    test_tcp_reclaimed();
    test_port_taken();
    test_spinning_but_healthy();
    test_unix_reclaimed(dir);
    alarm(0);
    rmdir(dir);
    return 0;
}
