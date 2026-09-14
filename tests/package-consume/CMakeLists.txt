cmake_minimum_required(VERSION 3.22)

# ---------------------------------------------------------------------------------------
# Consumer-side check for the release archive produced by `make build_package`.
#
# A STANDALONE project that knows nothing about the repository it lives in. `make
# verify_package` configures it against a throwaway directory holding nothing but the
# extracted release tarball, so it does exactly what a downstream application does:
# find_package() the CMake package, include the umbrella header, link the exported
# ondewo:: target. If this configures, builds and runs, the archive is publishable.
#
#   cmake -S tests/package-consume -B <build> \
#     -DCMAKE_PREFIX_PATH=<extracted archive root> \
#     -DONDEWO_PACKAGE_PREFIX=<extracted archive root> \
#     -DONDEWO_LIBRARY_NAME=ondewo_t2s_client
#
# This is NOT tests/CMakeLists.txt: that suite consumes the install tree in the repository
# root and asserts what the stubs contain, this one asserts that the tarball about to be
# attached to a GitHub release is self-contained and relocatable.
# ---------------------------------------------------------------------------------------

project(ondewo_package_consume LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Must match LIBRARY_NAME in the repository Makefile; that is the CMake package the release
# archive installs. Passed in by `make verify_package`, defaulted here for a manual run.
set(ONDEWO_LIBRARY_NAME "ondewo_t2s_client" CACHE STRING "Installed ONDEWO client package to consume")
set(ONDEWO_PACKAGE_PREFIX "" CACHE PATH "Extracted release archive the package MUST resolve from")

find_package(${ONDEWO_LIBRARY_NAME} CONFIG REQUIRED)

# Prove the package really came out of the extracted archive. Without this the whole check
# would pass just as happily against a system-wide or repository-root install tree that
# happens to be on the search path - i.e. it would verify everything except the artifact
# that is about to be published.
if(ONDEWO_PACKAGE_PREFIX)
  get_filename_component(_expected "${ONDEWO_PACKAGE_PREFIX}" REALPATH)
  get_filename_component(_actual "${${ONDEWO_LIBRARY_NAME}_DIR}" REALPATH)
  string(FIND "${_actual}" "${_expected}/" _inside)
  if(NOT _inside EQUAL 0)
    message(FATAL_ERROR
      "find_package(${ONDEWO_LIBRARY_NAME}) resolved to ${_actual}, which is outside the "
      "extracted release archive at ${_expected}")
  endif()
endif()
message(STATUS "consuming ${ONDEWO_LIBRARY_NAME} from ${${ONDEWO_LIBRARY_NAME}_DIR}")

add_executable(package_consume main.cpp)

target_compile_options(package_consume PRIVATE -Wall -Wextra)

target_link_libraries(package_consume PRIVATE ondewo::${ONDEWO_LIBRARY_NAME})

# Name one .proto the archive must have registered, derived from the INSTALLED headers so that
# nothing here is product-specific: <include root>/ondewo/t2s/text-to-speech.pb.h names the
# proto file "ondewo/t2s/text-to-speech.proto". The *.grpc.pb.h twin of each header is
# filtered out - its stem would name a "<file>.grpc.proto" that does not exist.
get_target_property(_include_dirs ondewo::${ONDEWO_LIBRARY_NAME} INTERFACE_INCLUDE_DIRECTORIES)
list(GET _include_dirs 0 _include_root)
file(GLOB_RECURSE _headers RELATIVE "${_include_root}" "${_include_root}/*.pb.h")
list(FILTER _headers EXCLUDE REGEX "\\.grpc\\.pb\\.h$")
if(NOT _headers)
  message(FATAL_ERROR "the extracted archive installs no *.pb.h under ${_include_root}")
endif()
list(SORT _headers)
# Prefer one of the client's OWN protos over any google/ imports it pulled in: alphabetically
# google/api/annotations.proto sorts first and declares neither a message nor a service, so it
# would make for a thin probe. This client imports nothing outside the well-known types, so the
# archive carries no google/ header at all today and the filter is a no-op - it stays because a
# single added import would change that. Fall back to the full list if a client ever generates
# nothing outside google/.
set(_own_headers ${_headers})
list(FILTER _own_headers EXCLUDE REGEX "^google/")
if(_own_headers)
  set(_headers ${_own_headers})
endif()
list(GET _headers 0 _probe_header)
string(REGEX REPLACE "\\.pb\\.h$" ".proto" _probe_proto "${_probe_header}")
target_compile_definitions(package_consume PRIVATE ONDEWO_PROBE_PROTO_FILE="${_probe_proto}")

# Pull EVERY member of the packaged archive into this binary, not just the ones main.cpp needs.
# A static archive otherwise contributes only the objects that resolve an undefined symbol, and
# a generated <file>.pb.o registers its descriptors from a static initialiser that nothing
# references by name - so without this the link would succeed against an archive that shipped
# almost nothing. Linking the whole archive also proves it is internally consistent: every
# object resolves, with no duplicate and no missing symbol. Apple's ld64 spells it -force_load.
if(APPLE)
  target_link_options(package_consume PRIVATE
    "-Wl,-force_load,$<TARGET_FILE:ondewo::${ONDEWO_LIBRARY_NAME}>")
else()
  target_link_options(package_consume PRIVATE
    "-Wl,--whole-archive"
    "$<TARGET_FILE:ondewo::${ONDEWO_LIBRARY_NAME}>"
    "-Wl,--no-whole-archive")
endif()
