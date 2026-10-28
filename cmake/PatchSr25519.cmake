# Copies sr25519-donna's sources into the build tree and applies the fixes
# libpdot needs, leaving the submodule untouched. Sets SR25519_DIR.
# Each patch fails the configure step if upstream changes under it.
#
# 1. curve25519_expand reads uint32_t/uint64_t through casts of unaligned
#    byte pointers on little-endian hosts. That is undefined behaviour,
#    trips UBSan and can raise SIGBUS on strict-alignment CPUs. Replaced
#    with donna's own byte-wise U8TO32_LE / U8TO64_LE loads.
# 2. derive_scalar_and_chaincode passes bignum256modm* where the element
#    array is expected. Same address, but GCC 14 rejects the types.
# 3. elligator_ristretto_flavor (VRF only) tests the sign of a field element
#    on its unreduced limbs instead of its canonical encoding.
# 4. elligator_ristretto_flavor (VRF only) does a 96-byte conditional move
#    into a 40-byte field element: a stack overflow.
# 3 and 4 follow curve25519-dalek's elligator_ristretto_flavor. libpdot
# never calls the VRF, so these are not covered by its tests.
# 5. Merlin's Keccak-f runs on native uint64_t lanes aliased over the byte
#    state. Lanes are now loaded and stored little-endian around the
#    permutation, so big-endian hosts compute the same transcript.
# 6. Merlin encodes lengths as the first 4 bytes of a native uint64_t, which
#    are the high bytes on big-endian hosts. Now an explicit LE u32.
# Without 5 and 6, sr25519 signatures made or checked on big-endian hosts
# are incompatible with schnorrkel.

set(_sr_src ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sr25519-donna/src)
set(SR25519_DIR ${CMAKE_CURRENT_BINARY_DIR}/sr25519-donna)

if(NOT EXISTS ${_sr_src}/sr25519.c)
  message(FATAL_ERROR "sr25519-donna missing. Run: git submodule update --init")
endif()

file(REMOVE_RECURSE ${SR25519_DIR})
file(COPY ${_sr_src}/ DESTINATION ${SR25519_DIR})

function(_sr_patch id file mode old new)
  set(_f ${SR25519_DIR}/${file})
  file(READ ${_f} _txt)
  if(mode STREQUAL REGEX)
    string(REGEX REPLACE "${old}" "${new}" _patched "${_txt}")
  else()
    string(REPLACE "${old}" "${new}" _patched "${_txt}")
  endif()
  if(_patched STREQUAL _txt)
    message(FATAL_ERROR "sr25519-donna patch ${id} no longer applies to ${file}")
  endif()
  file(WRITE ${_f} "${_patched}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_sr_src}/${file})
endfunction()

foreach(_h curve25519-donna-64bit.h curve25519-donna-32bit.h)
  _sr_patch(1 ed25519-donna/${_h} REGEX
    "\\*\\(uint(32|64)_t \\*\\)\\(in \\+ ([0-9]+)\\)"
    "U8TO\\1_LE(in + \\2)")
endforeach()

_sr_patch(2 sr25519.c PLAIN
  "expand256_modm(scalar, buf, 64);"
  "expand256_modm(*scalar, buf, 64);")

_sr_patch(3 ristretto255.c PLAIN
  "int8_t s_prime_is_pos = !bignum25519_is_negative(s_prime);"
  "unsigned char s_prime_bytes[32];
    curve25519_contract(s_prime_bytes, s_prime);
    int8_t s_prime_is_pos = !bignum25519_is_negative(s_prime_bytes);")

_sr_patch(4 ristretto255.c PLAIN
  "curve25519_move_conditional_bytes(c, r, !Ns_D_is_sq);"
  "bignum25519 r_copy;
    curve25519_copy(r_copy, r);
    curve25519_swap_conditional(c, r_copy, !Ns_D_is_sq);")

_sr_patch(5 merlin.c PLAIN
  "static /*inline*/ void keccakf(void* state) {
    uint64_t* a = (uint64_t*)state;"
  "static void keccakf_lanes(uint64_t* a);

static void keccakf(void* state) {
    uint8_t* s = (uint8_t*)state;
    uint64_t lanes[25];
    int j, k;

    for (j = 0; j < 25; j++) {
        lanes[j] = 0;
        for (k = 0; k < 8; k++)
            lanes[j] |= (uint64_t)s[8 * j + k] << (8 * k);
    }
    keccakf_lanes(lanes);
    for (j = 0; j < 25; j++)
        for (k = 0; k < 8; k++)
            s[8 * j + k] = (uint8_t)(lanes[j] >> (8 * k));
}

static void keccakf_lanes(uint64_t* a) {")

_sr_patch(6 merlin.c REGEX
  "uint64_t ([a-z_]+_len_bytes) = ([a-z_]+);"
  "uint8_t \\1[4] = { (uint8_t)(\\2), (uint8_t)((\\2) >> 8), (uint8_t)((\\2) >> 16), (uint8_t)((\\2) >> 24) };")
_sr_patch(6 merlin.c REGEX
  "\\(uint8_t\\*\\)&([a-z_]+_len_bytes)"
  "\\1")
