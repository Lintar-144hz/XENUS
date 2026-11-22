#include <xenus/net.h>

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks;
static int failures;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg));           \
        }                                                                    \
    } while (0)

#define CHECK_INT(got, want, msg)                                            \
    do {                                                                     \
        long g_ = (long)(got);                                               \
        long w_ = (long)(want);                                              \
        checks++;                                                            \
        if (g_ != w_) {                                                      \
            failures++;                                                      \
            printf("FAIL %s:%d: %s (got %ld, want %ld)\n", __FILE__,         \
                   __LINE__, (msg), g_, w_);                                 \
        }                                                                    \
    } while (0)

static uint16_t read_u16(const unsigned char *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void write_u16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)(v & 0xff);
}

static size_t build_query(const char *host, uint16_t qtype,
                          unsigned char *buf, size_t cap)
{
    size_t pos = 12;
    const char *p = host;

    if (cap < 32)
        return 0;
    memset(buf, 0, cap);
    write_u16(buf, 0x5cd1);
    write_u16(buf + 2, 0x0100);
    write_u16(buf + 4, 1);
    while (*p != '\0') {
        const char *dot = strchr(p, '.');
        size_t label = dot != NULL ? (size_t)(dot - p) : strlen(p);

        if (label == 0 || label > 63 || pos + label + 1 >= cap)
            return 0;
        buf[pos++] = (unsigned char)label;
        memcpy(buf + pos, p, label);
        pos += label;
        if (dot == NULL)
            break;
        p = dot + 1;
    }
    buf[pos++] = 0;
    if (pos + 4 > cap)
        return 0;
    write_u16(buf + pos, qtype);
    pos += 2;
    write_u16(buf + pos, 1);
    return pos + 2;
}

static void test_offline(void)
{
    xenus_net_config_t cfg;
    xenus_net_config_t custom;
    xenus_net_info_t info;
    xenus_net_addr_t addr;
    const xenus_net_config_t *got;
    char ipv6[XENUS_NET_TEXT_MAX];

    xenus_net_shutdown();
    CHECK_INT(xenus_net_is_ready(), 0, "starts shut down");
    CHECK(xenus_net_config() == NULL, "config null when shut down");
    CHECK_INT(xenus_net_resolve(NULL, &addr, 1, 100), XENUS_NET_ERR_ARG,
              "resolve null host rejected");
    CHECK_INT(xenus_net_resolve("127.0.0.1", NULL, 1, 100),
              XENUS_NET_ERR_ARG, "resolve null output rejected");
    CHECK_INT(xenus_net_resolve("127.0.0.1", &addr, 0, 100),
              XENUS_NET_ERR_ARG, "resolve zero capacity rejected");
    CHECK_INT(xenus_net_tcp_connect(NULL, 80, 100), XENUS_NET_ERR_ARG,
              "tcp null host rejected");
    CHECK_INT(xenus_net_udp_open(NULL, 53, 100), XENUS_NET_ERR_ARG,
              "udp null host rejected");
    CHECK_INT(xenus_net_send(-1, "x", 1, 100), XENUS_NET_ERR_ARG,
              "send bad fd rejected");
    CHECK_INT(xenus_net_recv(-1, &addr, 1, 100), XENUS_NET_ERR_ARG,
              "recv bad fd rejected");
    CHECK_INT(xenus_net_close(-1), XENUS_NET_ERR_ARG,
              "close bad fd rejected");
    CHECK(strcmp(xenus_net_strerror(XENUS_NET_OK), "success") == 0,
          "strerror success");
    CHECK(strstr(xenus_net_strerror(XENUS_NET_ERR_TIMEOUT), "timed out") != NULL,
          "strerror timeout");

    xenus_net_config_default(&cfg);
    CHECK_INT(cfg.timeout_ms, XENUS_NET_TIMEOUT_DEFAULT_MS,
              "default timeout");
    CHECK_INT(cfg.retries, 1, "default retries");
    CHECK(cfg.nameserver_count >= 0 &&
              cfg.nameserver_count <= XENUS_NET_NS_MAX,
          "default nameserver count range");

    CHECK_INT(xenus_net_init(NULL), XENUS_NET_OK, "init");
    CHECK_INT(xenus_net_is_ready(), 1, "ready after init");
    got = xenus_net_config();
    CHECK(got != NULL, "config available");
    if (got != NULL) {
        CHECK_INT(got->timeout_ms, XENUS_NET_TIMEOUT_DEFAULT_MS,
                  "init timeout");
        CHECK_INT(got->retries, 1, "init retries");
    }
    CHECK_INT(xenus_net_resolve("127.0.0.1", &addr, 1, 100), 1,
              "numeric ipv4 resolve");
    CHECK_INT(addr.family, XENUS_NET_IPV4, "numeric ipv4 family");
    CHECK(strcmp(addr.text, "127.0.0.1") == 0, "numeric ipv4 text");
    CHECK_INT(xenus_net_resolve("::1", &addr, 1, 100), 1,
              "numeric ipv6 resolve");
    CHECK_INT(addr.family, XENUS_NET_IPV6, "numeric ipv6 family");
    CHECK(strcmp(addr.text, "::1") == 0, "numeric ipv6 text");

    snprintf(ipv6, sizeof(ipv6), "%s", "127.0.0.1");
    memset(&custom, 0, sizeof(custom));
    custom.timeout_ms = 250;
    custom.retries = 2;
    custom.nameserver_count = 1;
    snprintf(custom.nameservers[0], sizeof(custom.nameservers[0]), "%s",
             ipv6);
    CHECK_INT(xenus_net_init(&custom), XENUS_NET_OK, "custom init");
    got = xenus_net_config();
    CHECK(got != NULL && got->timeout_ms == 250 && got->retries == 2,
          "custom timeout/retries kept");
    CHECK(got != NULL && got->nameserver_count == 1 &&
              strcmp(got->nameservers[0], "127.0.0.1") == 0,
          "custom nameserver kept");

    CHECK_INT(xenus_net_get_info(&info), XENUS_NET_OK, "get info");
    CHECK_INT(info.nameserver_count, 1, "info nameserver count");
    CHECK_INT(info.socket_ok, 1, "socket works");
    CHECK_INT(info.available, 1, "driver available");
    CHECK_INT(xenus_net_get_info(NULL), XENUS_NET_ERR_ARG,
              "get info null rejected");

    custom.nameserver_count = 1;
    snprintf(custom.nameservers[0], sizeof(custom.nameservers[0]), "%s",
             "not-an-ip");
    CHECK_INT(xenus_net_init(&custom), XENUS_NET_OK, "init bad ns");
    CHECK_INT(xenus_net_resolve("example.com", &addr, 1, 20),
              XENUS_NET_ERR_RESOLVE, "bad nameserver reports resolve error");
    xenus_net_shutdown();
    CHECK_INT(xenus_net_is_ready(), 0, "shut down");
}

static int parse_dns_answer(const unsigned char *p, size_t len,
                            uint16_t want_id)
{
    uint16_t qd;
    uint16_t an;
    size_t pos = 12;
    size_t i;

    if (len < 12 || read_u16(p) != want_id)
        return 0;
    if ((read_u16(p + 2) & 0x8000) == 0)
        return 0;
    qd = read_u16(p + 4);
    an = read_u16(p + 6);
    for (i = 0; i < qd; i++) {
        for (;;) {
            unsigned char c;

            if (pos >= len)
                return 0;
            c = p[pos++];
            if (c == 0)
                break;
            if ((c & 0xc0) == 0xc0) {
                pos++;
                break;
            }
            pos += c;
        }
        pos += 4;
        if (pos > len)
            return 0;
    }
    return an > 0 && (read_u16(p + 2) & 0x000f) == 0;
}

static void test_udp_dns(const xenus_net_info_t *info, const char *host)
{
    unsigned char query[512];
    unsigned char response[2048];
    size_t query_len;
    xenus_net_addr_t from;
    uint16_t from_port = 0;
    int fd;
    int n;
    int rc;

    if (info->nameserver_count == 0)
        return;
    query_len = build_query(host, 1, query, sizeof(query));
    CHECK(query_len > 0, "build udp dns query");
    if (query_len == 0)
        return;
    fd = xenus_net_udp_open(info->nameservers[0], 53, 2000);
    CHECK(fd >= 0, "udp open nameserver");
    if (fd < 0) {
        printf("  udp open: %s\n", xenus_net_strerror(fd));
        return;
    }
    rc = xenus_net_send(fd, query, query_len, 2000);
    CHECK_INT(rc, (int)query_len, "udp send dns query");
    if (rc == (int)query_len) {
        n = xenus_net_recvfrom(fd, response, sizeof(response), 3000, &from,
                              &from_port);
        CHECK(n > 0, "udp recv dns response");
        if (n > 0) {
            CHECK_INT(from.family, XENUS_NET_IPV4, "udp source family");
            CHECK_INT(from_port, 53, "udp source port");
            CHECK(parse_dns_answer(response, (size_t)n, 0x5cd1) != 0,
                  "udp dns answer parsed");
        } else {
            printf("  udp recv: %s\n", xenus_net_strerror(n));
        }
    } else {
        printf("  udp send: %s\n", xenus_net_strerror(rc));
    }
    CHECK_INT(xenus_net_close(fd), XENUS_NET_OK, "udp close");
}

static void test_http(const char *host)
{
    char request[512];
    char response[2048];
    int fd;
    int n;

    snprintf(request, sizeof(request),
             "GET / HTTP/1.0\r\nHost: %s\r\nUser-Agent: xenus-net-test\r\n"
             "Connection: close\r\n\r\n", host);
    fd = xenus_net_tcp_connect(host, 80, 5000);
    CHECK(fd >= 0, "tcp connect http");
    if (fd < 0) {
        printf("  http connect: %s\n", xenus_net_strerror(fd));
        return;
    }
    n = xenus_net_send_all(fd, request, strlen(request), 5000);
    CHECK_INT(n, (int)strlen(request), "send http request");
    if (n == (int)strlen(request)) {
        n = xenus_net_recv(fd, response, sizeof(response) - 1, 5000);
        CHECK(n > 0, "recv http response");
        if (n > 0) {
            response[n] = '\0';
            CHECK(strncmp(response, "HTTP/", 5) == 0,
                  "http response starts with status line");
            printf("  http: %.43s\n", response);
        } else {
            printf("  http recv: %s\n", xenus_net_strerror(n));
        }
    } else {
        printf("  http send: %s\n", xenus_net_strerror(n));
    }
    CHECK_INT(xenus_net_close(fd), XENUS_NET_OK, "close http");
}

static void test_tls_transport(const char *host)
{
    int fd;

    fd = xenus_net_tcp_connect(host, 443, 5000);
    CHECK(fd >= 0, "tcp connect https transport");
    if (fd < 0)
        printf("  https transport: %s\n", xenus_net_strerror(fd));
    else
        CHECK_INT(xenus_net_close(fd), XENUS_NET_OK, "close https transport");
}

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "example.com";
    xenus_net_addr_t addrs[XENUS_NET_ADDR_MAX];
    xenus_net_info_t info;
    int count;
    int i;

    test_offline();
    CHECK_INT(xenus_net_init(NULL), XENUS_NET_OK, "live init");
    CHECK_INT(xenus_net_get_info(&info), XENUS_NET_OK, "live get info");
    printf("net: nameservers=%d socket=%d available=%d\n",
           info.nameserver_count, info.socket_ok, info.available);
    for (i = 0; i < info.nameserver_count; i++)
        printf("net: ns[%d]=%s\n", i, info.nameservers[i]);
    if (!info.available) {
        printf("net: no usable resolver/socket, live checks skipped\n");
        xenus_net_shutdown();
        printf("net_test: %d checks, %d failures\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
    count = xenus_net_resolve(host, addrs, XENUS_NET_ADDR_MAX, 5000);
    CHECK(count > 0, "resolve live host");
    if (count > 0) {
        for (i = 0; i < count && i < 3; i++)
            printf("net: %s -> %s\n", host, addrs[i].text);
    } else {
        printf("  resolve: %s\n", xenus_net_strerror(count));
    }
    test_udp_dns(&info, host);
    if (count > 0) {
        test_http(host);
        test_tls_transport(host);
    }
    xenus_net_shutdown();
    printf("net_test: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
