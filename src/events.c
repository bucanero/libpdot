#include <stdio.h>
#include <string.h>

#include "pdot.h"
#include "decode.h"
#include "events.h"
#include "hash.h"

void pdot_events_key(uint8_t key[32])
{
    pdot_twox128(key, "System", 6);
    pdot_twox128(key + 16, "Events", 6);
}

/* Module { index: u8, error: [u8; 4] }: the first error byte is the variant. */
static int module_error(const pdot_metadata *md, uint32_t ty, pdot_reader *r,
                        char *out, size_t out_len)
{
    uint64_t pallet_index = 256, error_index = 256;
    const pdot_md_pallet *pl;
    pdot_md_iter it;
    pdot_md_field f;
    const char *name;
    size_t name_len;
    int rc;

    if (pdot_md_iter_fields(md, ty, &it))
        return PDOT_E_METADATA;
    while ((rc = pdot_md_iter_next(&it, &f)) == 1) {
        if (pdot_md_name_eq(f.name, f.name_len, "index")) {
            if ((rc = pdot_decode_uint(md, f.ty, r, &pallet_index)))
                return rc;
        } else if (pdot_md_name_eq(f.name, f.name_len, "error")) {
            const uint8_t *e;
            size_t start = r->pos;
            if ((rc = pdot_decode_skip(md, f.ty, r)))
                return rc;
            if (r->pos == start)
                return PDOT_E_DECODE;
            e = r->p + start;
            error_index = e[0];
        } else if ((rc = pdot_decode_skip(md, f.ty, r))) {
            return rc;
        }
    }
    if (rc < 0 || pallet_index > 255 || error_index > 255)
        return PDOT_E_DECODE;

    pl = pdot_md_pallet_at(md, (uint8_t)pallet_index);
    if (pl && pl->has_error &&
        pdot_md_variant_at(md, pl->error_ty, (uint8_t)error_index, &name, &name_len,
                           NULL, 0, NULL) == 0)
        snprintf(out, out_len, "%.*s.%.*s", (int)pl->name_len, pl->name,
                 (int)name_len, name);
    else
        snprintf(out, out_len, "Module(%u, %u)", (unsigned)pallet_index,
                 (unsigned)error_index);
    return 0;
}

int pdot_dispatch_error(const pdot_metadata *md, uint32_t ty, pdot_reader *r,
                        char *out, size_t out_len)
{
    pdot_md_iter it, inner;
    pdot_md_field f;
    const char *name, *iname;
    size_t len, ilen;
    enum pdot_typedef kind;
    pdot_reader t;
    int rc;

    if ((rc = pdot_decode_variant(md, ty, r, &it, &name, &len)))
        return rc;
    if (pdot_md_name_eq(name, len, "Module") && it.left == 1 &&
        pdot_md_iter_next(&it, &f) == 1)
        return module_error(md, f.ty, r, out, out_len);

    snprintf(out, out_len, "%.*s", (int)len, name);
    while ((rc = pdot_md_iter_next(&it, &f)) == 1) {
        /* A sole enum field, as in Token(TokenError): name its variant too. */
        if (it.left == 0 && strchr(out, '.') == NULL &&
            pdot_md_typedef(md, f.ty, &t, &kind) == 0 && kind == PDOT_TYPE_VARIANT) {
            if ((rc = pdot_decode_variant(md, f.ty, r, &inner, &iname, &ilen)))
                return rc;
            snprintf(out + strlen(out), out_len - strlen(out), ".%.*s", (int)ilen, iname);
            while ((rc = pdot_md_iter_next(&inner, &f)) == 1)
                if ((rc = pdot_decode_skip(md, f.ty, r)))
                    return rc;
            if (rc < 0)
                return rc;
        } else if ((rc = pdot_decode_skip(md, f.ty, r))) {
            return rc;
        }
    }
    return rc < 0 ? rc : 0;
}

/* Phase::ApplyExtrinsic(u32) yields its index; other phases yield none. */
static int read_phase(const pdot_metadata *md, uint32_t ty, pdot_reader *r,
                      int *applies, uint32_t *index)
{
    pdot_md_iter it;
    pdot_md_field f;
    const char *name;
    size_t len;
    uint64_t v;
    int rc;

    *applies = 0;
    if ((rc = pdot_decode_variant(md, ty, r, &it, &name, &len)))
        return rc;
    if (pdot_md_name_eq(name, len, "ApplyExtrinsic") && it.left == 1 &&
        pdot_md_iter_next(&it, &f) == 1) {
        if ((rc = pdot_decode_uint(md, f.ty, r, &v)) || v > UINT32_MAX)
            return rc ? rc : PDOT_E_DECODE;
        *applies = 1;
        *index = (uint32_t)v;
        return 0;
    }
    while ((rc = pdot_md_iter_next(&it, &f)) == 1)
        if ((rc = pdot_decode_skip(md, f.ty, r)))
            return rc;
    return rc < 0 ? rc : 0;
}

/*
 * RuntimeEvent is an enum of pallets whose single field is the pallet's own
 * Event enum. Records the events libpdot cares about, skips the rest.
 */
static int read_event(const pdot_metadata *md, uint32_t ty, pdot_reader *r,
                      int mine, pdot_ext_outcome *out)
{
    pdot_md_iter outer, it;
    pdot_md_field f;
    const char *pallet, *name;
    size_t plen, len;
    int rc;

    if ((rc = pdot_decode_variant(md, ty, r, &outer, &pallet, &plen)))
        return rc;
    if (outer.left != 1 || pdot_md_iter_next(&outer, &f) != 1)
        return PDOT_E_METADATA;
    if (!mine)
        return pdot_decode_skip(md, f.ty, r);

    out->nevents++;
    if ((rc = pdot_decode_variant(md, f.ty, r, &it, &name, &len)))
        return rc;
    if (pdot_md_name_eq(pallet, plen, "System") && pdot_md_name_eq(name, len, "ExtrinsicSuccess"))
        out->success = 1;
    while ((rc = pdot_md_iter_next(&it, &f)) == 1) {
        if (pdot_md_name_eq(pallet, plen, "System") &&
            pdot_md_name_eq(name, len, "ExtrinsicFailed") &&
            pdot_md_name_eq(f.name, f.name_len, "dispatch_error")) {
            out->failed = 1;
            rc = pdot_dispatch_error(md, f.ty, r, out->error, sizeof out->error);
        } else if (pdot_md_name_eq(pallet, plen, "TransactionStorage") &&
                   pdot_md_name_eq(name, len, "Stored") &&
                   pdot_md_name_eq(f.name, f.name_len, "index")) {
            uint64_t v;
            if (!(rc = pdot_decode_uint(md, f.ty, r, &v))) {
                out->has_stored = 1;
                out->stored_index = (uint32_t)v;
            }
        } else {
            rc = pdot_decode_skip(md, f.ty, r);
        }
        if (rc)
            return rc;
    }
    return rc < 0 ? rc : 0;
}

int pdot_events_outcome(const pdot_metadata *md, const uint8_t *events, size_t len,
                        uint32_t index, pdot_ext_outcome *out)
{
    pdot_md_storage st;
    enum pdot_typedef kind;
    pdot_reader r, t;
    uint64_t n, i, seq_elem;
    uint32_t record_ty;
    int rc;

    memset(out, 0, sizeof *out);
    if (pdot_md_storage_entry(md, "System", "Events", &st) || st.is_map ||
        pdot_md_typedef(md, st.value_ty, &t, &kind) || kind != PDOT_TYPE_SEQUENCE ||
        pdot_read_compact(&t, &seq_elem) || seq_elem >= md->ntypes)
        return PDOT_E_METADATA;
    record_ty = (uint32_t)seq_elem;

    pdot_reader_init(&r, events, len);
    if (pdot_read_compact(&r, &n) || n > len)
        return PDOT_E_DECODE;
    for (i = 0; i < n; i++) {
        pdot_md_iter it;
        pdot_md_field f;
        int applies = 0, mine = 0;
        uint32_t at = 0;

        /* EventRecord { phase, event, topics }, in declared order. */
        if (pdot_md_iter_fields(md, record_ty, &it))
            return PDOT_E_METADATA;
        while ((rc = pdot_md_iter_next(&it, &f)) == 1) {
            if (pdot_md_name_eq(f.name, f.name_len, "phase")) {
                rc = read_phase(md, f.ty, &r, &applies, &at);
                mine = applies && at == index;
            } else if (pdot_md_name_eq(f.name, f.name_len, "event")) {
                rc = read_event(md, f.ty, &r, mine, out);
            } else {
                rc = pdot_decode_skip(md, f.ty, &r);
            }
            if (rc)
                return rc;
        }
        if (rc < 0)
            return rc;
    }
    return pdot_reader_remaining(&r) == 0 ? 0 : PDOT_E_DECODE;
}
