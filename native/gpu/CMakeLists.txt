# CMakeLists.txt -- Dirge of Cerberus native GPU renderer (Milestone P3, GPU).
#
# Builds the real OpenGL 3.3-core renderer. The platform context backend is
# selected here: EGL offscreen on Linux (this headless build server), a WGL /
# swapchain backend on Windows (to be added as gfx_context_wgl.c -- the render
# core, shaders and GL loader are identical). See README.md.

cmake_minimum_required(VERSION 3.16)
project(doc_gpu_renderer C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

set(MODEL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../assets/models)
set(TEX_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/../assets/textures)

# Sibling native loaders, compiled in read-only (never modified in place).
# libdtex.a is built by its own Makefile; build it on demand.
add_custom_command(
    OUTPUT  ${TEX_DIR}/libdtex.a
    COMMAND make -C ${TEX_DIR} libdtex.a
    COMMENT "Building libdtex.a (texture decoder + PNG writer)")
add_custom_target(libdtex DEPENDS ${TEX_DIR}/libdtex.a)

# Portable render core + GL loader (no platform code).
set(CORE_SRC
    renderer_gl.c
    gl_api.c
    ${MODEL_DIR}/doc_model.c)

# Platform context backend. Default: EGL (Linux headless).
set(GFX_BACKEND "EGL" CACHE STRING "GL context backend: EGL or WGL")

if(GFX_BACKEND STREQUAL "EGL")
    list(APPEND PLATFORM_SRC gfx_context_egl.c)
    set(PLATFORM_LIBS EGL GL)
elseif(GFX_BACKEND STREQUAL "WGL")
    # Windows: add gfx_context_wgl.c (not present on this headless box).
    list(APPEND PLATFORM_SRC gfx_context_wgl.c)
    set(PLATFORM_LIBS opengl32 gdi32)
endif()

add_executable(gpu_renderer main.c ${CORE_SRC} ${PLATFORM_SRC})
add_dependencies(gpu_renderer libdtex)
target_include_directories(gpu_renderer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${MODEL_DIR} ${TEX_DIR})
target_link_libraries(gpu_renderer ${TEX_DIR}/libdtex.a ${PLATFORM_LIBS} m)
