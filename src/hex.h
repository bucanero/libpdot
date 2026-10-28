/* Hex encoding with optional "0x" prefix. */
#ifndef PDOT_HEX_H
#define PDOT_HEX_H

#include <stddef.h>
#include <stdint.h>

/* Writes "0x" + 2*len hex digits + NUL; out needs 2*len + 3 bytes. */
void pdot_hex_encode(const uint8_t *in, size_t len, char *out);

/* Decodes into out (cap bytes); "0x" prefix optional. Returns 0 or PDOT_E_DECODE. */
int pdot_hex_decode(const char *hex, uint8_t *out, size_t cap, size_t *len);

/* Decodes into a fresh allocation, freed with free(). */
int pdot_hex_decode_alloc(const char *hex, uint8_t **out, size_t *len);

#endif
