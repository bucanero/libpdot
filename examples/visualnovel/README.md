# The Last Week of Summer

A short visual novel in which every playthrough is a story recorded on
Polkadot Asset Hub, built on the [Suika3](https://github.com/awemorris/suika3)
engine with libpdot. There is no game server and no operator: the player's
account and the chain are all there is.

- **Two signatures: one to start, one to seal.** Starting a story, the
  player signs one `Utility.batch_all`: it funds a fresh session key, mints
  the story NFT to the player in the game's collection (public minting),
  approves the session key to write attributes on it, and records the
  player's name and the seed block as the player's own attribute. In
  between, the game signs alone.
- **Every summer is different.** The hash of the newest final block when the
  story starts, together with the player's account, sets three character
  traits from 0 to 100: how bold Xiaoling is, how much Midori trusts you, how
  patient Xiaoling is. The same answer can delight one player's Xiaoling and
  hurt another's, so friends who choose alike still get different endings.
- **Every choice on chain, without stalling a frame.** Each choice is written
  to the NFT from the session key (`set_attribute`, in the session key's
  attribute namespace) through `pdot_async`. A write shows up within seconds
  when it is in a block (`pdot_async_set_included_fn`), and again once
  final.
- **The player seals the ending, and nothing is left behind.** pallet-nfts
  takes an attribute's deposit from whoever pays for writing it, so the
  session key's choices hold about 0.01 WND each. At the end the session
  key signs the final choices and ending for its namespace, and the player
  submits them (`Nfts.set_attributes_pre_signed`): the player, who owns the
  NFT, now holds the deposits, and the session key's are released. Once
  that is final, the session key sends everything back (`transfer_all`) and
  its account is gone. The game waits for both and says so on screen
  ("Story sealed on chain: all 4 choices and the ending, block B, final")
  before the closing scene.
- **Save and Load.** The engine's own save also makes sure every choice so
  far is on the NFT, and confirms it once final ("Saved on chain: all N
  choices, block B, final"). Load restores the local save, and the choices
  up to that point come back with it. Both also print a `[chain]` line.
- **Quit any time.** The next run checks the unfinished story on chain:
  still the player's, not ended, the session key funded, the seed block's
  hash unchanged (`pdot_block_hash_get`). Then it replays the recorded
  choices in skip mode and carries on from where the player left off.

## Layout

```
suika3/              Suika3, a submodule pinned to the commit this was tested on
patches/suika3.patch the engine hooks the chain needs (applied by the build)
src/chain.c          the chain tags: everything that uses libpdot
game/                the story (start.novel) and its title; the rest of the
                     game folder is Suika3's own sample assets
CMakeLists.txt       applies the patch, builds libpdot and the engine,
                     assembles a playable game folder
```

The patch is small: Suika3 calls `s3i_chain_poll()` once a frame, installs
the chain tags at startup, and calls `s3i_chain_save()` / `s3i_chain_loaded()`
after its own save and load. It also stops the engine from forcing a
universal macOS binary, which Homebrew's single-architecture libraries
cannot link into.

The story uses four tags from [`src/chain.c`](src/chain.c); its header
comment documents them in full:

```
[chain_start collection="1" name="Player" traits="xiao_bold mido_trust xiao_patience"]
[chain_choose name="c1" text1="Sure, let's go!" value1="yes" text2="Not today." value2="no"]
[chain_choice var="result"]
[chain_ending name="${ending_name}"]
```

## Building

Suika3 is not fetched with libpdot's other submodules (it is about 200 MB with
its own submodules). From libpdot's root:

```bash
git submodule update --init --checkout --recursive examples/visualnovel/suika3
```

Then, on macOS (`brew install cmake ninja mbedtls@3 cjson`; curl ships with
macOS):

```bash
cd examples/visualnovel
cmake --preset macos
cmake --build --preset macos
```

The first configure applies `patches/suika3.patch` to the submodule; libpdot's
`git status` does not show the change (`ignore = dirty` in `.gitmodules`). The
playable game ends up in `build/game`. On Linux, use the `linux` preset; CI
builds it ([`ci/visualnovel.sh`](../../ci/visualnovel.sh) lists the Debian
packages), but nobody has played it there yet.

## Playing

The game needs an Asset Hub node and a collection that anyone can mint into.
libpdot's development networks set up both: `examples/asteroids/devnet.sh`
(Asset Hub and Bulletin) or `examples/asteroids/devnet-relay.sh` (under a
relay chain, for proven results). After the asteroids trophy collection 0,
they create the story collection 1, owned by //Alice with public minting.
`devnet-relay.sh` also writes a checkpoint and prints the game's command
with `PDOT_RELAY` and `PDOT_CHECKPOINT` set.

Run the game from inside its folder (it keeps saves and the unfinished story
there):

```bash
cd examples/visualnovel/build/game && ./Suika3.app/Contents/MacOS/Suika3
```

Settings come from the environment:

| Variable | Meaning | Default |
|---|---|---|
| `PDOT_RPC` | Asset Hub RPC | `http://127.0.0.1:9944` |
| `PDOT_PLAYER_SEED` | the player's sr25519 seed, hex | //Alice (dev chains only) |
| `PDOT_PLAYER_NAME` | the name on the story NFT | the tag's `name=` |
| `PDOT_COLLECTION` | the story collection | the tag's `collection=` (1) |
| `PDOT_FUND` | tokens sent to the session key | 1 |
| `PDOT_RELAY`, `PDOT_CHECKPOINT` | prove results from relay-chain finality | off |

Without a chain the story still plays, with a local seed, and says so.

## Updating Suika3

Move the submodule to a newer commit and refresh the patch:

```bash
cd examples/visualnovel/suika3
git checkout -- . && git fetch && git checkout NEW_COMMIT && git submodule update --recursive
git apply --3way ../patches/suika3.patch
git diff > ../patches/suika3.patch
cd .. && git -C ../.. add examples/visualnovel/suika3 examples/visualnovel/patches
```

## License

Suika3 is under the zlib license, © Awe Morris / SCHOLA SUIKAE; the patch and
`game/` are changes to it, and the game folder the build assembles is
Suika3's sample assets with this story added. `src/chain.c` is part of the
libpdot sample.
