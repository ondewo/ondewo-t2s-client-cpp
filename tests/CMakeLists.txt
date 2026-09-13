cmake_minimum_required(VERSION 3.22)

# ---------------------------------------------------------------------------------------
# Test suite for the committed ONDEWO T2S C++ client stubs.
#
# A STANDALONE project on purpose - it is not add_subdirectory()'d from the repository root.
# The root CMakeLists.txt is shipped verbatim by the ondewo-cpp-proto-compiler image and is
# overwritten on every regeneration, so anything added there would be lost. Consuming the
# INSTALLED package instead is also the stronger test: it exercises find_package(), the
# exported ondewo:: target and the installed header layout exactly as a downstream
# application does.
#
#   cmake -S tests -B build-tests -DCMAKE_PREFIX_PATH=<repo root>
#   cmake --build build-tests && ctest --test-dir build-tests
#
# `make unit_test` / `make coverage` in the repository root wrap both.
# ---------------------------------------------------------------------------------------

project(ondewo_client_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Must match LIBRARY_NAME in the repository Makefile; that is the CMake package the proto
# compiler installed. Passed in by `make unit_test`, defaulted here for a manual run.
set(ONDEWO_LIBRARY_NAME "ondewo_t2s_client" CACHE STRING "Installed ONDEWO client package to test")

option(ONDEWO_ENABLE_COVERAGE "Instrument the hand-written test sources for gcov" OFF)

find_package(${ONDEWO_LIBRARY_NAME} CONFIG REQUIRED)
find_package(GTest REQUIRED)

include(GoogleTest)
enable_testing()

add_executable(ondewo_client_tests
  descriptor_probe.cc
  product_config.cc
  test_generated_stubs.cc
  test_typed_api.cc)

# -fno-exceptions: nothing in this suite throws or catches - GoogleTest reports failures
# through its own non-throwing path. Without it every function that returns a container gets
# a compiler-generated unwind block pinned to its closing brace, which gcov then reports as an
# unexecuted line ("=====") that no test can ever reach. Dropping the landing pads makes the
# coverage report describe the code that was actually written.
target_compile_options(ondewo_client_tests PRIVATE -Wall -Wextra -fno-exceptions)

target_link_libraries(ondewo_client_tests PRIVATE
  ondewo::${ONDEWO_LIBRARY_NAME}
  GTest::gtest
  GTest::gtest_main)

# Pull EVERY member of the client archive into the test binary, not just the ones the test
# sources happen to name.
#
# A static archive only contributes the objects needed to resolve an undefined symbol, and a
# generated <file>.pb.o registers its descriptors from a static initialiser that nothing
# references by name. Without this the descriptor pool would hold exactly the few .proto files
# test_typed_api.cc mentions and the pool-wide sweeps would silently check almost nothing.
# Linking the whole archive also proves it is internally consistent - every object resolves,
# with no duplicate or missing symbol.
#
# --whole-archive includes archive members unconditionally, so it works from LINK_FLAGS even
# though CMake emits those ahead of the object files; the transitive protobuf/gRPC libraries
# still come afterwards via the imported target above. Apple's ld64 spells the same thing
# -force_load.
if(APPLE)
  target_link_options(ondewo_client_tests PRIVATE
    "-Wl,-force_load,$<TARGET_FILE:ondewo::${ONDEWO_LIBRARY_NAME}>")
else()
  target_link_options(ondewo_client_tests PRIVATE
    "-Wl,--whole-archive"
    "$<TARGET_FILE:ondewo::${ONDEWO_LIBRARY_NAME}>"
    "-Wl,--no-whole-archive")
endif()

# Coverage instruments ONLY this target, never the client library: the archive under lib/ is
# built separately and without --coverage, so no .gcno for a generated *.pb.cc can exist and
# machine-generated code cannot leak into the metric by accident. -O0 keeps gcov's line
# mapping faithful.
if(ONDEWO_ENABLE_COVERAGE)
  target_compile_options(ondewo_client_tests PRIVATE --coverage -O0 -g)
  target_link_options(ondewo_client_tests PRIVATE --coverage)
endif()

# One CTest case per TEST(), discovered by running the binary at build time - a test that
# fails to register shows up as a missing case rather than as a silently smaller run.
gtest_discover_tests(ondewo_client_tests
  DISCOVERY_TIMEOUT 120
  PROPERTIES TIMEOUT 300)
