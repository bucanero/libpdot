/*
 * A lying RPC node, end to end. A local JSON-RPC server replays the proofs
 * recorded from Paseo (tests/fixtures/verify-paseo.json): the GRANDPA
 * handover from set 469 to 470, the justification of a later relay block, its
 * header, the Paras::Heads proof and the Bulletin storage proofs. The client
 * talks to it over HTTP like to any node, with a checkpoint in set 469.
 *
 * Served honestly, pdot_bulletin_verify_cid proves the CID stored and the
 * checkpoint moves to set 470, through pdot_client_checkpoint and through
 * pdot_async (where a watch, not a job, moves it). Then the server forges
 * one answer at a time, and every forgery must end in PDOT_E_UNVERIFIED with
 * the checkpoint unchanged, never in a wrong value or a "not stored". The
 * server uses POSIX sockets or Winsock.
 */
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <stdlib.h>

#include <cjson/cJSON.h>

#include "pdot.h"
#include "client.h"
#include "grandpa.h"
#include "hex.h"
#include "scale.h"
#include "thread.h"
#include "test.h"

#if defined(_WIN32)
typedef SOCKET sock_t;
typedef int sock_len;
#define poll WSAPoll
#define close_socket closesocket
#define strncasecmp _strnicmp
#else
typedef int sock_t;
typedef ssize_t sock_len;
#define INVALID_SOCKET (-1)
#define close_socket close
#endif

enum {
    F_NONE,
    F_JUSTIFICATION,     /* a signature in the relay block's justification */
    F_HANDOVER_JUST,     /* a signature in set 469's last justification */
    F_HANDOVER_SET,      /* an authority in the header announcing set 470 */
    F_RELAY_HEADER,      /* the relay header's state root */
    F_PARAS_PROOF,       /* a node of the Paras::Heads proof */
    F_STORED_PROOF,      /* a node of the Bulletin storage proof */
    F_STORED_WITHHELD,   /* the storage proof left out: "not stored"? */
    F_REPLAY             /* set 469's justification served again as newest */
};

static const char *const forgery_name[] = {
    "honest", "relay justification signature", "handover justification signature",
    "authority announced in the handover header", "relay header state root",
    "Paras::Heads proof node", "Bulletin storage proof node", "storage proof withheld",
    "old justification replayed as the newest"
};

typedef struct {
    uint8_t *p;
    size_t   len;
} blob;

static struct {
    sock_t  fd;
    int     port;
    pdot_mutex lock;         /* guards the four below, shared with the test */
    int     stop;
    int     forge;
    int     requests;
    int     handovers;       /* set 469's justification served */

    blob    just, handover_just, relay_header, handover_header, metadata;
    cJSON  *paras_proof, *stored_proof, *retention_proof;
    pdot_header rh, hh;
} srv;

/* The shared fields, read or changed under the lock. */
static int get(int *field)
{
    int v;

    pdot_mutex_lock(&srv.lock);
    v = *field;
    pdot_mutex_unlock(&srv.lock);
    return v;
}

static void set(int *field, int v)
{
    pdot_mutex_lock(&srv.lock);
    *field = v;
    pdot_mutex_unlock(&srv.lock);
}

static void bump(int *field)
{
    pdot_mutex_lock(&srv.lock);
    (*field)++;
    pdot_mutex_unlock(&srv.lock);
}

/* ---- Fixtures --------------------------------------------------------------- */

static blob load_file(const char *path)
{
    blob b = { NULL, 0 };
    FILE *f = fopen(path, "rb");
    long n;

    if (!f)
        return b;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b.p = malloc((size_t)n + 1);
    if (b.p && fread(b.p, 1, (size_t)n, f) == (size_t)n)
        b.len = (size_t)n;
    fclose(f);
    return b;
}

static blob hex_field(const cJSON *o, const char *k)
{
    blob b = { NULL, 0 };
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);

    if (cJSON_IsString(v))
        pdot_hex_decode_alloc(v->valuestring, &b.p, &b.len);
    return b;
}

/* Offset of the first precommit's signature in an encoded justification. */
static size_t first_signature(const blob *j)
{
    pdot_reader r;
    uint64_t n;

    /* round u64 | target hash | target u32 | Vec<SignedPrecommit> */
    pdot_reader_init(&r, j->p + 44, j->len - 44);
    pdot_read_compact(&r, &n);
    return 44 + r.pos + 36;  /* precommit (hash, number) then the signature */
}

/* ---- Answers ---------------------------------------------------------------- */

static cJSON *hex_json(const uint8_t *p, size_t len)
{
    char *s = malloc(2 * len + 3);
    cJSON *j;

    pdot_hex_encode(p, len, s);
    j = cJSON_CreateString(s);
    free(s);
    return j;
}

/* An encoded header as chain_getHeader returns it, optionally with a byte flipped. */
static cJSON *header_json(const blob *h, int flip_root, int flip_grandpa)
{
    cJSON *o = cJSON_CreateObject(), *logs = cJSON_CreateArray(), *digest = cJSON_CreateObject();
    pdot_reader r;
    const uint8_t *p;
    uint8_t root[32];
    uint64_t number, n, i;
    char num[16];

    pdot_reader_init(&r, h->p, h->len);
    pdot_read_raw(&r, 32, &p);
    cJSON_AddItemToObject(o, "parentHash", hex_json(p, 32));
    pdot_read_compact(&r, &number);
    snprintf(num, sizeof num, "0x%x", (unsigned)number);
    cJSON_AddStringToObject(o, "number", num);
    pdot_read_raw(&r, 32, &p);
    memcpy(root, p, 32);
    if (flip_root)
        root[7] ^= 1;
    cJSON_AddItemToObject(o, "stateRoot", hex_json(root, 32));
    pdot_read_raw(&r, 32, &p);
    cJSON_AddItemToObject(o, "extrinsicsRoot", hex_json(p, 32));
    pdot_read_compact(&r, &n);
    for (i = 0; i < n; i++) {
        size_t start = r.pos;
        uint8_t variant = 0, *item;
        const uint8_t *engine = NULL, *data = NULL;
        size_t dlen = 0;

        pdot_read_u8(&r, &variant);
        if (variant == 4 || variant == 5 || variant == 6)
            pdot_read_raw(&r, 4, &engine);
        if (variant != 8)
            pdot_read_bytes(&r, &data, &dlen);
        item = malloc(r.pos - start);
        memcpy(item, h->p + start, r.pos - start);
        /* Consensus FRNK ScheduledChange: change a byte of an authority's key. */
        if (flip_grandpa && variant == 4 && memcmp(engine, "FRNK", 4) == 0 && dlen > 40)
            item[(size_t)(data - h->p) - start + 8] ^= 1;
        cJSON_AddItemToArray(logs, hex_json(item, r.pos - start));
        free(item);
    }
    cJSON_AddItemToObject(digest, "logs", logs);
    cJSON_AddItemToObject(o, "digest", digest);
    return o;
}

/* FinalityProof { block, justification: Vec<u8>, unknown_headers: Vec<Header> } */
static cJSON *finality_json(const uint8_t block[32], const blob *just, int flip)
{
    pdot_buf b;
    cJSON *j;

    pdot_buf_init(&b);
    pdot_scale_raw(&b, block, 32);
    pdot_scale_compact(&b, just->len);
    pdot_scale_raw(&b, just->p, just->len);
    pdot_scale_compact(&b, 0);
    if (flip)  /* the justification ends one byte (the empty header list) before the end */
        b.data[b.len - 1 - just->len + first_signature(just)] ^= 1;
    j = hex_json(b.data, b.len);
    pdot_buf_free(&b);
    return j;
}

/* A read proof: the nodes of each list, one node optionally changed. */
static cJSON *proof_json(const cJSON *a, const cJSON *b, int flip)
{
    cJSON *o = cJSON_CreateObject(), *list = cJSON_CreateArray(), *item;
    int idx = 0, target = cJSON_GetArraySize(a) / 2;

    cJSON_AddStringToObject(o, "at", "0x00");
    cJSON_ArrayForEach(item, a) {
        if (flip && idx == target) {
            uint8_t *d;
            size_t n;
            pdot_hex_decode_alloc(item->valuestring, &d, &n);
            d[n / 2] ^= 0x10;
            cJSON_AddItemToArray(list, hex_json(d, n));
            free(d);
        } else {
            cJSON_AddItemToArray(list, cJSON_CreateString(item->valuestring));
        }
        idx++;
    }
    if (b)
        cJSON_ArrayForEach(item, b)
            cJSON_AddItemToArray(list, cJSON_CreateString(item->valuestring));
    cJSON_AddItemToObject(o, "proof", list);
    return o;
}

static int is_hash(const cJSON *params, const uint8_t hash[32])
{
    uint8_t h[32];
    size_t n;
    const cJSON *p = cJSON_GetArrayItem(params, 0);

    return cJSON_IsString(p) && pdot_hex_decode(p->valuestring, h, 32, &n) == 0 && n == 32 &&
           memcmp(h, hash, 32) == 0;
}

/* The answer to one call; NULL for a JSON null result, *error for an error. */
static cJSON *answer(int relay, const char *method, const cJSON *params, const char **error)
{
    int f = get(&srv.forge);

    *error = NULL;
    if (relay && strcmp(method, "grandpa_proveFinality") == 0) {
        const cJSON *n = cJSON_GetArrayItem(params, 0);
        uint32_t from = cJSON_IsNumber(n) ? (uint32_t)n->valuedouble : 0;
        if (from <= srv.hh.number || f == F_REPLAY) {
            bump(&srv.handovers);
            return finality_json(srv.hh.hash, &srv.handover_just, f == F_HANDOVER_JUST);
        }
        if (from <= srv.rh.number)
            return finality_json(srv.rh.hash, &srv.just, f == F_JUSTIFICATION);
        return NULL;
    }
    if (strcmp(method, "chain_getHeader") == 0) {
        if (relay && is_hash(params, srv.rh.hash))
            return header_json(&srv.relay_header, f == F_RELAY_HEADER, 0);
        if (relay && is_hash(params, srv.hh.hash))
            return header_json(&srv.handover_header, 0, f == F_HANDOVER_SET);
        return NULL;
    }
    if (strcmp(method, "state_getReadProof") == 0) {
        if (relay)
            return proof_json(srv.paras_proof, NULL, f == F_PARAS_PROOF);
        if (f == F_STORED_WITHHELD)
            return proof_json(srv.retention_proof, NULL, 0);
        return proof_json(srv.stored_proof, srv.retention_proof, f == F_STORED_PROOF);
    }
    if (strcmp(method, "chain_getBlockHash") == 0) {
        static const uint8_t genesis[32] = { 0x11 };
        return hex_json(genesis, 32);
    }
    if (strcmp(method, "state_getRuntimeVersion") == 0) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "specName", "bulletin-paseo");
        cJSON_AddNumberToObject(o, "specVersion", 1000026);
        cJSON_AddNumberToObject(o, "transactionVersion", 1);
        return o;
    }
    if (strcmp(method, "state_getMetadata") == 0)
        return hex_json(srv.metadata.p, srv.metadata.len);
    *error = "Method not found";
    return NULL;
}

/* ---- HTTP ------------------------------------------------------------------- */

static void send_all(sock_t fd, const char *p, size_t n)
{
    while (n) {
        sock_len w = send(fd, p, (int)n, 0);
        if (w <= 0)
            return;
        p += w;
        n -= (size_t)w;
    }
}

static void serve(sock_t fd)
{
    size_t cap = 65536, len = 0, body_at = 0, want = 0;
    char *buf = malloc(cap + 1), *out, head[160];
    cJSON *req, *resp;
    const char *error;

    for (;;) {  /* headers, then Content-Length bytes of body */
        sock_len r;
        if (len == cap)
            break;
        r = recv(fd, buf + len, (int)(cap - len), 0);
        if (r <= 0)
            break;
        len += (size_t)r;
        buf[len] = '\0';
        if (!body_at) {
            char *e = strstr(buf, "\r\n\r\n"), *cl;
            if (!e)
                continue;
            body_at = (size_t)(e + 4 - buf);
            for (cl = buf; cl < e; cl++)
                if (strncasecmp(cl, "\r\nContent-Length:", 17) == 0)
                    want = strtoul(cl + 17, NULL, 10);
        }
        if (len >= body_at + want)
            break;
    }
    if (!body_at || strncmp(buf, "POST ", 5) != 0) {  /* e.g. a WebSocket upgrade */
        static const char nf[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                                 "Connection: close\r\n\r\n";
        send_all(fd, nf, sizeof nf - 1);
        free(buf);
        return;
    }
    bump(&srv.requests);
    req = cJSON_ParseWithLength(buf + body_at, len - body_at);
    resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(cJSON_GetObjectItem(req, "id"), 1));
    {
        const cJSON *m = cJSON_GetObjectItem(req, "method");
        cJSON *res = answer(strncmp(buf + 5, "/relay", 6) == 0,
                            cJSON_IsString(m) ? m->valuestring : "",
                            cJSON_GetObjectItem(req, "params"), &error);
        if (error) {
            cJSON *e = cJSON_CreateObject();
            cJSON_AddNumberToObject(e, "code", -32601);
            cJSON_AddStringToObject(e, "message", error);
            cJSON_AddItemToObject(resp, "error", e);
        } else {
            cJSON_AddItemToObject(resp, "result", res ? res : cJSON_CreateNull());
        }
    }
    out = cJSON_PrintUnformatted(resp);
    snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
             "Content-Length: %zu\r\nConnection: close\r\n\r\n", strlen(out));
    send_all(fd, head, strlen(head));
    send_all(fd, out, strlen(out));
    cJSON_free(out);
    cJSON_Delete(req);
    cJSON_Delete(resp);
    free(buf);
}

static void server_main(void *arg)
{
    (void)arg;
    while (!get(&srv.stop)) {
        struct pollfd p;
        sock_t fd;

        p.fd = srv.fd;
        p.events = POLLIN;
        if (poll(&p, 1, 50) <= 0)
            continue;
        if ((fd = accept(srv.fd, NULL, NULL)) == INVALID_SOCKET)
            continue;
        serve(fd);
        close_socket(fd);
    }
}

static int server_start(pdot_thread *t)
{
    struct sockaddr_in a;
    socklen_t alen = sizeof a;
    int one = 1;

#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa))
        return -1;
#endif
    srv.fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(srv.fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv.fd, (struct sockaddr *)&a, sizeof a) || listen(srv.fd, 16) ||
        getsockname(srv.fd, (struct sockaddr *)&a, &alen))
        return -1;
    srv.port = ntohs(a.sin_port);
    return pdot_thread_start(t, server_main, NULL);
}

/* ---- Client side ------------------------------------------------------------ */

static char relay_url[64], para_url[64];

/* "PDCK" | 1 | genesis | set_id | anchor number | anchor hash | authorities */
static void checkpoint_469(const cJSON *ho, uint8_t **ck, size_t *len)
{
    blob auth = hex_field(ho, "authorities");
    static const uint8_t genesis[32] = { 0 };
    pdot_buf b;

    pdot_buf_init(&b);
    pdot_scale_raw(&b, (const uint8_t *)"PDCK", 4);
    pdot_scale_u8(&b, 1);
    pdot_scale_raw(&b, genesis, 32);
    pdot_scale_u64(&b, (uint64_t)cJSON_GetObjectItem(ho, "set_id")->valuedouble);
    pdot_scale_u32(&b, srv.hh.number - 1);   /* anchored just before the handover */
    pdot_scale_raw(&b, srv.hh.parent, 32);
    pdot_scale_raw(&b, auth.p, auth.len);
    free(auth.p);
    *ck = b.data;
    *len = b.len;
}

/* Set id and anchor number of a checkpoint blob. */
static void ck_progress(const uint8_t *ck, uint64_t *set_id, uint32_t *anchor)
{
    pdot_reader r;

    pdot_reader_init(&r, ck, 4 + 1 + 32 + 8 + 4);
    pdot_read_skip(&r, 37);
    pdot_read_u64(&r, set_id);
    pdot_read_u32(&r, anchor);
}

static pdot_client *client(const uint8_t *ck, size_t len)
{
    pdot_client *c = pdot_client_new(para_url, NULL);

    pdot_client_set_timeout(c, 10000);
    CHECK(pdot_client_set_verifier(c, relay_url, 1501, ck, len) == 0);
    return c;
}

/* One forgery against a fresh client: it must be caught, the checkpoint kept. */
static void forged(int f, const uint8_t *ck, size_t ck_len, const pdot_cid *cid)
{
    pdot_client *c = client(ck, ck_len);
    pdot_storage_proof p;
    uint8_t *after;
    size_t after_len;
    int rc, req = get(&srv.requests);

    set(&srv.forge, f);
    rc = pdot_bulletin_verify_cid(c, cid, &p);
    printf("  %-44s -> %s: %s (%d requests)\n", forgery_name[f], pdot_strerror(rc),
           pdot_last_error(c), get(&srv.requests) - req);
    CHECK(rc == PDOT_E_UNVERIFIED);
    /* Whatever set change was proven before the forgery may stand; nothing past it. */
    CHECK(pdot_client_checkpoint(c, &after, &after_len) == 0);
    {
        uint64_t s0, s1;
        uint32_t a0, a1;
        ck_progress(ck, &s0, &a0);
        ck_progress(after, &s1, &a1);
        CHECK(s1 <= 470 && (s1 == s0 || (s1 == 470 && a1 == srv.hh.number)));
    }
    pdot_free(after);
    pdot_client_free(c);
}

struct watched {
    int heads, checkpoints;
    uint8_t *ck;
    size_t ck_len;
};

static void on_head(void *ud, int rc, const char *error, const void *result)
{
    struct watched *w = ud;
    const pdot_final_block *b = result;

    if (rc == 0 && b->verified && b->relay_block == srv.rh.number)
        w->heads++;
    else if (rc && rc != PDOT_E_CANCELLED)
        fprintf(stderr, "  watch: %s\n", error);
}

struct job {
    int done, rc;
    uint32_t block;
};

static void on_job(void *ud, int rc, const char *error, const void *result)
{
    struct job *j = ud;

    j->done = 1;
    j->rc = rc;
    if (rc)
        fprintf(stderr, "  job: %s\n", error);
    else
        j->block = ((const pdot_storage_proof *)result)->block;
}

static void on_checkpoint(void *ud, const uint8_t *ck, size_t len)
{
    struct watched *w = ud;

    w->checkpoints++;
    free(w->ck);
    w->ck = malloc(len);
    memcpy(w->ck, ck, len);
    w->ck_len = len;
}

int main(void)
{
    cJSON *j = NULL, *ho;
    blob text = load_file("verify-paseo.json");
    pdot_thread t;
    pdot_cid cid;
    uint8_t *ck469, *ck470 = NULL;
    size_t ck469_len, ck470_len = 0;
    int f;

    if (text.p) {
        text.p[text.len] = '\0';
        j = cJSON_Parse((const char *)text.p);
    }
    free(text.p);
    CHECK(j != NULL);
    if (!j)
        TEST_DONE();
    ho = cJSON_GetObjectItem(j, "handover");
    srv.just = hex_field(j, "justification");
    srv.relay_header = hex_field(j, "relay_header");
    srv.handover_just = hex_field(ho, "justification");
    srv.handover_header = hex_field(ho, "header");
    srv.paras_proof = cJSON_GetObjectItem(j, "paras_heads_proof");
    srv.stored_proof = cJSON_GetObjectItem(j, "stored_proof");
    srv.retention_proof = cJSON_GetObjectItem(j, "retention_proof");
    srv.metadata = load_file("metadata-v15-paseo.scale");
    CHECK(srv.metadata.len > 0);
    CHECK(pdot_header_decode(srv.relay_header.p, srv.relay_header.len, &srv.rh) == 0);
    CHECK(pdot_header_decode(srv.handover_header.p, srv.handover_header.len, &srv.hh) == 0);
    CHECK(pdot_cid_from_string(&cid, cJSON_GetObjectItem(j, "cid")->valuestring) == 0);
    if (pdot_mutex_init(&srv.lock) != 0 || server_start(&t) != 0) {
        CHECK(!"the fake node did not start");
        TEST_DONE();
    }
    snprintf(relay_url, sizeof relay_url, "http://127.0.0.1:%d/relay", srv.port);
    snprintf(para_url, sizeof para_url, "http://127.0.0.1:%d/para", srv.port);
    checkpoint_469(ho, &ck469, &ck469_len);
    printf("fake node on port %d: handover at relay #%u, relay block #%u\n", srv.port,
           srv.hh.number, srv.rh.number);

    /* Honest: proven stored, and the checkpoint follows the handover to set 470. */
    {
        pdot_client *c = client(ck469, ck469_len);
        pdot_storage_proof p;
        uint64_t set_id;
        uint32_t anchor;
        int rc = pdot_bulletin_verify_cid(c, &cid, &p);

        printf("  %-44s -> %s%s%s\n", forgery_name[F_NONE], pdot_strerror(rc), rc ? ": " : "",
               rc ? pdot_last_error(c) : "");
        CHECK(rc == 0 && p.block == 2511088 && p.index == 2 && p.relay_block == srv.rh.number);
        CHECK(pdot_client_checkpoint(c, &ck470, &ck470_len) == 0);
        ck_progress(ck470, &set_id, &anchor);
        CHECK(set_id == 470 && anchor == srv.hh.number);
        pdot_client_free(c);
    }

    /* Every forgery, from set 469 (through the handover) and from set 470. */
    for (f = F_JUSTIFICATION; f <= F_REPLAY; f++) {
        forged(f, ck469, ck469_len, &cid);
        if (f != F_HANDOVER_JUST && f != F_HANDOVER_SET && ck470)
            forged(f, ck470, ck470_len, &cid);
    }
    set(&srv.forge, F_NONE);

    /*
     * pdot_async: a watch alone (no job runs) walks the handover, and the
     * checkpoint it reaches is what pdot_async_checkpoint and the callback give.
     */
    {
        struct watched w;
        pdot_async *a = pdot_async_new(client(ck469, ck469_len));
        uint8_t *ck = NULL;
        size_t len = 0;
        uint64_t set_id = 0;
        uint32_t anchor = 0;
        int i;

        memset(&w, 0, sizeof w);
        CHECK(a != NULL);
        CHECK(pdot_async_checkpoint(a, &ck, &len) == 0 && len == ck469_len &&
              memcmp(ck, ck469, len) == 0);
        pdot_free(ck);
        pdot_async_set_checkpoint_fn(a, on_checkpoint, &w);
        CHECK(pdot_async_watch_head(a, on_head, &w) != 0);
        for (i = 0; i < 1000 && (!w.heads || !w.checkpoints); i++) {
            pdot_async_poll(a);
            pdot_sleep_ms(10);
        }
        CHECK(w.heads >= 1 && w.checkpoints == 1);
        CHECK(pdot_async_checkpoint(a, &ck, &len) == 0);
        CHECK(w.ck && len == w.ck_len && memcmp(ck, w.ck, len) == 0);
        ck_progress(ck, &set_id, &anchor);
        printf("  pdot_async watch: head proven from relay #%u, checkpoint set %llu at #%u\n",
               srv.rh.number, (unsigned long long)set_id, anchor);
        CHECK(set_id == 470 && anchor == srv.hh.number);
        CHECK(ck470 && len == ck470_len && memcmp(ck, ck470, len) == 0);
        pdot_free(ck);
        free(w.ck);

        /* A job then starts from the watch's progress: the handover is not walked again. */
        {
            struct job j2;
            int handovers = get(&srv.handovers);

            memset(&j2, 0, sizeof j2);
            CHECK(pdot_async_verify_cid(a, &cid, on_job, &j2) != 0);
            for (i = 0; i < 1000 && !j2.done; i++) {
                pdot_async_poll(a);
                pdot_sleep_ms(10);
            }
            CHECK(j2.done && j2.rc == 0 && j2.block == 2511088);
            CHECK(get(&srv.handovers) == handovers);
            printf("  pdot_async job: proven stored at #%u, handover proofs fetched: %d\n",
                   j2.block, get(&srv.handovers) - handovers);
        }
        pdot_async_free(a);
    }

    set(&srv.stop, 1);
    pdot_thread_join(t);
    pdot_mutex_destroy(&srv.lock);
    close_socket(srv.fd);
#if defined(_WIN32)
    WSACleanup();
#endif
    free(srv.just.p);
    free(srv.handover_just.p);
    free(srv.relay_header.p);
    free(srv.handover_header.p);
    free(srv.metadata.p);
    pdot_free(ck469);
    pdot_free(ck470);
    cJSON_Delete(j);
    TEST_DONE();
}
