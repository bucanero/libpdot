# pdot-asteroids

A small vector space-rocks game that uses libpdot the way a real game would,
plus `asteroids-operator`, the game's server. The game holds the player's
keys; the operator holds the operator key, re-runs every claimed game from
its replay and checks the score on chain before it acts on it.

- **One signature from the player.** At startup the player's account adds a
  fresh session key as a `NonTransfer` proxy and funds it, in one transaction
  (`pdot_proxy_add`). After that the game signs alone, and the key cannot
  move the player's funds.
- **Replays.** The game is a deterministic simulation ([`sim.c`](sim.c)):
  fixed 60 Hz ticks, a seeded integer RNG and only IEEE-exact float
  operations, so the same seed and inputs give the same game on any build.
  Every tick's input is recorded; a game's replay ([`replay.h`](replay.h))
  is the seed plus run-length-coded inputs, about 1 KB a minute.
- **Scores on chain, without stalling a frame.** At game over the player
  enters 3 initials, arcade style, and the screen shows what recording the
  score would cost (`pdot_tx_fee`). Enter sends
  `System.remark_with_event("pdot-asteroids score=N name=ABC replay=CID")`
  from the session key, acting for the player. The CID is the replay's
  (`pdot_cid_compute`), so the score commits to the game that made it. All chain work runs on `pdot_async`, and the HUD
  shows the result: block, fee paid, and whether it was proven.
- **A top-10 leaderboard on the Bulletin chain.** The board is a small JSON
  document (see [`board.h`](board.h)) stored on Bulletin. The trophy
  collection's `board` attribute on Asset Hub holds its CID. The title
  screen reads the CID, fetches the board from Bulletin and checks it
  against the CID, so a node can withhold the board but not change it
  (`pdot_bulletin_fetch`). Each entry names the Asset Hub block holding its
  score's remark, so anyone can check it.
- **Claims: only games really played.** When a score could make the top 10
  or beat the record, the game sends the operator a claim: account, score,
  initials, the block holding the remark, and the replay. The operator
  re-runs the replay with the same `sim.c` and requires a finished game
  with exactly that score. Then it finds the remark in that block, from that
  account, naming that replay's CID (`pdot_remark_find`; through a proxy the
  event names the player, not the session key). Anything else is refused.
  It then stores the replay on Bulletin, merges the score into the newest
  board (each entry names its replay) and stores that too
  (`pdot_bulletin_store`), and for a new record mints a trophy NFT whose
  metadata names the replay. The mint, its metadata and the collection's
  `board` and `best` attributes go in one `Utility.batch_all`, all or
  nothing. Each remark is honoured once.
- **Anyone can check the board.** `asteroids-operator --audit` needs no key:
  it fetches every entry's replay from Bulletin (checked against its CID),
  re-runs it to the score, and shows when each item expires
  (`pdot_bulletin_stored`).
- **Watch any score being made.** On the title screen, a row's number (0
  for the tenth) fetches that entry's replay from Bulletin and plays it
  back with the same `sim.c`; holding shift plays it four times as fast.
  The end shows whether it reaches the board's score. Rows from before
  replays are dimmed.
- **What the board names stays stored.** Bulletin keeps data for a
  retention period. Every few minutes the operator checks the board and
  every replay on it (`pdot_bulletin_stored`) and renews those within a
  quarter of the period of expiring (`pdot_bulletin_renew`).
- **Trophies.** `best` holds `SCORE ADDRESS ABC`, the score, the account and
  the initials. The title screen reads it and the board back from the
  chain, not from the operator's answer, along with how many trophies the
  player owns (`pdot_nft_attribute_get`, `pdot_async_nft_owned`).
- **Live state.** Watches keep the newest final block and the session key's
  balance in the HUD (`pdot_async_watch_head`, `pdot_async_watch_balance`).
- **Closing.** Escape sends the session key's leftover funds back to the
  player (`Balances.transfer_all`), then the player removes the proxy.
- **Sessions survive restarts.** The session key's seed is saved in
  `asteroids-session.key` (readable by your user only) before the key goes
  on chain. If the game quits without closing, the next run resumes the
  session with no signature. If it was left half closed (funds returned but
  proxy still there, or the reverse), the next run cleans it up first. A
  clean close deletes the file.

The game's libpdot use is in [`chain.c`](chain.c): one `pdot_async` for
Asset Hub, whose worker also drives a plain client for Bulletin and posts
claims with libcurl. [`main.c`](main.c) is the game, and [`font.c`](font.c)
is a 16-segment stroke font, so the only dependency besides libpdot and its
own libraries is SDL2. [`sim.c`](sim.c) and [`replay.c`](replay.c) are the
game and its replays, and [`board.c`](board.c) the leaderboard document and
the board and record reads: the game and the operator share all three.
[`operator.c`](operator.c) is the operator: blocking libpdot calls behind a
small HTTP server. [`replay_test.c`](replay_test.c) plays scripted games and
checks that replays re-run to their scores and that edited ones don't.

## The operator

```
GET  /health  {"ok": true, "operator": "5FHn...", "collection": 0, "proven": false}
POST /claim   {"account": "5Grw...", "score": 4190, "name": "DAM", "block": 342,
               "replay": "50444152..."}                               (hex)
           -> {"ok": true, "ticks": 5969, "replay": "bafk...", "score_proven": false,
               "rank": 1, "board": "bafk...", "stored_at": 51, "record": true,
               "trophy": 3, "block": 344, ...}
           or {"ok": false, "error": "the replay scores 3800, not 9999"}
              (400 bad request, 422 replay, 403 not on chain, 409 claimed or not final, 502)
```

It handles one claim at a time, in arrival order, so two games can't
overwrite each other's board update. Honoured claims (block and extrinsic
index) are kept in `asteroids-claims.txt`. A claim that fails halfway can be
sent again: a score already on the board is not added twice, and the
Asset Hub changes are all or nothing. A claim needs only the block number,
so a game that couldn't reach the operator can claim later, for example
with curl.

## Run it on a local dev chain (macOS)

```bash
brew install sdl2
cmake -S . -B build-game -DPDOT_BUILD_ASTEROIDS=ON \
  -DCMAKE_PREFIX_PATH="$(brew --prefix mbedtls@3);$(brew --prefix cjson);$(brew --prefix sdl2)"
cmake --build build-game -j
```

In one terminal, start the local dev chains. The script builds an Asset Hub
node (port 9944) and a Bulletin node (port 9945) from Westend's runtimes.
//Bob creates trophy collection 0 on Asset Hub, and //Eve authorizes //Bob
to store on Bulletin:

```bash
OMNI_NODE=/path/to/polkadot-omni-node examples/asteroids/devnet.sh
```

Once it prints `ready`, start the operator in a second terminal and the
game in a third:

```bash
build-game/examples/asteroids/asteroids-operator
```

```bash
build-game/examples/asteroids/asteroids
```

Controls: the arrows turn and thrust, space fires. At game over, type your
initials, or change them with up and down and move with left and right, then
press enter to record the score. Escape or closing the window closes the session. After
2 seconds, pressing it again quits at once, and the next run finishes the
cleanup.

The defaults are the well-known dev keys: //Alice is the player, and the
operator signs as //Bob. In the game, `--player`, `--collection`, `--rpc`,
`--bulletin URL` (or `off`, for no leaderboard), `--operator URL` (or `off`,
to only record scores), `--fund` and `--session FILE` change them.
`--offline` plays without a chain. `--screenshot FILE` saves the title
screen as a BMP, closes the session and quits. The operator takes `--rpc`,
`--bulletin`, `--operator SEED`, `--collection`, `--listen ADDR:PORT`
(127.0.0.1:8090), `--claims FILE`, `--renew-check SECONDS` (300) and
`--renew-before BLOCKS` (a quarter of the retention period).

## Proven results

Pass `--relay URL --checkpoint FILE` (create the file with `pdot-bulletin
checkpoint`) and every result is proven from relay-chain finality:

- in the game: the session key and score transactions (block, place in the
  block body, events), the record, board attribute and trophy reads, and
  the head and balance watches. The HUD then says "proven" instead of
  "trusted RPC".
- in the operator: the claimed remark (the block's events, through a read
  proof), the board and record reads, the replay and board stores on
  Bulletin, its Asset Hub batch, and the retention reads and renewals. It
  keeps its checkpoint file current as it goes.

`devnet.sh` has no relay chain, so it always shows "trusted RPC". For proven
mode, [`devnet-relay.sh`](devnet-relay.sh) runs a westend-local relay chain
(validators //Alice and //Bob) with Asset Hub as parachain 1000 and Bulletin
as parachain 2487, on the same ports, does the same setup with proven
results, and writes a checkpoint:

```bash
VALIDATOR_EXTRA_ARGS=--insecure-validator-i-know-what-i-do \
POLKADOT=/path/to/polkadot OMNI_NODE=/path/to/polkadot-omni-node \
  examples/asteroids/devnet-relay.sh
```

Validators want Linux's secure-validator sandbox; on macOS pass the opt-out
above, for these throwaway local validators only. Once it prints `ready`,
it shows the operator and game commands, each with `--relay` and its own
copy of the checkpoint.

## Not covered yet

- **Replays prove a game was played, not who played it.** A bot, or a
  tool-assisted run built offline, makes a valid replay too. The game also
  picks its own seed, so a player could look for an easy one; deriving the
  seed from a recent block hash would stop that.
- **Determinism has rules.** `sim.c` must stay free of libm transcendental
  functions and fused multiply-add (`-ffp-contract=off`, set in CMake), and
  needs SSE-style floats (it refuses to build with x87's excess precision).
  `replay_test --digest` prints a digest of each scripted game's final
  state, to compare across builds: arm64 at -O0 with sanitizers, arm64 at
  -O3 and x86_64 match. Changing `sim.c` changes the game: old replays then
  no longer re-run, so a real game would version it (the replay header has
  a version byte for that).
- **The operator is a sample server.** Plain HTTP, no authentication, rate
  limits or TLS, and a claim blocks the next one for a few blocks. It
  listens on loopback by default; put it behind a proper front end before
  exposing it.
- **Renewal needs the operator running.** If it is down for longer than
  the margin, items can expire; the audit shows each one's expiry. Boards
  replaced by newer ones and replays that fell off the board are left to
  expire.
- **Events from older runtimes.** The operator decodes a claimed block's
  events with the current metadata, so claims for blocks from before a
  runtime upgrade that changed events may fail.
