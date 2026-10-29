/* SCALE codec: a growable writer and a bounds-checked reader. */
#ifndef PDOT_SCALE_H
#define PDOT_SCALE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Writer. Errors are sticky: after an allocation failure every later write
 * is a no-op and err stays set, so callers check it once at the end.
 */
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
    int      err;
} pdot_buf;

void pdot_buf_init(pdot_buf *b);
void pdot_buf_free(pdot_buf *b);
int  pdot_buf_reserve(pdot_buf *b, size_t extra);

void pdot_scale_raw(pdot_buf *b, const uint8_t *p, size_t n);
void pdot_scale_u8(pdot_buf *b, uint8_t v);
void pdot_scale_u16(pdot_buf *b, uint16_t v);
void pdot_scale_u32(pdot_buf *b, uint32_t v);
void pdot_scale_u64(pdot_buf *b, uint64_t v);
void pdot_scale_bool(pdot_buf *b, int v);
void pdot_scale_compact(pdot_buf *b, uint64_t v);
/* Vec<u8>: compact length prefix followed by the bytes. */
void pdot_scale_bytes(pdot_buf *b, const uint8_t *p, size_t n);

size_t pdot_scale_compact_len(uint64_t v);

/*
 * Reader. Each call returns 0 or PDOT_E_DECODE. Reads never go past len.
 */
typedef struct {
    const uint8_t *p;
    size_t         len;
    size_t         pos;
} pdot_reader;

void pdot_reader_init(pdot_reader *r, const uint8_t *p, size_t len);
size_t pdot_reader_remaining(const pdot_reader *r);

int pdot_read_raw(pdot_reader *r, size_t n, const uint8_t **out);
int pdot_read_skip(pdot_reader *r, size_t n);
int pdot_read_u8(pdot_reader *r, uint8_t *v);
int pdot_read_u16(pdot_reader *r, uint16_t *v);
int pdot_read_u32(pdot_reader *r, uint32_t *v);
int pdot_read_u64(pdot_reader *r, uint64_t *v);
int pdot_read_bool(pdot_reader *r, int *v);
/* Rejects non-canonical encodings and values above u64. */
int pdot_read_compact(pdot_reader *r, uint64_t *v);
/* Skips a compact of any width (up to the 67-byte big mode), e.g. Compact<u128>. */
int pdot_read_compact_skip(pdot_reader *r);
/* Vec<u8>: points into the input, no copy. */
int pdot_read_bytes(pdot_reader *r, const uint8_t **p, size_t *n);

#endif
