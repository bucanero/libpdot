#include "pdot.h"
#include "rng.h"

int pdot_signer_sign(const pdot_signer *s, const uint8_t *msg, size_t len,
                     uint8_t *sig_out)
{
    if (!s || !s->sign || !sig_out || (!msg && len))
        return PDOT_E_INVALID;
    return s->sign(s, msg, len, sig_out);
}

void pdot_signer_wipe(pdot_signer *s)
{
    if (s)
        pdot_wipe(s, sizeof *s);
}
