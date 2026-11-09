# The Last Week of Summer

A short visual novel in which every playthrough is a story recorded on
Polkadot Asset Hub, built on the [Suika3](https://github.com/awemorris/suika3)
engine with libpdot. There is no game server and no operator: the player's
account and the chain are all there is.

- **One signature, then the game signs alone.** Starting a story, the player
  signs one `Utility.batch_all`: it funds a fresh session key, mints the
  story NFT to the player in the game's collection (public minting), approves
  the session key to write attributes on it, and records the player's name
  and the seed block as the player's own attribute.
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
- **The ending, confirmed.** The last choices, the ending and the return of
  the session key's funds go in one transaction. The game waits until it is
  final and says so on screen ("Story saved on chain: all 4 choices and the
  ending, block B, final") before the closing scene.
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
playable game ends up in `build/game`. A `linux` preset is there too, but has
not been tried.

## Playing

The game needs an Asset Hub node and a collection that anyone can mint into.
Start libpdot's development network: `examples/asteroids/devnet.sh` (Asset
Hub alone) or `devnet-relay.sh` (with a relay chain, for proven results). Both
create the asteroids trophy collection 0. Then create the story collection,
which will be 1, as //Alice:

```bash
build/pdot-bulletin --rpc http://127.0.0.1:9944 --seed e5be9a5092b81bca64be81d212e7f2f9eba183bb7a90954f7b76361f6edb5c0a call Nfts create '["5GrwvaEF5zXb26Fz9rcQpDWS57CtERHpNehXCPcNoHGKutQY", {"settings": 0, "max_supply": null, "mint_settings": {"mint_type": "Public", "price": null, "start_block": null, "end_block": null, "default_item_settings": 0}}]'
```

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

Each story leaves about 0.02 WND reserved in its session key: the deposits
for the attributes it wrote (pallet-nfts charges the account that writes in
its own namespace).

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
