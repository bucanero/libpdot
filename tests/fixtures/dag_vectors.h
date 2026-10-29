/*
 * Chunked-file manifests from the Bulletin TypeScript SDK's libraries
 * (@ipld/dag-pb 4, ipfs-unixfs 13, multiformats 14), following
 * UnixFsDagBuilder.build. Data byte i is (i * 31 + 7) & 0xff.
 * Regenerate with tools/dag-vectors/gen.mjs. Do not edit.
 */
static const struct {
    const char *name;
    size_t      size, chunk;
    const char *manifest;  /* hex */
    const char *root;
} dag_vectors[] = {
    { "three_small", 1000, 400,
      "122d0a260155a0e40220d0ac996dc9cf9d28a218858854970f81b3134d66231993053d6c6e527e57624c1200189003122d0a260155a0e4022028bee9a878a51e3325e18a560ffa5fad1fa4e9a88001a1f53fe363d3eec98b0a1200189003122d0a260155a0e40220ed3b28311dd6efb4405784067ce20277522f150d2111535fa7df68ea9d823376120018c8010a0e080218e80720900320900320c801",
      "bafykbzaceavh3ic757ve55kdzz3jrhlj635jqkdngppswhuuepa3mo4t5ryle" },
    { "exact_two", 2048, 1024,
      "122d0a260155a0e40220acb61fad7b8eb85172fe559f00c492e1398fc34ab8d0109ac52b150bf3e514be1200188008122d0a260155a0e40220acb61fad7b8eb85172fe559f00c492e1398fc34ab8d0109ac52b150bf3e514be12001880080a0b0802188010208008208008",
      "bafykbzaceccl6asvykaoc4v33zdleu55vp6fvnaz4hcqcbuiukpcrsbu73ik6" },
    { "one_mib_tail", 2109497, 1048576,
      "122e0a260155a0e402206ad33531d50e77e7a08d307d0e1341f9396ba9f91bd6c8c65910b5d7a3e7dcad120018808040122e0a260155a0e402206ad33531d50e77e7a08d307d0e1341f9396ba9f91bd6c8c65910b5d7a3e7dcad120018808040122d0a260155a0e402209e6e97ccfaa9d50a58c682d3abca756161a62f69867781091320dd1969bfc73b120018b9600a12080218b9e08001208080402080804020b960",
      "bafykbzacebo7o3fdezucvhcvuoh26j66uw5nttweudj6lzbvidj2crxisgpsy" },
    { "single", 10, 1048576,
      "122c0a260155a0e402208b02e17653a788e3ccd861c27d55f099d6fde015a1523c77b306dc36e27154281200180a0a060802180a200a",
      "bafykbzaceahknxy6p5xnd5ba5uzaxquzy2ef3lnx3fgbitldncakgtzc6avnw" },
};
