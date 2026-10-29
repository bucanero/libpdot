Regenerates `tests/fixtures/dag_vectors.h` with the JavaScript libraries the
Bulletin TypeScript SDK uses, in a throwaway container:

```sh
docker run --rm -v "$PWD/tools/dag-vectors":/w -w /w node:22-slim sh -c \
  'npm init -y >/dev/null && npm install --silent @ipld/dag-pb@4 ipfs-unixfs@13 multiformats@14 @noble/hashes@2 && node gen.mjs' > vectors.json
```

then convert `vectors.json` into the header's table.
