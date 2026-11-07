# pdot-asteroids: built from the top-level project with -DPDOT_BUILD_ASTEROIDS=ON.
# SDL2 on macOS: brew install sdl2 (or sdl2-compat), found through CMAKE_PREFIX_PATH.
find_package(SDL2 REQUIRED CONFIG)

# sim.c is the game itself, which the operator re-runs from replays: it must
# compute the same floats everywhere, so no fused multiply-add.
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  set_source_files_properties(sim.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
elseif(MSVC)
  set_source_files_properties(sim.c PROPERTIES COMPILE_OPTIONS "/fp:strict")
endif()

add_executable(asteroids main.c chain.c board.c font.c sim.c replay.c)
# cJSON for the leaderboard document and libcurl for the operator: the same
# packages libpdot uses.
target_link_libraries(asteroids PRIVATE pdot SDL2::SDL2 CURL::libcurl ${PDOT_CJSON})
if(TARGET SDL2::SDL2main)
  target_link_libraries(asteroids PRIVATE SDL2::SDL2main)
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(asteroids PRIVATE m)
endif()
target_compile_options(asteroids PRIVATE ${PDOT_WARNINGS})

# The operator: a small HTTP server holding the operator key (POSIX sockets).
if(NOT WIN32)
  add_executable(asteroids-operator operator.c board.c sim.c replay.c)
  target_link_libraries(asteroids-operator PRIVATE pdot ${PDOT_CJSON})
  target_compile_options(asteroids-operator PRIVATE ${PDOT_WARNINGS})
endif()

# Replays re-run to their scores; edited ones do not check.
if(PDOT_BUILD_TESTS)
  enable_testing()
  add_executable(asteroids-replay-test replay_test.c replay.c sim.c)
  target_compile_options(asteroids-replay-test PRIVATE ${PDOT_WARNINGS})
  add_test(NAME asteroids_replay COMMAND asteroids-replay-test)
endif()
