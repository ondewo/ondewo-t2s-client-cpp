# Release History

*****************

## Release ONDEWO T2S C++ Client 0.1.0

### New Features

* Initial release of `ondewo-t2s-client-cpp`, the C++ gRPC client library for the
  [ONDEWO T2S API](https://github.com/ondewo/ondewo-t2s-api) (Text-to-Speech). The stubs are generated
  from the `ondewo-t2s-api` submodule by the `ondewo-cpp-proto-compiler` image of
  [ondewo-proto-compiler](https://github.com/ondewo/ondewo-proto-compiler) 5.15.0, using protoc's
  built-in `--cpp_out` for the messages and `grpc_cpp_plugin` for the service stubs.
* Ships as a consumable CMake package: `find_package(ondewo_t2s_client CONFIG REQUIRED)` plus
  `target_link_libraries(my_app PRIVATE ondewo::ondewo_t2s_client)`. The package installs the generated
  headers under `include/ondewo_t2s_client/`, the static archive
  `lib/libondewo_t2s_client.a` and `lib/cmake/ondewo_t2s_client/`, and re-finds `Protobuf` and
  `gRPC` on the consumer's machine through `find_dependency`.
* `public-api.h` is an umbrella header that includes every generated `*.pb.h` and `*.grpc.pb.h`, so a single
  `#include "public-api.h"` gives access to the whole API surface.
* The generated stub sources are committed to the repository. C++ has no package registry, so a git tag is the
  distribution channel: a plain `git clone` or a CMake `FetchContent_Declare` yields a buildable project with
  no Docker and no code generation step.
* `make build` reproduces the whole pipeline - submodule checkout at the pinned tags, compiler image build,
  code generation and a host-native CMake build - and `make test` verifies that every `.proto` produced a
  header and that the installed CMake package is consumable by a downstream project.

*****************
