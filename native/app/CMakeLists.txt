# =====================================================================
# native/app  --  native application shell for the Dirge of Cerberus
# purist PC port. Links the reauthored engine library (doc_engine) and
# runs the integrated per-frame loop.
#
# Build:
#   cmake -S . -B build && cmake --build build
#   ./build/dirge-app
# =====================================================================
cmake_minimum_required(VERSION 3.16)
project(dirge_app C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -pedantic)
endif()

# Build the reauthored engine library (read-only sibling dir) in-tree so the
# app links the real subsystems and picks up their public include dirs.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../engine
                 ${CMAKE_CURRENT_BINARY_DIR}/engine)

add_executable(dirge-app main.c)
target_link_libraries(dirge-app PRIVATE doc_engine)

# convenience alias
add_custom_target(native-app DEPENDS dirge-app)
