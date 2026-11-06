/*
 * Polkadot Asset Hub reads recorded into tests/fixtures/assets-polkadot.json
 * by tools/record-assets.c and checked by tests/test_assets.c.
 */
#ifndef ASSETS_CASES_H
#define ASSETS_CASES_H

#include <string.h>

#include "assets.h"

/* A USDt holder, and the owner of NFT 752/56, as account ids. */
static const uint8_t usdt_holder[32] = {
    0x08, 0x62, 0x2f, 0xf9, 0xde, 0xd7, 0x47, 0x6d, 0x40, 0x17, 0x86, 0x85, 0xd0, 0xa0, 0xa9, 0xd2,
    0x8c, 0xd3, 0x96, 0x47, 0x2d, 0xba, 0x36, 0xeb, 0x73, 0x5f, 0x48, 0xca, 0x54, 0xa0, 0xc7, 0xd7,
};

static const char nft_owner_ss58[] = "15wD8upZxRZijKkF7JZDdaaFXuRZJhFyYpFQC5j6FaZL5Pza";

/* Indices into cases[], for the checks in test_assets.c. */
enum {
    CASE_USDT_INFO, CASE_USDT_HOLDING, CASE_OWNER_BALANCE, CASE_NFT, CASE_ATTR_ITEM,
    CASE_ATTR_COLLECTION, CASE_NO_ASSET, CASE_NO_NFT, CASE_NO_HOLDING, NCASES
};

/* Fills cases[NCASES]; the owner's account id is passed in, decoded from SS58. */
static void assets_cases(pdot_query cases[NCASES], const uint8_t owner[32])
{
    static const uint8_t university[] = "University", royalty[] = "royalty";
    static const uint8_t nobody[32] = { 0x42 };
    size_t i;

    for (i = 0; i < NCASES; i++)
        memset(&cases[i], 0, sizeof cases[i]);
    cases[CASE_USDT_INFO].kind = PDOT_Q_ASSET_INFO;
    cases[CASE_USDT_INFO].id = 1984;
    cases[CASE_USDT_HOLDING].kind = PDOT_Q_ASSET_BALANCE;
    cases[CASE_USDT_HOLDING].id = 1984;
    memcpy(cases[CASE_USDT_HOLDING].account, usdt_holder, 32);
    cases[CASE_OWNER_BALANCE].kind = PDOT_Q_BALANCE;
    memcpy(cases[CASE_OWNER_BALANCE].account, owner, 32);
    cases[CASE_NFT].kind = PDOT_Q_NFT_ITEM;
    cases[CASE_NFT].id = 752;
    cases[CASE_NFT].item = 56;
    cases[CASE_ATTR_ITEM].kind = PDOT_Q_NFT_ATTRIBUTE;
    cases[CASE_ATTR_ITEM].id = 785;
    cases[CASE_ATTR_ITEM].has_item = 1;
    cases[CASE_ATTR_ITEM].item = 19;
    cases[CASE_ATTR_ITEM].ns = PDOT_NFT_NS_COLLECTION_OWNER;
    cases[CASE_ATTR_ITEM].bytes = university;
    cases[CASE_ATTR_ITEM].len = sizeof university - 1;
    cases[CASE_ATTR_COLLECTION].kind = PDOT_Q_NFT_ATTRIBUTE;
    cases[CASE_ATTR_COLLECTION].id = 158;
    cases[CASE_ATTR_COLLECTION].ns = PDOT_NFT_NS_COLLECTION_OWNER;
    cases[CASE_ATTR_COLLECTION].bytes = royalty;
    cases[CASE_ATTR_COLLECTION].len = sizeof royalty - 1;
    cases[CASE_NO_ASSET].kind = PDOT_Q_ASSET_INFO;
    cases[CASE_NO_ASSET].id = 4000000000u;
    cases[CASE_NO_NFT].kind = PDOT_Q_NFT_ITEM;
    cases[CASE_NO_NFT].id = 752;
    cases[CASE_NO_NFT].item = 4000000000u;
    cases[CASE_NO_HOLDING].kind = PDOT_Q_ASSET_BALANCE;
    cases[CASE_NO_HOLDING].id = 1984;
    memcpy(cases[CASE_NO_HOLDING].account, nobody, 32);
}

#endif
