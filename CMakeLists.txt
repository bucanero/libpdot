cmake_minimum_required(VERSION 3.16)
project(libpdot VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(PDOT_BUILD_TESTS  "Build unit tests" ON)
option(PDOT_SANITIZE     "Build with ASan and UBSan" OFF)
option(PDOT_FUZZ         "Build libFuzzer targets (clang)" OFF)
option(PDOT_WERROR       "Treat warnings in libpdot's own code as errors" OFF)
option(PDOT_WITH_SR25519 "Build the sr25519 signer (sr25519-donna)" ON)
option(PDOT_WITH_ECDSA   "Build the ECDSA secp256k1 signer (mbedTLS)" ON)
option(PDOT_SR25519_FORCE_32BIT "Use sr25519-donna's 32-bit field arithmetic" OFF)

# mbedTLS 3.6 LTS. On macOS: -DCMAKE_PREFIX_PATH=$(brew --prefix mbedtls@3)
find_package(MbedTLS 3.6 REQUIRED CONFIG)
if(MbedTLS_VERSION VERSION_GREATER_EQUAL 4)
  message(FATAL_ERROR "libpdot needs mbedTLS 3.6 LTS, found ${MbedTLS_VERSION}")
endif()

find_package(CURL 7.68 REQUIRED)

# cJSON ships a CMake package on Homebrew; fall back to a plain search.
find_package(cJSON CONFIG QUIET)
if(TARGET cjson)
  set(PDOT_CJSON cjson)
else()
  find_path(CJSON_INCLUDE_DIR cjson/cJSON.h REQUIRED)
  find_library(CJSON_LIBRARY cjson REQUIRED)
  add_library(pdot_cjson INTERFACE)
  target_include_directories(pdot_cjson INTERFACE ${CJSON_INCLUDE_DIR})
  target_link_libraries(pdot_cjson INTERFACE ${CJSON_LIBRARY})
  set(PDOT_CJSON pdot_cjson)
endif()

if(MSVC)
  set(PDOT_WARNINGS /W4)
else()
  set(PDOT_WARNINGS -Wall -Wextra -pedantic -Wshadow -Wstrict-prototypes)
endif()
if(PDOT_WERROR)
  list(APPEND PDOT_WARNINGS $<IF:$<C_COMPILER_ID:MSVC>,/WX,-Werror>)
endif()

if(PDOT_SANITIZE AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                      -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
endif()

# Vendored third-party code builds without our warning flags.
add_library(pdot_blake2 OBJECT third_party/blake2/blake2b-ref.c)
target_include_directories(pdot_blake2 PUBLIC third_party/blake2)
set_target_properties(pdot_blake2 PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(PDOT_WITH_SR25519)
  include(cmake/PatchSr25519.cmake)  # sets SR25519_DIR
  add_library(pdot_sr25519 OBJECT
    ${SR25519_DIR}/sr25519.c
    ${SR25519_DIR}/merlin.c
    ${SR25519_DIR}/ristretto255.c
    ${SR25519_DIR}/memzero.c
    ${SR25519_DIR}/vrf.c)
  target_include_directories(pdot_sr25519 PUBLIC ${SR25519_DIR})
  # Hash and RNG hooks come from our header, force-included, so the
  # submodule's placeholder hook headers stay untouched.
  target_compile_definitions(pdot_sr25519 PRIVATE
    SR25519_CUSTOMHASH SR25519_CUSTOMRANDOM
    ED25519_CUSTOMHASH ED25519_CUSTOMRANDOM ED25519_NO_INLINE_ASM
    $<$<BOOL:${PDOT_SR25519_FORCE_32BIT}>:ED25519_FORCE_32BIT>)
  if(MSVC)
    target_compile_options(pdot_sr25519 PRIVATE
      /FI${CMAKE_CURRENT_SOURCE_DIR}/src/sr25519_hooks.h)
  else()
    target_compile_options(pdot_sr25519 PRIVATE
      -include ${CMAKE_CURRENT_SOURCE_DIR}/src/sr25519_hooks.h)
    # Upstream uses 0b literals, a GNU extension.
    set_target_properties(pdot_sr25519 PROPERTIES C_EXTENSIONS ON)
  endif()
  target_link_libraries(pdot_sr25519 PRIVATE MbedTLS::mbedcrypto)
  set_target_properties(pdot_sr25519 PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

add_library(pdot
  src/error.c
  src/scale.c
  src/hash.c
  src/cid.c
  src/ss58.c
  src/client.c
  src/rng.c
  src/signer.c
  src/hex.c
  src/http.c
  src/rpc.c
  src/metadata.c
  src/decode.c
  src/events.c
  src/dag.c
  src/tx.c
  src/bulletin.c
  $<TARGET_OBJECTS:pdot_blake2>)
target_link_libraries(pdot PUBLIC MbedTLS::mbedcrypto PRIVATE CURL::libcurl ${PDOT_CJSON})
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(pdot PRIVATE Threads::Threads)
endif()
if(PDOT_WITH_SR25519)
  target_sources(pdot PRIVATE src/signer_sr25519.c $<TARGET_OBJECTS:pdot_sr25519>)
  target_include_directories(pdot SYSTEM PRIVATE ${SR25519_DIR})
  target_compile_definitions(pdot PUBLIC PDOT_WITH_SR25519)
endif()
if(PDOT_WITH_ECDSA)
  target_sources(pdot PRIVATE src/signer_ecdsa.c)
  target_compile_definitions(pdot PUBLIC PDOT_WITH_ECDSA)
endif()
target_include_directories(pdot
  PUBLIC  include
  PRIVATE src)
target_include_directories(pdot SYSTEM PRIVATE third_party/blake2 third_party/xxhash)
target_compile_options(pdot PRIVATE ${PDOT_WARNINGS})

if(PDOT_WITH_SR25519 AND PDOT_WITH_ECDSA)
  add_executable(pdot-bulletin tools/pdot-bulletin.c)
  target_link_libraries(pdot-bulletin PRIVATE pdot)
  target_compile_options(pdot-bulletin PRIVATE ${PDOT_WARNINGS})
endif()

if(PDOT_FUZZ)
  foreach(f metadata decode)
    add_executable(fuzz_${f} fuzz/fuzz_${f}.c)
    target_link_libraries(fuzz_${f} PRIVATE pdot)
    target_include_directories(fuzz_${f} PRIVATE src third_party/blake2)
    target_compile_options(fuzz_${f} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_${f} PRIVATE -fsanitize=fuzzer,address,undefined)
  endforeach()
endif()

if(PDOT_BUILD_TESTS)
  enable_testing()
  set(PDOT_TESTS scale hash cid ss58 metadata events dag)
  if(PDOT_WITH_SR25519 AND PDOT_WITH_ECDSA)
    list(APPEND PDOT_TESTS signer tx)
  endif()
  foreach(t ${PDOT_TESTS})
    add_executable(test_${t} tests/test_${t}.c)
    target_link_libraries(test_${t} PRIVATE pdot)
    target_include_directories(test_${t} PRIVATE src tests third_party/blake2)
    target_compile_options(test_${t} PRIVATE ${PDOT_WARNINGS})
    target_link_libraries(test_${t} PRIVATE ${PDOT_CJSON})
    add_test(NAME ${t} COMMAND test_${t}
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures)
  endforeach()
endif()
