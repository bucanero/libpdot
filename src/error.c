#include <stdlib.h>

#include "pdot.h"

const char *pdot_strerror(int err)
{
    switch (err) {
    case PDOT_OK:            return "ok";
    case PDOT_E_INVALID:     return "invalid argument";
    case PDOT_E_NOMEM:       return "out of memory";
    case PDOT_E_DECODE:      return "malformed input";
    case PDOT_E_BUFFER:      return "output buffer too small";
    case PDOT_E_CHECKSUM:    return "checksum mismatch";
    case PDOT_E_UNSUPPORTED: return "unsupported";
    case PDOT_E_MISMATCH:    return "data does not match hash";
    case PDOT_E_CRYPTO:      return "crypto failure";
    case PDOT_E_RNG:         return "random number generator unavailable";
    case PDOT_E_NETWORK:     return "network error";
    case PDOT_E_RPC:         return "node returned an error";
    case PDOT_E_NOTFOUND:    return "not found";
    case PDOT_E_TIMEOUT:     return "timed out";
    case PDOT_E_METADATA:    return "unexpected runtime metadata";
    default:                 return "unknown error";
    }
}

void pdot_free(void *p)
{
    free(p);
}
