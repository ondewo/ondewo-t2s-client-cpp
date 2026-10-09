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

Apart from the header-only channel helper in `client/`, everything this repository ships is generated:

| Path                            | What it is                                                               |
| ------------------------------- | ------------------------------------------------------------------------ |
| `api/`                          | the generated stubs - `*.pb.h` / `*.pb.cc` and `*.grpc.pb.h` / `*.grpc.pb.cc` |
| `public-api.h`                  | umbrella header that `#include`s every generated header                  |
| `CMakeLists.txt`                | builds the stubs into a static library and installs a CMake package      |
| `ondewo-client-config.cmake.in` | template for the installed `<library>-config.cmake`                      |
| `ondewo-t2s-api/`                   | submodule - the `.proto` source of truth                                 |
| `ondewo-proto-compiler/`        | submodule - the code generator, pinned to a release tag                  |
| `client/ondewo/client/channel.h` | **hand-written** - TLS / mutual-TLS channel factory, see [below](#tls-mutual-tls-and-certificates) |

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

### Install the published release archive

C++ has no package registry, so every [GitHub release](https://github.com/ondewo/ondewo-t2s-client-cpp/releases)
carries the built package as an asset: `ondewo_t2s_client-<version>-<platform>.tar.gz`, plus a
`.sha256` next to it. The archive is the CMake install tree - the static library, the public headers and the
package-config files - so consuming it is one `find_package`, with no compiler run and no Docker.

```shell
version=6.6.0
platform=linux-x86_64     ## uname -s | tr A-Z a-z, then uname -m
archive=ondewo_t2s_client-${version}-${platform}.tar.gz

gh release download "${version}" --repo ondewo/ondewo-t2s-client-cpp --pattern "${archive}*"
sha256sum -c "${archive}.sha256"          ## macOS: shasum -a 256 -c
tar -xzf "${archive}" -C /opt             ## anywhere - the package is relocatable
```

Then point CMake at the extracted directory and consume it exactly as you would a system-installed package:

```cmake
find_package(ondewo_t2s_client CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE ondewo::ondewo_t2s_client)
```

```shell
cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/ondewo_t2s_client-${version}-${platform}
```

`#include "public-api.h"` then reaches the whole API surface; the exported target carries the include
directory and re-finds `Protobuf` and `gRPC` on your machine.

Two things the archive does **not** do, both by construction:

- It is a **binary** artifact for one platform, and protobuf gives
  [no cross-version guarantee](https://protobuf.dev/support/cross-version-runtime-guarantee/) between generated
  code and runtime. `PACKAGE-INFO.txt` inside the archive records the platform, the `protoc`, the
  `libprotobuf`/`libgrpc++` and the compiler it was built with - if your protobuf differs, build from source
  with one of the two options below instead.
- It is not registered with vcpkg, Conan or any other package manager, and there is nothing to `install` from a
  registry. The release asset and the git tag are the whole distribution story.

### Build from source

Using CMake `FetchContent` - no Docker, no install step:

```cmake
include(FetchContent)
# The library, target and package name. Set it BEFORE MakeAvailable: the built-in default is the
# generic `ondewo_grpc_client`, which collides if you pull in more than one ONDEWO C++ client.
set(ONDEWO_LIBRARY_NAME ondewo_t2s_client CACHE STRING "" FORCE)
FetchContent_Declare(
  ondewo_t2s_client
  GIT_REPOSITORY https://github.com/ondewo/ondewo-t2s-client-cpp.git
  GIT_TAG        6.6.0)
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

### Develop on this repository

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

To open the channel with TLS or mutual TLS from PEM content, use `<ondewo/client/channel.h>` - see
[TLS, mutual TLS and certificates](#tls-mutual-tls-and-certificates).

## TLS, mutual TLS and certificates

gRPC encrypts with **TLS**. "SSL" in names such as `grpc::SslCredentials` or `SetSslTargetNameOverride` is legacy
naming; no SSL protocol version is ever negotiated.

The hand-written, header-only helper `<ondewo/client/channel.h>` (source under `client/`, installed next to
the generated headers and linked through the same `ondewo::ondewo_t2s_client` target) builds the channel from a
`ondewo::client::ClientConfig`. It behaves exactly like the other ONDEWO SDKs (reference:
[ondewo-client-utils-python](https://github.com/ondewo/ondewo-client-utils-python)):

| Mode                               | `use_secure_channel` | Config fields                                                          |
|------------------------------------|----------------------|------------------------------------------------------------------------|
| Plaintext (not for production)     | `false`              | none                                                                   |
| TLS, platform trust store          | `true` (default)     | none (`grpc_cert` empty)                                               |
| TLS, custom CA                     | `true`               | `grpc_cert` = PEM of the CA that signed the server certificate         |
| Mutual TLS                         | `true`               | `grpc_cert` (or empty for the trust store) plus `grpc_client_cert` and `grpc_client_key` |

Rules the code enforces:

- The three PEM fields hold **PEM content**, **not file paths**. Read the files yourself (see below).
- `grpc_client_cert` and `grpc_client_key` go together. Setting only one makes `ValidateConfig` /
  `CreateChannelCredentials` / `CreateChannel` return `INVALID_ARGUMENT` **before gRPC is called**: grpc-core does
  not report half an identity, it can `abort()` the whole process. Both empty means server-authenticated TLS.
- `use_secure_channel = false` with a client certificate returns `INVALID_ARGUMENT` instead of silently dropping
  the identity. A plaintext channel otherwise works, and writes a warning naming `host:port` to `std::clog`
  (redirect `std::clog` to route it into your logging).
- Errors are returned as `grpc::Status`, never thrown, so the header works with `-fno-exceptions`. No error message
  renders a PEM, a key or the config; they name the field and `host:port`.
- `ClientConfig::ToString()` / `operator<<` print the certificates as their size only and a non-empty
  `grpc_client_key` as `***REDACTED***` (an empty one stays empty).
- A bare IPv6 literal host is bracketed (`::1` connects to `[::1]:50051`); a bracketed host or one with a scheme
  (`ipv6:[::1]`, `dns:///...`, `unix:/...`) is used as it is.
- The server certificate is verified against `grpc_cert` (or the trust store), and the host you connect to must
  match one of the certificate's subject alternative names (SAN). When you connect by IP and the certificate has no
  IP SAN, set `args.SetSslTargetNameOverride("<name in the SAN>")` on the channel arguments.

```cpp
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>

#include <ondewo/client/channel.h>

#include "public-api.h"

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

int main() {
  ondewo::client::ClientConfig config;
  config.host = "10.0.0.5";
  config.port = "50051";
  config.grpc_cert = ReadFile("certs/ca.pem");
  config.grpc_client_cert = ReadFile("certs/client.pem");  // leave both out for server-authenticated TLS
  config.grpc_client_key = ReadFile("certs/client.key");

  grpc::ChannelArguments args = ondewo::client::DefaultChannelArguments();
  args.SetSslTargetNameOverride("t2s.example.internal");  // only when connecting by IP

  std::shared_ptr<grpc::Channel> channel;
  const grpc::Status status = ondewo::client::CreateChannel(config, args, &channel);
  if (!status.ok()) {
    std::cerr << status.error_message() << '\n';  // never contains a PEM or the key
    return 1;
  }
  // One channel serves every stub of the server: one connection, one TLS handshake.
  auto stub = ondewo::t2s::Text2Speech::NewStub(channel);
  return 0;
}
```

`CreateChannel(config, &channel)` uses `DefaultChannelArguments()` unchanged. Those are the channel options of the
other ONDEWO SDKs: maximum message size 2^31-1 both ways, `keepalive_time_ms=30000`, `keepalive_timeout_ms=20000`,
`keepalive_permit_without_calls=0`, `http2.max_pings_without_data=2`, `http2.ping_timeout_ms=20000` and
`max_reconnect_backoff_ms=5000`. Gaps: gRPC 1.51 (the version the stubs are pinned to) does not know
`grpc.http2.ping_timeout_ms` and ignores it - it waits `keepalive_timeout_ms` for a ping reply instead, which is the
same 20 s; and the per-method retry policy of the Python SDKs (retry idempotent methods only) is not applied, so only
gRPC's transparent retries are active. Add a `grpc.service_config` to the arguments if you need retries.

### A test PKI with openssl

A CA, a server certificate with SANs, and a client certificate with the `clientAuth` extended key usage. For tests
only: the keys are unencrypted. `tests/test_tls.cc` generates the same PKI at test time.

```bash
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 365 \
  -subj "/CN=Test CA" -keyout ca.key -out ca.pem

printf 'subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1\nextendedKeyUsage=serverAuth\n' > server.ext
openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
  -subj "/CN=localhost" -keyout server.key -out server.csr
openssl x509 -req -in server.csr -CA ca.pem -CAkey ca.key -CAcreateserial -days 365 \
  -extfile server.ext -out server.pem

printf 'extendedKeyUsage=clientAuth\n' > client.ext
openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
  -subj "/CN=my-client" -keyout client.key -out client.csr
openssl x509 -req -in client.csr -CA ca.pem -CAkey ca.key -CAcreateserial -days 365 \
  -extfile client.ext -out client.pem

chmod 600 *.key
openssl verify -CAfile ca.pem server.pem client.pem
```

The client then uses `ca.pem` / `client.pem` / `client.key`; a server that requires client certificates uses
`server.pem` / `server.key` and trusts `ca.pem` for its clients.

### TLS security notes

- `ClientConfig` holds the private key in memory as plain text. Load it from a file with mode `0600` or from a
  secret store at startup; never commit it and never hard-code it.
- `ToString()` redacts the key, but any serialization of the config you write yourself (JSON, a settings file)
  carries it in clear text unless you leave it out: treat such a file as a secret.
- Do not log the raw fields; log `config` (its `operator<<`) or `config.HostAndPort()`.

### TLS troubleshooting

A failed handshake comes back as status `UNAVAILABLE` on the first RPC; the cause is in `status.error_message()` and
in the gRPC log (`GRPC_VERBOSITY=debug GRPC_TRACE=tsi` for more):

- **`Ssl handshake failed: SSL_ERROR_SSL: ... certificate verify failed`**: `grpc_cert` is not the CA that issued the server certificate, or the server does not send its intermediate
  certificates. With an empty `grpc_cert` the server's CA is not in the platform trust store.
- **`Peer name <host> is not in peer certificate`** (newer gRPC: `Hostname Verification Check failed`): the host
  you connect to is not in the server certificate's SAN. Connect by a name in the SAN, add the SAN, or set
  `SetSslTargetNameOverride`.
- **`Socket closed`** (or another `UNAVAILABLE`) against a server that requires client certificates: no client
  certificate was presented, or one the server's CA did not issue. The server log names the reason (e.g.
  `peer did not return a certificate` / `certificate verify failed`). Set `grpc_client_cert` / `grpc_client_key`.
- **`empty address list`** with `Could not load any root certificate` / `Invalid cert chain file` /
  `Handshaker factory creation failed` in the gRPC log: `grpc_cert`, `grpc_client_cert` or `grpc_client_key` holds
  something that is not PEM, typically a file path. Pass the file's content instead.
- **`INVALID_ARGUMENT`** from `CreateChannel` itself: half a client identity, or a client identity on a plaintext
  channel - the message says which.

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
- `publish_dry_run` builds the release archive and then consumes it: it extracts the tarball into a throwaway
  tree and builds `tests/package-consume` against nothing but that - `find_package()` is asserted to resolve
  inside the extracted archive, and the whole static library is forced into the link, so an incomplete archive
  fails here rather than after it has been published. It needs no credentials and uploads nothing, and CI runs
  it on every push.

`tests/test_tls.cc` exercises `<ondewo/client/channel.h>` against real in-process gRPC servers with a PKI the
openssl CLI generates at test time (no key is committed): TLS, mutual TLS, a server refusing a client without a
certificate or with one from an unrelated CA, a wrong CA, the system trust store, a non-PEM CA, CRLF PEMs, server
name override and `[::1]` (skipped without IPv6 loopback), plus half a client identity, plaintext with an identity,
the insecure warning and the redaction - no PEM, key or config in any message. `make coverage` holds `client/` to
the same 100 % line floor as `tests/`.

## Release

See `RELEASE.md` for the release history and the Makefile's Release and Package chapters for the automation
(`make ondewo_release`). There is no package registry for C++, so a release is a git tag, a GitHub release,
and the built package attached to it:

| Target                 | What it does                                                                      |
| ---------------------- | --------------------------------------------------------------------------------- |
| `make build_package`   | stages the CMake install tree into `dist/<library>-<version>-<platform>.tar.gz` + `.sha256` |
| `make verify_package`  | extracts that archive and consumes it from an unrelated CMake project              |
| `make publish_dry_run` | both of the above - the credential-free packaging gate, also run by CI             |
| `make publish`         | the dry-run, then `gh release upload` of the archive and its checksum              |

`make release` runs `make publish` after `make push_to_gh`, because `gh release upload` needs the release to
exist. Pushing the version tag additionally starts `.github/workflows/release.yml`, which rebuilds the archive
on a pinned runner, verifies it the same way and attaches it with the `ONDEWO_GH_TOKEN` repository secret -
whichever gets there first wins, and the other replaces its own identical asset. The only credential involved
anywhere is the GitHub token (`GITHUB_GH_TOKEN`, read from `account_github.env` in the devops-accounts repo by
`make ondewo_release`); there is no registry account.

The workflow authenticates with the repository secret **`ONDEWO_GH_TOKEN`** - the same value as
`GITHUB_GH_TOKEN` above. It cannot be called `GITHUB_GH_TOKEN`, because GitHub reserves every secret name
starting with `GITHUB_`; `ONDEWO_GH_TOKEN` is the name every ONDEWO client repository uses for it.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Commit messages follow
[Conventional Commits](https://www.conventionalcommits.org/) (`feat: …`, `fix(scope): …`, `docs: …`); do **not**
write the JIRA ticket prefix by hand - the `giticket` pre-commit hook reads it from the branch name and prepends
`[<ticket>]` on commit.

## License

Apache License 2.0 — see [LICENSE](LICENSE).

[//]: # (Generated with the ONDEWO proto compiler - see https://github.com/ondewo/ondewo-proto-compiler)
