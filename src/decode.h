/*
 * Generic SCALE decoding driven by the metadata type registry. Values are
 * walked in place; nothing is allocated. Every read is bounds-checked, and
 * nesting is limited so hostile type graphs cannot exhaust the stack.
 */
#ifndef PDOT_DECODE_H
#define PDOT_DECODE_H

#include <stddef.h>
#include <stdint.h>

#include "metadata.h"
#include "scale.h"

/* Skips one value of type ty. */
int pdot_decode_skip(const pdot_metadata *md, uint32_t ty, pdot_reader *r);

/* Reads an unsigned integer: u8..u64, Compact<_>, or a one-field wrapper of those. */
int pdot_decode_uint(const pdot_metadata *md, uint32_t ty, pdot_reader *r, uint64_t *v);

/*
 * Reads an enum's variant index at r and returns the variant's name; it is
 * then iterated with pdot_md_iter_next and each field decoded in order.
 */
int pdot_decode_variant(const pdot_metadata *md, uint32_t ty, pdot_reader *r,
                        pdot_md_iter *fields, const char **name, size_t *name_len);

/*
 * Positions r at the field reached by a dotted path of field names
 * ("extent.bytes") inside a composite value of type ty; returns its type.
 */
int pdot_decode_seek(const pdot_metadata *md, uint32_t ty, pdot_reader *r,
                     const char *path, uint32_t *field_ty);

/* pdot_decode_seek + pdot_decode_uint on a fresh reader over p. */
int pdot_decode_path_uint(const pdot_metadata *md, uint32_t ty, const uint8_t *p,
                          size_t len, const char *path, uint64_t *v);

#endif
