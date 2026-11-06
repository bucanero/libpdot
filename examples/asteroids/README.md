# pdot-asteroids

A small vector space-rocks game that uses libpdot the way a real game would:

- **One signature from the player.** At startup the player's account adds a
  fresh session key as a `NonTransfer` proxy and funds it, in one transaction
  (`pdot_proxy_add`). After that the game signs alone, and the key cannot
  move the player's funds.
- **Scores on chain, without stalling a frame.** At game over the player
  enters 3 initials, arcade style, and the screen shows what recording the
  score would cost (`pdot_tx_fee`). Enter sends
  `System.remark_with_event("pdot-asteroids score=N name=ABC")` from the
  session key, acting for the player. All chain work runs on `pdot_async`, and the HUD
  shows the result: block, fee paid, and whether it was proven.
- **A top-10 leaderboard on the Bulletin chain.** The board is a small JSON
  document (see [`board.h`](board.h)) stored on Bulletin. The trophy
  collection's `board` attribute on Asset Hub holds its CID. The title
  screen reads the CID, fetches the board from Bulletin and checks it
  against the CID, so a node can withhold the board but not change it
  (`pdot_bulletin_fetch`). When a score makes the top 10, the operator
  merges it into the newest board, stores the result
  (`pdot_bulletin_store`) and points the attribute at the new CID. Each
  entry names the Asset Hub block holding its score's remark.
- **Trophies.** A score above the record makes the game's operator account
  mint an NFT to the player and update the collection's `best` attribute
  to `SCORE ADDRESS ABC`, the score, the account and the initials.
  The title screen reads both back: the record holder, and how many trophies
  the player owns (`pdot_nft_attribute_get`, `pdot_async_nft_owned`).
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

All libpdot use is in [`chain.c`](chain.c): one `pdot_async` for Asset Hub,
whose worker also drives a plain client for Bulletin, so a leaderboard
update (read, merge, store, point) runs as one ordered job. [`main.c`](main.c) is the game,
and [`font.c`](font.c) is a 16-segment stroke font, so the only dependency
besides libpdot is SDL2.

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

Once it prints `ready`, start the game in another terminal:

```bash
build-game/examples/asteroids/asteroids
```

Controls: the arrows turn and thrust, space fires. At game over, type your
initials, or change them with up and down and move with left and right, then
press enter to record the score. Escape or closing the window closes the session. After
2 seconds, pressing it again quits at once, and the next run finishes the
cleanup.

The defaults are the well-known dev keys: //Alice is the player and //Bob
the operator. `--player`, `--operator`, `--collection`, `--rpc`,
`--bulletin URL` (or `off`, for no leaderboard), `--fund` and `--session FILE`
change them. `--offline` plays without a chain. `--screenshot FILE` saves
the title screen as a BMP, closes the session and quits.

## Proven results

Pass `--relay URL --checkpoint FILE` (create the file with `pdot-bulletin
checkpoint`) and every result is proven from relay-chain finality:

- the score transaction's block, its place in the block body, and its events;
- the record and trophy reads;
- the head and balance watches.

The HUD then says "proven" instead of "trusted RPC". The local dev node has
no relay chain, so it always shows "trusted RPC".

## Not covered yet

- **The operator key lives in the game.** That's fine on a dev chain. A real
  game would mint trophies from a server, or let the player mint through an
  approval from the operator.
- **Scores aren't checked.** Anyone can send a remark with any score. Anti-cheat
  (replays, server checks) is out of scope.
- **The board expires.** Bulletin keeps data for a retention period, so a
  board that doesn't change for that long needs renewing
  (`pdot_bulletin_renew`). The game doesn't do that yet.
- **Concurrent updates.** Each update merges into the newest board, but two
  updates at the same moment can still overwrite each other. A single
  operator service would apply them one at a time.
