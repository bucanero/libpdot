# The Last Week of Summer: a visual novel on the Suika3 engine, with every
# story recorded on Polkadot Asset Hub through libpdot.
#
# Suika3 is a submodule pinned to the commit this was tested on; the few
# engine hooks the chain needs are in patches/suika3.patch, applied here at
# configure time. The libpdot side is src/chain.c; the story is in game/.
# Standalone: configure this directory, not libpdot's root (see README.md).
cmake_minimum_required(VERSION 3.18)
project(pdot_visualnovel C)

set(SUIKA3_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/suika3")
set(SUIKA3_PATCH "${CMAKE_CURRENT_SOURCE_DIR}/patches/suika3.patch")
set(LIBPDOT_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/../..")

if(NOT EXISTS "${SUIKA3_DIR}/external/PlayfieldEngine/CMakeLists.txt")
  message(FATAL_ERROR "Suika3 is not checked out. From libpdot's root:\n"
    "  git submodule update --init --checkout --recursive examples/visualnovel/suika3")
endif()

# The engine hooks: applied once, left in place (.gitmodules ignores the
# submodule's dirty state). A patch that applies neither way means the
# submodule is not at the pinned commit.
find_package(Git REQUIRED)
execute_process(
  COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${SUIKA3_PATCH}"
  WORKING_DIRECTORY "${SUIKA3_DIR}"
  RESULT_VARIABLE already OUTPUT_QUIET ERROR_QUIET)
if(NOT already EQUAL 0)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply "${SUIKA3_PATCH}"
    WORKING_DIRECTORY "${SUIKA3_DIR}"
    RESULT_VARIABLE rc ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "patches/suika3.patch does not apply: is suika3 at the "
      "commit libpdot pins? (git submodule update --checkout)\n${err}")
  endif()
  message(STATUS "Applied patches/suika3.patch to suika3")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SUIKA3_PATCH}")

# libpdot, the library only.
set(PDOT_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(PDOT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${LIBPDOT_DIR}" libpdot EXCLUDE_FROM_ALL)

# The engine. SUIKA_WITH_PDOT keeps it from forcing a universal macOS
# binary, which Homebrew's single-architecture libraries cannot link into.
set(SUIKA_WITH_PDOT ON)
add_subdirectory("${SUIKA3_DIR}" suika3)

target_sources(suika3 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/chain.c")
target_include_directories(suika3 PRIVATE "${SUIKA3_DIR}/src")
target_compile_definitions(suika3 PRIVATE S3_USE_PDOT)
target_link_libraries(suika3 PRIVATE pdot)

# A playable folder: Suika3's sample assets, this story over them, and the
# engine. Run it from inside that folder (see README.md).
set(GAME_DIR "${CMAKE_BINARY_DIR}/game")
if(APPLE)
  set(engine_src "$<TARGET_BUNDLE_DIR:suika3>")
  set(engine_dst "${GAME_DIR}/Suika3.app")
  set(engine_copy copy_directory)
else()
  set(engine_src "$<TARGET_FILE:suika3>")
  set(engine_dst "${GAME_DIR}/")
  set(engine_copy copy)
endif()
add_custom_target(game ALL
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${SUIKA3_DIR}/game" "${GAME_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${GAME_DIR}/.vscode"
  COMMAND "${CMAKE_COMMAND}" -E copy
          "${CMAKE_CURRENT_SOURCE_DIR}/game/start.novel"
          "${CMAKE_CURRENT_SOURCE_DIR}/game/config.ini"
          "${CMAKE_CURRENT_SOURCE_DIR}/game/main.ray"
          "${GAME_DIR}/"
  COMMAND "${CMAKE_COMMAND}" -E ${engine_copy} "${engine_src}" "${engine_dst}"
  COMMENT "Assembling the playable game in ${GAME_DIR}"
  VERBATIM)
add_dependencies(game suika3)
