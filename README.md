<div align="center">
  <table>
    <tr>
      <td>
        <a href="https://ondewo.com/">
            <img width="400px" src="https://raw.githubusercontent.com/ondewo/ondewo-logos/master/ondewo_we_automate_your_phone_calls.png"/>
        </a>
      </td>
    </tr>
    <tr>
       <td align="center">
          <a href="https://www.linkedin.com/company/ondewo"><img width="40px" src="https://cdn-icons-png.flaticon.com/512/3536/3536505.png"></a>
          <a href="https://www.facebook.com/ondewo"><img width="40px" src="https://cdn-icons-png.flaticon.com/512/733/733547.png"></a>
          <a href="https://twitter.com/ondewo"><img width="40px" src="https://cdn-icons-png.flaticon.com/512/733/733579.png"></a>
          <a href="https://www.instagram.com/ondewo.ai/"><img width="40px" src="https://cdn-icons-png.flaticon.com/512/174/174855.png"></a>
       </td>
    </tr>
  </table>
  <h1 align="center">
    ONDEWO T2S Client C++
  </h1>
</div>

## Overview

`ondewo-t2s-client-cpp` is the C++ gRPC client library for the
[ONDEWO T2S API](https://github.com/ondewo/ondewo-t2s-api) - ONDEWO's Text-to-Speech service. It is a
compiled version of that API, generated with the
[ONDEWO PROTO COMPILER](https://github.com/ondewo/ondewo-proto-compiler). The API
[documentation](https://ondewo.github.io) describes every service and message in detail.

ONDEWO APIs use [Protocol Buffers](https://github.com/google/protobuf) version 3 (proto3) as their Interface
Definition Language (IDL) to define the API interface and the structure of the payload messages. The same
interface definition is used for the gRPC versions of the API in all languages.

There is **no hand-written code** in this repository. Everything it ships is generated:

| Path                            | What it is                                                               |
| ------------------------------- | ------------------------------------------------------------------------ |
| `api/`                          | the generated stubs - `*.pb.h` / `*.pb.cc` and `*.grpc.pb.h` / `*.grpc.pb.cc` |
| `public-api.h`                  | umbrella header that `#include`s every generated header                  |
| `CMakeLists.txt`                | builds the stubs into a static library and installs a CMake package      |
| `ondewo-client-config.cmake.in` | template for the installed `<library>-config.cmake`                      |
| `ondewo-t2s-api/`                   | submodule - the `.proto` source of truth                                 |
| `ondewo-proto-compiler/`        | submodule - the code generator, pinned to a release tag                  |

The generated sources are committed deliberately: C++ has no package registry, so a git tag is this client's
distribution channel and a plain `git clone` has to yield a buildable CMake project.

## Requirements

To **consume** the library:

- CMake >= 3.22 and a C++17 compiler
- `libprotobuf-dev` and `libgrpc++-dev` (plus `libgrpc-dev`, which ships `gRPCConfig.cmake`)

protobuf C++ gives
[no cross-version guarantee](https://protobuf.dev/support/cross-version-runtime-guarantee/) between generated
code and runtime - they must match **exactly**. The stubs in `api/` are generated against the protobuf and gRPC
versions pinned by `ondewo-proto-compiler/cpp/Dockerfile` (`ARG PROTOBUF_VERSION` / `ARG GRPC_VERSION`), which
are the versions Debian stable and Ubuntu 24.04 ship. If your distribution ships a different protobuf, rebuild
the stubs against it with `make build` rather than linking the committed ones.

To **regenerate** the stubs you additionally need Docker.

## Setup

Using CMake `FetchContent` - no Docker, no install step:

```cmake
include(FetchContent)
# The library, target and package name. Set it BEFORE MakeAvailable: the built-in default is the
# generic `ondewo_grpc_client`, which collides if you pull in more than one ONDEWO C++ client.
set(ONDEWO_LIBRARY_NAME ondewo_t2s_client CACHE STRING "" FORCE)
FetchContent_Declare(
  ondewo_t2s_client
  GIT_REPOSITORY https://github.com/ondewo/ondewo-t2s-client-cpp.git
  GIT_TAG        0.1.0)
FetchContent_MakeAvailable(ondewo_t2s_client)

# Note the UNqualified target name: the `ondewo::` namespace is created by the install/export step
# below, so it does not exist when the project is pulled in with add_subdirectory/FetchContent.
target_link_libraries(my_app PRIVATE ondewo_t2s_client)
```

`FetchContent_MakeAvailable` runs `add_subdirectory`, so this client's `install()` rules become part of your
project's install as well. Pass `EXCLUDE_FROM_ALL` to `FetchContent_Declare` (CMake >= 3.28) if that is not
what you want.

Using a system-wide install:

```shell
git clone https://github.com/ondewo/ondewo-t2s-client-cpp.git
cd ondewo-t2s-client-cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DONDEWO_LIBRARY_NAME=ondewo_t2s_client \
  -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build --parallel
sudo cmake --install build
```

Then, in the consuming project:

```cmake
find_package(ondewo_t2s_client CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE ondewo::ondewo_t2s_client)
```

Setting up a development checkout of this repository itself:

```shell
git clone https://github.com/ondewo/ondewo-t2s-client-cpp.git   ## Clone repository
cd ondewo-t2s-client-cpp                                        ## Change into repo directory
make setup_developer_environment_locally              ## Submodules + pre-commit hooks
make build                                            ## Regenerate the stubs and build the library
make test                                             ## Verify the result
```

## Usage

Every message and service stub is reachable through the umbrella header. Include it, or include the single
generated header you need (`api/ondewo/t2s/<file>.grpc.pb.h`) to keep compile times down.

Requests are authenticated with a bearer token passed as gRPC call metadata. The key is the lowercase
`authorization` - gRPC rejects metadata keys containing uppercase characters.

```cpp
#include <cstdlib>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "public-api.h"

int main() {
  // Use grpc::InsecureChannelCredentials() only against a local, unencrypted deployment.
  auto channel = grpc::CreateChannel("t2s.ondewo.com:443", grpc::SslCredentials({}));

  // Replace Services/Request/Response with the service you need - see the API documentation.
  auto stub = ondewo::t2s::Services::NewStub(channel);

  grpc::ClientContext context;
  context.AddMetadata("authorization", "Bearer " + std::string(getenv("ONDEWO_TOKEN")));

  ondewo::t2s::Request request;
  ondewo::t2s::Response response;

  const grpc::Status status = stub->SomeRpc(&context, request, &response);
  if (!status.ok()) {
    return 1;
  }
  return 0;
}
```

## Regenerating the stubs

Generation runs entirely inside the `ondewo-cpp-proto-compiler` docker image, so no protoc, no gRPC plugin and
no network access are needed on the host once the image exists.

```shell
make build
```

That is the whole pipeline, and each step is also a target of its own:

1. `make update_submodules` - `git submodule update --init --recursive`
2. `make checkout_defined_submodule_versions` - check out the tags pinned in the Makefile's Variables chapter
3. `make build_compiler` - build the image from the `ondewo-proto-compiler` submodule
4. `make generate_ondewo_protos` - run the image over the `.proto` tree
5. `make build_library` - configure, compile and install the library with CMake on the host

Step 4 is the contract with the compiler image, and it is a single `docker run`:

```shell
docker run \
  -v $(pwd):/input-volume \
  -v $(pwd):/output-volume \
  ondewo-cpp-proto-compiler ondewo-t2s-api ondewo ondewo_t2s_client
```

The three positional arguments are the proto root relative to the input volume, the sub-directory of that root
whose protos are the compilation entry points, and the CMake target / package / archive name. Imports are
resolved transitively, so `google/` must not be listed - the well-known types already inside `libprotobuf` are
excluded on purpose, since generating them again would break the link on duplicate symbols.

Notes on the volumes:

- The image copies the input volume into an internal scratch directory and compiles there, so the mounted
  `.proto` sources are never modified.
- In the output volume it deletes only what it owns - `api/`, `include/ondewo_t2s_client/`,
  `lib/libondewo_t2s_client.a` and `lib/cmake/ondewo_t2s_client/` - so a proto that was renamed or
  deleted upstream leaves no orphan header behind, and nothing else in the repository is touched.
  `public-api.h` is wholly generated and is simply overwritten.
- `CMakeLists.txt` and `ondewo-client-config.cmake.in` are never overwritten once they exist. The versions used
  for the current build are written to `api/CMakeLists.txt.generated` and
  `api/ondewo-client-config.cmake.in.generated` instead, so you can diff and adopt them after a compiler bump.
- The container runs as root, so `make generate_ondewo_protos` calls `make fix_file_ownership` afterwards,
  which `chown`s the generated files back to you. It may prompt for `sudo`; it is a labelled no-op when you
  are already root or `sudo` is not installed, and never fails the build.

There is no `-it` anywhere in the codegen invocation - it breaks every non-interactive caller with
`cannot attach stdin to a TTY-enabled container because stdin is not a terminal`. Keep it only for the
interactive `--entrypoint /bin/bash` debug command.

## Testing

```shell
make test
```

- `check_build` asserts that every `.proto` under `ondewo-t2s-api/ondewo` produced a matching `.pb.h`.
- `smoke_test` writes a throwaway project that does `find_package(ondewo_t2s_client CONFIG REQUIRED)`,
  includes `public-api.h` and links `ondewo::ondewo_t2s_client`, then compiles and runs it. That is the
  one check that proves the exported CMake package, the umbrella header and the link line all work together
  the way a downstream application uses them.

## Release

See `RELEASE.md` for the release history and the Makefile's Release chapter for the automation
(`make ondewo_release`). Releases are published as GitHub releases and git tags; there is no package registry
for C++.

[//]: # (Generated with the ONDEWO proto compiler - see https://github.com/ondewo/ondewo-proto-compiler)
