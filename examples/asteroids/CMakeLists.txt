# pdot-asteroids: built from the top-level project with -DPDOT_BUILD_ASTEROIDS=ON.
# SDL2 on macOS: brew install sdl2 (or sdl2-compat), found through CMAKE_PREFIX_PATH.
find_package(SDL2 REQUIRED CONFIG)

add_executable(asteroids main.c chain.c board.c font.c)
# cJSON for the leaderboard document: the same package libpdot uses.
target_link_libraries(asteroids PRIVATE pdot SDL2::SDL2 ${PDOT_CJSON})
if(TARGET SDL2::SDL2main)
  target_link_libraries(asteroids PRIVATE SDL2::SDL2main)
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(asteroids PRIVATE m)
endif()
target_compile_options(asteroids PRIVATE ${PDOT_WARNINGS})
