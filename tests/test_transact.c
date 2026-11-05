/*
 * Transactions on Polkadot Asset Hub's metadata: calls encoded from JSON
 * (checked against bytes worked out by hand from the SCALE rules), the
 * ChargeAssetTxPayment extension, the fee and proxied-call outcome in
 * events, Proxy.Proxies decoding, and amount parsing.
 */
#include <stdlib.h>

#include <cjson/cJSON.h>

#include "pdot.h"
#include "assets.h"
#include "client.h"
#include "encode.h"
#include "events.h"
#include "tx.h"
#include "test.h"

#define ALICE_HEX "d43593c715fdd31c61141abd04a99fd6822c8558854ccde39a5684e7a56da27d"
#define ALICE_SS58 "5GrwvaEF5zXb26Fz9rcQpDWS57CtERHpNehXCPcNoHGKutQY"

static const uint8_t alice_seed[32] = {
    0xe5, 0xbe, 0x9a, 0x50, 0x92, 0xb8, 0x1b, 0xca, 0x64, 0xbe, 0x81, 0xd2, 0x12, 0xe7, 0xf2, 0xf9,
    0xeb, 0xa1, 0x83, 0xbb, 0x7a, 0x90, 0x95, 0x4f, 0x7b, 0x76, 0x36, 0x1f, 0x6e, 0xdb, 0x5c, 0x0a,
};

static uint8_t *load(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *p = NULL;
    long n;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
        (p = malloc((size_t)n)) && fread(p, 1, (size_t)n, f) == (size_t)n)
        *len = (size_t)n;
    else {
        free(p);
        p = NULL;
    }
    fclose(f);
    return p;
}

/* Encodes pallet.call(json) and compares with hex; NULL hex expects failure containing err. */
static void call_is(const pdot_metadata *md, const char *pallet, const char *call,
                    const char *json, const char *hex, const char *err_part)
{
    cJSON *args = json ? cJSON_Parse(json) : NULL;
    char err[200] = "";
    pdot_buf b;
    int rc;

    CHECK(!json || args);
    pdot_buf_init(&b);
    rc = pdot_encode_call_json(md, pallet, call, args, &b, err, sizeof err);
    if (hex) {
        if (rc)
            fprintf(stderr, "  %s.%s %s: %s\n", pallet, call, json, err);
        CHECK(rc == 0);
        CHECK_HEX(b.data, b.len, hex);
    } else {
        CHECK(rc != 0 && b.len == 0);
        if (!strstr(err, err_part))
            fprintf(stderr, "  %s.%s %s: got \"%s\", wanted \"%s\"\n", pallet, call, json, err,
                    err_part);
        CHECK(strstr(err, err_part) != NULL);
    }
    pdot_buf_free(&b);
    cJSON_Delete(args);
}

static void test_calls(const pdot_metadata *md)
{
    /* Balances = 10, transfer_keep_alive = 3; MultiAddress::Id = 0; 10^12 as Compact. */
    call_is(md, "Balances", "transfer_keep_alive", "[\"0x" ALICE_HEX "\", 1000000000000]",
            "0a0300" ALICE_HEX "070010a5d4e8", NULL);
    call_is(md, "Balances", "transfer_keep_alive", "[\"" ALICE_SS58 "\", \"1000000000000\"]",
            "0a0300" ALICE_HEX "070010a5d4e8", NULL);
    call_is(md, "Balances", "transfer_keep_alive",
            "{\"value\": 1000000000000, \"dest\": {\"Id\": \"" ALICE_SS58 "\"}}",
            "0a0300" ALICE_HEX "070010a5d4e8", NULL);
    /* u128::MAX takes the 17-byte compact form. */
    call_is(md, "Balances", "transfer_keep_alive",
            "[\"" ALICE_SS58 "\", \"340282366920938463463374607431768211455\"]",
            "0a0300" ALICE_HEX "33ffffffffffffffffffffffffffffffff", NULL);
    /* Assets = 50, transfer_keep_alive = 9: Compact<u32> id 1984, Compact<u128> 5. */
    call_is(md, "Assets", "transfer_keep_alive", "[1984, \"" ALICE_SS58 "\", 5]",
            "3209011f00" ALICE_HEX "14", NULL);
    /* Nfts = 52, transfer = 6: u32 collection and item. */
    call_is(md, "Nfts", "transfer", "[752, 56, \"" ALICE_SS58 "\"]",
            "3406f002000038000000" "00" ALICE_HEX, NULL);
    /* Proxy = 42, add_proxy = 1: ProxyType::Assets = 3, delay u32. */
    call_is(md, "Proxy", "add_proxy", "[\"" ALICE_SS58 "\", \"Assets\", 10]",
            "2a0100" ALICE_HEX "030a000000", NULL);
    /* Options: null is None, a bare value or {"Some": v} is Some. */
    call_is(md, "Nfts", "mint", "[1, 2, \"" ALICE_SS58 "\", null]",
            "3403" "01000000" "02000000" "00" ALICE_HEX "00", NULL);
    /* Bytes: text or hex. System = 0, remark = 0. */
    call_is(md, "System", "remark", "[\"hi\"]", "0000086869", NULL);
    call_is(md, "System", "remark", "[\"0x6869\"]", "0000086869", NULL);
    call_is(md, "System", "remark", "{\"remark\": [104, 105]}", "0000086869", NULL);

    /* Errors name the argument. */
    call_is(md, "Balances", "transfer_keep_alive", "[\"" ALICE_SS58 "\", -1]", NULL,
            "Balances.transfer_keep_alive.value: -1 is not a whole number");
    call_is(md, "Balances", "transfer_keep_alive", "[\"" ALICE_SS58 "\", 1e20]", NULL,
            "pass larger ones as a string");
    call_is(md, "Balances", "transfer_keep_alive", "{\"dest\": \"" ALICE_SS58 "\"}", NULL,
            "value: missing");
    call_is(md, "Balances", "transfer_keep_alive",
            "{\"dest\": \"" ALICE_SS58 "\", \"value\": 1, \"memo\": 2}", NULL,
            "has fields the type does not");
    call_is(md, "Balances", "transfer_keep_alive", "[\"5Gnotanaddress\", 1]", NULL,
            "dest.Id: expected an SS58 address");
    call_is(md, "Proxy", "add_proxy", "[\"" ALICE_SS58 "\", \"Wizard\", 0]", NULL,
            "ProxyType has no variant Wizard");
    call_is(md, "Proxy", "add_proxy", "[\"" ALICE_SS58 "\", \"Any\", 4294967296]", NULL,
            "delay: does not fit in u32");
    call_is(md, "Balances", "transfer_keep_alive", "[1]", NULL, "expected 2 values");
    call_is(md, "Balances", "nope", "[]", NULL, "no such call");
    call_is(md, "Balances", "transfer_keep_alive", "\"x\"", NULL, "an array in order");
}

/* compact len | 84 | 00 account | 01 sig(64) | era(2) nonce tip asset_id mode | call */
static void test_asset_tip(const pdot_metadata *md)
{
    pdot_signer s;
    pdot_tx_params p;
    pdot_buf xt;
    static const uint8_t call[] = { 0x00, 0x00, 0x04, 0x68 };  /* System.remark("h") */
    size_t off;

    CHECK(pdot_signer_sr25519_from_seed(&s, alice_seed) == 0);
    memset(&p, 0, sizeof p);
    p.md = md;
    memset(p.genesis, 0x11, 32);
    p.spec_version = 1;
    p.tx_version = 1;
    p.nonce = 5;
    p.era_period = 64;
    p.era_block = 100;
    p.tip.lo = 1000;
    pdot_buf_init(&xt);
    CHECK(pdot_tx_build(NULL, &p, &s, call, sizeof call, &xt) == 0);
    off = 2 + 1 + 33 + 65;  /* the length takes 2 bytes here */
    CHECK(xt.len == off + 2 + 1 + 2 + 1 + 1 + sizeof call);
    CHECK(xt.data[2] == 0x84 && xt.data[36] == 0x01);
    CHECK(xt.data[off + 2] == 5 << 2);                       /* nonce */
    CHECK(xt.data[off + 3] == 0xa1 && xt.data[off + 4] == 0x0f);  /* Compact 1000 */
    CHECK(xt.data[off + 5] == 0x00);                         /* asset_id: None */
    CHECK(xt.data[off + 6] == 0x00);                         /* CheckMetadataHash off */
    CHECK(memcmp(xt.data + xt.len - sizeof call, call, sizeof call) == 0);
    pdot_buf_free(&xt);
    pdot_signer_wipe(&s);
}

/* Builds System.Events from JSON with the encoder, then reads an outcome back. */
static int outcome_of(const pdot_metadata *md, const char *json, uint32_t index,
                      pdot_ext_outcome *oc)
{
    pdot_md_storage st;
    cJSON *v = cJSON_Parse(json);
    char err[200] = "";
    pdot_buf b;
    int rc;

    CHECK(v != NULL);
    CHECK(pdot_md_storage_entry(md, "System", "Events", &st) == 0);
    pdot_buf_init(&b);
    rc = pdot_encode_json(md, st.value_ty, v, &b, err, sizeof err);
    if (rc)
        fprintf(stderr, "  events: %s\n", err);
    CHECK(rc == 0);
    if (!rc)
        rc = pdot_events_outcome(md, b.data, b.len, index, oc);
    pdot_buf_free(&b);
    cJSON_Delete(v);
    return rc;
}

#define SUCCESS_EVENT                                                                      \
    "{\"System\": {\"ExtrinsicSuccess\": {\"dispatch_info\": {\"weight\": {\"ref_time\": 1, " \
    "\"proof_size\": 2}, \"class\": \"Normal\", \"pays_fee\": \"Yes\"}}}}"
#define FEE_EVENT(n)                                                                       \
    "{\"AssetTxPayment\": {\"AssetTxFeePaid\": {\"who\": \"" ALICE_SS58 "\", "             \
    "\"actual_fee\": " n ", \"tip\": 0, \"asset_id\": {\"parents\": 0, \"interior\": \"Here\"}}}}"

static void test_events(const pdot_metadata *md)
{
    pdot_ext_outcome oc;

    /* A proxied transfer that failed inside, in extrinsic 1, after extrinsic 0's fee. */
    CHECK(outcome_of(md,
                     "[[{\"ApplyExtrinsic\": 0}, " FEE_EVENT("999") ", []],"
                     " [{\"ApplyExtrinsic\": 1}, {\"Proxy\": {\"ProxyExecuted\": {\"result\":"
                     "   {\"Err\": {\"Token\": \"FundsUnavailable\"}}}}}, []],"
                     " [{\"ApplyExtrinsic\": 1}, " FEE_EVENT("\"4700350055\"") ", []],"
                     " [{\"ApplyExtrinsic\": 1}, " SUCCESS_EVENT ", []],"
                     " [\"Finalization\", " SUCCESS_EVENT ", []]]",
                     1, &oc) == 0);
    CHECK(oc.success && !oc.failed && oc.proxy_failed);
    CHECK(strcmp(oc.error, "Token.FundsUnavailable (in the proxied call)") == 0);
    CHECK(oc.has_fee && oc.fee.lo == 4700350055u && oc.fee.hi == 0);
    CHECK(oc.nevents == 3);

    /* A proxied call that worked. */
    CHECK(outcome_of(md,
                     "[[{\"ApplyExtrinsic\": 2}, {\"Proxy\": {\"ProxyExecuted\": {\"result\":"
                     "   {\"Ok\": null}}}}, []],"
                     " [{\"ApplyExtrinsic\": 2}, " SUCCESS_EVENT ", []]]",
                     2, &oc) == 0);
    CHECK(oc.success && !oc.proxy_failed && !oc.has_fee);

    /* A module error inside a proxy names pallet and error. Balances = 10, error 2. */
    CHECK(outcome_of(md,
                     "[[{\"ApplyExtrinsic\": 0}, {\"Proxy\": {\"ProxyExecuted\": {\"result\":"
                     "   {\"Err\": {\"Module\": {\"index\": 10, \"error\": \"0x02000000\"}}}}}}, []]]",
                     0, &oc) == 0);
    CHECK(oc.proxy_failed && strncmp(oc.error, "Balances.", 9) == 0);
}

static void test_proxies(pdot_client *c)
{
    pdot_md_storage st;
    pdot_query q;
    pdot_value v;
    pdot_final_block at;
    pdot_proxies out;
    pdot_buf b;
    char err[200] = "";
    cJSON *j = cJSON_Parse(
        "[[{\"delegate\": \"" ALICE_SS58 "\", \"proxy_type\": \"Any\", \"delay\": 0},"
        "  {\"delegate\": \"0x0101010101010101010101010101010101010101010101010101010101010101\","
        "   \"proxy_type\": \"NonTransfer\", \"delay\": 7}], \"10036500000\"]");

    CHECK(pdot_md_storage_entry(c->md, "Proxy", "Proxies", &st) == 0);
    pdot_buf_init(&b);
    CHECK(pdot_encode_json(c->md, st.value_ty, j, &b, err, sizeof err) == 0);
    memset(&q, 0, sizeof q);
    q.kind = PDOT_Q_PROXIES;
    memset(&at, 0, sizeof at);
    at.number = 42;
    v.data = b.data;
    v.len = b.len;
    v.present = 1;
    CHECK(pdot_query_decode(c, &q, &v, &at, &out) == 0);
    CHECK(out.n == 2 && out.at.number == 42);
    CHECK(strcmp(out.list[0].type, "Any") == 0 && out.list[0].delay == 0);
    CHECK_HEX(out.list[0].delegate, 32, ALICE_HEX);
    CHECK(strcmp(out.list[1].type, "NonTransfer") == 0 && out.list[1].delay == 7);
    CHECK(out.deposit.lo == 10036500000ull);
    v.present = 0;
    CHECK(pdot_query_decode(c, &q, &v, &at, &out) == 0 && out.n == 0);
    pdot_buf_free(&b);
    cJSON_Delete(j);
}

static void test_parse(void)
{
    pdot_u128 v;

    CHECK(pdot_u128_parse("1.5", 12, &v) == 0 && v.lo == 1500000000000ull && v.hi == 0);
    CHECK(pdot_u128_parse("12.34", 2, &v) == 0 && v.lo == 1234);
    CHECK(pdot_u128_parse("7", 0, &v) == 0 && v.lo == 7);
    CHECK(pdot_u128_parse("0.000000000001", 12, &v) == 0 && v.lo == 1);
    CHECK(pdot_u128_parse("340282366920938463463374607431768211455", 0, &v) == 0 &&
          v.lo == UINT64_MAX && v.hi == UINT64_MAX);
    CHECK(pdot_u128_parse("340282366920938463463374607431768211456", 0, &v) == PDOT_E_INVALID);
    CHECK(pdot_u128_parse("1.001", 2, &v) == PDOT_E_INVALID);  /* finer than the token */
    CHECK(pdot_u128_parse("", 2, &v) == PDOT_E_INVALID);
    CHECK(pdot_u128_parse(".", 2, &v) == PDOT_E_INVALID);
    CHECK(pdot_u128_parse("1.2.3", 2, &v) == PDOT_E_INVALID);
    CHECK(pdot_u128_parse("-1", 2, &v) == PDOT_E_INVALID);
    CHECK(pdot_u128_parse("1e3", 2, &v) == PDOT_E_INVALID);
}

int main(void)
{
    pdot_client *c = pdot_client_new("http://127.0.0.1:1", NULL);
    size_t len;
    uint8_t *blob = load("metadata-v15-asset-hub-polkadot.scale", &len);

    test_parse();
    if (!c || !blob || pdot_metadata_parse(blob, len, &c->md)) {
        fprintf(stderr, "cannot load the Asset Hub metadata\n");
        return 1;
    }
    free(blob);
    test_calls(c->md);
    test_asset_tip(c->md);
    test_events(c->md);
    test_proxies(c);
    pdot_client_free(c);
    TEST_DONE();
}
