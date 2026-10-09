cmake_minimum_required(VERSION 3.21)
# New-game skeleton. Copy template/ to C:\_APPS\CLAUDE\<game>, rename
# EXAMPLE_DLAA / example_dlaa / example:: and the ini, then build with
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release
project(EXAMPLE_DLAA LANGUAGES C CXX ASM_MASM)

if(PROJECT_IS_TOP_LEVEL)
  set(DLAA_CORE_DIR ${CMAKE_SOURCE_DIR}/../dlaa-core CACHE PATH "dlaa-core checkout")
  add_subdirectory(${DLAA_CORE_DIR} dlaa_core)
endif()

# The game's own shaders: dlaa_core_compile_shader(src/mv.hlsl cs_5_0 kMvCs mv.h)
# then SHADERS mv below.

add_library(example_dlaa SHARED src/game.cpp)
dlaa_core_add_mod(example_dlaa)

if(PROJECT_IS_TOP_LEVEL)
  # tests/smoke_test.cpp from dlaa-core: drives the built proxy.
  add_executable(smoke_test ${DLAA_CORE_DIR}/tests/smoke_test.cpp)
  target_link_libraries(smoke_test PRIVATE dxgi d3dcompiler)
endif()
