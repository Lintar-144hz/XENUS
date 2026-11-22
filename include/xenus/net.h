#ifndef XENUS_APP_NET_H
#define XENUS_APP_NET_H

#include <xenus/common.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XENUS_NET_HOST_MAX 255
#define XENUS_NET_NS_MAX 4
#define XENUS_NET_ADDR_MAX 8
#define XENUS_NET_TEXT_MAX 64
#define XENUS_NET_TIMEOUT_DEFAULT_MS 5000

typedef enum {
    XENUS_NET_IPV4 = 4,
    XENUS_NET_IPV6 = 6
} xenus_net_family_t;

typedef struct {
    xenus_net_family_t family;
    unsigned char bytes[16];
    char text[XENUS_NET_TEXT_MAX];
} xenus_net_addr_t;

typedef struct {
    int timeout_ms;
    int retries;
    int nameserver_count;
    char nameservers[XENUS_NET_NS_MAX][XENUS_NET_TEXT_MAX];
} xenus_net_config_t;

typedef struct {
    int available;
    int socket_ok;
    int nameserver_count;
    char nameservers[XENUS_NET_NS_MAX][XENUS_NET_TEXT_MAX];
} xenus_net_info_t;

enum {
    XENUS_NET_OK = 0,
    XENUS_NET_ERR = -20,
    XENUS_NET_ERR_ARG = -21,
    XENUS_NET_ERR_STATE = -22,
    XENUS_NET_ERR_TIMEOUT = -23,
    XENUS_NET_ERR_RESOLVE = -24,
    XENUS_NET_ERR_CONNECT = -25,
    XENUS_NET_ERR_IO = -26,
    XENUS_NET_ERR_NOMEM = -27
};

void xenus_net_config_default(xenus_net_config_t *out);
int xenus_net_init(const xenus_net_config_t *cfg);
void xenus_net_shutdown(void);
int xenus_net_is_ready(void);
const xenus_net_config_t *xenus_net_config(void);
int xenus_net_get_info(xenus_net_info_t *out);

int xenus_net_resolve(const char *host, xenus_net_addr_t *out,
                     size_t out_capacity, int timeout_ms);
int xenus_net_tcp_connect(const char *host, uint16_t port, int timeout_ms);
int xenus_net_tcp_connect_addr(const xenus_net_addr_t *addr, uint16_t port,
                              int timeout_ms);
int xenus_net_udp_open(const char *host, uint16_t port, int timeout_ms);
int xenus_net_udp_open_addr(const xenus_net_addr_t *addr, uint16_t port,
                           int timeout_ms);
int xenus_net_send(int fd, const void *data, size_t len, int timeout_ms);
int xenus_net_send_all(int fd, const void *data, size_t len, int timeout_ms);
int xenus_net_recv(int fd, void *data, size_t len, int timeout_ms);
int xenus_net_recvfrom(int fd, void *data, size_t len, int timeout_ms,
                      xenus_net_addr_t *from, uint16_t *from_port);
int xenus_net_close(int fd);
const char *xenus_net_strerror(int status);

#ifdef __cplusplus
}
#endif

#endif
