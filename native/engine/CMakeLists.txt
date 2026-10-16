# =====================================================================
# native/engine  --  reauthored Dirge of Cerberus game logic (purist port)
#
# Builds:
#   doc_engine         static library  (math/ + scene/)
#   test_math          unit test       (dmath + vu0)
#   test_scene_graph   unit + oracle-parity test (scene graph)
#   test_object_list   unit + oracle-parity test (object-list traversal)
#   test_engine_state  unit + oracle-parity test (engine spine state machine)
#   test_input         unit + oracle-parity + mutation test (controller input)
#
# Portable: Linux now, Windows later. No platform-specific code; the only
# system dependency is libm (math), linked only where present.
# =====================================================================
cmake_minimum_required(VERSION 3.16)
project(doc_engine C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Warnings: the whole module is expected to build clean under -Wall -Wextra.
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

add_library(doc_engine STATIC
    math/dmath.c
    math/vu0.c
    scene/scene_graph.c
    scene/object_list.c
    scene/material.c
    scene/material_loader.c
    scene/camera.c
    scene/light.c
    scene/actor.c
    core/engine_state.c
    core/frame_loop.c
    core/input.c
)
target_include_directories(doc_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/math
    ${CMAKE_CURRENT_SOURCE_DIR}/scene
    ${CMAKE_CURRENT_SOURCE_DIR}/core
)

# libm for the math lib (not needed / not present on MSVC)
find_library(DOC_LIBM m)
if(DOC_LIBM)
    target_link_libraries(doc_engine PUBLIC ${DOC_LIBM})
endif()

enable_testing()

add_executable(test_math tests/test_math.c)
target_link_libraries(test_math PRIVATE doc_engine)
add_test(NAME test_math COMMAND test_math)

add_executable(test_scene_graph tests/test_scene_graph.c)
target_link_libraries(test_scene_graph PRIVATE doc_engine)
add_test(NAME test_scene_graph COMMAND test_scene_graph)

add_executable(test_object_list tests/test_object_list.c)
target_link_libraries(test_object_list PRIVATE doc_engine)
add_test(NAME test_object_list COMMAND test_object_list)

add_executable(test_engine_state tests/test_engine_state.c)
target_link_libraries(test_engine_state PRIVATE doc_engine)
add_test(NAME test_engine_state COMMAND test_engine_state)

add_executable(test_material tests/test_material.c)
target_link_libraries(test_material PRIVATE doc_engine)
add_test(NAME test_material COMMAND test_material)

add_executable(test_material_loader tests/test_material_loader.c)
target_link_libraries(test_material_loader PRIVATE doc_engine)
# pass every z143 chunk manifest if baked (emit_loader_manifest.py); each runs
# or skips cleanly on its own, so the test is green with or without the disc.
set(_z143_loader_dir ${CMAKE_CURRENT_SOURCE_DIR}/../assets/models/out/z143_loader)
set(_z143_manifests "")
foreach(_cid 0012 0112 0212 0312 0412 0512 0612 0712 0912 0a12 0b12 0c12 0d12)
    list(APPEND _z143_manifests ${_z143_loader_dir}/068f${_cid}.manifest)
endforeach()
add_test(NAME test_material_loader COMMAND test_material_loader ${_z143_manifests})

add_executable(test_frame_loop tests/test_frame_loop.c)
target_link_libraries(test_frame_loop PRIVATE doc_engine)
add_test(NAME test_frame_loop COMMAND test_frame_loop)

add_executable(test_input tests/test_input.c)
target_link_libraries(test_input PRIVATE doc_engine)
add_test(NAME test_input COMMAND test_input)

add_executable(test_camera tests/test_camera.c)
target_link_libraries(test_camera PRIVATE doc_engine)
add_test(NAME test_camera COMMAND test_camera)

add_executable(test_light tests/test_light.c)
target_link_libraries(test_light PRIVATE doc_engine)
add_test(NAME test_light COMMAND test_light)

add_executable(test_actor tests/test_actor.c)
target_link_libraries(test_actor PRIVATE doc_engine)
add_test(NAME test_actor COMMAND test_actor)
