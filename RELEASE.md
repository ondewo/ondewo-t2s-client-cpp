# Release History

*****************

## Release ONDEWO T2S C++ Client 6.7.1

### New Features

* [[OND236-120]](https://ondewo.atlassian.net/browse/OND236-120) Offline-token start-up, as the python SDK has it: a handed-off Keycloak OFFLINE refresh token (`refresh_token`) replaces the password. The provider then starts with ONE `grant_type=refresh_token` exchange and never sends a password grant, not even when the token is refused, because Keycloak's brute-force detection counts failed password logins and N processes sharing one technical user would lock it. A Keycloak config needs `password` or `refresh_token`; with both, the refresh token wins.
* [[OND236-120]](https://ondewo.atlassian.net/browse/OND236-120) New header-only `<ondewo/client/keycloak.h>` beside `channel.h`: the Keycloak fields (`keycloak_url`, `realm`, `client_id`, `username`, `password`, `refresh_token`, `token_expiration_in_s`, `verify_ssl`) live on `ondewo::client::ClientConfig`, and the provider attaches `authorization: Bearer <token>` to every call. The token endpoint is reached through a pluggable `KeycloakTransport`; the production transport is libcurl in `<ondewo/client/keycloak_curl.h>` (link `CURL::libcurl` only when you include it).

### Bug Fixes

* [[OND236-120]](https://ondewo.atlassian.net/browse/OND236-120) A quoted error body is cut at 500 characters (UTF-8 code points, as python's `body[:500]`), never splitting a multi-byte character.
* [[OND236-120]](https://ondewo.atlassian.net/browse/OND236-120) A provider whose `token_expiration_in_s` deadline ended its background refresh is still returned by the registry, as in python; only a provider closed with `Stop()` / `Close()` is replaced. The new `closed()` accessor tells the two apart.

### Improvements

* [[OND236-120]](https://ondewo.atlassian.net/browse/OND236-120) A failed token refresh never ends the refreshing: every failure (an unreachable or timed-out token endpoint, a non-2xx answer, a body that is not a JSON object, a missing `access_token`, a non-numeric `expires_in`) is retried with a full-jitter backoff from 5 s doubling up to 300 s, which the background refresh and the per-call refresh share, so a Keycloak outage costs one token request per backoff step instead of one per call. A success resets the backoff.
* [[OND236-120]](https://ondewo.atlassian.net/browse/OND236-120) A token that is still valid keeps being served while its refresh fails; only an EXPIRED token that cannot be refreshed fails, with a Keycloak authentication error that chains the underlying cause. A malformed token response changes no provider state, and an error about a response without an `access_token` names its field names only, never their values.
* [[OND236-120]](https://ondewo.atlassian.net/browse/OND236-120) One provider is shared per credential set: the registry key is a SHA-256 over the Keycloak connection and credential fields (the password and the refresh token enter it only as hashes), so two configs of different tenants never share a provider and all clients of one identity log in once.
* [[OND236-120]](https://ondewo.atlassian.net/browse/OND236-120) The token request times out after 10 s, quoted Keycloak error bodies are cut at 500 characters, each failed attempt logs one WARNING (attempt number, retry delay, remaining token validity) and the first success afterwards one INFO; no error message and no log line carries a password or a token.
* [[OND236-120]](https://ondewo.atlassian.net/browse/OND236-120) The behaviour is pinned by the same test catalogue as the python SDK (`test_keycloak_refresh_resilience.py`, `test_client_config_refresh_token.py`), plus edge cases: every failure kind, the backoff ladder and its bounds, concurrent callers during an outage (one token request), malformed responses, credential-free logs and the credential-keyed registry.

*****************

## Release ONDEWO T2S C++ Client 6.7.0

### Improvements

* Tracking API Version [6.7.0](https://github.com/ondewo/ondewo-t2s-api/releases/tag/6.7.0) ( [Documentation](https://ondewo.github.io/ondewo-t2s-api/) )
* [[OND236-120]](https://ondewo.atlassian.net/browse/OND236-120) Generated with ondewo-proto-compiler 5.17.0 (protoc 35.1). Runtime unchanged: Debian trixie protobuf 3.21.12 / grpc 1.51.1.

*****************

## Release ONDEWO T2S C++ Client 6.6.1

### New Features

* TLS / mutual TLS: new header-only channel helper `ondewo/client/channel.h`, installed beside the generated
  headers and on the exported `ondewo::ondewo_t2s_client` target's include path. `ondewo::client::ClientConfig`
  (`host`, `port`, `grpc_cert`, `grpc_client_cert`, `grpc_client_key`, `use_secure_channel`) takes PEM **content**,
  not file paths. `CreateChannel`, `CreateChannelCredentials`, `ValidateConfig` and `DefaultChannelArguments()`
  report errors as `grpc::Status` `INVALID_ARGUMENT`, never as exceptions.
* An empty `grpc_cert` uses the platform trust store. Half a client identity (certificate without key or key
  without certificate) and a plaintext channel with a client identity are refused before gRPC is called. A
  plaintext channel logs a warning on `std::clog` naming `host:port`. No error message renders a PEM, a key or the
  config; `ToString()` / `operator<<` print the client key as `***REDACTED***`. Bare IPv6 hosts are bracketed;
  CRLF PEMs work.
* Default channel arguments match the ONDEWO Python SDKs: keepalive every 30 s during calls with a 20 s timeout,
  `grpc.http2.max_pings_without_data=2`, reconnect backoff capped at 5 s, 2^31-1 byte send/receive message limit.
  Documented gaps: gRPC 1.51 ignores `grpc.http2.ping_timeout_ms`, and no per-method retry policy is configured.
* README: new section "TLS, mutual TLS and certificates" (modes, loading a PEM from a file, test PKI, security
  notes, troubleshooting).

### Improvements

* `tests/test_tls.cc`: real handshakes against in-process servers with a PKI generated by the openssl CLI at test
  time - TLS, mutual TLS, server rejecting a missing or foreign client certificate, wrong CA, system trust store,
  non-PEM CA (error, not crash), CRLF PEMs, server-name override, `[::1]` - plus the refusal, warning, redaction
  and default-argument cases. `make coverage` now also holds `client/` to 100 % line coverage.
* `tests/test_release_notes.sh` (CTest) pins the release-notes slice, the heading spelling, the `*****` separators
  and non-empty notes for the current version.
* The pinned `ondewo-proto-compiler` submodule moves from 5.15.1 to 5.15.2 (its fixes concern the PHP, Rust and
  JavaScript images and the release automation; the C++ generation is unchanged).
* Tracking API Version [6.6.0](https://github.com/ondewo/ondewo-t2s-api/releases/tag/6.6.0) ( [Documentation](https://ondewo.github.io/ondewo-t2s-api/) )

*****************

## Release ONDEWO T2S C++ Client 6.6.0

### New Features

* Initial release of `ondewo-t2s-client-cpp`, the C++ gRPC client library for the
  [ONDEWO T2S API](https://github.com/ondewo/ondewo-t2s-api) (Text-to-Speech). The stubs are generated
  from the `ondewo-t2s-api` submodule by the `ondewo-cpp-proto-compiler` image of
  [ondewo-proto-compiler](https://github.com/ondewo/ondewo-proto-compiler) 5.15.1, using protoc's
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
