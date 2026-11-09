/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * Chain tags: a story recorded on Polkadot Asset Hub, through libpdot.
 * Added to Suika3 for the libpdot sample; not part of upstream Suika3.
 *
 *   [chain_start collection="1" name="Dave" ask_name="true" traits="rei_warmth,mio_trust"]
 *	With ask_name="true", a new story first asks the player's name (up
 *	to 16 letters, digits and spaces; Return confirms, Backspace
 *	deletes; empty keeps name=). A continued story keeps the name on
 *	its NFT. Then the player signs one transaction (Utility.batch_all): it funds a
 *	fresh session key, mints the story NFT to the player in the game's
 *	collection (public minting: no operator), approves the session key
 *	to set attributes on it, and records "name=... block=N" as the
 *	player's own attribute. Block N is the newest final block when the
 *	story starts; its hash and the player's account seed the story,
 *	setting each trait variable to 0..100, so the same choice lands
 *	differently for different players. The tag waits only until the
 *	transaction is in a block (pdot_async_set_included_fn), not for
 *	finality.
 *
 *   [chain_choose name="c1" text1="Yes" value1="yes" text2="No" value2="no"]
 *	[choose] (same arguments) and [chain_choice] in one: the picked
 *	value goes into the variable and on chain. While a story is being
 *	continued, it picks the choice recorded on chain instead, without
 *	showing the options.
 *
 *   [chain_choice var="result"]
 *	Appends that variable's value to the story's choices and records
 *	them on the NFT, from the session key, under its own attribute
 *	namespace. Does not wait: one write is in flight at a time, and
 *	choices made meanwhile go out together in the next.
 *
 *   [chain_ending name="B"]
 *	The player seals the story, with a second signature: the last
 *	choices and the ending, signed by the session key for its own
 *	attribute namespace, submitted and paid for by the player, who owns
 *	the NFT (Nfts.set_attributes_pre_signed). pallet-nfts takes an
 *	attribute's deposit from whoever pays for it, so this also moves the
 *	session key's deposits to the player. Once that is final, the session
 *	key sends everything it holds back to the player (transfer_all) and is
 *	gone: nothing is left behind. Waits for both, showing the progress at
 *	the top left of the screen, so the player knows the story is complete
 *	on chain before the game says so. A choices write still in flight is
 *	waited for first: it is the session key's, the seal the player's, so
 *	nothing else would keep it from landing after the seal.
 *
 * Continuing: [chain_start] saves the session key and the story's item
 * and seed block to pdot-story.txt (readable by this user only) before
 * the story goes on chain, and the ending deletes it. If the game quits
 * in between, the next [chain_start] for the same player checks the story
 * on chain: still the player's, not ended, the session key funded and the
 * seed block's hash matching (pdot_block_hash_get, while the chain keeps
 * it). Then it continues that story: the same traits, and the choices
 * recorded on the NFT replayed by [chain_choose] in skip mode, up to
 * where the player left off. Otherwise a new story starts. The chain is
 * read at its newest final block, so a choice still waiting for finality
 * when the game quit is not there yet: the choices this machine recorded
 * (pdot-story-choices.txt) are replayed instead when the chain's are
 * their beginning, since the write carrying the rest is already on its
 * way and every write carries all of them.
 *
 * Save and load: the engine's own save (the save screen, quick save) also
 * makes sure every choice so far is on the NFT, and shows it at the top
 * left of the screen until it is final: "Saved on chain: all N choices,
 * block B, final". Load restores the local save as usual; the choices up
 * to that point come back with it (the chain_choices variable), so the
 * next save records those.
 *
 * Variables for the script: chain_status ("ok", "offline: ..." or
 * "error: ..."), chain_resumed (1 when continuing), chain_item,
 * chain_block, chain_collection, player_name, and one per trait. Without
 * a chain the story still plays, with a local seed.
 *
 * Settings come from the environment (a sample, not a wallet):
 *   PDOT_RPC		Asset Hub RPC (default http://127.0.0.1:9944)
 *   PDOT_PLAYER_SEED	the player's sr25519 seed, hex (default //Alice:
 *			dev chains only)
 *   PDOT_RELAY, PDOT_CHECKPOINT
 *			prove results from relay-chain finality
 *   PDOT_FUND		tokens sent to the session key (default 1)
 *   PDOT_PLAYER_NAME	the name on the story NFT (else asked, or the tag's
 *			name=); set, the game does not ask
 *   PDOT_COLLECTION	the story collection (else the tag's collection=)
 */

#include <suika3/suika3.h>
#include "cmd.h"
#include "conf.h"
#include "tag.h"

#include <pdot.h>
#include <playfield/playfield.h>  /* key presses, for the name prompt */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#define STORY_FILE	"pdot-story.txt"
#define CHOICES_FILE	"pdot-story-choices.txt"  /* this machine's latest choices */

/* [choose]'s implementation (src/cmd_choose.c), which [chain_choose] wraps. */
bool s3i_tag_choose(void *p);

#define CHOICES_MAX	200	/* stays under Asset Hub's attribute value limit */
#define NAME_MAX	24
#define ENDING_MAX	32
#define TRAITS_MAX	256

enum { ST_NONE, ST_RESUMING, ST_NAMING, ST_STARTING, ST_OPEN, ST_ENDING, ST_ENDED, ST_FAILED };
enum { J_START, J_WRITE, J_END, J_RESUME };

struct job {
	int		kind;
	char		choices[CHOICES_MAX + 1];
	char		ending[ENDING_MAX + 1];
	pdot_tx_result	tx;
	int		sealed;		/* end: the player's seal is final; tx is the funds' return */
	pdot_tx_result	seal;		/* end: the player's seal */

	/* start: filled on the worker before the transaction is sent;
	   resume: from the story file */
	pdot_signer	session;
	uint32_t	item;
	uint32_t	block;
	uint8_t		block_hash[32];

	/* resume: what the chain says */
	int		resume;		/* 1: continue it; 0: start a new story */
	int		finished;	/* the story has its ending */
	char		name[NAME_MAX + 1];	/* the name on it */
	char		why[160];	/* why not, when resume is 0 */
};

static struct {
	pdot_async	*a;
	pdot_signer	player, session;
	char		fund[32];
	uint32_t	collection, item, block;
	uint8_t		block_hash[32];
	char		name[NAME_MAX + 1];
	int		ask_name;	/* a new story asks the player's name first */
	int		name_keys;	/* Return and Backspace as they were last frame */
	char		traits[TRAITS_MAX];
	char		choices[CHOICES_MAX + 1];
	int		state;
	int		writing;	/* a choices write is in flight */
	uint64_t	write_id;	/* its pdot_async job */
	int		dirty;		/* choices changed since it was sent */

	/* continuing: the choices recorded on chain, replayed by [chain_choose] */
	char		replay[CHOICES_MAX + 1];
	size_t		replay_at;
	int		replaying;
	char		choose_var[64];	/* the running [chain_choose]'s variable */

	/* save: waiting for these choices to be final on chain */
	int		saving;
	char		save_want[CHOICES_MAX + 1];
	char		final_choices[CHOICES_MAX + 1];	/* the last ones known final */
	int		notice_timed;	/* clear the notice when the timer runs out */
	uint64_t	notice_sw;
} g;

/* ---- helpers ------------------------------------------------------- */

static void
status(
	const char *fmt,
	...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fprintf(stderr, "[chain] %s\n", buf);
}

/*
 * A line at the top left of the screen (text layer 8, above the stage,
 * not part of saves). NULL clears it; done lines clear after a while.
 */
static void
notice(
	const char *msg,
	int error,
	int done)
{
	struct s3_drawmsg *d;
	struct s3_image *img;
	const int w = 760, h = 44;

	g.notice_timed = 0;
	if (!msg) {
		s3_set_layer_image(S3_LAYER_TEXT8, NULL);
		return;
	}
	if (!(img = s3_create_image(w, h)))
		return;
	s3_fill_image_rect(img, 0, 0, w, h, s3_make_pixel(170, 0, 0, 0));
	d = s3_create_drawmsg(img, msg, conf_msgbox_font_select, 22, 22, 0, 0, 10, 10, w, h,
			      0, 0, 0, 0, 0, 0,
			      error ? s3_make_pixel(255, 255, 120, 120) :
				      done ? s3_make_pixel(255, 160, 255, 160) :
					     s3_make_pixel(255, 255, 255, 255),
			      s3_make_pixel(255, 0, 0, 0), 0, false, false, true, false, true,
			      false, false, false, true, true, NULL, false);
	if (d) {
		s3_draw_message(d, s3_count_drawmsg_chars(d, NULL));
		s3_destroy_drawmsg(d);
	}
	s3_notify_image_update(img);
	s3_set_layer_image(S3_LAYER_TEXT8, img);  /* the layer owns it now */
	s3_set_layer_position(S3_LAYER_TEXT8, 12, 12);
	s3_set_layer_alpha(S3_LAYER_TEXT8, 255);
	if (done) {
		g.notice_timed = 1;
		s3_reset_lap_timer(&g.notice_sw);
	}
}

/* A notice about saving or loading, also logged so it shows in the terminal. */
static void
save_notice(
	const char *msg,
	int error,
	int done)
{
	status("%s", msg);
	notice(msg, error, done);
}

static size_t
count_choices(
	const char *s)
{
	size_t n = *s ? 1 : 0;

	for (; *s; s++)
		n += *s == ',';
	return n;
}

static void
hex32(
	const uint8_t b[32],
	char out[67])
{
	int i;

	out[0] = '0';
	out[1] = 'x';
	for (i = 0; i < 32; i++)
		snprintf(out + 2 + 2 * i, 3, "%02x", b[i]);
}

static int
parse_seed(
	const char *hex,
	uint8_t out[32])
{
	unsigned b;
	int i;

	if (hex[0] == '0' && hex[1] == 'x')
		hex += 2;
	if (strlen(hex) != 64)
		return -1;
	for (i = 0; i < 32; i++) {
		if (sscanf(hex + 2 * i, "%2x", &b) != 1)
			return -1;
		out[i] = (uint8_t)b;
	}
	return 0;
}

/* Keeps [A-Za-z0-9_-] (and spaces when allowed): it goes into JSON and on chain as it is. */
static void
clean(
	char *dst,
	size_t cap,
	const char *src,
	int spaces)
{
	size_t n = 0;

	for (; src && *src && n + 1 < cap; src++) {
		char ch = *src;
		if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
		    (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ||
		    (spaces && ch == ' '))
			dst[n++] = ch;
	}
	dst[n] = '\0';
}

static uint64_t
splitmix(
	uint64_t *x)
{
	uint64_t z = (*x += 0x9e3779b97f4a7c15ull);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

/*
 * Each trait in g.traits gets 0..100 from the block's hash and the
 * player's account: a different cast of characters for every player and
 * every story.
 */
static void
set_traits(
	const uint8_t block_hash[32],
	const uint8_t account[32])
{
	char name[64];
	const char *s = g.traits;
	uint64_t x = 0xcbf29ce484222325ull;
	size_t n;
	int i;

	for (i = 0; i < 32; i++)
		x = (x ^ block_hash[i]) * 0x100000001b3ull;
	for (i = 0; i < 32; i++)
		x = (x ^ account[i]) * 0x100000001b3ull;
	while (*s) {
		int v;

		/* names separated by commas or spaces */
		while (*s == ',' || *s == ' ')
			s++;
		for (n = 0; s[n] && s[n] != ',' && s[n] != ' '; n++)
			;
		if (!n)
			break;
		snprintf(name, sizeof(name), "%.*s", (int)n, s);
		s += n;
		v = (int)(splitmix(&x) % 101);
		s3_set_variable_int(name, v);
		status("trait %s = %d", name, v);
	}
}

/* ---- the story file -------------------------------------------------- */

/*
 * "pdot-story 1 SEED PLAYER COLLECTION ITEM BLOCK BLOCKHASH": the session
 * key's seed (hex), the player it belongs to, the story NFT and its seed
 * block. Runs on the worker; g's fields it reads are fixed by then.
 */
static int
save_story(
	const uint8_t seed[32],
	const struct job *j)
{
	char s[67], h[67], player[PDOT_SS58_MAX_LEN];
	FILE *f;
#if !defined(_WIN32)
	mode_t old = umask(077);  /* a key: readable by this user only */
	f = fopen(STORY_FILE, "w");
	umask(old);
#else
	f = fopen(STORY_FILE, "w");
#endif
	if (!f)
		return -1;
	hex32(seed, s);
	hex32(j->block_hash, h);
	pdot_ss58_encode(g.player.account_id, 42, player, sizeof(player));
	fprintf(f, "pdot-story 1 %s %s %u %u %u %s\n", s + 2, player, g.collection, j->item,
		j->block, h + 2);
	return fclose(f) ? -1 : 0;
}

/* A saved story of this player in this collection, into j. 1 if there is one. */
static int
load_story(
	struct job *j)
{
	char seed_hex[65], player[PDOT_SS58_MAX_LEN], mine[PDOT_SS58_MAX_LEN], hash_hex[65];
	unsigned version, collection, item, block;
	uint8_t seed[32];
	FILE *f = fopen(STORY_FILE, "r");
	int ok;

	if (!f)
		return 0;
	ok = fscanf(f, "pdot-story %u %64s %63s %u %u %u %64s", &version, seed_hex, player,
		    &collection, &item, &block, hash_hex) == 7;
	fclose(f);
	pdot_ss58_encode(g.player.account_id, 42, mine, sizeof(mine));
	if (!ok || version != 1 || strcmp(player, mine) != 0 || collection != g.collection) {
		status("%s is another player's or collection's story: starting a new one", STORY_FILE);
		return 0;
	}
	ok = parse_seed(seed_hex, seed) == 0 &&
	     pdot_signer_sr25519_from_seed(&j->session, seed) == 0 &&
	     parse_seed(hash_hex, j->block_hash) == 0;
	memset(seed, 0, sizeof(seed));
	memset(seed_hex, 0, sizeof(seed_hex));
	if (!ok)
		return 0;
	j->item = item;
	j->block = block;
	return 1;
}

/* ---- work: runs on pdot_async's worker thread ----------------------- */

static int
work_start(
	pdot_client *c,
	void *ud)
{
	struct job *j = ud;
	char symbol[16], amount[64], player[67], session[67], json[1536];
	uint8_t decimals, seed[32];
	pdot_balance bal;
	pdot_u128 fund;
	uint8_t *call;
	size_t len;
	int rc;

	if ((rc = pdot_client_token(c, symbol, sizeof(symbol), &decimals)) ||
	    (rc = pdot_u128_parse(g.fund, decimals, &fund)) ||
	    (rc = pdot_u128_format(fund, 0, amount, sizeof(amount))))
		return rc;

	/* The newest final block (proven with a verifier): the story's seed. */
	if ((rc = pdot_balance_get(c, g.player.account_id, &bal)))
		return rc;
	j->block = bal.at.number;
	memcpy(j->block_hash, bal.at.hash, 32);

	/* A fresh session key; the item number comes from the same randomness. */
	if ((rc = pdot_signer_sr25519_random(&j->session, seed)))
		return rc;
	j->item = ((uint32_t)seed[0] | (uint32_t)seed[1] << 8 | (uint32_t)seed[2] << 16 |
		   (uint32_t)seed[3] << 24) & 0x7fffffffu;
	/* Saved before it goes on chain, so a quit cannot strand the story. */
	rc = save_story(seed, j);
	memset(seed, 0, sizeof(seed));
	if (rc) {
		snprintf(j->why, sizeof(j->why), "cannot write %s", STORY_FILE);
		return PDOT_E_INVALID;
	}

	hex32(g.player.account_id, player);
	hex32(j->session.account_id, session);
	snprintf(json, sizeof(json),
		 "[["
		 "{\"Balances\":{\"transfer_keep_alive\":[\"%s\",\"%s\"]}},"
		 "{\"Nfts\":{\"mint\":[%u,%u,\"%s\",null]}},"
		 "{\"Nfts\":{\"approve_item_attributes\":[%u,%u,\"%s\"]}},"
		 "{\"Nfts\":{\"set_attribute\":[%u,%u,\"ItemOwner\",\"story\",\"name=%s block=%u\"]}}"
		 "]]",
		 session, amount,
		 g.collection, j->item, player,
		 g.collection, j->item, session,
		 g.collection, j->item, g.name, j->block);
	if ((rc = pdot_call_from_json(c, "Utility", "batch_all", json, &call, &len)))
		return rc;
	rc = pdot_tx_submit(c, &g.player, call, len, NULL, &j->tx);
	pdot_free(call);
	return rc;
}

static int
work_write(
	pdot_client *c,
	void *ud)
{
	struct job *j = ud;
	char session[67], json[768];
	uint8_t *call;
	size_t len;
	int rc;

	hex32(g.session.account_id, session);
	snprintf(json, sizeof(json), "[%u,%u,{\"Account\":\"%s\"},\"choices\",\"%s\"]",
		 g.collection, g.item, session, j->choices);
	if ((rc = pdot_call_from_json(c, "Nfts", "set_attribute", json, &call, &len)))
		return rc;
	rc = pdot_tx_submit(c, &g.session, call, len, NULL, &j->tx);
	pdot_free(call);
	return rc;
}

/*
 * The ending, in two transactions. The player seals the story: the last
 * choices and the ending go in the session key's attribute namespace,
 * signed by the session key (Nfts.set_attributes_pre_signed) but submitted
 * and paid for by the player, who owns the NFT. That moves the attributes'
 * deposits from the session key to the player. Once that is final, the
 * session key sends everything it holds back to the player and is gone.
 */
static int
work_end(
	pdot_client *c,
	void *ud)
{
	struct job *j = ud;
	char session[67], player[67], zero[131], json[1024];
	uint8_t *call, sig[64];
	size_t len, data_len;
	int rc;

	hex32(g.session.account_id, session);
	hex32(g.player.account_id, player);

	/*
	 * Encoded with a blank signature first: the call is pallet, call, the
	 * data, then MultiSignature::Sr25519 (1 + 64 bytes) and the signer
	 * (32), so the data the session key signs is what lies between.
	 */
	memset(zero, '0', sizeof(zero) - 1);
	zero[0] = '0';
	zero[1] = 'x';
	zero[sizeof(zero) - 1] = '\0';
	snprintf(json, sizeof(json),
		 "[[%u,%u,[[\"choices\",\"%s\"],[\"ending\",\"%s\"]],{\"Account\":\"%s\"},"
		 "4294967295],{\"Sr25519\":\"%s\"},\"%s\"]",
		 g.collection, g.item, j->choices, j->ending, session, zero, session);
	if ((rc = pdot_call_from_json(c, "Nfts", "set_attributes_pre_signed", json, &call, &len)))
		return rc;
	if (len < 2 + 97) {
		pdot_free(call);
		return PDOT_E_INVALID;
	}
	data_len = len - 2 - 97;
	if ((rc = pdot_signer_sign(&g.session, call + 2, data_len, sig))) {
		pdot_free(call);
		return rc;
	}
	memcpy(call + 2 + data_len + 1, sig, 64);
	rc = pdot_tx_submit(c, &g.player, call, len, NULL, &j->seal);
	pdot_free(call);
	if (rc) {
		j->tx = j->seal;  /* the error to report */
		return rc;
	}
	j->sealed = 1;

	snprintf(json, sizeof(json), "[\"%s\",false]", player);
	if ((rc = pdot_call_from_json(c, "Balances", "transfer_all", json, &call, &len)))
		return rc;
	rc = pdot_tx_submit(c, &g.session, call, len, NULL, &j->tx);
	pdot_free(call);
	return rc;
}

/*
 * Whether the saved story can be continued: still the player's, no
 * ending yet, the session key still funded, and the seed block's hash
 * the one saved (proven with a verifier). Its choices come back from the
 * NFT: the chain, not the file, says how far the player got.
 */
static int
work_resume(
	pdot_client *c,
	void *ud)
{
	struct job *j = ud;
	const char *key;
	pdot_nft_item item;
	pdot_nft_attribute a;
	pdot_balance bal;
	pdot_final_block at;
	uint8_t hash[32];
	int rc;

	if ((rc = pdot_nft_item_get(c, g.collection, j->item, &item)))
		return rc;
	if (!item.exists || memcmp(item.owner, g.player.account_id, 32) != 0) {
		snprintf(j->why, sizeof(j->why), "story %u is no longer the player's", j->item);
		return 0;
	}
	key = "ending";
	if ((rc = pdot_nft_attribute_get(c, g.collection, 1, j->item, PDOT_NFT_NS_ACCOUNT,
					 j->session.account_id, (const uint8_t *)key, strlen(key),
					 &a)))
		return rc;
	if (a.exists) {
		j->finished = 1;
		snprintf(j->why, sizeof(j->why), "story %u already has its ending", j->item);
		return 0;
	}
	if ((rc = pdot_balance_get(c, j->session.account_id, &bal)))
		return rc;
	if (!bal.exists) {
		snprintf(j->why, sizeof(j->why), "the session key has no funds left");
		return 0;
	}
	rc = pdot_block_hash_get(c, j->block, hash, &at);
	if (rc == 0 && memcmp(hash, j->block_hash, 32) != 0) {
		snprintf(j->why, sizeof(j->why), "block %u's hash is not the saved one", j->block);
		return 0;
	}
	if (rc && rc != PDOT_E_NOTFOUND)  /* not found: older than the chain keeps */
		return rc;
	/* The name the story was started with: "name=... block=N", the player's. */
	key = "story";
	if ((rc = pdot_nft_attribute_get(c, g.collection, 1, j->item, PDOT_NFT_NS_ITEM_OWNER,
					 NULL, (const uint8_t *)key, strlen(key), &a)))
		return rc;
	if (a.exists && a.len > 5 && memcmp(a.value, "name=", 5) == 0) {
		char v[128];
		char *end;
		snprintf(v, sizeof(v), "%.*s", (int)a.len - 5, (const char *)a.value + 5);
		if ((end = strstr(v, " block=")) != NULL)
			*end = '\0';
		clean(j->name, sizeof(j->name), v, 1);
	}
	key = "choices";
	if ((rc = pdot_nft_attribute_get(c, g.collection, 1, j->item, PDOT_NFT_NS_ACCOUNT,
					 j->session.account_id, (const uint8_t *)key, strlen(key),
					 &a)))
		return rc;
	if (a.exists)
		snprintf(j->choices, sizeof(j->choices), "%.*s", (int)a.len, (const char *)a.value);
	else
		j->choices[0] = '\0';
	j->resume = 1;
	return 0;
}

/* ---- results: run inside s3i_chain_poll, on the game thread ---------- */

static void
fail(
	const char *what,
	const char *error)
{
	char buf[256];

	g.state = ST_FAILED;
	snprintf(buf, sizeof(buf), "error: %s: %s", what, error);
	s3_set_variable_string("chain_status", buf);
	status("%s", buf);
}

/* A job's transaction is in a best block: pending, but the story can go on. */
static void
on_included(
	void *ud,
	uint64_t id,
	void *job_ud,
	const pdot_tx_result *where)
{
	struct job *j = job_ud;

	(void)ud;
	(void)id;
	switch (j->kind) {
	case J_START:
		g.session = j->session;
		g.item = j->item;
		g.block = j->block;
		memcpy(g.block_hash, j->block_hash, 32);
		g.state = ST_OPEN;
		s3_set_variable_int("chain_item", (int)g.item);
		s3_set_variable_int("chain_block", (int)g.block);
		s3_set_variable_int("chain_collection", (int)g.collection);
		s3_set_variable_string("chain_status", "ok");
		set_traits(g.block_hash, g.player.account_id);
		{
			char addr[PDOT_SS58_MAX_LEN];
			pdot_ss58_encode(g.session.account_id, 42, addr, sizeof(addr));
			status("story NFT %u/%u minted in block %u (seeded from block %u); "
			       "session key %s; waiting for finality",
			       g.collection, g.item, where->block, g.block, addr);
		}
		break;
	case J_WRITE:
		status("choices \"%s\" in block %u", j->choices, where->block);
		if (g.saving && strcmp(j->choices, g.save_want) == 0) {
			char buf[160];
			snprintf(buf, sizeof(buf), "Saving on chain: in block %u, waiting for finality...",
				 where->block);
			notice(buf, 0, 0);
		}
		break;
	case J_END:
		{
			char buf[160];
			if (!j->sealed) {
				status("ending \"%s\" sealed by the player in block %u", j->ending,
				       where->block);
				snprintf(buf, sizeof(buf),
					 "Sealing your ending: in block %u, waiting for finality...",
					 where->block);
			} else {
				/* The worker sends the return only once the seal is final. */
				status("ending sealed by the player, final (block %u%s)", j->seal.block,
				       j->seal.verified ? ", proven" : "");
				status("session key's funds returning in block %u", where->block);
				snprintf(buf, sizeof(buf),
					 "Ending sealed; returning the session key's funds "
					 "(block %u)...", where->block);
			}
			notice(buf, 0, 0);
		}
		break;
	}
}

static void queue_write(void);
static struct job *queue(int kind, int (*work)(pdot_client *, void *), const char *ending,
			 uint64_t *id);

/* Fast-forward through what was already played: skip mode, unseen text included. */
static void
replay_begin(void)
{
	g.replaying = 1;
	s3_set_config("msgbox.skip_unseen", "true");
	s3_start_skip_mode();
	s3_show_skipmode_banner(true);
}

static void
replay_end(void)
{
	g.replaying = 0;
	s3_stop_skip_mode();
	s3_show_skipmode_banner(false);
	s3_set_config("msgbox.skip_unseen", "false");
	status("caught up: the story continues from here");
}

/* The next recorded choice into value; 0 when there are none left. */
static int
replay_next(
	char *value,
	size_t cap)
{
	const char *s = g.replay + g.replay_at;
	size_t n;

	if (!*s)
		return 0;
	for (n = 0; s[n] && s[n] != ','; n++)
		;
	snprintf(value, cap, "%.*s", (int)n, s);
	g.replay_at += n + (s[n] == ',');
	return 1;
}

/* ---- the name prompt -------------------------------------------- */

/* One line of text on img. */
static void
draw_line(
	struct s3_image *img,
	const char *text,
	int size,
	int x,
	int y,
	s3_pixel_t color)
{
	struct s3_drawmsg *d;

	d = s3_create_drawmsg(img, text, conf_msgbox_font_select, size, size, 0, 0, x, y,
			      s3_get_image_width(img), s3_get_image_height(img),
			      0, 0, 0, 0, 0, 0, color, s3_make_pixel(255, 0, 0, 0), 0,
			      false, false, true, false, true, false, false, false, true, true,
			      NULL, false);
	if (d) {
		s3_draw_message(d, s3_count_drawmsg_chars(d, NULL));
		s3_destroy_drawmsg(d);
	}
}

/* The prompt in the middle of the screen (text layer 7); NULL name hides it. */
static void
draw_name_prompt(
	const char *name)
{
	struct s3_image *img;
	char line[NAME_MAX + 8];
	const int w = 640, h = 190;

	if (!name) {
		s3_set_layer_image(S3_LAYER_TEXT7, NULL);
		return;
	}
	if (!(img = s3_create_image(w, h)))
		return;
	s3_fill_image_rect(img, 0, 0, w, h, s3_make_pixel(200, 0, 0, 0));
	draw_line(img, "Who is living this summer?", 30, 30, 24, s3_make_pixel(255, 255, 255, 255));
	snprintf(line, sizeof(line), "%s_", name);
	draw_line(img, line, 36, 30, 78, s3_make_pixel(255, 255, 230, 140));
	draw_line(img, "Type your name, then press Return. It goes on your story NFT.", 18, 30,
		  142, s3_make_pixel(255, 190, 190, 190));
	s3_notify_image_update(img);
	s3_set_layer_image(S3_LAYER_TEXT7, img);  /* the layer owns it now */
	s3_set_layer_position(S3_LAYER_TEXT7, (1280 - w) / 2, (720 - h) / 2 - 40);
	s3_set_layer_alpha(S3_LAYER_TEXT7, 255);
}

/* A new story: the player names it first if asked to, else it opens now. */
static void
begin_new(void)
{
	if (g.ask_name) {
		g.state = ST_NAMING;
		g.name[0] = '\0';
		g.name_keys = 3;  /* ignore a Return still held from the title screen */
		draw_name_prompt(g.name);
		status("asking the player's name");
		return;
	}
	if (!queue(J_START, work_start, NULL, NULL)) {
		fail("the story was not opened", "could not queue the transaction");
		return;
	}
	g.state = ST_STARTING;
	status("opening the story: %s signs once", g.name);
}

/*
 * A frame of the prompt. Playfield reports letters and digits for the frame
 * they are pressed in; Return and Backspace while held, so those count when
 * they go down.
 */
static void
name_frame(void)
{
	static const struct { const bool *key; char lower; } keys[] = {
		{ &pf_is_a_key_pressed, 'a' }, { &pf_is_b_key_pressed, 'b' },
		{ &pf_is_c_key_pressed, 'c' }, { &pf_is_d_key_pressed, 'd' },
		{ &pf_is_e_key_pressed, 'e' }, { &pf_is_f_key_pressed, 'f' },
		{ &pf_is_g_key_pressed, 'g' }, { &pf_is_h_key_pressed, 'h' },
		{ &pf_is_i_key_pressed, 'i' }, { &pf_is_j_key_pressed, 'j' },
		{ &pf_is_k_key_pressed, 'k' }, { &pf_is_l_key_pressed, 'l' },
		{ &pf_is_m_key_pressed, 'm' }, { &pf_is_n_key_pressed, 'n' },
		{ &pf_is_o_key_pressed, 'o' }, { &pf_is_p_key_pressed, 'p' },
		{ &pf_is_q_key_pressed, 'q' }, { &pf_is_r_key_pressed, 'r' },
		{ &pf_is_s_key_pressed, 's' }, { &pf_is_t_key_pressed, 't' },
		{ &pf_is_u_key_pressed, 'u' }, { &pf_is_v_key_pressed, 'v' },
		{ &pf_is_w_key_pressed, 'w' }, { &pf_is_x_key_pressed, 'x' },
		{ &pf_is_y_key_pressed, 'y' }, { &pf_is_z_key_pressed, 'z' },
		{ &pf_is_1_key_pressed, '1' }, { &pf_is_2_key_pressed, '2' },
		{ &pf_is_3_key_pressed, '3' }, { &pf_is_4_key_pressed, '4' },
		{ &pf_is_5_key_pressed, '5' }, { &pf_is_6_key_pressed, '6' },
		{ &pf_is_7_key_pressed, '7' }, { &pf_is_8_key_pressed, '8' },
		{ &pf_is_9_key_pressed, '9' }, { &pf_is_0_key_pressed, '0' },
	};
	const size_t max = 16;
	int held = (pf_is_return_key_pressed ? 1 : 0) | (pf_is_backspace_key_pressed ? 2 : 0);
	int down = held & ~g.name_keys;
	size_t n = strlen(g.name), i;
	int changed = 0;

	g.name_keys = held;
	for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		if (*keys[i].key && n < max) {
			char ch = keys[i].lower;
			if (ch >= 'a' && ch <= 'z' && (pf_is_shift_key_pressed || n == 0 ||
						      g.name[n - 1] == ' '))
				ch = (char)(ch - 'a' + 'A');  /* capitalized as typed */
			g.name[n++] = ch;
			g.name[n] = '\0';
			changed = 1;
		}
	}
	if (pf_is_space_key_pressed && n > 0 && n < max && g.name[n - 1] != ' ') {
		g.name[n++] = ' ';
		g.name[n] = '\0';
		changed = 1;
	}
	if ((down & 2) && n > 0) {
		g.name[--n] = '\0';
		changed = 1;
	}
	if (down & 1) {
		while (n > 0 && g.name[n - 1] == ' ')
			g.name[--n] = '\0';
		if (n == 0)
			snprintf(g.name, sizeof(g.name), "Player");
		draw_name_prompt(NULL);
		s3_set_variable_string("player_name", g.name);
		g.ask_name = 0;
		begin_new();
		return;
	}
	if (changed)
		draw_name_prompt(g.name);
}

/* The saved story checks out: carry on with it. */
static void
resume(
	struct job *j)
{
	g.session = j->session;
	g.item = j->item;
	g.block = j->block;
	memcpy(g.block_hash, j->block_hash, 32);
	if (j->name[0]) {
		snprintf(g.name, sizeof(g.name), "%s", j->name);
		s3_set_variable_string("player_name", g.name);
	}
	snprintf(g.choices, sizeof(g.choices), "%s", j->choices);
	{
		/* Ahead of the chain's final state? Then the rest is in flight. */
		char local[CHOICES_MAX + 1];
		unsigned item;
		FILE *f = fopen(CHOICES_FILE, "r");
		size_t n = strlen(j->choices);

		if (f && fscanf(f, "%u %200s", &item, local) == 2 && item == j->item &&
		    strlen(local) > n && strncmp(local, j->choices, n) == 0 &&
		    (n == 0 || local[n] == ',')) {
			status("the chain has \"%s\" so far; this machine recorded \"%s\": "
			       "continuing from that (the rest is still finalizing)", j->choices, local);
			snprintf(g.choices, sizeof(g.choices), "%s", local);
		}
		if (f)
			fclose(f);
	}
	snprintf(g.replay, sizeof(g.replay), "%s", g.choices);
	snprintf(g.final_choices, sizeof(g.final_choices), "%s", j->choices);
	s3_set_variable_string("chain_choices", g.choices);
	g.replay_at = 0;
	g.state = ST_OPEN;
	s3_set_variable_int("chain_item", (int)g.item);
	s3_set_variable_int("chain_block", (int)g.block);
	s3_set_variable_int("chain_collection", (int)g.collection);
	s3_set_variable_int("chain_resumed", 1);
	s3_set_variable_string("chain_status", "ok");
	set_traits(g.block_hash, g.player.account_id);
	status("continuing story %u/%u (seeded from block %u), choices so far: \"%s\"",
	       g.collection, g.item, g.block, g.choices);
	if (g.replay[0])
		replay_begin();
}

static void
job_done(
	void *ud,
	int rc,
	const char *error,
	const void *result)
{
	struct job *j = ud;
	const char *why = j->why[0] ? j->why : rc == PDOT_E_DISPATCH ? j->tx.error : error;

	(void)result;
	switch (j->kind) {
	case J_RESUME:
		if (rc == 0 && j->resume) {
			resume(j);
			break;
		}
		status("not continuing the saved story: %s; starting a new one",
		       rc ? error : j->why);
		if (j->finished) {
			remove(STORY_FILE);
			remove(CHOICES_FILE);
		}
		begin_new();
		break;
	case J_START:
		if (rc)
			fail("the story was not opened", why);
		else
			status("story NFT %u/%u final (block %u%s)", g.collection, g.item,
			       j->tx.block, j->tx.verified ? ", proven" : "");
		break;
	case J_WRITE:
		g.writing = 0;
		if (rc == PDOT_E_CANCELLED)
			break;  /* the ending took over; the write itself still lands */
		if (rc) {
			status("choices not recorded: %s", why);
			if (g.saving) {
				char buf[200];
				g.saving = 0;
				snprintf(buf, sizeof(buf), "Not saved on chain: %s", why);
				save_notice(buf, 1, 1);
			}
		} else {
			status("choices \"%s\" final (block %u%s)", j->choices, j->tx.block,
			       j->tx.verified ? ", proven" : "");
			snprintf(g.final_choices, sizeof(g.final_choices), "%s", j->choices);
			if (g.saving && strcmp(j->choices, g.save_want) == 0) {
				char buf[160];
				g.saving = 0;
				snprintf(buf, sizeof(buf), "Saved on chain: all %u choices, block %u, %s",
					 (unsigned)count_choices(j->choices), j->tx.block,
					 j->tx.verified ? "final and proven" : "final");
				save_notice(buf, 0, 1);
			}
		}
		if (g.dirty && g.state == ST_OPEN && strcmp(g.final_choices, g.choices) != 0)
			queue_write();
		else
			g.dirty = 0;
		break;
	case J_END:
		if (rc && !j->sealed) {
			char buf[200];
			fail("the ending was not recorded", why);
			snprintf(buf, sizeof(buf), "Ending not saved on chain: %s", why);
			notice(buf, 1, 1);
		} else {
			char buf[200];
			g.state = ST_ENDED;
			snprintf(buf, sizeof(buf), "Story sealed on chain: all %u choices and the ending, "
				 "block %u, %s", (unsigned)count_choices(j->choices), j->seal.block,
				 j->seal.verified ? "final and proven" : "final");
			notice(buf, 0, 1);
			if (rc) {
				status("ending sealed by the player, final (block %u%s)", j->seal.block,
				       j->seal.verified ? ", proven" : "");
				char addr[PDOT_SS58_MAX_LEN];
				pdot_ss58_encode(g.session.account_id, 42, addr, sizeof(addr));
				status("the session key's funds were not returned: %s; they stay with "
				       "%s (its seed is in %s)", why, addr, STORY_FILE);
			} else {
				status("session key emptied: everything it held is back with the player "
				       "(block %u%s)", j->tx.block, j->tx.verified ? ", proven" : "");
				remove(STORY_FILE);  /* nothing left to continue */
				remove(CHOICES_FILE);
			}
		}
		break;
	}
	pdot_signer_wipe(&j->session);
	free(j);
}

/* The job carries its own copy of the choices (and ending): the worker never reads g's. */
static struct job *
queue(
	int kind,
	int (*work)(pdot_client *, void *),
	const char *ending,
	uint64_t *id)
{
	struct job *j = calloc(1, sizeof(*j));
	uint64_t got;

	if (!j)
		return NULL;
	j->kind = kind;
	snprintf(j->choices, sizeof(j->choices), "%s", g.choices);
	if (ending)
		clean(j->ending, sizeof(j->ending), ending, 1);
	if (!(got = pdot_async_run(g.a, work, job_done, j))) {
		free(j);
		return NULL;
	}
	if (id)
		*id = got;
	return j;
}

static void
queue_write(void)
{
	g.dirty = 0;
	g.writing = queue(J_WRITE, work_write, NULL, &g.write_id) != NULL;
}

/* ---- setup ----------------------------------------------------------- */

/* The client and the player's key, from the environment. NULL on success. */
static const char *
open_chain(void)
{
	static char why[256];
	const char *rpc = getenv("PDOT_RPC") ? getenv("PDOT_RPC") : "http://127.0.0.1:9944";
	const char *seed_hex = getenv("PDOT_PLAYER_SEED");
	const char *relay = getenv("PDOT_RELAY"), *ck = getenv("PDOT_CHECKPOINT");
	uint8_t seed[32];
	pdot_client *c;

	if (!seed_hex) {
		/* //Alice: a dev account, for local dev chains only */
		seed_hex = "e5be9a5092b81bca64be81d212e7f2f9eba183bb7a90954f7b76361f6edb5c0a";
		status("PDOT_PLAYER_SEED not set: playing as //Alice (dev chains only)");
	}
	if (parse_seed(seed_hex, seed) || pdot_signer_sr25519_from_seed(&g.player, seed))
		return "PDOT_PLAYER_SEED is not 32 bytes of hex";
	memset(seed, 0, sizeof(seed));
	snprintf(g.fund, sizeof(g.fund), "%s", getenv("PDOT_FUND") ? getenv("PDOT_FUND") : "1");

	if (!(c = pdot_client_new(rpc, NULL)))
		return "out of memory";
	if (pdot_client_set_metadata_hash(c, 1, NULL, -1)) {
		snprintf(why, sizeof(why), "metadata hash: %s", pdot_last_error(c));
		pdot_client_free(c);
		return why;
	}
	if (relay && ck) {
		FILE *f = fopen(ck, "rb");
		uint8_t buf[1 << 16];
		size_t n = f ? fread(buf, 1, sizeof(buf), f) : 0;

		if (f)
			fclose(f);
		if (!n || pdot_client_set_verifier(c, relay, 1000, buf, n)) {
			snprintf(why, sizeof(why), "verifier: %s", n ? pdot_last_error(c) : ck);
			pdot_client_free(c);
			return why;
		}
		status("results proven from relay-chain finality (%s)", relay);
	}
	if (!(g.a = pdot_async_new(c)))
		return "out of memory";
	pdot_async_set_included_fn(g.a, on_included, NULL);
	status("Asset Hub at %s", rpc);
	return NULL;
}

/* No chain: the story still plays, with traits from a local seed. */
static void
go_offline(
	const char *why)
{
	uint8_t local[32];
	uint64_t x = (uint64_t)time(NULL);
	int i;

	for (i = 0; i < 32; i++)
		local[i] = (uint8_t)splitmix(&x);
	g.state = ST_FAILED;
	{
		char buf[256];
		snprintf(buf, sizeof(buf), "offline: %s", why);
		s3_set_variable_string("chain_status", buf);
		status("%s", buf);
	}
	set_traits(local, local);
}

/* ---- tags ------------------------------------------------------------ */

static const char *start_props[] = { "collection", "name", "ask_name", "traits", NULL };
static const char *choice_props[] = { "var", NULL };
static const char *ending_props[] = { "name", NULL };

static bool
tag_chain_start(
	void *p)
{
	const char *why;

	(void)p;
	if (!s3_is_in_command_repetition()) {
		if (!s3i_check_tag_properties(start_props))
			return false;
		s3_evaluate_tag();  /* ${var} in arguments */
		if (g.state != ST_NONE)
			return s3_move_to_next_tag();  /* one story per run */
		g.collection = getenv("PDOT_COLLECTION") ?
			(uint32_t)strtoul(getenv("PDOT_COLLECTION"), NULL, 10) :
			(uint32_t)s3_get_tag_arg_int("collection", false, 0);
		clean(g.name, sizeof(g.name), getenv("PDOT_PLAYER_NAME") ? getenv("PDOT_PLAYER_NAME") :
		      s3_get_tag_arg_string("name", true, "Player"), 1);
		if (!g.name[0])
			snprintf(g.name, sizeof(g.name), "Player");
		s3_set_variable_string("player_name", g.name);
		g.ask_name = !getenv("PDOT_PLAYER_NAME") &&
			     strcmp(s3_get_tag_arg_string("ask_name", true, "false"), "true") == 0;
		snprintf(g.traits, sizeof(g.traits), "%s", s3_get_tag_arg_string("traits", true, ""));
		g.choices[0] = '\0';
		if ((why = open_chain()) != NULL) {
			go_offline(why);
			return s3_move_to_next_tag();
		}
		s3_set_variable_int("chain_resumed", 0);
		s3_set_variable_string("chain_status", "starting");
		{
			/* A story left unfinished by an earlier run comes first. */
			struct job probe;
			memset(&probe, 0, sizeof(probe));
			if (load_story(&probe)) {
				/* filled in before the worker can see it */
				struct job *j = calloc(1, sizeof(*j));
				if (j) {
					j->kind = J_RESUME;
					j->session = probe.session;
					j->item = probe.item;
					j->block = probe.block;
					memcpy(j->block_hash, probe.block_hash, 32);
					if (pdot_async_run(g.a, work_resume, job_done, j)) {
						g.state = ST_RESUMING;
						status("found story %u in %s: checking it on chain",
						       probe.item, STORY_FILE);
					} else {
						pdot_signer_wipe(&j->session);
						free(j);
					}
				}
				pdot_signer_wipe(&probe.session);
			}
		}
		if (g.state != ST_RESUMING)
			begin_new();
		s3_start_command_repetition();
	}
	if (g.state == ST_NAMING)
		name_frame();
	if (g.state == ST_NAMING || g.state == ST_STARTING || g.state == ST_RESUMING)
		return true;  /* s3i_chain_poll delivers the result */
	s3_stop_command_repetition();
	if (g.state == ST_FAILED) {
		uint8_t zero[32] = { 0 };
		set_traits(zero, g.player.account_id);  /* still a cast for this player */
	}
	return s3_move_to_next_tag();
}

/* Appends a choice and records it on chain (the next write carries it). */
static void
add_choice(
	const char *raw)
{
	char value[24];
	size_t n;

	clean(value, sizeof(value), raw, 0);
	n = strlen(g.choices);
	if (n + strlen(value) + 2 > sizeof(g.choices)) {
		status("too many choices to record: \"%s\" kept locally only", value);
		return;
	}
	snprintf(g.choices + n, sizeof(g.choices) - n, "%s%s", n ? "," : "", value);
	s3_set_variable_string("chain_choices", g.choices);  /* saved with local saves */
	if (g.state == ST_OPEN) {
		FILE *f = fopen(CHOICES_FILE, "w");
		if (f) {
			fprintf(f, "%u %s\n", g.item, g.choices);
			fclose(f);
		}
		if (g.writing)
			g.dirty = 1;  /* goes out with the next write */
		else
			queue_write();
	}
}

static bool
tag_chain_choice(
	void *p)
{
	(void)p;
	if (!s3i_check_tag_properties(choice_props))
		return false;
	s3_evaluate_tag();
	add_choice(s3_get_variable_string(s3_get_tag_arg_string("var", false, "")));
	return s3_move_to_next_tag();
}

/*
 * [choose] with recording. While continuing a story it takes the choice
 * recorded on chain instead of asking; the rest is [choose]'s own code.
 */
static bool
tag_chain_choose(
	void *p)
{
	char value[24];

	if (!s3_is_in_command_repetition()) {
		s3_evaluate_tag();
		snprintf(g.choose_var, sizeof(g.choose_var), "%s",
			 s3_get_tag_arg_string("name", true, ""));
		if (g.replaying) {
			if (replay_next(value, sizeof(value))) {
				s3_set_variable_string(g.choose_var, value);
				status("replayed choice %s = %s", g.choose_var, value);
				if (!g.replay[g.replay_at])
					replay_end();
				return s3_move_to_next_tag();
			}
			replay_end();
		}
	}
	if (!s3i_tag_choose(p))
		return false;
	/*
	 * [choose] has moved on: the player picked (or there was nothing to
	 * pick). Unless it stopped to open the system menu (save, load...):
	 * then nothing was picked, and the tag runs again when the menu
	 * closes, or a load replaces the story.
	 */
	if (!s3_is_in_command_repetition() && !s3_is_gui_running() && g.choose_var[0]) {
		add_choice(s3_get_variable_string(g.choose_var));
		g.choose_var[0] = '\0';
	}
	return true;
}

static bool
tag_chain_ending(
	void *p)
{
	struct job *j;

	(void)p;
	if (!s3_is_in_command_repetition()) {
		if (!s3i_check_tag_properties(ending_props))
			return false;
		if (g.state != ST_OPEN)
			return s3_move_to_next_tag();  /* offline: nothing to record */
		s3_evaluate_tag();
		/*
		 * The ending carries every choice. A choices write in flight is
		 * waited for (jobs run in order): it is the session key's, the
		 * seal the player's, so nothing else keeps it from landing after
		 * the seal and taking a deposit back.
		 */
		g.dirty = 0;
		if (!(j = queue(J_END, work_end, s3_get_tag_arg_string("name", false, ""), NULL))) {
			fail("the ending was not recorded", "could not queue the transaction");
			return s3_move_to_next_tag();
		}
		g.state = ST_ENDING;
		status("sealing the ending \"%s\": the player signs", j->ending);
		notice("Sealing your ending on chain (your signature)...", 0, 0);
		s3_start_command_repetition();
	}
	if (g.state == ST_ENDING)
		return true;
	s3_stop_command_repetition();
	return s3_move_to_next_tag();
}

bool
s3i_install_chain_tags(void)
{
	return s3_install_tag("Tag_chain_start", tag_chain_start) &&
	       s3_install_tag("Tag_chain_choose", tag_chain_choose) &&
	       s3_install_tag("Tag_chain_choice", tag_chain_choice) &&
	       s3_install_tag("Tag_chain_ending", tag_chain_ending);
}

void
s3i_chain_poll(void)
{
	if (g.a)
		pdot_async_poll(g.a);
	if (g.notice_timed && s3_get_lap_timer_millisec(&g.notice_sw) > 6000)
		notice(NULL, 0, 0);
}

/*
 * Called after the engine's local save: makes sure every choice so far is
 * on chain, and says so on screen once it is final.
 */
void
s3i_chain_save(void)
{
	if (g.state != ST_OPEN) {
		if (g.state != ST_NONE)
			save_notice("Saved locally only: this story is not on chain", 1, 1);
		return;
	}
	snprintf(g.save_want, sizeof(g.save_want), "%s", g.choices);
	if (!g.writing && strcmp(g.final_choices, g.choices) == 0) {
		char buf[160];
		snprintf(buf, sizeof(buf), "Saved on chain: all %u choices were already final",
			 (unsigned)count_choices(g.choices));
		save_notice(buf, 0, 1);
		return;
	}
	g.saving = 1;
	save_notice("Saving your choices on chain...", 0, 0);
	if (!g.writing)
		queue_write();  /* writes everything so far */
	else
		g.dirty = 1;    /* after the write in flight, unless it already carries them */
}

/* Called after the engine's local load: the choices up to that point come back. */
void
s3i_chain_loaded(void)
{
	const char *c = s3_get_variable_string("chain_choices");

	snprintf(g.choices, sizeof(g.choices), "%s", c ? c : "");
	g.replaying = 0;
	if (g.state == ST_OPEN) {
		char buf[160];
		snprintf(buf, sizeof(buf), "Loaded: %u choices so far%s",
			 (unsigned)count_choices(g.choices),
			 strcmp(g.choices, g.final_choices) == 0 ? ", all final on chain" :
			 "; saving records them on chain");
		save_notice(buf, 0, 1);
	}
}
