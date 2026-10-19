// mbedTLS overrides for the console (MBEDTLS_USER_CONFIG_FILE). Same as spot5.
#pragma once
// Entropy comes from the kernel CSPRNG (sysctl KERN_ARND), see entropy_ps5.c.
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
// Roots are embedded (ca_bundle.c); no cert loading from disk.
#undef MBEDTLS_FS_IO
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
