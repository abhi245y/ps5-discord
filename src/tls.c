#include "tls.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <pthread.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <psa/crypto.h>

#include "log.h"

extern const unsigned char ca_bundle_pem[];
extern const size_t ca_bundle_pem_len;

static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_x509_crt roots;
static mbedtls_ssl_config conf;
// The DRBG is shared by every connection (gateway, REST, images) on several threads.
static pthread_mutex_t drbg_lock = PTHREAD_MUTEX_INITIALIZER;

static int locked_random(void *unused, unsigned char *out, size_t len)
{
    (void)unused;
    pthread_mutex_lock(&drbg_lock);
    int ret = mbedtls_ctr_drbg_random(&drbg, out, len);
    pthread_mutex_unlock(&drbg_lock);
    return ret;
}

static void log_err(const char *what, int ret)
{
    char msg[128];
    mbedtls_strerror(ret, msg, sizeof msg);
    LOG("tls: %s failed: -0x%04x %s", what, (unsigned)-ret, msg);
}

int tls_global_init(void)
{
    int ret;
    if (psa_crypto_init() != PSA_SUCCESS) // TLS 1.3 in mbedTLS 3.6 needs PSA
        return -1;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_x509_crt_init(&roots);
    mbedtls_ssl_config_init(&conf);
    if ((ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, NULL, 0)) != 0)
        return log_err("drbg seed", ret), ret;
    // Partial failures are fine: a few odd roots in a system bundle may not parse.
    if ((ret = mbedtls_x509_crt_parse(&roots, ca_bundle_pem, ca_bundle_pem_len)) < 0)
        return log_err("ca parse", ret), ret;
    if ((ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
        return log_err("config", ret), ret;
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&conf, &roots, NULL);
    mbedtls_ssl_conf_rng(&conf, locked_random, NULL);
    return 0;
}

int tls_random(void *unused, unsigned char *out, size_t len)
{
    return locked_random(unused, out, len);
}

// No socket waits for ever: a peer that accepts and then stalls (or Wi-Fi that drops
// mid-record) costs at most these, and the thread gets an error back.
#define CONNECT_MS 10000
#define IO_SECONDS 15

#ifdef __PROSPERO__
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetPoolDestroy(int pool);
int sceNetResolverCreate(const char *name, int pool, int flags);
int sceNetResolverDestroy(int rid);
// As shadPS4 has it (OrbisNetResolverInfo, 384 bytes).
typedef struct {
    uint8_t addr[16]; // in_addr, or in6_addr
    uint32_t af, pad[3];
} resolver_addr;
typedef struct {
    resolver_addr addrs[10];
    uint32_t records, records_v4, pad[14];
} resolver_info;
int sceNetResolverStartNtoaMultipleRecords(int rid, const char *host, resolver_info *info, int timeout, int retry,
                                           int flags);

// Every A record: the SDK's getaddrinfo asks for one (sceNetResolverStartNtoa), and when
// that one is a CDN node this network cannot reach, every try fails. 0 when none.
static int resolve_all(const char *host, struct in_addr *out, int cap)
{
    resolver_info info;
    memset(&info, 0, sizeof info);
    int pool = sceNetPoolCreate("tls", 0x4000, 0), rid = pool >= 0 ? sceNetResolverCreate("tls", pool, 0) : -1, n = 0;
    if (rid >= 0 && sceNetResolverStartNtoaMultipleRecords(rid, host, &info, 0, 0, 0) >= 0)
        for (uint32_t i = 0; i < info.records && i < 10 && n < cap; i++)
            if (info.addrs[i].af == AF_INET)
                memcpy(&out[n++], info.addrs[i].addr, 4);
    if (rid >= 0)
        sceNetResolverDestroy(rid);
    if (pool >= 0)
        sceNetPoolDestroy(pool);
    return n;
}
#endif

// The last few addresses a connect timed out on: tried last next time, so one dead node
// costs one timeout, not one per connection.
static _Atomic uint32_t dead[4];
static _Atomic unsigned dead_next;

static int is_dead(const struct sockaddr *sa)
{
    if (sa->sa_family != AF_INET)
        return 0;
    uint32_t ip = ((const struct sockaddr_in *)sa)->sin_addr.s_addr;
    for (int i = 0; i < 4; i++)
        if (dead[i] == ip)
            return 1;
    return 0;
}

// One address with a deadline: the socket, or -1 (logged).
static int connect_one(const char *host, const struct sockaddr *sa, socklen_t len, int i, int n)
{
    int fd = socket(sa->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
        return -1;
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int ok = connect(fd, sa, len) == 0, err = ok ? 0 : errno, waited = 0;
    if (!ok && err == EINPROGRESS) {
        struct pollfd p = {.fd = fd, .events = POLLOUT};
        socklen_t err_len = sizeof err;
        waited = poll(&p, 1, CONNECT_MS);
        if (waited != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) != 0)
            err = waited == 0 ? ETIMEDOUT : errno;
        ok = err == 0;
    }
    if (!ok) {
        char ip[INET6_ADDRSTRLEN] = "?";
        const void *at = sa->sa_family == AF_INET6 ? (const void *)&((const struct sockaddr_in6 *)sa)->sin6_addr
                                                   : (const void *)&((const struct sockaddr_in *)sa)->sin_addr;
        inet_ntop(sa->sa_family, at, ip, sizeof ip);
        LOG("tls: connect to %s (%s, %d of %d) failed: %s", host, ip, i + 1, n, strerror(err));
        if (err == ETIMEDOUT && sa->sa_family == AF_INET)
            dead[dead_next++ % 4] = ((const struct sockaddr_in *)sa)->sin_addr.s_addr;
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, flags); // blocking again: mbedTLS's recv then reports a timeout as an error
    struct timeval tv = {.tv_sec = IO_SECONDS};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return fd;
}

// TCP connect with a deadline (mbedtls_net_connect blocks for the system's minute or more)
// to each address in turn, the ones that timed out lately last; then blocking I/O with
// send and receive timeouts. 0 or an mbedTLS net error.
static int connect_timed(mbedtls_net_context *net, const char *host, const char *port)
{
    struct sockaddr_storage addrs[10];
    socklen_t lens[10];
    int n = 0;
#ifdef __PROSPERO__
    struct in_addr v4[10];
    int found = resolve_all(host, v4, 10);
    for (; n < found; n++) {
        struct sockaddr_in *a = (struct sockaddr_in *)&addrs[n];
        *a = (struct sockaddr_in){.sin_len = sizeof *a, .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(port)),
                                  .sin_addr = v4[n]};
        lens[n] = sizeof *a;
    }
#endif
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_protocol = IPPROTO_TCP}, *list;
    if (n == 0) {
        if (getaddrinfo(host, port, &hints, &list) != 0)
            return MBEDTLS_ERR_NET_UNKNOWN_HOST;
        for (struct addrinfo *a = list; a && n < 10; a = a->ai_next, n++) {
            memcpy(&addrs[n], a->ai_addr, a->ai_addrlen);
            lens[n] = a->ai_addrlen;
        }
        freeaddrinfo(list);
    }
    for (int pass = 0; pass < 2; pass++) // the live ones, then the dead ones
        for (int i = 0; i < n; i++) {
            if (is_dead((struct sockaddr *)&addrs[i]) != pass)
                continue;
            int fd = connect_one(host, (struct sockaddr *)&addrs[i], lens[i], i, n);
            if (fd >= 0) {
                net->fd = fd;
                return 0;
            }
        }
    return MBEDTLS_ERR_NET_CONNECT_FAILED;
}

int tls_connect(tls_conn *c, const char *host, const char *port)
{
    int ret;
    mbedtls_net_init(&c->net);
    mbedtls_ssl_init(&c->ssl);
    if ((ret = connect_timed(&c->net, host, port)) != 0)
        return log_err("connect", ret), ret;
    if ((ret = mbedtls_ssl_setup(&c->ssl, &conf)) != 0 ||
        (ret = mbedtls_ssl_set_hostname(&c->ssl, host)) != 0)
        return log_err("setup", ret), ret;
    mbedtls_ssl_set_bio(&c->ssl, &c->net, mbedtls_net_send, mbedtls_net_recv, NULL);
    while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0)
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            return log_err("handshake", ret), ret;
    return 0;
}

int tls_wait(tls_conn *c, int timeout_ms)
{
    if (mbedtls_ssl_get_bytes_avail(&c->ssl) > 0) // already decrypted, poll would miss it
        return 1;
    struct pollfd p = {.fd = c->net.fd, .events = POLLIN};
    int r = poll(&p, 1, timeout_ms);
    return r < 0 ? -1 : r > 0;
}

int tls_read(tls_conn *c, void *buf, size_t len)
{
    for (;;) {
        int ret = mbedtls_ssl_read(&c->ssl, buf, len);
        if (ret >= 0)
            return ret;
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
            return 0;
        // TLS 1.3 session tickets arrive as reads; just read again.
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
            ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
            continue;
        log_err("read", ret);
        return ret;
    }
}

int tls_write_all(tls_conn *c, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    while (len > 0) {
        int ret = mbedtls_ssl_write(&c->ssl, p, len);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (ret < 0)
            return log_err("write", ret), ret;
        p += ret;
        len -= (size_t)ret;
    }
    return 0;
}

void tls_close(tls_conn *c)
{
    mbedtls_ssl_close_notify(&c->ssl);
    mbedtls_ssl_free(&c->ssl);
    mbedtls_net_free(&c->net);
}
