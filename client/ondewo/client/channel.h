// Copyright 2020-2026 ONDEWO GmbH
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Channel factory for the ONDEWO gRPC C++ clients: plaintext, TLS and mutual TLS.
//
// HAND-WRITTEN, not generated. It lives under client/, outside api/ (which every regeneration
// wipes), and the root CMakeLists.txt installs it next to the generated headers, so a consumer
// includes it as <ondewo/client/channel.h>. Header-only on purpose: nothing is added to the
// static archive, and the test suite measures its coverage directly.
//
// The same contract as every other ONDEWO SDK (reference: ondewo-client-utils-python 4.1.x):
//   - grpc_cert / grpc_client_cert / grpc_client_key hold PEM CONTENT, never a file path.
//   - grpc_client_cert and grpc_client_key go together. Half a pair is refused HERE, before any
//     gRPC call: grpc-core does not report half an identity, it CHECK-fails and abort()s the
//     whole process.
//   - Plaintext with a client identity is refused instead of silently dropping the identity.
//   - No error message and no ToString() renders a PEM, a key or the whole config.
//
// Errors are reported as grpc::Status (INVALID_ARGUMENT), never thrown, so the header works in
// code bases compiled with -fno-exceptions.

#pragma once

#include <climits>
#include <cstddef>
#include <iostream>
#include <memory>
#include <ostream>
#include <string>

#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/channel_arguments.h>
#include <grpcpp/support/status.h>

namespace ondewo {
namespace client {

// What ToString() prints in place of a non-empty grpc_client_key.
inline constexpr const char* kRedacted = "***REDACTED***";

// Largest message the default channel arguments let through in either direction (2**31 - 1),
// the same limit as the Python SDKs' MAX_MESSAGE_LENGTH.
inline constexpr int kMaxMessageLength = INT_MAX;

// Connection settings of one ONDEWO server.
struct ClientConfig {
  // Host name or IP address. A bare IPv6 literal ("::1") is bracketed by HostAndPort().
  std::string host;
  std::string port;
  // PEM content of the CA (or self-signed server certificate) to trust. Empty: the platform's
  // default trust store (gRPC's bundled roots, or GRPC_DEFAULT_SSL_ROOTS_FILE_PATH).
  std::string grpc_cert;
  // PEM content of the client certificate chain for mutual TLS. Set together with
  // grpc_client_key, or leave both empty for server-authenticated TLS.
  std::string grpc_client_cert;
  // PEM content of the client's private key. Never rendered by ToString().
  std::string grpc_client_key;
  // false opens a PLAINTEXT channel - not for production, and refused with a client identity.
  bool use_secure_channel = true;

  // "host:port", with a bare IPv6 literal bracketed ("[::1]:50051"); a host that is already
  // bracketed or carries a scheme ("ipv6:[::1]", "dns:///...", "unix:/...") is left alone.
  std::string HostAndPort() const;

  // A one-line rendering safe for logs: the certificates appear as their size only, and a
  // non-empty grpc_client_key as ***REDACTED*** (an empty one stays empty).
  std::string ToString() const;
};

inline std::ostream& operator<<(std::ostream& out, const ClientConfig& config) {
  return out << config.ToString();
}

namespace internal {

// True for a bare IPv6 literal: hex digits, ':' and '.' (an embedded IPv4 tail) with at least
// two colons, optionally followed by a %zone. Anything else - a name, an IPv4 address, a
// bracketed literal, "scheme:..." - is false. Portable (no inet_pton), so Windows needs no
// winsock header for it.
inline bool IsBareIpv6Literal(const std::string& host) {
  const std::string address = host.substr(0, host.find('%'));
  std::size_t colons = 0;
  for (const char c : address) {
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!hex && c != ':' && c != '.') return false;
    if (c == ':') ++colons;
  }
  return colons >= 2;
}

inline std::string SizeOnly(const std::string& pem) {
  return pem.empty() ? std::string() : "<" + std::to_string(pem.size()) + " bytes>";
}

inline grpc::Status Invalid(const std::string& message) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, message);
}

}  // namespace internal

inline std::string ClientConfig::HostAndPort() const {
  return internal::IsBareIpv6Literal(host) ? "[" + host + "]:" + port : host + ":" + port;
}

inline std::string ClientConfig::ToString() const {
  return "ClientConfig{host=" + host + ", port=" + port +
         ", grpc_cert=" + internal::SizeOnly(grpc_cert) +
         ", grpc_client_cert=" + internal::SizeOnly(grpc_client_cert) +
         ", grpc_client_key=" + (grpc_client_key.empty() ? "" : kRedacted) +
         ", use_secure_channel=" + (use_secure_channel ? "true" : "false") + "}";
}

// Checks the config without touching gRPC: both-or-neither for the client identity, and no
// client identity on a plaintext channel. The message names fields and host:port only.
inline grpc::Status ValidateConfig(const ClientConfig& config) {
  if (config.grpc_client_cert.empty() != config.grpc_client_key.empty()) {
    return internal::Invalid("ClientConfig for " + config.HostAndPort() +
                             " sets only one of grpc_client_cert and grpc_client_key; set both to"
                             " use mutual TLS, or neither.");
  }
  if (!config.use_secure_channel && !config.grpc_client_cert.empty()) {
    return internal::Invalid("ClientConfig for " + config.HostAndPort() +
                             " carries a client certificate for mutual TLS, but use_secure_channel"
                             " is false and would send it nowhere; use a secure channel.");
  }
  return grpc::Status::OK;
}

// The channel arguments every ONDEWO SDK uses (see ondewo-client-utils-python
// _DEFAULT_GRPC_OPTIONS). Adjust the returned object and pass it to CreateChannel() to add or
// override arguments, e.g. args.SetSslTargetNameOverride("nlu.example.internal").
inline grpc::ChannelArguments DefaultChannelArguments() {
  grpc::ChannelArguments args;
  args.SetMaxSendMessageSize(kMaxMessageLength);
  args.SetMaxReceiveMessageSize(kMaxMessageLength);
  // Pings only while calls are active, and at most 2 without data: a default grpc-core server
  // answers a client that keeps pinging a silent stream with GOAWAY "too_many_pings".
  args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 30000);
  args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 20000);
  args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 0);
  args.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 2);
  // Newer grpc-core waits http2.ping_timeout_ms (default 60 s) for a ping reply; gRPC 1.51 does
  // not know this argument, ignores it and uses keepalive_timeout_ms for that wait instead.
  args.SetInt("grpc.http2.ping_timeout_ms", 20000);
  // Reconnect within 5 s of a server coming back instead of grpc-core's default of up to 120 s.
  args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 5000);
  return args;
}

// The credentials for config: insecure, TLS (system roots or grpc_cert) or mutual TLS. Validates
// first; on error *credentials is left untouched and gRPC is never called.
inline grpc::Status CreateChannelCredentials(const ClientConfig& config,
                                             std::shared_ptr<grpc::ChannelCredentials>* credentials) {
  const grpc::Status status = ValidateConfig(config);
  if (!status.ok()) return status;
  if (!config.use_secure_channel) {
    std::clog << "WARNING: ondewo::client: using an INSECURE (plaintext) gRPC channel to "
              << config.HostAndPort() << '\n';
    *credentials = grpc::InsecureChannelCredentials();
    return grpc::Status::OK;
  }
  grpc::SslCredentialsOptions options;
  // An empty pem_root_certs makes gRPC use the default trust store. The identity is either both
  // fields or none (ValidateConfig), so grpc-core never sees half a pair or an empty PEM.
  options.pem_root_certs = config.grpc_cert;
  options.pem_cert_chain = config.grpc_client_cert;
  options.pem_private_key = config.grpc_client_key;
  *credentials = grpc::SslCredentials(options);
  return grpc::Status::OK;
}

// Opens a channel to config.HostAndPort() with the given channel arguments. On error *channel
// is left untouched and nothing is opened.
inline grpc::Status CreateChannel(const ClientConfig& config, const grpc::ChannelArguments& args,
                                  std::shared_ptr<grpc::Channel>* channel) {
  std::shared_ptr<grpc::ChannelCredentials> credentials;
  const grpc::Status status = CreateChannelCredentials(config, &credentials);
  if (!status.ok()) return status;
  *channel = grpc::CreateCustomChannel(config.HostAndPort(), credentials, args);
  return grpc::Status::OK;
}

// Opens a channel with DefaultChannelArguments(). One channel serves every service stub of a
// server: build it once and hand it to each XStub(channel) - one connection, one handshake.
inline grpc::Status CreateChannel(const ClientConfig& config, std::shared_ptr<grpc::Channel>* channel) {
  return CreateChannel(config, DefaultChannelArguments(), channel);
}

}  // namespace client
}  // namespace ondewo
