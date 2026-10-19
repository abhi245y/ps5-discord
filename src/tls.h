// Blocking TLS client over mbedTLS.
#pragma once
#include <stddef.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

typedef struct {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
} tls_conn;

int tls_global_init(void); // once per process; 0 on success
// Process CSPRNG (seeded by tls_global_init), mbedTLS f_rng signature.
int tls_random(void *unused, unsigned char *out, size_t len);
int tls_connect(tls_conn *c, const char *host, const char *port);
// Wait until data is readable or timeout_ms passes: 1 readable, 0 timeout, <0 error.
int tls_wait(tls_conn *c, int timeout_ms);
int tls_read(tls_conn *c, void *buf, size_t len);         // >0 bytes, 0 closed, <0 error
int tls_write_all(tls_conn *c, const void *buf, size_t len); // 0 ok, <0 error
void tls_close(tls_conn *c);
