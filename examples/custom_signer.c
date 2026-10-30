/*
 * Keep keys out of libpdot: a signer is a callback, so signing can happen in
 * a hardware wallet, a secure enclave, a game server or another process.
 *
 * This example forwards to a built-in signer held in `ctx`, and logs what it
 * is asked to sign. A real integration would send `msg` to the device and
 * return the signature it produces. Runs offline.
 */
#include <stdio.h>
#include <string.h>

#include <pdot.h>

/*
 * The contract: sign `msg` exactly as given and write s->sig_len bytes.
 *   sr25519: schnorrkel with signing context "substrate", 64 bytes.
 *   ECDSA:   secp256k1 over Blake2b-256(msg), r | s | recovery id, 65 bytes.
 * libpdot has already hashed payloads over 256 bytes, as Substrate requires.
 */
static int device_sign(const pdot_signer *s, const uint8_t *msg, size_t len,
                       uint8_t *sig_out)
{
    const pdot_signer *key_in_device = s->ctx;

    printf("device asked to sign %zu bytes\n", len);
    return pdot_signer_sign(key_in_device, msg, len, sig_out);
}

int main(void)
{
    /* //Alice; stands in for a key that never leaves the device. */
    static const uint8_t seed[32] = {
        0xe5, 0xbe, 0x9a, 0x50, 0x92, 0xb8, 0x1b, 0xca, 0x64, 0xbe, 0x81,
        0xd2, 0x12, 0xe7, 0xf2, 0xf9, 0xeb, 0xa1, 0x83, 0xbb, 0x7a, 0x90,
        0x95, 0x4f, 0x7b, 0x76, 0x36, 0x1f, 0x6e, 0xdb, 0x5c, 0x0a,
    };
    pdot_signer device, external;
    uint8_t sig[PDOT_SIG_MAX];
    char addr[PDOT_SS58_MAX_LEN];

    pdot_signer_sr25519_from_seed(&device, seed);

    /* Everything libpdot needs from an external signer: scheme, public key,
     * account, signature length and the callback. `secret` stays unused. */
    memset(&external, 0, sizeof external);
    external.scheme = PDOT_SCHEME_SR25519;
    memcpy(external.public_key, device.public_key, 32);
    external.public_key_len = 32;
    memcpy(external.account_id, device.public_key, 32);  /* sr25519: account = key */
    external.sig_len = 64;
    external.sign = device_sign;
    external.ctx = &device;

    pdot_ss58_encode(external.account_id, 42, addr, sizeof addr);
    printf("external signer for %s\n", addr);

    /* Pass &external to pdot_bulletin_store() and friends like any signer. */
    if (pdot_signer_sign(&external, (const uint8_t *)"payload", 7, sig))
        return 1;
    printf("signature starts %02x%02x%02x%02x...\n", sig[0], sig[1], sig[2], sig[3]);

    pdot_signer_wipe(&device);
    return 0;
}
