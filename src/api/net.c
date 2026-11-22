#include <xenus/net.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DNS_PORT 53
#define DNS_A 1
#define DNS_AAAA 28
#define DNS_CLASS_IN 1
#define DNS_RCODE_NXDOMAIN 3

static xenus_net_config_t g_cfg;
static int g_ready;
static unsigned int g_id_counter;

static long long now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int remaining_ms(long long deadline)
{
    long long left = deadline - now_ms();

    if (left <= 0)
        return 0;
    if (left > 60000)
        left = 60000;
    return (int)left;
}

static int wait_fd(int fd, short events, int timeout_ms)
{
    struct pollfd pfd;
    int rc;

    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    do {
        rc = poll(&pfd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR);
    if (rc <= 0)
        return rc;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
        return 1;
    return 1;
}

static int set_nonblock_cloexec(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
        return -1;
    return 0;
}

static int make_socket(int family, int type)
{
    int fd;
    int one = 1;

    fd = socket(family, type, 0);
    if (fd < 0)
        return XENUS_NET_ERR_IO;
    if (set_nonblock_cloexec(fd) != 0) {
        close(fd);
        return XENUS_NET_ERR_IO;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    return fd;
}

static int sockaddr_from_addr(const xenus_net_addr_t *addr, uint16_t port,
                              struct sockaddr_storage *ss, socklen_t *len)
{
    memset(ss, 0, sizeof(*ss));
    if (addr->family == XENUS_NET_IPV4) {
        struct sockaddr_in *in = (struct sockaddr_in *)ss;

        in->sin_family = AF_INET;
        in->sin_port = htons(port);
        memcpy(&in->sin_addr, addr->bytes, 4);
        *len = sizeof(*in);
        return 0;
    }
    if (addr->family == XENUS_NET_IPV6) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)ss;

        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(port);
        memcpy(&in6->sin6_addr, addr->bytes, 16);
        *len = sizeof(*in6);
        return 0;
    }
    return XENUS_NET_ERR_ARG;
}

static int addr_from_text(const char *text, xenus_net_addr_t *out)
{
    struct in_addr v4;
    struct in6_addr v6;

    if (text == NULL || out == NULL)
        return XENUS_NET_ERR_ARG;
    memset(out, 0, sizeof(*out));
    if (inet_pton(AF_INET, text, &v4) == 1) {
        out->family = XENUS_NET_IPV4;
        memcpy(out->bytes, &v4, 4);
        if (inet_ntop(AF_INET, &v4, out->text, sizeof(out->text)) == NULL)
            return XENUS_NET_ERR;
        return 0;
    }
    if (inet_pton(AF_INET6, text, &v6) == 1) {
        out->family = XENUS_NET_IPV6;
        memcpy(out->bytes, &v6, 16);
        if (inet_ntop(AF_INET6, &v6, out->text, sizeof(out->text)) == NULL)
            return XENUS_NET_ERR;
        return 0;
    }
    return XENUS_NET_ERR_RESOLVE;
}

static int add_nameserver(xenus_net_config_t *cfg, const char *text)
{
    xenus_net_addr_t addr;
    int i;

    if (cfg->nameserver_count >= XENUS_NET_NS_MAX)
        return 0;
    if (addr_from_text(text, &addr) != 0)
        return 0;
    for (i = 0; i < cfg->nameserver_count; i++) {
        if (strcmp(cfg->nameservers[i], addr.text) == 0)
            return 0;
    }
    snprintf(cfg->nameservers[cfg->nameserver_count],
             sizeof(cfg->nameservers[0]), "%s", addr.text);
    cfg->nameserver_count++;
    return 1;
}

static void parse_nameservers_file(xenus_net_config_t *cfg, const char *path)
{
    FILE *f;
    char line[256];

    f = fopen(path, "r");
    if (f == NULL)
        return;
    while (fgets(line, sizeof(line), f) != NULL) {
        char *p = line;
        char *end;
        char token[XENUS_NET_TEXT_MAX];

        while (*p == ' ' || *p == '\t')
            p++;
        if (strncmp(p, "nameserver", 10) != 0)
            continue;
        p += 10;
        if (*p != ' ' && *p != '\t')
            continue;
        while (*p == ' ' || *p == '\t')
            p++;
        end = p;
        while (*end != '\0' && *end != ' ' && *end != '\t' &&
               *end != '\n' && *end != '\r' && *end != '#')
            end++;
        if (end == p)
            continue;
        if ((size_t)(end - p) >= sizeof(token))
            continue;
        memcpy(token, p, (size_t)(end - p));
        token[end - p] = '\0';
        add_nameserver(cfg, token);
        if (cfg->nameserver_count >= XENUS_NET_NS_MAX)
            break;
    }
    fclose(f);
}

static void parse_nameservers_env(xenus_net_config_t *cfg)
{
    const char *env = getenv("XENUS_NET_DNS");
    char buf[XENUS_NET_NS_MAX * XENUS_NET_TEXT_MAX];
    char *save = NULL;
    char *tok;

    if (env == NULL || *env == '\0')
        return;
    snprintf(buf, sizeof(buf), "%s", env);
    tok = strtok_r(buf, ",; \t", &save);
    while (tok != NULL && cfg->nameserver_count < XENUS_NET_NS_MAX) {
        add_nameserver(cfg, tok);
        tok = strtok_r(NULL, ",; \t", &save);
    }
}

static void discover_nameservers(xenus_net_config_t *cfg)
{
    static const char *const paths[] = {
        "/etc/resolv.conf",
        "/run/systemd/resolve/resolv.conf",
        "/run/systemd/resolve/stub-resolv.conf",
        "/data/data/com.termux/files/usr/etc/resolv.conf"
    };
    size_t i;

    parse_nameservers_env(cfg);
    if (cfg->nameserver_count > 0)
        return;
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        parse_nameservers_file(cfg, paths[i]);
        if (cfg->nameserver_count > 0)
            return;
    }
}

void xenus_net_config_default(xenus_net_config_t *out)
{
    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));
    out->timeout_ms = XENUS_NET_TIMEOUT_DEFAULT_MS;
    out->retries = 1;
    discover_nameservers(out);
}

int xenus_net_init(const xenus_net_config_t *cfg)
{
    xenus_net_config_t next;
    int i;

    xenus_net_config_default(&next);
    if (cfg != NULL) {
        next.timeout_ms = cfg->timeout_ms > 0 ? cfg->timeout_ms
                                               : XENUS_NET_TIMEOUT_DEFAULT_MS;
        next.retries = cfg->retries > 0 ? cfg->retries : 1;
        if (next.retries > 5)
            next.retries = 5;
        if (cfg->nameserver_count > 0) {
            next.nameserver_count = 0;
            memset(next.nameservers, 0, sizeof(next.nameservers));
            for (i = 0; i < cfg->nameserver_count &&
                        i < XENUS_NET_NS_MAX; i++)
                add_nameserver(&next, cfg->nameservers[i]);
        }
    }
    g_cfg = next;
    g_ready = 1;
    return XENUS_NET_OK;
}

void xenus_net_shutdown(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_ready = 0;
}

int xenus_net_is_ready(void)
{
    return g_ready;
}

const xenus_net_config_t *xenus_net_config(void)
{
    return g_ready ? &g_cfg : NULL;
}

int xenus_net_get_info(xenus_net_info_t *out)
{
    int fd;

    if (out == NULL)
        return XENUS_NET_ERR_ARG;
    if (!g_ready)
        xenus_net_init(NULL);
    memset(out, 0, sizeof(*out));
    out->nameserver_count = g_cfg.nameserver_count;
    memcpy(out->nameservers, g_cfg.nameservers, sizeof(out->nameservers));
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) {
        out->socket_ok = 1;
        close(fd);
    }
    out->available = out->socket_ok && out->nameserver_count > 0;
    return XENUS_NET_OK;
}

static uint16_t read_u16(const unsigned char *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void write_u16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)(v & 0xff);
}

static uint16_t dns_next_id(void)
{
    unsigned char buf[2];
    int fd;
    ssize_t n;
    struct timespec ts;

    fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        n = read(fd, buf, sizeof(buf));
        close(fd);
        if (n == (ssize_t)sizeof(buf))
            return read_u16(buf);
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_id_counter++;
    return (uint16_t)((unsigned int)ts.tv_nsec ^
                      ((unsigned int)ts.tv_sec << 7) ^
                      ((unsigned int)getpid() << 3) ^ g_id_counter);
}

static int dns_build_query(const char *host, uint16_t qtype,
                           unsigned char *buf, size_t cap, size_t *out_len,
                           uint16_t *out_id)
{
    size_t len = strlen(host);
    size_t pos = 12;
    const char *p = host;
    uint16_t id = dns_next_id();

    if (len == 0 || len > XENUS_NET_HOST_MAX)
        return XENUS_NET_ERR_ARG;
    if (host[len - 1] == '.')
        len--;
    if (len == 0 || len > XENUS_NET_HOST_MAX)
        return XENUS_NET_ERR_ARG;
    if (cap < 12 + len + 6)
        return XENUS_NET_ERR_ARG;
    memset(buf, 0, cap);
    write_u16(buf, id);
    write_u16(buf + 2, 0x0100);
    write_u16(buf + 4, 1);
    while ((size_t)(p - host) < len) {
        const char *dot = memchr(p, '.', len - (size_t)(p - host));
        size_t label = dot != NULL ? (size_t)(dot - p)
                                   : len - (size_t)(p - host);

        if (label == 0 || label > 63)
            return XENUS_NET_ERR_ARG;
        if (pos + 1 + label >= cap)
            return XENUS_NET_ERR_ARG;
        buf[pos++] = (unsigned char)label;
        memcpy(buf + pos, p, label);
        pos += label;
        if (dot == NULL)
            break;
        p = dot + 1;
    }
    buf[pos++] = 0;
    if (pos + 4 > cap)
        return XENUS_NET_ERR_ARG;
    write_u16(buf + pos, qtype);
    pos += 2;
    write_u16(buf + pos, DNS_CLASS_IN);
    pos += 2;
    *out_len = pos;
    *out_id = id;
    return 0;
}

static int dns_skip_name(const unsigned char *p, size_t len, size_t *pos)
{
    size_t i = *pos;
    int labels = 0;

    while (i < len) {
        unsigned char c = p[i];

        if ((c & 0xc0) == 0xc0) {
            if (i + 1 >= len)
                return -1;
            *pos = i + 2;
            return 0;
        }
        if ((c & 0xc0) != 0)
            return -1;
        if (c == 0) {
            *pos = i + 1;
            return 0;
        }
        if (i + 1 + c > len)
            return -1;
        i += 1 + c;
        if (++labels > 128)
            return -1;
    }
    return -1;
}

static int dns_parse_response(const unsigned char *p, size_t len,
                              uint16_t want_id, xenus_net_addr_t *out,
                              size_t out_capacity, int *truncated)
{
    uint16_t flags, qd, an;
    size_t pos = 12;
    size_t count = 0;
    unsigned int i;
    int rcode;

    if (len < 12)
        return XENUS_NET_ERR_RESOLVE;
    if (read_u16(p) != want_id)
        return XENUS_NET_ERR_RESOLVE;
    flags = read_u16(p + 2);
    if ((flags & 0x8000) == 0)
        return XENUS_NET_ERR_RESOLVE;
    if ((flags & 0x0200) != 0) {
        if (truncated != NULL)
            *truncated = 1;
        return 0;
    }
    rcode = flags & 0x000f;
    if (rcode == DNS_RCODE_NXDOMAIN)
        return 0;
    if (rcode != 0)
        return XENUS_NET_ERR_RESOLVE;
    qd = read_u16(p + 4);
    an = read_u16(p + 6);
    for (i = 0; i < qd; i++) {
        if (dns_skip_name(p, len, &pos) != 0 || pos + 4 > len)
            return XENUS_NET_ERR_RESOLVE;
        pos += 4;
    }
    for (i = 0; i < an; i++) {
        uint16_t type, class_id, rdlen;

        if (dns_skip_name(p, len, &pos) != 0 || pos + 10 > len)
            return XENUS_NET_ERR_RESOLVE;
        type = read_u16(p + pos);
        class_id = read_u16(p + pos + 2);
        rdlen = read_u16(p + pos + 8);
        pos += 10;
        if (pos + rdlen > len)
            return XENUS_NET_ERR_RESOLVE;
        if (class_id == DNS_CLASS_IN && count < out_capacity) {
            if (type == DNS_A && rdlen == 4) {
                struct in_addr v4;

                memset(&out[count], 0, sizeof(out[count]));
                out[count].family = XENUS_NET_IPV4;
                memcpy(out[count].bytes, p + pos, 4);
                memcpy(&v4, p + pos, 4);
                if (inet_ntop(AF_INET, &v4, out[count].text,
                              sizeof(out[count].text)) != NULL)
                    count++;
            } else if (type == DNS_AAAA && rdlen == 16) {
                struct in6_addr v6;

                memset(&out[count], 0, sizeof(out[count]));
                out[count].family = XENUS_NET_IPV6;
                memcpy(out[count].bytes, p + pos, 16);
                memcpy(&v6, p + pos, 16);
                if (inet_ntop(AF_INET6, &v6, out[count].text,
                              sizeof(out[count].text)) != NULL)
                    count++;
            }
        }
        pos += rdlen;
    }
    return (int)count;
}

static int dns_send_udp(const xenus_net_addr_t *ns, const unsigned char *query,
                        size_t query_len, unsigned char *response,
                        size_t response_cap, int timeout_ms)
{
    struct sockaddr_storage ss;
    socklen_t ss_len;
    long long deadline = now_ms() + timeout_ms;
    int fd;
    int rc;
    int saved = XENUS_NET_ERR_RESOLVE;
    ssize_t n;

    if (sockaddr_from_addr(ns, DNS_PORT, &ss, &ss_len) != 0)
        return XENUS_NET_ERR_ARG;
    fd = make_socket(ns->family == XENUS_NET_IPV6 ? AF_INET6 : AF_INET,
                     SOCK_DGRAM);
    if (fd < 0)
        return fd;
    rc = connect(fd, (struct sockaddr *)&ss, ss_len);
    if (rc != 0 && errno != EINPROGRESS) {
        close(fd);
        return XENUS_NET_ERR_IO;
    }
    rc = wait_fd(fd, POLLOUT, remaining_ms(deadline));
    if (rc == 0) {
        close(fd);
        return XENUS_NET_ERR_TIMEOUT;
    }
    do {
        n = send(fd, query, query_len, 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        saved = errno == EAGAIN ? XENUS_NET_ERR_TIMEOUT : XENUS_NET_ERR_IO;
        close(fd);
        return saved;
    }
    for (;;) {
        rc = wait_fd(fd, POLLIN, remaining_ms(deadline));
        if (rc == 0) {
            close(fd);
            return XENUS_NET_ERR_TIMEOUT;
        }
        do {
            n = recv(fd, response, response_cap, 0);
        } while (n < 0 && errno == EINTR);
        if (n >= 0) {
            close(fd);
            return (int)n;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            close(fd);
            return XENUS_NET_ERR_IO;
        }
        if (remaining_ms(deadline) == 0) {
            close(fd);
            return XENUS_NET_ERR_TIMEOUT;
        }
    }
}

static int send_all_fd(int fd, const void *data, size_t len, int timeout_ms)
{
    const unsigned char *p = data;
    size_t done = 0;
    long long deadline = now_ms() + timeout_ms;

    while (done < len) {
        ssize_t n;
        int rc;

        rc = wait_fd(fd, POLLOUT, remaining_ms(deadline));
        if (rc == 0)
            return XENUS_NET_ERR_TIMEOUT;
#ifdef MSG_NOSIGNAL
        n = send(fd, p + done, len - done, MSG_NOSIGNAL);
#else
        n = send(fd, p + done, len - done, 0);
#endif
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (remaining_ms(deadline) == 0)
                return XENUS_NET_ERR_TIMEOUT;
            continue;
        }
        return XENUS_NET_ERR_IO;
    }
    return (int)done;
}

static int recv_exact_fd(int fd, void *data, size_t len, int timeout_ms)
{
    unsigned char *p = data;
    size_t done = 0;
    long long deadline = now_ms() + timeout_ms;

    while (done < len) {
        ssize_t n;
        int rc;

        rc = wait_fd(fd, POLLIN, remaining_ms(deadline));
        if (rc == 0)
            return XENUS_NET_ERR_TIMEOUT;
        n = recv(fd, p + done, len - done, 0);
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n == 0)
            return XENUS_NET_ERR_IO;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (remaining_ms(deadline) == 0)
                return XENUS_NET_ERR_TIMEOUT;
            continue;
        }
        return XENUS_NET_ERR_IO;
    }
    return (int)done;
}

static int dns_send_tcp(const xenus_net_addr_t *ns, const unsigned char *query,
                        size_t query_len, unsigned char *response,
                        size_t response_cap, int timeout_ms)
{
    unsigned char prefix[2];
    unsigned char *wire;
    size_t wire_len = query_len + 2;
    int fd;
    int rc;
    int result;

    if (wire_len > 65535)
        return XENUS_NET_ERR_ARG;
    wire = malloc(wire_len);
    if (wire == NULL)
        return XENUS_NET_ERR_NOMEM;
    write_u16(prefix, (uint16_t)query_len);
    memcpy(wire, prefix, 2);
    memcpy(wire + 2, query, query_len);
    fd = xenus_net_tcp_connect_addr(ns, DNS_PORT, timeout_ms);
    if (fd < 0) {
        free(wire);
        return fd;
    }
    rc = send_all_fd(fd, wire, wire_len, timeout_ms);
    free(wire);
    if (rc < 0) {
        xenus_net_close(fd);
        return rc;
    }
    rc = recv_exact_fd(fd, prefix, 2, timeout_ms);
    if (rc < 0) {
        xenus_net_close(fd);
        return rc;
    }
    {
        size_t n = read_u16(prefix);

        if (n == 0 || n > response_cap) {
            xenus_net_close(fd);
            return XENUS_NET_ERR_IO;
        }
        result = recv_exact_fd(fd, response, n, timeout_ms);
    }
    xenus_net_close(fd);
    return result < 0 ? result : (int)read_u16(prefix);
}

static int dns_query_type(const char *host, uint16_t type, int timeout_ms,
                          xenus_net_addr_t *out, size_t out_capacity)
{
    unsigned char query[512];
    unsigned char response[2048];
    size_t query_len = 0;
    uint16_t query_id = 0;
    int rc;
    int attempt;
    int i;

    rc = dns_build_query(host, type, query, sizeof(query), &query_len,
                         &query_id);
    if (rc != 0)
        return rc;
    for (i = 0; i < g_cfg.nameserver_count; i++) {
        xenus_net_addr_t ns;
        int truncated = 0;

        if (addr_from_text(g_cfg.nameservers[i], &ns) != 0)
            continue;
        for (attempt = 0; attempt < g_cfg.retries; attempt++) {
            int n = dns_send_udp(&ns, query, query_len, response,
                                 sizeof(response), timeout_ms);

            if (n > 0) {
                int count = dns_parse_response(response, (size_t)n, query_id,
                                               out, out_capacity, &truncated);

                if (truncated) {
                    n = dns_send_tcp(&ns, query, query_len, response,
                                     sizeof(response), timeout_ms);
                    if (n > 0)
                        count = dns_parse_response(response, (size_t)n,
                                                   query_id, out,
                                                   out_capacity, NULL);
                }
                if (count > 0)
                    return count;
                if (count < 0)
                    continue;
                if (!truncated)
                    break;
            } else if (n == XENUS_NET_ERR_TIMEOUT) {
                continue;
            } else {
                break;
            }
        }
    }
    return 0;
}

int xenus_net_resolve(const char *host, xenus_net_addr_t *out,
                     size_t out_capacity, int timeout_ms)
{
    xenus_net_addr_t numeric;
    int count;

    if (host == NULL || out == NULL || out_capacity == 0)
        return XENUS_NET_ERR_ARG;
    if (addr_from_text(host, &numeric) == 0) {
        out[0] = numeric;
        return 1;
    }
    if (!g_ready)
        xenus_net_init(NULL);
    if (g_cfg.nameserver_count == 0)
        return XENUS_NET_ERR_RESOLVE;
    if (timeout_ms <= 0)
        timeout_ms = g_cfg.timeout_ms;
    if (timeout_ms <= 0)
        timeout_ms = XENUS_NET_TIMEOUT_DEFAULT_MS;
    memset(out, 0, out_capacity * sizeof(out[0]));
    count = dns_query_type(host, DNS_A, timeout_ms, out, out_capacity);
    if (count > 0)
        return count;
    count = dns_query_type(host, DNS_AAAA, timeout_ms, out, out_capacity);
    if (count > 0)
        return count;
    return XENUS_NET_ERR_RESOLVE;
}

int xenus_net_tcp_connect_addr(const xenus_net_addr_t *addr, uint16_t port,
                              int timeout_ms)
{
    struct sockaddr_storage ss;
    socklen_t ss_len;
    long long deadline;
    int fd;
    int rc;
    int err = 0;
    socklen_t err_len = sizeof(err);

    if (addr == NULL)
        return XENUS_NET_ERR_ARG;
    if (timeout_ms <= 0)
        timeout_ms = XENUS_NET_TIMEOUT_DEFAULT_MS;
    deadline = now_ms() + timeout_ms;
    if (sockaddr_from_addr(addr, port, &ss, &ss_len) != 0)
        return XENUS_NET_ERR_ARG;
    fd = make_socket(addr->family == XENUS_NET_IPV6 ? AF_INET6 : AF_INET,
                     SOCK_STREAM);
    if (fd < 0)
        return fd;
    rc = connect(fd, (struct sockaddr *)&ss, ss_len);
    if (rc != 0 && errno != EINPROGRESS && errno != EINTR) {
        close(fd);
        return XENUS_NET_ERR_CONNECT;
    }
    if (rc != 0) {
        rc = wait_fd(fd, POLLOUT, remaining_ms(deadline));
        if (rc == 0) {
            close(fd);
            return XENUS_NET_ERR_TIMEOUT;
        }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) != 0 ||
            err != 0) {
            close(fd);
            return XENUS_NET_ERR_CONNECT;
        }
    }
    return fd;
}

int xenus_net_tcp_connect(const char *host, uint16_t port, int timeout_ms)
{
    xenus_net_addr_t addrs[XENUS_NET_ADDR_MAX];
    long long deadline;
    int count;
    int i;
    int last = XENUS_NET_ERR_CONNECT;

    if (host == NULL)
        return XENUS_NET_ERR_ARG;
    if (timeout_ms <= 0)
        timeout_ms = XENUS_NET_TIMEOUT_DEFAULT_MS;
    count = xenus_net_resolve(host, addrs, XENUS_NET_ADDR_MAX, timeout_ms);
    if (count < 0)
        return count;
    deadline = now_ms() + timeout_ms;
    for (i = 0; i < count; i++) {
        int left = remaining_ms(deadline);
        int fd;

        if (left == 0)
            return XENUS_NET_ERR_TIMEOUT;
        fd = xenus_net_tcp_connect_addr(&addrs[i], port, left);
        if (fd >= 0)
            return fd;
        last = fd;
    }
    return last;
}

int xenus_net_udp_open_addr(const xenus_net_addr_t *addr, uint16_t port,
                           int timeout_ms)
{
    struct sockaddr_storage ss;
    socklen_t ss_len;
    int fd;

    (void)timeout_ms;
    if (addr == NULL || sockaddr_from_addr(addr, port, &ss, &ss_len) != 0)
        return XENUS_NET_ERR_ARG;
    fd = make_socket(addr->family == XENUS_NET_IPV6 ? AF_INET6 : AF_INET,
                     SOCK_DGRAM);
    if (fd < 0)
        return fd;
    if (connect(fd, (struct sockaddr *)&ss, ss_len) != 0) {
        close(fd);
        return XENUS_NET_ERR_CONNECT;
    }
    return fd;
}

int xenus_net_udp_open(const char *host, uint16_t port, int timeout_ms)
{
    xenus_net_addr_t addrs[XENUS_NET_ADDR_MAX];
    int count;
    int i;

    if (host == NULL)
        return XENUS_NET_ERR_ARG;
    if (timeout_ms <= 0)
        timeout_ms = XENUS_NET_TIMEOUT_DEFAULT_MS;
    count = xenus_net_resolve(host, addrs, XENUS_NET_ADDR_MAX, timeout_ms);
    if (count < 0)
        return count;
    for (i = 0; i < count; i++) {
        int fd = xenus_net_udp_open_addr(&addrs[i], port, timeout_ms);

        if (fd >= 0)
            return fd;
    }
    return XENUS_NET_ERR_CONNECT;
}

int xenus_net_send(int fd, const void *data, size_t len, int timeout_ms)
{
    const unsigned char *p = data;
    size_t done = 0;
    long long deadline;

    if (fd < 0 || (data == NULL && len > 0))
        return XENUS_NET_ERR_ARG;
    if (timeout_ms <= 0)
        timeout_ms = XENUS_NET_TIMEOUT_DEFAULT_MS;
    deadline = now_ms() + timeout_ms;
    while (done < len) {
        ssize_t n;
        int rc;

        rc = wait_fd(fd, POLLOUT, remaining_ms(deadline));
        if (rc == 0)
            return XENUS_NET_ERR_TIMEOUT;
#ifdef MSG_NOSIGNAL
        n = send(fd, p + done, len - done, MSG_NOSIGNAL);
#else
        n = send(fd, p + done, len - done, 0);
#endif
        if (n > 0)
            return (int)(done + (size_t)n);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (remaining_ms(deadline) == 0)
                return XENUS_NET_ERR_TIMEOUT;
            continue;
        }
        return XENUS_NET_ERR_IO;
    }
    return (int)done;
}

int xenus_net_send_all(int fd, const void *data, size_t len, int timeout_ms)
{
    if (fd < 0 || (data == NULL && len > 0))
        return XENUS_NET_ERR_ARG;
    if (timeout_ms <= 0)
        timeout_ms = XENUS_NET_TIMEOUT_DEFAULT_MS;
    return send_all_fd(fd, data, len, timeout_ms);
}

int xenus_net_recv(int fd, void *data, size_t len, int timeout_ms)
{
    long long deadline;

    if (fd < 0 || data == NULL)
        return XENUS_NET_ERR_ARG;
    if (timeout_ms <= 0)
        timeout_ms = XENUS_NET_TIMEOUT_DEFAULT_MS;
    deadline = now_ms() + timeout_ms;
    for (;;) {
        ssize_t n;
        int rc = wait_fd(fd, POLLIN, remaining_ms(deadline));

        if (rc == 0)
            return XENUS_NET_ERR_TIMEOUT;
        n = recv(fd, data, len, 0);
        if (n >= 0)
            return (int)n;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (remaining_ms(deadline) == 0)
                return XENUS_NET_ERR_TIMEOUT;
            continue;
        }
        return XENUS_NET_ERR_IO;
    }
}

int xenus_net_recvfrom(int fd, void *data, size_t len, int timeout_ms,
                      xenus_net_addr_t *from, uint16_t *from_port)
{
    long long deadline;

    if (fd < 0 || data == NULL)
        return XENUS_NET_ERR_ARG;
    if (timeout_ms <= 0)
        timeout_ms = XENUS_NET_TIMEOUT_DEFAULT_MS;
    deadline = now_ms() + timeout_ms;
    for (;;) {
        struct sockaddr_storage ss;
        socklen_t ss_len = sizeof(ss);
        ssize_t n;
        int rc = wait_fd(fd, POLLIN, remaining_ms(deadline));

        if (rc == 0)
            return XENUS_NET_ERR_TIMEOUT;
        n = recvfrom(fd, data, len, 0, (struct sockaddr *)&ss, &ss_len);
        if (n >= 0) {
            if (from != NULL)
                memset(from, 0, sizeof(*from));
            if (from != NULL && ss.ss_family == AF_INET) {
                struct sockaddr_in *in = (struct sockaddr_in *)&ss;

                from->family = XENUS_NET_IPV4;
                memcpy(from->bytes, &in->sin_addr, 4);
                (void)inet_ntop(AF_INET, &in->sin_addr, from->text,
                                sizeof(from->text));
            } else if (from != NULL && ss.ss_family == AF_INET6) {
                struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&ss;

                from->family = XENUS_NET_IPV6;
                memcpy(from->bytes, &in6->sin6_addr, 16);
                (void)inet_ntop(AF_INET6, &in6->sin6_addr, from->text,
                                sizeof(from->text));
            }
            if (from_port != NULL) {
                if (ss.ss_family == AF_INET)
                    *from_port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
                else if (ss.ss_family == AF_INET6)
                    *from_port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
            }
            return (int)n;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (remaining_ms(deadline) == 0)
                return XENUS_NET_ERR_TIMEOUT;
            continue;
        }
        return XENUS_NET_ERR_IO;
    }
}

int xenus_net_close(int fd)
{
    if (fd < 0)
        return XENUS_NET_ERR_ARG;
    if (close(fd) != 0)
        return XENUS_NET_ERR_IO;
    return XENUS_NET_OK;
}

const char *xenus_net_strerror(int status)
{
    switch (status) {
    case XENUS_NET_OK:
        return "success";
    case XENUS_NET_ERR:
        return "network error";
    case XENUS_NET_ERR_ARG:
        return "invalid argument";
    case XENUS_NET_ERR_STATE:
        return "invalid state";
    case XENUS_NET_ERR_TIMEOUT:
        return "connection timed out";
    case XENUS_NET_ERR_RESOLVE:
        return "name resolution failed";
    case XENUS_NET_ERR_CONNECT:
        return "connection failed";
    case XENUS_NET_ERR_IO:
        return "socket i/o error";
    case XENUS_NET_ERR_NOMEM:
        return "out of memory";
    default:
        return "unknown network error";
    }
}
