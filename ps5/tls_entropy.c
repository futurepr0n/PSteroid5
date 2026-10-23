/* Hardware entropy for mbedTLS (MBEDTLS_ENTROPY_HARDWARE_ALT). */
#include <stddef.h>
#include <stdint.h>

#if defined(__PROSPERO__)
/* The kernel's arc4rand via sysctl (libkernel). libSceRandom is not used: it was
 * listed as needed but never loaded into the title, leaving a null import that
 * crashed the first TLS handshake on the console (2026-10-23 klog). */
#include <sys/types.h>
#include <sys/sysctl.h>
#else
#include <sys/random.h>
#endif

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen) {
    (void)data;
    *olen = 0;
    while (*olen < len) {
        size_t chunk = len - *olen > 64 ? 64 : len - *olen;
#if defined(__PROSPERO__)
        int mib[2] = {CTL_KERN, KERN_ARND};
        size_t got = chunk;
        if (sysctl(mib, 2, output + *olen, &got, NULL, 0) != 0 || got == 0) return -1;
        chunk = got;
#else
        ssize_t got = getrandom(output + *olen, chunk, 0);
        if (got <= 0) return -1;
        chunk = (size_t)got;
#endif
        *olen += chunk;
    }
    return 0;
}
