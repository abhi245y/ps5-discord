#include "tls.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
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

// TCP connect with a deadline (mbedtls_net_connect blocks for the system's minute or more),
// then blocking I/O with send and receive timeouts. 0 or an mbedTLS net error.
static int connect_timed(mbedtls_net_context *net, const char *host, const char *port)
{
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_protocol = IPPROTO_TCP}, *list;
    if (getaddrinfo(host, port, &hints, &list) != 0)
        return MBEDTLS_ERR_NET_UNKNOWN_HOST;
    int ret = MBEDTLS_ERR_NET_CONNECT_FAILED;
    for (struct addrinfo *a = list; a; a = a->ai_next) {
        int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) {
            ret = MBEDTLS_ERR_NET_SOCKET_FAILED;
            continue;
        }
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int ok = connect(fd, a->ai_addr, a->ai_addrlen) == 0, err = ok ? 0 : errno, waited = 0;
        if (!ok && err == EINPROGRESS) {
            struct pollfd p = {.fd = fd, .events = POLLOUT};
            socklen_t len = sizeof err;
            waited = poll(&p, 1, CONNECT_MS);
            if (waited != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0)
                err = waited == 0 ? ETIMEDOUT : errno;
            ok = err == 0;
        }
        if (!ok) {
            char ip[INET6_ADDRSTRLEN] = "?";
            const void *at = a->ai_family == AF_INET6 ? (const void *)&((struct sockaddr_in6 *)a->ai_addr)->sin6_addr
                                                      : (const void *)&((struct sockaddr_in *)a->ai_addr)->sin_addr;
            inet_ntop(a->ai_family, at, ip, sizeof ip);
            LOG("tls: connect to %s (%s) failed: %s", host, ip, strerror(err));
            close(fd);
            continue;
        }
        fcntl(fd, F_SETFL, flags); // blocking again: mbedTLS's recv then reports a timeout as an error
        struct timeval tv = {.tv_sec = IO_SECONDS};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        net->fd = fd;
        ret = 0;
        break;
    }
    freeaddrinfo(list);
    return ret;
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
