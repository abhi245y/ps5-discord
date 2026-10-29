// mbedTLS overrides shared by the console and host builds (MBEDTLS_USER_CONFIG_FILE).
#pragma once
// TLS runs on many threads at once (gateway, request threads, voice, Go Live). TLS 1.3
// goes through PSA, whose key slots and RNG are locked only with threading on.
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD
