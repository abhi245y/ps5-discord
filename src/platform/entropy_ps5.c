// mbedTLS entropy source on the console: kernel CSPRNG via sysctl (from spot5).
#include <stddef.h>
#include <sys/sysctl.h>

// The kernel returns at most 256 bytes per call.
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;
    int mib[2] = {CTL_KERN, KERN_ARND};
    *olen = 0;
    while (*olen < len) {
        size_t n = len - *olen < 256 ? len - *olen : 256;
        if (sysctl(mib, 2, output + *olen, &n, NULL, 0) != 0 || n == 0)
            return -1;
        *olen += n;
    }
    return 0;
}
