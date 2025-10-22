cmake_minimum_required(VERSION 3.21)
project(bee_sim C)
set(CMAKE_C_STANDARD 11)

# vcpkg toolchain gets passed on the command line; use CONFIG find
find_package(SDL2 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)

add_executable(bee_sim
  src/main.c
  src/app/app.c
  src/config/params.c
  src/sim/bee.c
  src/sim/bee_path.c
  src/sim/hive.c
  src/sim/plants.c
  src/sim/sim.c
  src/world/hex_world.c
  src/platform/sdl_io.c
  src/render/hex_draw.c
  src/render/gl_backend.c
  src/ui/ui.c
  src/util/log.c
)

target_include_directories(bee_sim PRIVATE include)

target_link_libraries(bee_sim PRIVATE
  glad::glad
  $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
  $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
)

if (MSVC)
  target_link_libraries(bee_sim PRIVATE opengl32)
  target_compile_options(bee_sim PRIVATE /W4 /permissive-)
else()
  target_compile_options(bee_sim PRIVATE -O3 -march=native -Wall -Wextra -Wpedantic)
endif()
