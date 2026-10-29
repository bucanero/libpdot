/*
 * Metadata v14/v15 reader. Indexes the type registry, pallets and the
 * transaction extension list; everything else is skipped by length.
 * Names point into the owned metadata copy and are not NUL-terminated.
 */
#ifndef PDOT_METADATA_H
#define PDOT_METADATA_H

#include <stddef.h>
#include <stdint.h>

#include "scale.h"

typedef struct {
    const char *name;
    size_t      name_len;
    uint32_t    ty;           /* explicit data, in the extrinsic */
    uint32_t    implicit_ty;  /* signed but not included */
} pdot_md_extension;

typedef struct {
    const char *name;
    size_t      name_len;
    uint8_t     index;
    int         has_calls;
    uint32_t    calls_ty;
    size_t      storage_off;  /* offset of the storage prefix; 0 if none */
    int         has_error;
    uint32_t    error_ty;     /* the pallet's Error enum */
} pdot_md_pallet;

/* A field of a composite type or enum variant; name is empty if unnamed. */
typedef struct {
    const char *name;
    size_t      name_len;
    uint32_t    ty;
} pdot_md_field;

/* Upper bound on fields returned by the field lookups below. */
#define PDOT_MD_MAX_FIELDS 16

enum pdot_hasher {
    PDOT_HASHER_BLAKE2_128, PDOT_HASHER_BLAKE2_256, PDOT_HASHER_BLAKE2_128_CONCAT,
    PDOT_HASHER_TWOX128, PDOT_HASHER_TWOX256, PDOT_HASHER_TWOX64_CONCAT,
    PDOT_HASHER_IDENTITY
};

#define PDOT_MD_MAX_HASHERS 4

typedef struct {
    const char *prefix;       /* pallet storage prefix */
    size_t      prefix_len;
    int         is_map;
    uint8_t     hashers[PDOT_MD_MAX_HASHERS];
    size_t      nhashers;
    uint32_t    key_ty;
    uint32_t    value_ty;
} pdot_md_storage;

enum pdot_typedef {
    PDOT_TYPE_COMPOSITE, PDOT_TYPE_VARIANT, PDOT_TYPE_SEQUENCE, PDOT_TYPE_ARRAY,
    PDOT_TYPE_TUPLE, PDOT_TYPE_PRIMITIVE, PDOT_TYPE_COMPACT, PDOT_TYPE_BITSEQ
};

typedef struct pdot_metadata {
    uint8_t           *blob;
    size_t             len;
    uint8_t            version;
    uint32_t           ntypes;
    uint32_t          *type_off;   /* offset of each Type record */
    pdot_md_pallet    *pallets;
    size_t             npallets;
    pdot_md_extension *ext;
    size_t             next;
    uint8_t            extrinsic_version;
    int                have_xt_types;  /* v15 only */
    uint32_t           address_ty, call_ty, signature_ty;
    size_t             parsed_len;  /* bytes read; runtime APIs etc. follow */
} pdot_metadata;

/* Parses a "meta"-prefixed blob. Copies it. Returns PDOT_E_DECODE/UNSUPPORTED. */
int  pdot_metadata_parse(const uint8_t *blob, size_t len, pdot_metadata **out);
void pdot_metadata_free(pdot_metadata *md);

const pdot_md_pallet *pdot_md_find_pallet(const pdot_metadata *md, const char *name);

/* Index of the named variant of an enum type, and its field count. */
int pdot_md_variant(const pdot_metadata *md, uint32_t ty, const char *name,
                    uint8_t *index, size_t *nfields);

/* Same, also returning up to max_fields field type ids. */
int pdot_md_variant_fields(const pdot_metadata *md, uint32_t ty, const char *name,
                           uint8_t *index, uint32_t *field_tys, size_t max_fields,
                           size_t *nfields);

/*
 * Variant of enum ty with the given index: its name and up to max fields.
 * *nfields is the true count, which may exceed max.
 */
int pdot_md_variant_at(const pdot_metadata *md, uint32_t ty, uint8_t index,
                       const char **name, size_t *name_len,
                       pdot_md_field *fields, size_t max, size_t *nfields);

/* Fields of a composite type, or elements of a tuple (unnamed). */
int pdot_md_fields(const pdot_metadata *md, uint32_t ty, pdot_md_field *fields,
                   size_t max, size_t *nfields);

/* Streams the fields of a composite, tuple or enum variant without a size limit. */
typedef struct {
    pdot_reader r;
    uint64_t    left;
    int         tuple;   /* tuple elements are bare type ids */
} pdot_md_iter;

int pdot_md_iter_fields(const pdot_metadata *md, uint32_t ty, pdot_md_iter *it);
int pdot_md_iter_variant(const pdot_metadata *md, uint32_t ty, uint8_t index,
                         pdot_md_iter *it, const char **name, size_t *name_len);
/* 1 and *f filled, 0 at the end, or a negative error. */
int pdot_md_iter_next(pdot_md_iter *it, pdot_md_field *f);

const pdot_md_pallet *pdot_md_pallet_at(const pdot_metadata *md, uint8_t index);

/* Pallet and call indices for pallet.call, optionally with argument types. */
int pdot_md_call(const pdot_metadata *md, const char *pallet, const char *call,
                 uint8_t *pallet_index, uint8_t *call_index);
int pdot_md_call_fields(const pdot_metadata *md, const char *pallet, const char *call,
                        uint8_t *pallet_index, uint8_t *call_index,
                        uint32_t *field_tys, size_t max_fields, size_t *nfields);

/* scale-info primitive kind of ty (PDOT_PRIM_*), or PDOT_E_METADATA. */
#define PDOT_PRIM_U8  3
#define PDOT_PRIM_U32 5
int pdot_md_primitive(const pdot_metadata *md, uint32_t ty, uint8_t *prim);

int pdot_md_storage_entry(const pdot_metadata *md, const char *pallet,
                          const char *entry, pdot_md_storage *out);

/* Positions r at the TypeDef of ty and returns its kind. */
int pdot_md_typedef(const pdot_metadata *md, uint32_t ty, pdot_reader *r,
                    enum pdot_typedef *kind);

/* Last path segment of ty ("MultiAddress"), or empty for unnamed types. */
int pdot_md_type_name(const pdot_metadata *md, uint32_t ty, const char **name,
                      size_t *len);

/* 1 if values of ty always encode to zero bytes. */
int pdot_md_is_zero_sized(const pdot_metadata *md, uint32_t ty);

int pdot_md_name_eq(const char *name, size_t len, const char *cstr);

#endif
