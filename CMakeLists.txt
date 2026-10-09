cmake_minimum_required(VERSION 3.21)
# Shared D3D11 proxy / hook / DLAA core for the per-game DLAA mods.
#
# A game uses it with
#   add_subdirectory(${DLAA_CORE_DIR} dlaa_core)
#   dlaa_core_compile_shader(src/dlss/mv.hlsl cs_5_0 kMvCs mv.h)
#   add_library(my_dlaa SHARED src/game.cpp ...)
#   dlaa_core_add_mod(my_dlaa SHADERS mv)
# Built on its own it builds the template mod and the core tests.
project(dlaa_core LANGUAGES C CXX ASM_MASM)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "dlaa_core is x64 only: configure with -A x64")
endif()

set(DLAA_CORE_ROOT ${CMAKE_CURRENT_SOURCE_DIR} CACHE INTERNAL "")
# Generated shader headers of the core and the game, one include dir for all.
set(DLAA_SHADER_DIR ${CMAKE_BINARY_DIR}/generated CACHE INTERNAL "")

# Static CRT so the mod has no redistributable dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(external/minhook EXCLUDE_FROM_ALL)

# NVIDIA DLSS SDK (external/dlss): NGX headers + static-CRT import library.
set(DLSS_SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/dlss CACHE INTERNAL "")
set(NGX_LIB_DIR ${DLSS_SDK_DIR}/lib/Windows_x86_64/x64)

file(GLOB FXC_DIRS LIST_DIRECTORIES true "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/10.*/x64")
list(SORT FXC_DIRS COMPARE NATURAL ORDER DESCENDING)
find_program(FXC_EXECUTABLE fxc HINTS ${FXC_DIRS} REQUIRED)

# Precompiles an HLSL file (entry point `main`) into ${DLAA_SHADER_DIR}/<header>
# with the bytecode in `variable`. `source` is relative to the calling
# directory. Creates the target fxc_<header without .h>: link it to every user
# with dlaa_core_use_shaders, never list the header as a source (several
# targets running the same custom command race in parallel builds).
function(dlaa_core_compile_shader source profile variable header)
  get_filename_component(abs ${source} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
  get_filename_component(stem ${header} NAME_WE)
  add_custom_command(
    OUTPUT ${DLAA_SHADER_DIR}/${header}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DLAA_SHADER_DIR}
    COMMAND ${FXC_EXECUTABLE} /nologo /T ${profile} /E main /O3 /Vn ${variable}
            /Fh ${DLAA_SHADER_DIR}/${header} ${abs}
    DEPENDS ${abs}
    COMMENT "fxc ${source}"
    VERBATIM)
  add_custom_target(fxc_${stem} DEPENDS ${DLAA_SHADER_DIR}/${header})
endfunction()

# Makes `target` wait for the listed shaders (header stems) and see their headers.
function(dlaa_core_use_shaders target)
  foreach(stem ${ARGN})
    add_dependencies(${target} fxc_${stem})
  endforeach()
  target_include_directories(${target} PRIVATE ${DLAA_SHADER_DIR})
endfunction()

# Compile settings of every core and mod target.
function(dlaa_core_target_defaults target)
  set_target_properties(${target} PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_compile_features(${target} PRIVATE cxx_std_20)
  target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:/W4 /permissive- /utf-8>)
endfunction()

dlaa_core_compile_shader(shaders/debug_blit_vs.hlsl vs_5_0 kDebugBlitVs debug_blit_vs.h)
dlaa_core_compile_shader(shaders/debug_blit_ps.hlsl ps_5_0 kDebugBlitPs debug_blit_ps.h)

add_library(dlaa_core STATIC
  src/capture.cpp
  src/config.cpp
  src/debug_overlay.cpp
  src/frame_dump.cpp
  src/hooks.cpp
  src/log.cpp
  src/mip_bias.cpp
  src/ngx.cpp
  src/shader_registry.cpp
  src/vtable_indices.c
)
dlaa_core_target_defaults(dlaa_core)
dlaa_core_use_shaders(dlaa_core debug_blit_vs debug_blit_ps)
target_include_directories(dlaa_core PUBLIC include PRIVATE src)
target_include_directories(dlaa_core SYSTEM PUBLIC ${DLSS_SDK_DIR}/include)
# SDK headers use nameless unions in their C-mode COM declarations.
set_source_files_properties(src/vtable_indices.c PROPERTIES COMPILE_OPTIONS /wd4201)
target_link_libraries(dlaa_core PUBLIC minhook dxguid
  ${NGX_LIB_DIR}/nvsdk_ngx_s$<$<CONFIG:Debug>:_dbg>.lib)

# Turns a SHARED library of game sources (one of them defines dlaa_core::game)
# into the d3d11.dll proxy. SHADERS: header stems from dlaa_core_compile_shader.
# NO_DLSS_RUNTIME: do not copy nvngx_dlss.dll next to the built dll.
function(dlaa_core_add_mod target)
  cmake_parse_arguments(ARG "NO_DLSS_RUNTIME" "" "SHADERS" ${ARGN})
  target_sources(${target} PRIVATE
    ${DLAA_CORE_ROOT}/src/dllmain.cpp
    ${DLAA_CORE_ROOT}/src/export_stubs.asm
    ${DLAA_CORE_ROOT}/src/exports.def)
  set_target_properties(${target} PROPERTIES OUTPUT_NAME "d3d11")
  dlaa_core_target_defaults(${target})
  if(ARG_SHADERS)
    dlaa_core_use_shaders(${target} ${ARG_SHADERS})
  endif()
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(${target} PRIVATE dlaa_core)
  if(NOT ARG_NO_DLSS_RUNTIME)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DLSS_SDK_DIR}/nvngx_dlss.dll
              $<TARGET_FILE_DIR:${target}>)
  endif()
endfunction()

if(PROJECT_IS_TOP_LEVEL)
  # The template mod doubles as the core's integration test.
  add_subdirectory(template)

  # Loads a built proxy and drives device / swap chain / draws / Present.
  add_executable(smoke_test tests/smoke_test.cpp)
  target_link_libraries(smoke_test PRIVATE dxgi d3dcompiler)

  # NGX init + one DLAA feature evaluated on synthetic textures.
  add_executable(ngx_test tests/ngx_test.cpp)
  dlaa_core_target_defaults(ngx_test)
  target_link_libraries(ngx_test PRIVATE dlaa_core d3d11)
  add_custom_command(TARGET ngx_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DLSS_SDK_DIR}/nvngx_dlss.dll
            $<TARGET_FILE_DIR:ngx_test>)
endif()
