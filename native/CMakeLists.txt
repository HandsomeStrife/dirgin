# =====================================================================
# native/  --  top-level CMake for the purist native port.
#
# Currently wires in only the reauthored engine (native/engine/). The
# sibling asset tools (native/assets/) and renderer (native/renderer/)
# keep their own hand-written Makefiles and are NOT driven from here, so
# this file is safe to add alongside them. As those are migrated to CMake
# they can be add_subdirectory()'d below.
# =====================================================================
cmake_minimum_required(VERSION 3.16)
project(dirge_native C)

enable_testing()
add_subdirectory(engine)
