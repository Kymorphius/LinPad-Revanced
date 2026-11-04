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
//                     nor locked (the proxy's own outbound connections)
//   lease N           redirect for the next N seconds (0..3600); renew it
//   bypass CIDR...    destinations left alone (replaces the list)
//   netlock on|off    fail closed (below)
// Loopback and unspecified destinations are never redirected. An address that
// is none of these nor on the bypass list is "outside".
//
// Without the netlock, a connection goes direct when the lease runs out (for
// example because the proxy's manager stopped). With it nothing reaches the
// outside except through the proxy or as the exempt uid: a TCP connect() with
// no current lease, and any datagram (UDP, ICMP) to an outside address, fails
// with EPERM, as when Linux's netfilter drops a locally generated packet. The
// app turns it on at launch from its netlock preference, before the first
// process runs.
#ifndef FS_SOCKREDIRECT_H
#define FS_SOCKREDIRECT_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
struct proc_data;

// For a TCP connect() to the host address dst by effective uid euid: 1 if it
// is redirected, with the proxy address in *target; 0 if it goes direct;
// _EPERM if the netlock blocks it. A retry of a redirected non-blocking
// connect passes the port it used before as fixed_port and is redirected
// again regardless of the lease.
int sockredirect_tcp(const struct sockaddr *dst, unsigned euid, uint16_t fixed_port,
        struct sockaddr_storage *target, socklen_t *target_len);

// Whether dst (any family) is an outside address.
bool sockredirect_outside(const struct sockaddr *dst);
// Whether the netlock keeps euid from sending to an outside address: 0, or
// _EPERM. For datagrams, checked on every send, to the given address or to a
// connected socket's outside peer.
int sockredirect_locked(unsigned euid);
// sockredirect_locked if dst is outside, else 0.
int sockredirect_datagram(const struct sockaddr *dst, unsigned euid);

void sockredirect_set_netlock(bool on);

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
