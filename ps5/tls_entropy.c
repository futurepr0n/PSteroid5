/* Hardware entropy for mbedTLS (MBEDTLS_ENTROPY_HARDWARE_ALT). */
#include <stddef.h>
#include <stdint.h>

#if defined(__PROSPERO__)
int sceRandomGetRandomNumber(uint8_t *buf, size_t size); /* libSceRandom; 0 on success */
#else
#include <sys/random.h>
#endif

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen) {
    (void)data;
    *olen = 0;
    while (*olen < len) {
        size_t chunk = len - *olen > 64 ? 64 : len - *olen;
#if defined(__PROSPERO__)
        if (sceRandomGetRandomNumber(output + *olen, chunk) != 0) return -1;
#else
        ssize_t got = getrandom(output + *olen, chunk, 0);
        if (got <= 0) return -1;
        chunk = (size_t)got;
#endif
        *olen += chunk;
    }
    return 0;
}
