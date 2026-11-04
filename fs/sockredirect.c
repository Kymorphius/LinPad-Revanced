#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "fs/sockredirect.h"
#include "fs/proc.h"
#include "kernel/errno.h"
#include "util/sync.h"

#define MAX_BYPASS 32
#define MAX_LEASE_SECONDS 3600
#define NS_PER_SEC 1000000000ull

// Private, link-local, CGNAT (Tailscale) and multicast ranges stay direct
// until the manager says otherwise.
static const char default_bypass[] =
    "0.0.0.0/8 10.0.0.0/8 100.64.0.0/10 127.0.0.0/8 169.254.0.0/16 172.16.0.0/12 "
    "192.168.0.0/16 224.0.0.0/3 ::1/128 fc00::/7 fe80::/10 ff00::/8";

struct cidr {
    int family;
    uint8_t addr[16];
    unsigned prefix;
};

struct config {
    uint16_t tcp_port;
    bool exempt;
    unsigned exempt_uid;
    uint64_t lease_until_ns;
    unsigned nbypass;
    struct cidr bypass[MAX_BYPASS];
};

// The original destination of one redirected client socket, keyed by the
// client's local address as the proxy sees it as its peer.
struct entry {
    bool v6;
    uint16_t port;
    struct sockaddr_storage dst;
    socklen_t len;
};

static lock_t lock = LOCK_INITIALIZER;
static struct config config;
static bool defaults_loaded;
static uint64_t redirected_total, lookups_total, lookup_misses;
static struct entry *entries;
static size_t nentries, capentries;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
}

static bool parse_cidr(const char *text, struct cidr *out) {
    char buf[64];
    if (strlen(text) >= sizeof(buf))
        return false;
    strcpy(buf, text);
    char *slash = strchr(buf, '/');
    if (slash != NULL)
        *slash = '\0';
    memset(out, 0, sizeof(*out));
    unsigned max;
    if (inet_pton(AF_INET, buf, out->addr) == 1) {
        out->family = AF_INET;
        max = 32;
    } else if (inet_pton(AF_INET6, buf, out->addr) == 1) {
        out->family = AF_INET6;
        max = 128;
    } else {
        return false;
    }
    out->prefix = max;
    if (slash != NULL) {
        char *end;
        unsigned long prefix = strtoul(slash + 1, &end, 10);
        if (slash[1] == '\0' || *end != '\0' || prefix > max)
            return false;
        out->prefix = (unsigned) prefix;
    }
    return true;
}

// Space-separated CIDRs into list; false on any malformed one or too many.
static bool parse_bypass(const char *text, struct cidr *list, unsigned *count) {
    char *copy = strdup(text);
    if (copy == NULL)
        return false;
    unsigned n = 0;
    bool ok = true;
    char *save = NULL;
    for (char *tok = strtok_r(copy, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        if (n == MAX_BYPASS || !parse_cidr(tok, &list[n])) {
            ok = false;
            break;
        }
        n++;
    }
    free(copy);
    if (ok)
        *count = n;
    return ok;
}

// The caller holds lock.
static void load_defaults(void) {
    if (defaults_loaded)
        return;
    defaults_loaded = true;
    parse_bypass(default_bypass, config.bypass, &config.nbypass);
}

// AF_INET or AF_INET6 into *out, with an IPv4-mapped IPv6 address turned into
// plain IPv4; false for other families.
static bool normalize(const struct sockaddr *in, struct sockaddr_storage *out, socklen_t *len) {
    memset(out, 0, sizeof(*out));
    if (in->sa_family == AF_INET) {
        memcpy(out, in, sizeof(struct sockaddr_in));
        *len = sizeof(struct sockaddr_in);
        return true;
    }
    if (in->sa_family != AF_INET6)
        return false;
    const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *) in;
    if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
        struct sockaddr_in *v4 = (struct sockaddr_in *) out;
#if defined(__APPLE__)
        v4->sin_len = sizeof(*v4);
#endif
        v4->sin_family = AF_INET;
        v4->sin_port = in6->sin6_port;
        memcpy(&v4->sin_addr, &in6->sin6_addr.s6_addr[12], 4);
        *len = sizeof(*v4);
        return true;
    }
    memcpy(out, in, sizeof(struct sockaddr_in6));
    *len = sizeof(struct sockaddr_in6);
    return true;
}

static const uint8_t *addr_bytes(const struct sockaddr_storage *addr) {
    if (addr->ss_family == AF_INET)
        return (const uint8_t *) &((const struct sockaddr_in *) addr)->sin_addr;
    return ((const struct sockaddr_in6 *) addr)->sin6_addr.s6_addr;
}

static uint16_t addr_port(const struct sockaddr_storage *addr) {
    if (addr->ss_family == AF_INET)
        return ntohs(((const struct sockaddr_in *) addr)->sin_port);
    return ntohs(((const struct sockaddr_in6 *) addr)->sin6_port);
}

static bool cidr_contains(const struct cidr *c, const struct sockaddr_storage *addr) {
    if (c->family != addr->ss_family)
        return false;
    const uint8_t *a = addr_bytes(addr);
    unsigned full = c->prefix / 8, rest = c->prefix % 8;
    if (memcmp(a, c->addr, full) != 0)
        return false;
    if (rest == 0)
        return true;
    uint8_t mask = (uint8_t) (0xff << (8 - rest));
    return (a[full] & mask) == (c->addr[full] & mask);
}

// Loopback and unspecified addresses, whatever the bypass list says: the
// proxy itself listens on loopback.
static bool never_redirected(const struct sockaddr_storage *addr) {
    const uint8_t *a = addr_bytes(addr);
    if (addr->ss_family == AF_INET)
        return a[0] == 127 || (a[0] | a[1] | a[2] | a[3]) == 0;
    static const uint8_t any[16], loop[16] = {[15] = 1};
    return memcmp(a, any, 16) == 0 || memcmp(a, loop, 16) == 0;
}

bool sockredirect_target(const struct sockaddr *dst, unsigned euid, uint16_t fixed_port,
        struct sockaddr_storage *target, socklen_t *target_len) {
    struct sockaddr_storage d;
    socklen_t dlen;
    if (!normalize(dst, &d, &dlen))
        return false;
    uint16_t port = fixed_port;
    if (port == 0) {
        lock(&lock);
        load_defaults();
        bool redirect = config.tcp_port != 0 && now_ns() < config.lease_until_ns &&
            !(config.exempt && euid == config.exempt_uid) && !never_redirected(&d);
        for (unsigned i = 0; redirect && i < config.nbypass; i++)
            if (cidr_contains(&config.bypass[i], &d))
                redirect = false;
        if (redirect) {
            port = config.tcp_port;
            redirected_total++;
        }
        unlock(&lock);
        if (!redirect)
            return false;
    }
    memset(target, 0, sizeof(*target));
    if (dst->sa_family == AF_INET) {
        struct sockaddr_in *v4 = (struct sockaddr_in *) target;
#if defined(__APPLE__)
        v4->sin_len = sizeof(*v4);
#endif
        v4->sin_family = AF_INET;
        v4->sin_port = htons(port);
        v4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        *target_len = sizeof(*v4);
    } else {
        // An IPv4 destination reached through an IPv6 socket keeps IPv4 on
        // the proxy side (::ffff:127.0.0.1), so the proxy asks SOL_IP for it.
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *) target;
#if defined(__APPLE__)
        v6->sin6_len = sizeof(*v6);
#endif
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(port);
        if (d.ss_family == AF_INET) {
            v6->sin6_addr.s6_addr[10] = v6->sin6_addr.s6_addr[11] = 0xff;
            v6->sin6_addr.s6_addr[12] = 127;
            v6->sin6_addr.s6_addr[15] = 1;
        } else {
            v6->sin6_addr = in6addr_loopback;
        }
        *target_len = sizeof(*v6);
    }
    return true;
}

// The caller holds lock.
static struct entry *find_entry(bool v6, uint16_t port) {
    for (size_t i = 0; i < nentries; i++)
        if (entries[i].v6 == v6 && entries[i].port == port)
            return &entries[i];
    return NULL;
}

uint16_t sockredirect_remember(int real_fd, const struct sockaddr *dst, bool *v6) {
    struct sockaddr_storage local, key;
    socklen_t llen = sizeof(local), klen;
    if (getsockname(real_fd, (struct sockaddr *) &local, &llen) < 0 || !normalize((struct sockaddr *) &local, &key, &klen))
        return 0;
    struct sockaddr_storage d;
    socklen_t dlen;
    if (!normalize(dst, &d, &dlen))
        return 0;
    bool key_v6 = key.ss_family == AF_INET6;
    uint16_t port = addr_port(&key);
    if (port == 0)
        return 0;
    lock(&lock);
    struct entry *e = find_entry(key_v6, port);
    if (e == NULL) {
        if (nentries == capentries) {
            size_t cap = capentries ? capentries * 2 : 64;
            struct entry *grown = realloc(entries, cap * sizeof(*grown));
            if (grown == NULL) {
                unlock(&lock);
                return 0;
            }
            entries = grown;
            capentries = cap;
        }
        e = &entries[nentries++];
    }
    e->v6 = key_v6;
    e->port = port;
    e->dst = d;
    e->len = dlen;
    unlock(&lock);
    *v6 = key_v6;
    return port;
}

void sockredirect_forget(bool v6, uint16_t port) {
    lock(&lock);
    struct entry *e = find_entry(v6, port);
    if (e != NULL)
        *e = entries[--nentries];
    unlock(&lock);
}

int sockredirect_lookup(bool v6, uint16_t port, struct sockaddr_storage *original, socklen_t *len) {
    lock(&lock);
    struct entry *e = find_entry(v6, port);
    if (e != NULL) {
        *original = e->dst;
        *len = e->len;
    }
    unlock(&lock);
    return e != NULL ? 0 : _ENOENT;
}

int sockredirect_original_dst(int accepted_fd, bool v6, struct sockaddr_storage *original, socklen_t *len) {
    struct sockaddr_storage peer, key;
    socklen_t plen = sizeof(peer), klen;
    int err = _ENOENT;
    if (getpeername(accepted_fd, (struct sockaddr *) &peer, &plen) == 0 &&
            normalize((struct sockaddr *) &peer, &key, &klen)) {
        err = sockredirect_lookup(key.ss_family == AF_INET6, addr_port(&key), original, len);
        // Linux answers SOL_IP for IPv4 and SOL_IPV6 for IPv6 destinations only.
        if (err == 0 && (original->ss_family == AF_INET6) != v6)
            err = _ENOENT;
    }
    lock(&lock);
    lookups_total++;
    if (err != 0)
        lookup_misses++;
    unlock(&lock);
    return err;
}

void sockredirect_show(struct proc_data *buf) {
    lock(&lock);
    load_defaults();
    uint64_t now = now_ns();
    bool active = config.tcp_port != 0 && now < config.lease_until_ns;
    proc_printf(buf, "active %d\n", active);
    proc_printf(buf, "tcp_port %u\n", config.tcp_port);
    if (config.exempt)
        proc_printf(buf, "exempt_uid %u\n", config.exempt_uid);
    else
        proc_printf(buf, "exempt_uid none\n");
    proc_printf(buf, "lease_remaining %llu\n",
            (unsigned long long) (active ? (config.lease_until_ns - now + NS_PER_SEC - 1) / NS_PER_SEC : 0));
    proc_printf(buf, "bypass");
    for (unsigned i = 0; i < config.nbypass; i++) {
        char text[INET6_ADDRSTRLEN];
        inet_ntop(config.bypass[i].family, config.bypass[i].addr, text, sizeof(text));
        proc_printf(buf, " %s/%u", text, config.bypass[i].prefix);
    }
    proc_printf(buf, "\n");
    proc_printf(buf, "redirected %llu\n", (unsigned long long) redirected_total);
    proc_printf(buf, "original_dst_lookups %llu\n", (unsigned long long) lookups_total);
    proc_printf(buf, "original_dst_misses %llu\n", (unsigned long long) lookup_misses);
    proc_printf(buf, "open %zu\n", nentries);
    unlock(&lock);
}

int sockredirect_update(const char *text, size_t size, unsigned euid) {
    if (euid != 0)
        return _EPERM;
    char *copy = strndup(text, size);
    if (copy == NULL)
        return _ENOMEM;
    lock(&lock);
    load_defaults();
    // All lines apply together or not at all.
    struct config next = config;
    int err = 0;
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line && err == 0; line = strtok_r(NULL, "\n", &save)) {
        char *value = strchr(line, ' ');
        if (value != NULL)
            *value++ = '\0';
        char *end = NULL;
        unsigned long n = value != NULL ? strtoul(value, &end, 10) : 0;
        bool number = value != NULL && *value != '\0' && *end == '\0';
        if (strcmp(line, "tcp_port") == 0 && number && n <= 65535) {
            next.tcp_port = (uint16_t) n;
        } else if (strcmp(line, "exempt_uid") == 0 && value != NULL && strcmp(value, "none") == 0) {
            next.exempt = false;
        } else if (strcmp(line, "exempt_uid") == 0 && number && n <= 0xffffffffUL) {
            next.exempt = true;
            next.exempt_uid = (unsigned) n;
        } else if (strcmp(line, "lease") == 0 && number && n <= MAX_LEASE_SECONDS) {
            next.lease_until_ns = n ? now_ns() + n * NS_PER_SEC : 0;
        } else if (strcmp(line, "bypass") == 0) {
            if (!parse_bypass(value != NULL ? value : "", next.bypass, &next.nbypass))
                err = _EINVAL;
        } else if (line[0] != '\0') {
            err = _EINVAL;
        }
    }
    if (err == 0)
        config = next;
    unlock(&lock);
    free(copy);
    return err;
}
