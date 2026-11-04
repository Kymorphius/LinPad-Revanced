// Transparent TCP redirection for guest programs. LinPad has no netfilter and
// no TUN device: a guest socket is a host socket, so there is nowhere to put an
// iptables REDIRECT rule. Instead, while a root-held lease is current, connect()
// to an outside address is sent to a local proxy, which recovers the original
// destination with getsockopt(SO_ORIGINAL_DST) as behind REDIRECT (sing-box's
// "redirect" inbound does exactly that). IPv4 destinations go to 127.0.0.1 and
// IPv6 ones to ::1, so the proxy queries the matching family.
//
// Control: /proc/ish/transparent_proxy. Writes (root only) are lines of
//   tcp_port N        proxy port on 127.0.0.1 and ::1 (0 turns it off)
//   exempt_uid N|none connections by this effective uid are never redirected
//                     (the proxy's own outbound connections)
//   lease N           redirect for the next N seconds (0..3600); renew it
//   bypass CIDR...    destinations left alone (replaces the list)
// Loopback and unspecified destinations are never redirected. When the lease
// runs out, for example because the proxy's manager stopped, connections go
// direct again.
#ifndef FS_SOCKREDIRECT_H
#define FS_SOCKREDIRECT_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
struct proc_data;

// For a TCP connect() to the host address dst by effective uid euid: whether
// it is redirected, and if so the proxy address in *target. A retry of a
// redirected non-blocking connect passes the port it used before as
// fixed_port and is redirected again regardless of the lease.
bool sockredirect_target(const struct sockaddr *dst, unsigned euid, uint16_t fixed_port,
        struct sockaddr_storage *target, socklen_t *target_len);

// After a redirected connect() started on host socket real_fd: remember dst
// under the socket's local address. Returns that local port (0 on failure)
// and whether it is IPv6, for sockredirect_forget.
uint16_t sockredirect_remember(int real_fd, const struct sockaddr *dst, bool *v6);
void sockredirect_forget(bool v6, uint16_t port);

// The original destination of the connection accepted as host socket
// accepted_fd, in the family asked for (false: sockaddr_in, true:
// sockaddr_in6), host layout. Returns 0 or _ENOENT.
int sockredirect_original_dst(int accepted_fd, bool v6, struct sockaddr_storage *original, socklen_t *len);
// The original destination of the redirected client socket with this local port.
int sockredirect_lookup(bool v6, uint16_t port, struct sockaddr_storage *original, socklen_t *len);

void sockredirect_show(struct proc_data *buf);
int sockredirect_update(const char *text, size_t size, unsigned euid);

#endif
