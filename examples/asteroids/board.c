#include "board.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#define GAME "pdot-asteroids"

static int valid_name(const char *s)
{
    int i;
    for (i = 0; i < 3; i++)
        if (s[i] < 'A' || s[i] > 'Z')
            return 0;
    return s[3] == '\0';
}

/* A JSON number that is a whole uint32_t. */
static int get_u32(const cJSON *o, const char *key, uint32_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > 4294967295.0 ||
        v->valuedouble != (double)(uint32_t)v->valuedouble)
        return -1;
    *out = (uint32_t)v->valuedouble;
    return 0;
}

int board_parse(const uint8_t *data, size_t len, board *b)
{
    cJSON *root = cJSON_ParseWithLength((const char *)data, len);
    const cJSON *game, *entries, *it;
    int rc = -1;

    memset(b, 0, sizeof *b);
    if (!root)
        return -1;
    game = cJSON_GetObjectItemCaseSensitive(root, "game");
    entries = cJSON_GetObjectItemCaseSensitive(root, "entries");
    if (!cJSON_IsString(game) || strcmp(game->valuestring, GAME) != 0 || !cJSON_IsArray(entries))
        goto out;
    cJSON_ArrayForEach(it, entries) {
        board_entry e;
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(it, "name");
        const cJSON *acct = cJSON_GetObjectItemCaseSensitive(it, "account");
        uint8_t id[32];

        memset(&e, 0, sizeof e);
        if (b->n == BOARD_MAX || get_u32(it, "score", &e.score) || get_u32(it, "block", &e.block) ||
            !cJSON_IsString(name) || strlen(name->valuestring) != 3 ||
            !cJSON_IsString(acct) || pdot_ss58_decode(acct->valuestring, id, NULL))
            goto out;
        memcpy(e.name, name->valuestring, 4);
        if (!valid_name(e.name))
            goto out;
        snprintf(e.account, sizeof e.account, "%s", acct->valuestring);
        /* Kept in order whatever order the document has. */
        if (!board_insert(b, &e))
            goto out;
    }
    rc = 0;
out:
    cJSON_Delete(root);
    if (rc)
        memset(b, 0, sizeof *b);
    return rc;
}

char *board_json(const board *b)
{
    cJSON *root = cJSON_CreateObject(), *entries;
    char *out = NULL;
    size_t i;

    if (!root || !cJSON_AddStringToObject(root, "game", GAME) ||
        !cJSON_AddNumberToObject(root, "version", 1) ||
        !(entries = cJSON_AddArrayToObject(root, "entries")))
        goto out;
    for (i = 0; i < b->n; i++) {
        cJSON *e = cJSON_CreateObject();
        if (!e)
            goto out;
        cJSON_AddItemToArray(entries, e);
        if (!cJSON_AddNumberToObject(e, "score", b->e[i].score) ||
            !cJSON_AddStringToObject(e, "name", b->e[i].name) ||
            !cJSON_AddStringToObject(e, "account", b->e[i].account) ||
            !cJSON_AddNumberToObject(e, "block", b->e[i].block))
            goto out;
    }
    if ((out = cJSON_PrintUnformatted(root))) {  /* cJSON's allocator: copy for free() */
        char *copy = malloc(strlen(out) + 1);
        if (copy)
            strcpy(copy, out);
        cJSON_free(out);
        out = copy;
    }
out:
    cJSON_Delete(root);
    return out;
}

int board_rank(const board *b, uint32_t score)
{
    size_t i;
    for (i = 0; i < b->n && b->e[i].score >= score; i++)
        ;
    return i < BOARD_MAX ? (int)i + 1 : 0;
}

int board_insert(board *b, const board_entry *e)
{
    int rank = board_rank(b, e->score);
    size_t at;

    if (!rank)
        return 0;
    at = (size_t)rank - 1;
    if (b->n == BOARD_MAX)
        b->n--;  /* the last one drops off */
    memmove(&b->e[at + 1], &b->e[at], (b->n - at) * sizeof b->e[0]);
    b->e[at] = *e;
    b->n++;
    return rank;
}
