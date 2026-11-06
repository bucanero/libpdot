/*
 * SCALE encoding of JSON values driven by the metadata type registry, the
 * inverse of decode.h. Used to build any call from readable arguments:
 *
 *   integers     a JSON number (exact up to 2^53) or a decimal string
 *   accounts     an SS58 address or 0x-prefixed hex, for [u8; 32] and
 *                any one-field wrapper of it (AccountId32, MultiAddress::Id)
 *   byte strings 0x-prefixed hex, or any other string as its UTF-8 bytes
 *   structs      an object by field name, or an array in field order
 *   enums        "Variant", or {"Variant": fields}; null is None, and an
 *                Option also takes its Some value directly
 *   one-field wrappers take their field's value directly
 */
#ifndef PDOT_ENCODE_H
#define PDOT_ENCODE_H

#include <stddef.h>
#include <stdint.h>

#include <cjson/cJSON.h>

#include "metadata.h"
#include "scale.h"

/*
 * Appends v encoded as type ty. On failure, err holds a message naming the
 * position in v that did not fit ("args.dest: ..."), and out is unchanged.
 */
int pdot_encode_json(const pdot_metadata *md, uint32_t ty, const cJSON *v, pdot_buf *out,
                     char *err, size_t err_len);

/*
 * pallet.call with args (an array in order, an object by name, or NULL when
 * the call takes none): pallet index | call index | encoded arguments.
 */
int pdot_encode_call_json(const pdot_metadata *md, const char *pallet, const char *call,
                          const cJSON *args, pdot_buf *out, char *err, size_t err_len);

/*
 * Starts pallet.call: writes both indices and returns an iterator over its
 * argument fields, for calls built partly from raw bytes (a nested call).
 */
int pdot_encode_call_begin(const pdot_metadata *md, const char *pallet, const char *call,
                           pdot_buf *out, pdot_md_iter *args);

#endif
