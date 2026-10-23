cmake_minimum_required(VERSION 3.16)
project(painite LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(PAINITE_NATIVE "Tune for the build machine (-march=native); do not use for distributable binaries" ON)
option(PAINITE_LTO "Link-time optimization" ON)
set(PAINITE_PGO "off" CACHE STRING "Profile-guided optimisation: off | generate | use (see build.sh --pgo)")

find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

file(GLOB PAINITE_VG_SOURCES CONFIGURE_DEPENDS src/vg/*.cpp)
file(GLOB PAINITE_SIM_SOURCES CONFIGURE_DEPENDS src/sim/*.cpp)
set_source_files_properties(${PAINITE_VG_SOURCES} PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")   # the port of Mojang's float and double math must not fuse operations
add_executable(painite
  src/main.cpp src/config.cpp src/data.cpp src/log.cpp src/server.cpp src/worker.cpp src/java.cpp src/bedrock.cpp src/rak.cpp src/auth.cpp src/hash.cpp src/vitals.cpp src/mobs.cpp src/redstone.cpp src/blocktick.cpp src/fluids.cpp src/containers.cpp src/nbt.cpp src/anvil.cpp src/blocks.cpp src/world.cpp src/jchunk.cpp src/lists.cpp src/game.cpp src/commands.cpp src/java_game.cpp src/bchunk.cpp src/bedrock_game.cpp src/xbox.cpp src/skin.cpp src/region.cpp src/worldgen.cpp src/worldgen_dims.cpp src/portal.cpp src/structures.cpp src/structures_stronghold.cpp src/structures_village.cpp src/structures_misc.cpp ${PAINITE_VG_SOURCES} ${PAINITE_SIM_SOURCES})
target_link_libraries(painite PRIVATE ZLIB::ZLIB OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
target_compile_options(painite PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-plt)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
if(PAINITE_PGO STREQUAL "generate")
  target_compile_options(painite PRIVATE -fprofile-generate -fprofile-update=atomic)
  target_link_options(painite PRIVATE -fprofile-generate)
elseif(PAINITE_PGO STREQUAL "use")
  target_compile_options(painite PRIVATE -fprofile-use -fprofile-correction -Wno-missing-profile)
  target_link_options(painite PRIVATE -fprofile-use)
endif()
if(PAINITE_NATIVE)
  target_compile_options(painite PRIVATE -march=native)
endif()
if(PAINITE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT ipo_ok OUTPUT ipo_msg)
  if(ipo_ok)
    set_property(TARGET painite PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
  endif()
endif()

add_executable(painite_unit EXCLUDE_FROM_ALL tests/unit.cpp src/hash.cpp src/config.cpp src/log.cpp src/nbt.cpp src/anvil.cpp src/lists.cpp src/skin.cpp)
target_include_directories(painite_unit PRIVATE src)
target_link_libraries(painite_unit PRIVATE OpenSSL::Crypto ZLIB::ZLIB)

install(TARGETS painite RUNTIME DESTINATION bin)
install(FILES painite.toml DESTINATION share/painite)

add_executable(painite_genstats EXCLUDE_FROM_ALL tests/gen_stats.cpp src/worldgen.cpp src/worldgen_dims.cpp src/structures.cpp src/structures_stronghold.cpp src/structures_village.cpp src/structures_misc.cpp src/blocks.cpp src/data.cpp src/nbt.cpp src/log.cpp src/hash.cpp)
target_include_directories(painite_genstats PRIVATE src)
target_link_libraries(painite_genstats PRIVATE OpenSSL::Crypto ZLIB::ZLIB Threads::Threads)

# End-to-end test suite in one binary: cmake --build build --target painite_tests && build/painite_tests
file(GLOB PAINITE_TEST_SOURCES CONFIGURE_DEPENDS tests/cpp/*.cpp)
list(REMOVE_ITEM PAINITE_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/cpp/load.cpp ${CMAKE_CURRENT_SOURCE_DIR}/tests/cpp/refgen.cpp ${CMAKE_CURRENT_SOURCE_DIR}/tests/cpp/vgcheck.cpp ${CMAKE_CURRENT_SOURCE_DIR}/tests/cpp/gencmp.cpp ${CMAKE_CURRENT_SOURCE_DIR}/tests/cpp/gensweep.cpp ${CMAKE_CURRENT_SOURCE_DIR}/tests/cpp/v3check.cpp)
add_executable(painite_tests EXCLUDE_FROM_ALL ${PAINITE_TEST_SOURCES})
target_include_directories(painite_tests PRIVATE tests/cpp)
target_link_libraries(painite_tests PRIVATE OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB Threads::Threads)
target_compile_options(painite_tests PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)

# Load test: build/painite_load --port P --pid SERVER_PID --bots 40
add_executable(painite_load EXCLUDE_FROM_ALL tests/cpp/load.cpp tests/cpp/jclient.cpp tests/cpp/jbot.cpp)
target_include_directories(painite_load PRIVATE tests/cpp)
target_link_libraries(painite_load PRIVATE OpenSSL::Crypto ZLIB::ZLIB Threads::Threads)
target_compile_options(painite_load PRIVATE -O2)

# Reference worlds made by Mojang's own server, for comparing world generation: build/painite_refgen --out DIR --seed 12345 --area -128 -128 127 127
add_executable(painite_refgen EXCLUDE_FROM_ALL tests/cpp/refgen.cpp tests/cpp/server.cpp tests/cpp/jclient.cpp tests/cpp/jbot.cpp)
target_include_directories(painite_refgen PRIVATE tests/cpp)
target_link_libraries(painite_refgen PRIVATE OpenSSL::Crypto ZLIB::ZLIB Threads::Threads)
target_compile_options(painite_refgen PRIVATE -O2)

# The port of Mojang's world generation (src/vg) and the tool that checks it value by value against Mojang's own code (tools/oracle).
set(PAINITE_VG_CORE ${PAINITE_VG_SOURCES})
list(FILTER PAINITE_VG_CORE EXCLUDE REGEX "src/vg/gen\\.cpp$")
add_executable(painite_vgcheck EXCLUDE_FROM_ALL tests/cpp/vgcheck.cpp ${PAINITE_VG_CORE})
target_include_directories(painite_vgcheck PRIVATE src)
target_link_libraries(painite_vgcheck PRIVATE OpenSSL::Crypto)
target_compile_options(painite_vgcheck PRIVATE -O2 -ffp-contract=off -Wall -Wextra -Wno-unused-parameter)

set(PAINITE_V3_CORE src/vg/noise3.cpp src/vg/df3.cpp src/vg/terrain3.cpp src/vg/blockdata.cpp src/vg/random.cpp src/vg/climate.cpp src/vg/registry.cpp src/vg/norm263.cpp src/vg/json.cpp src/blocks.cpp src/data.cpp src/nbt.cpp src/log.cpp src/hash.cpp)
add_executable(painite_v3check EXCLUDE_FROM_ALL tests/cpp/v3check.cpp ${PAINITE_V3_CORE})
target_include_directories(painite_v3check PRIVATE src)
target_link_libraries(painite_v3check PRIVATE OpenSSL::Crypto ZLIB::ZLIB Threads::Threads)
target_compile_options(painite_v3check PRIVATE -O2 -ffp-contract=off -Wall -Wextra -Wno-unused-parameter)

add_executable(painite_gencmp EXCLUDE_FROM_ALL tests/cpp/gencmp.cpp ${PAINITE_VG_SOURCES} src/world.cpp src/worldgen.cpp src/worldgen_dims.cpp src/structures.cpp src/structures_stronghold.cpp src/structures_village.cpp src/structures_misc.cpp
  src/blocks.cpp src/data.cpp src/nbt.cpp src/anvil.cpp src/log.cpp src/hash.cpp)
target_include_directories(painite_gencmp PRIVATE src)
target_link_libraries(painite_gencmp PRIVATE OpenSSL::Crypto ZLIB::ZLIB Threads::Threads)
target_compile_options(painite_gencmp PRIVATE -O2 -ffp-contract=off -Wall -Wextra -Wno-unused-parameter)

add_executable(painite_gensweep EXCLUDE_FROM_ALL tests/cpp/gensweep.cpp ${PAINITE_VG_SOURCES} src/world.cpp src/worldgen.cpp src/worldgen_dims.cpp src/structures.cpp src/structures_stronghold.cpp src/structures_village.cpp src/structures_misc.cpp
  src/blocks.cpp src/data.cpp src/nbt.cpp src/anvil.cpp src/log.cpp src/hash.cpp)
target_include_directories(painite_gensweep PRIVATE src)
target_link_libraries(painite_gensweep PRIVATE OpenSSL::Crypto ZLIB::ZLIB Threads::Threads)
target_compile_options(painite_gensweep PRIVATE -O2 -ffp-contract=off -Wall -Wextra -Wno-unused-parameter)

# quick compile and test of the gameplay core (inventory, menus, ...) without the rest of the server
set(PAINITE_SIM_TEST_SOURCES ${PAINITE_SIM_SOURCES})
list(FILTER PAINITE_SIM_TEST_SOURCES EXCLUDE REGEX "src/sim/entity_manager\\.cpp$")
add_executable(painite_simtest EXCLUDE_FROM_ALL tests/sim/simtest.cpp ${PAINITE_SIM_TEST_SOURCES} src/blocks.cpp src/vg/json.cpp src/vg/random.cpp src/nbt.cpp)
target_compile_options(painite_simtest PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(painite_simtest PRIVATE OpenSSL::Crypto)
