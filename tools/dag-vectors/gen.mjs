// Mirrors sdk/typescript FixedSizeChunker + UnixFsDagBuilder.build + calculateCid,
// using the same libraries, to produce manifest vectors for libpdot.
import * as dagPB from "@ipld/dag-pb"
import { UnixFS } from "ipfs-unixfs"
import { CID } from "multiformats/cid"
import * as digest from "multiformats/hashes/digest"
import { blake2b } from "@noble/hashes/blake2.js"

const cid = (data, codec) => CID.createV1(codec, digest.create(0xb220, blake2b(data, { dkLen: 32 })))
const pattern = (n) => Uint8Array.from({ length: n }, (_, i) => (i * 31 + 7) & 0xff)

const cases = [
  { name: "three_small", size: 1000, chunk: 400 },
  { name: "exact_two", size: 2048, chunk: 1024 },
  { name: "one_mib_tail", size: (2 << 20) + 12345, chunk: 1 << 20 },
  { name: "single", size: 10, chunk: 1 << 20 },
]
const out = []
for (const c of cases) {
  const data = pattern(c.size)
  const chunks = []
  for (let off = 0; off < data.length; off += c.chunk) chunks.push(data.subarray(off, Math.min(off + c.chunk, data.length)))
  const cids = chunks.map((d) => cid(d, 0x55))
  const node = dagPB.prepare({
    Data: new UnixFS({ type: "file", blockSizes: chunks.map((d) => BigInt(d.length)) }).marshal(),
    Links: chunks.map((d, i) => ({ Name: "", Tsize: d.length, Hash: cids[i] })),
  })
  const bytes = dagPB.encode(node)
  out.push({ ...c, manifest: Buffer.from(bytes).toString("hex"), root: cid(bytes, 0x70).toString() })
}
console.log(JSON.stringify(out, null, 1))
