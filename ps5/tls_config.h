/* mbedTLS user configuration for PSteroids5 (MBEDTLS_USER_CONFIG_FILE), applied
 * on top of the stock mbedtls_config.h. The client only needs TLS 1.2/1.3 over
 * its own sockets, with certificate checks against bundled roots. */
#pragma once

/* Randomness comes from tls_entropy.c: getrandom() on the host,
 * sysctl(KERN_ARND) on the PS5. */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* HTTP requests and the multiplayer socket run TLS on different threads at once;
 * mbedTLS 3.6 makes PSA crypto and the RNG thread-safe with this. */
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD

/* No files, no mbedTLS networking or timing: net_http.hpp owns the socket. */
#undef MBEDTLS_FS_IO
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C

/* Not used by a TLS client and pulls in extra code. */
#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#undef MBEDTLS_SSL_COOKIE_C
#undef MBEDTLS_SSL_TICKET_C
#undef MBEDTLS_SSL_CACHE_C
#undef MBEDTLS_SELF_TEST
