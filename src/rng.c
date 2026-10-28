#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/platform_util.h>

#include "pdot.h"
#include "rng.h"

#if defined(_WIN32)
#include <windows.h>
static INIT_ONCE rng_once = INIT_ONCE_STATIC_INIT;
static SRWLOCK rng_lock = SRWLOCK_INIT;
#define RNG_LOCK()   AcquireSRWLockExclusive(&rng_lock)
#define RNG_UNLOCK() ReleaseSRWLockExclusive(&rng_lock)
#else
#include <pthread.h>
static pthread_once_t rng_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t rng_lock = PTHREAD_MUTEX_INITIALIZER;
#define RNG_LOCK()   pthread_mutex_lock(&rng_lock)
#define RNG_UNLOCK() pthread_mutex_unlock(&rng_lock)
#endif

static mbedtls_entropy_context rng_entropy;
static mbedtls_ctr_drbg_context rng_drbg;
static int rng_status = PDOT_E_RNG;

static void rng_seed(void)
{
    static const unsigned char pers[] = "libpdot";

    mbedtls_entropy_init(&rng_entropy);
    mbedtls_ctr_drbg_init(&rng_drbg);
    if (mbedtls_ctr_drbg_seed(&rng_drbg, mbedtls_entropy_func, &rng_entropy,
                              pers, sizeof pers - 1) == 0)
        rng_status = 0;
}

#if defined(_WIN32)
static BOOL CALLBACK rng_seed_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    rng_seed();
    return TRUE;
}
#endif

int pdot_rng_init(void)
{
#if defined(_WIN32)
    InitOnceExecuteOnce(&rng_once, rng_seed_once, NULL, NULL);
#else
    pthread_once(&rng_once, rng_seed);
#endif
    return rng_status;
}

int pdot_rng_bytes(void *out, size_t len)
{
    unsigned char *p = out;
    int rc = 0;

    if (pdot_rng_init())
        return PDOT_E_RNG;
    RNG_LOCK();
    while (len > 0 && rc == 0) {
        size_t n = len < MBEDTLS_CTR_DRBG_MAX_REQUEST ? len : MBEDTLS_CTR_DRBG_MAX_REQUEST;
        rc = mbedtls_ctr_drbg_random(&rng_drbg, p, n);
        p += n;
        len -= n;
    }
    RNG_UNLOCK();
    return rc ? PDOT_E_RNG : 0;
}

int pdot_rng_mbedtls(void *unused, unsigned char *out, size_t len)
{
    (void)unused;
    return pdot_rng_bytes(out, len) ? MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED : 0;
}

void pdot_wipe(void *p, size_t len)
{
    mbedtls_platform_zeroize(p, len);
}
