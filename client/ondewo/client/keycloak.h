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

// Keycloak bearer-token authentication for the ONDEWO gRPC C++ clients.
//
// HAND-WRITTEN, not generated; header-only and installed beside channel.h, like it. It implements
// the cross-language ONDEWO SDK Keycloak specification (reference: the python SDKs'
// ondewo/<api>/utils/keycloak.py), so every SDK behaves the same against one realm:
//
//   - Start-up: with ClientConfig::refresh_token set (a handed-off OFFLINE token) ONE refresh grant,
//     otherwise ONE password grant (grant_type=password, scope=offline_access). A refused token is
//     NEVER answered with a password grant - Keycloak's brute-force detection counts failed logins,
//     and N containers sharing one technical user would lock it.
//   - A background thread refreshes the token 30 s before it expires. A failed refresh never ends
//     it: failures back off with full jitter from 5 s up to 300 s.
//   - Every gRPC call reads the token through the same lock (KeycloakMetadataPlugin): a token that
//     is still valid is served while its refresh fails, the lazy path honours the SAME backoff (an
//     outage never turns every call into a token request), and only an EXPIRED token fails the
//     call, before it is sent, with the KeycloakAuthenticationError text (the plugin answers
//     UNAUTHENTICATED; gRPC 1.51 reports every plugin failure to the caller as UNAVAILABLE).
//   - One provider (one login) per credential set, shared by every channel built from it.
//   - No password, refresh token or access token reaches a log line, an error message, ToString()
//     or the registry key.
//
// The HTTP exchange is a pluggable KeycloakTransport, so this header needs nothing beyond gRPC
// and protobuf. <ondewo/client/keycloak_curl.h> provides the production transport (libcurl);
// tests inject a fake one. This header uses C++ exceptions (the typed KeycloakAuthenticationError);
// the channel helper itself stays usable with -fno-exceptions.
//
// Typical use (see README.md):
//
//   #include <ondewo/client/keycloak_curl.h>
//   ondewo::client::ClientConfig config;   // host, port, grpc_cert, ... plus
//   config.keycloak_url = "https://host/auth";
//   config.realm = "ondewo-ccai-platform";
//   config.client_id = "ondewo-t2s-cai-sdk-public";
//   config.username = "technical-user@example.com";
//   config.refresh_token = offline_token;   // or config.password = ...
//   std::shared_ptr<grpc::Channel> channel;
//   grpc::Status status = ondewo::client::CreateAuthenticatedChannel(
//       config, ondewo::client::CurlKeycloakProviderOptions(), &channel);

#pragma once

#if !defined(__cpp_exceptions) && !defined(__EXCEPTIONS) && !defined(_CPPUNWIND)
#error "<ondewo/client/keycloak.h> reports failures as KeycloakAuthenticationError and needs C++ exceptions"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <iostream>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#endif
#if __has_include(<cxxabi.h>)
#include <cxxabi.h>
#define ONDEWO_CLIENT_KEYCLOAK_HAVE_CXXABI 1
#endif

#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/status.h>

#include <ondewo/client/channel.h>

namespace ondewo {
namespace client {

// ------------------------------------------------------------------------------------------
// Constants. Identical in every ONDEWO SDK; tests shrink them through KeycloakProviderOptions.

struct KeycloakTimings {
  // Refresh this long before the access token expires.
  double expiry_leeway_s = 30.0;
  // Floor of any background wait, so an expires_in of 0 cannot become a hot loop.
  double min_refresh_delay_s = 1.0;
  // First retry after a failed refresh; the backoff ceiling doubles from here ...
  double retry_base_delay_s = 5.0;
  // ... up to this.
  double retry_max_delay_s = 300.0;
  // 5 * 2^6 = 320 >= 300; caps the exponent so a huge failure count cannot overflow.
  int max_retry_exponent = 6;
  // The whole token-endpoint call (connect + read). Must stay below expiry_leeway_s: the provider
  // lock is held for one call.
  double http_timeout_s = 10.0;
  // A quoted error body is truncated to this many bytes.
  std::size_t max_error_body_chars = 500;
  // Bounded wait for the background thread on Stop().
  double thread_join_timeout_s = 5.0;
};

// ------------------------------------------------------------------------------------------
// Errors

// Every failure of a login, a refresh or the per-call read of an expired token. cause() holds the
// underlying exception where there is one. The message never contains a credential.
class KeycloakAuthenticationError : public std::runtime_error {
 public:
  explicit KeycloakAuthenticationError(const std::string& message, std::exception_ptr cause = nullptr)
      : std::runtime_error(message), cause_(std::move(cause)) {}

  const std::exception_ptr& cause() const noexcept { return cause_; }

 private:
  std::exception_ptr cause_;
};

// What a KeycloakTransport throws when the token endpoint could not be reached at all (DNS,
// connection refused, TLS handshake, timeout). kind() is a short class name such as
// "ConnectionError" or "Timeout"; it and what() are quoted in the resulting
// KeycloakAuthenticationError, so neither may carry a credential.
class KeycloakTransportError : public std::runtime_error {
 public:
  KeycloakTransportError(std::string kind, const std::string& message)
      : std::runtime_error(message), kind_(std::move(kind)) {}

  const std::string& kind() const noexcept { return kind_; }

 private:
  std::string kind_;
};

// ------------------------------------------------------------------------------------------
// Transport and logging seams

// One application/x-www-form-urlencoded POST to the token endpoint.
struct KeycloakHttpRequest {
  std::string url;
  // The form fields, in the order they are sent.
  std::vector<std::pair<std::string, std::string>> form;
  double timeout_s = 10.0;
  // Verify the token endpoint's TLS certificate (ClientConfig::verify_ssl), for THIS request only.
  bool verify_ssl = true;
};

struct KeycloakHttpResponse {
  long status_code = 0;
  std::string body;
};

// Sends one request and returns the answer, whatever its status. Throws KeycloakTransportError when
// no answer arrived; any other exception is treated as a bug and wrapped, never swallowed.
using KeycloakTransport = std::function<KeycloakHttpResponse(const KeycloakHttpRequest&)>;

enum class KeycloakLogLevel { kDebug, kInfo, kWarning };

// Receives every log line of a provider. Called with the provider's lock held: it must not call
// back into the provider.
using KeycloakLogSink = std::function<void(KeycloakLogLevel, const std::string&)>;

// The default sink: INFO and WARNING lines to std::clog with the level token first, DEBUG dropped.
inline void DefaultKeycloakLogSink(KeycloakLogLevel level, const std::string& message) {
  if (level == KeycloakLogLevel::kDebug) return;
  std::clog << (level == KeycloakLogLevel::kWarning ? "WARNING" : "INFO") << ": ondewo::client: " << message
            << '\n';
}

// ------------------------------------------------------------------------------------------
// Provider configuration

// The credential set of one provider. Field order is the cross-SDK order; refresh_token is LAST and
// fields are only ever appended, because aggregate initialisation is positional.
struct KeycloakCredentials {
  std::string keycloak_url;
  std::string realm;
  std::string client_id;
  std::string username;
  std::string password;
  std::optional<int> token_expiration_in_s;
  bool verify_ssl = true;
  std::string refresh_token;
};

inline KeycloakCredentials KeycloakCredentialsFrom(const ClientConfig& config) {
  return KeycloakCredentials{config.keycloak_url, config.realm,    config.client_id,
                             config.username,     config.password, config.token_expiration_in_s,
                             config.verify_ssl,   config.refresh_token};
}

// How a provider talks to Keycloak and keeps time. Only transport is required; every other member
// is a test seam with a production default.
struct KeycloakProviderOptions {
  // The token-endpoint HTTP client (CurlKeycloakTransport() in production).
  KeycloakTransport transport;
  // MONOTONIC seconds. Default: std::chrono::steady_clock.
  std::function<double()> time_fn;
  // Jitter fraction in [0, 1], consulted only after a failure. Default: a seeded mt19937_64.
  std::function<double()> random_fn;
  // Waits up to the given seconds for Stop(); returns true when stopped. Default: a condition
  // variable signalled by Stop(). Tests replace it to drive the background loop on a fake clock.
  std::function<bool(double)> wait_fn;
  // false: no background thread (the per-call path still refreshes on demand).
  bool start_background_refresh = true;
  // Default: DefaultKeycloakLogSink.
  KeycloakLogSink log_sink;
  KeycloakTimings timings;
};

class KeycloakTokenProvider;

namespace internal {

// Runs the background refresh loop on the CALLING thread until the provider is stopped or gone.
// The provider's own thread runs exactly this; tests call it directly with a scripted wait_fn.
void RunKeycloakRefreshLoop(const std::weak_ptr<KeycloakTokenProvider>& weak_provider);

// The full-jitter backoff: uniform in [base, min(base * 2^(n-1), max)], n <= 0 counts as 1, the
// fraction is clamped into [0, 1] (NaN counts as 0) and a huge n cannot overflow.
inline double KeycloakBackoffDelay(long long consecutive_failures, double fraction,
                                   const KeycloakTimings& timings = KeycloakTimings()) {
  const long long exponent =
      std::min<long long>(std::max<long long>(consecutive_failures, 1) - 1, timings.max_retry_exponent);
  const double ceiling =
      std::min(std::ldexp(timings.retry_base_delay_s, static_cast<int>(exponent)), timings.retry_max_delay_s);
  const double clamped = fraction >= 0.0 ? std::min(fraction, 1.0) : 0.0;
  return timings.retry_base_delay_s + clamped * (ceiling - timings.retry_base_delay_s);
}

// One decimal, as every SDK prints seconds.
inline std::string FormatSeconds(double seconds) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.1f", seconds);
  return buffer;
}

inline std::string DemangledName(const std::type_info& type) {
#if defined(ONDEWO_CLIENT_KEYCLOAK_HAVE_CXXABI)
  int status = 0;
  std::unique_ptr<char, void (*)(void*)> demangled(abi::__cxa_demangle(type.name(), nullptr, nullptr, &status),
                                                   std::free);
  if (status == 0 && demangled) return demangled.get();
#endif
  return type.name();  // GCOVR_EXCL_LINE: only reached without the Itanium C++ ABI
}

// "<Type>: <message>" of an exception, the form every failure is quoted in.
inline std::string DescribeException(const std::exception_ptr& error) {
  try {
    std::rethrow_exception(error);
  } catch (const KeycloakAuthenticationError& exception) {
    return std::string("KeycloakAuthenticationError: ") + exception.what();
  } catch (const KeycloakTransportError& exception) {
    return exception.kind() + ": " + exception.what();
  } catch (const std::exception& exception) {
    return DemangledName(typeid(exception)) + ": " + exception.what();
  } catch (...) {
    return "UnknownException: an exception that is not a std::exception";
  }
}

// body truncated to max_bytes, cut back to a UTF-8 character boundary.
inline std::string TruncateBody(const std::string& body, std::size_t max_bytes) {
  if (body.size() <= max_bytes) return body;
  std::size_t end = max_bytes;
  while (end > 0 && (static_cast<unsigned char>(body[end]) & 0xC0) == 0x80) --end;
  return body.substr(0, end);
}

// Parses a numeric string as python's float() does for the forms Keycloak could send
// ("300", " 300 ", "+1.5e2"); hexadecimal, NaN and trailing garbage are refused.
inline std::optional<double> ParseNumericString(const std::string& text) {
  if (text.find_first_of("xXnN") != std::string::npos) return std::nullopt;
  std::istringstream stream(text);
  stream.imbue(std::locale::classic());
  double value = 0.0;
  stream >> std::skipws >> value;
  if (stream.fail()) return std::nullopt;
  stream >> std::ws;
  if (!stream.eof()) return std::nullopt;
  return value;
}

inline const char* JsonTypeName(const google::protobuf::Value& value) {
  switch (value.kind_case()) {
    case google::protobuf::Value::kListValue:
      return "array";
    case google::protobuf::Value::kStringValue:
      return "string";
    case google::protobuf::Value::kNumberValue:
      return "number";
    case google::protobuf::Value::kBoolValue:
      return "boolean";
    default:
      return "null";
  }
}

// SHA-256 (FIPS 180-4) as lowercase hex. Self-contained so the header needs no crypto library.
inline std::string Sha256Hex(const std::string& input) {
  static constexpr std::uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::string message = input;
  const std::uint64_t bit_length = static_cast<std::uint64_t>(input.size()) * 8U;
  message.push_back(static_cast<char>(0x80));
  while (message.size() % 64 != 56) message.push_back('\0');
  for (int shift = 56; shift >= 0; shift -= 8) message.push_back(static_cast<char>((bit_length >> shift) & 0xFF));
  const auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
  for (std::size_t offset = 0; offset < message.size(); offset += 64) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = 0;
      for (int b = 0; b < 4; ++b) w[i] = (w[i] << 8) | static_cast<unsigned char>(message[offset + 4 * i + b]);
    }
    for (int i = 16; i < 64; ++i) {
      const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const std::uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string hex;
  for (const std::uint32_t word : h)
    for (int shift = 28; shift >= 0; shift -= 4) hex.push_back(kHex[(word >> shift) & 0xF]);
  return hex;
}

// The stop flag of one provider, shared with its thread so neither keeps the other alive.
class KeycloakStopSignal {
 public:
  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
    }
    changed_.notify_all();
  }

  bool IsStopped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopped_;
  }

  // true when stopped before (or while) waiting seconds.
  bool WaitFor(double seconds) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, std::chrono::duration<double>(seconds), [this] { return stopped_; });
  }

  void MarkFinished() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      finished_ = true;
    }
    changed_.notify_all();
  }

  bool IsFinished() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return finished_;
  }

  bool WaitUntilFinished(double seconds) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, std::chrono::duration<double>(seconds), [this] { return finished_; });
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool stopped_ = false;
  bool finished_ = false;
};

}  // namespace internal

// ------------------------------------------------------------------------------------------
// The token provider

class KeycloakTokenProvider {
  // Only Create() constructs one: the background thread needs the owning shared_ptr.
  struct ConstructionKey {};

 public:
  // Acquires the first token (one refresh grant with credentials.refresh_token when set, else one
  // password grant), then starts the background refresh. Throws KeycloakAuthenticationError when
  // the first token cannot be acquired (no retry, no password fallback, no thread started), and
  // std::invalid_argument when options.transport is empty.
  static std::shared_ptr<KeycloakTokenProvider> Create(KeycloakCredentials credentials,
                                                       KeycloakProviderOptions options) {
    if (!options.transport) {
      throw std::invalid_argument(
          "KeycloakProviderOptions.transport is empty; include <ondewo/client/keycloak_curl.h> and use "
          "CurlKeycloakProviderOptions(), or pass a KeycloakTransport of your own");
    }
    auto provider = std::make_shared<KeycloakTokenProvider>(ConstructionKey{}, std::move(credentials),
                                                            std::move(options));
    {
      std::lock_guard<std::mutex> lock(provider->mutex_);
      provider->AcquireInitialToken();
      if (provider->credentials_.token_expiration_in_s) {
        provider->login_deadline_ = provider->time_fn_() + *provider->credentials_.token_expiration_in_s;
      }
    }
    if (provider->start_background_refresh_) provider->StartBackgroundRefresh(provider);
    return provider;
  }

  KeycloakTokenProvider(ConstructionKey, KeycloakCredentials credentials, KeycloakProviderOptions options)
      : credentials_(std::move(credentials)),
        transport_(std::move(options.transport)),
        time_fn_(options.time_fn ? std::move(options.time_fn) : DefaultTime),
        random_fn_(std::move(options.random_fn)),
        wait_fn_(std::move(options.wait_fn)),
        log_sink_(options.log_sink ? std::move(options.log_sink) : KeycloakLogSink(DefaultKeycloakLogSink)),
        timings_(options.timings),
        start_background_refresh_(options.start_background_refresh),
        stop_(std::make_shared<internal::KeycloakStopSignal>()),
        refresh_token_(credentials_.refresh_token) {
    while (!credentials_.keycloak_url.empty() && credentials_.keycloak_url.back() == '/') {
      credentials_.keycloak_url.pop_back();
    }
    token_endpoint_ = credentials_.keycloak_url + "/realms/" + credentials_.realm + "/protocol/openid-connect/token";
    if (!random_fn_) {
      auto engine = std::make_shared<std::mt19937_64>(std::random_device{}());
      random_fn_ = [engine] { return std::uniform_real_distribution<double>(0.0, 1.0)(*engine); };
    }
  }

  KeycloakTokenProvider(const KeycloakTokenProvider&) = delete;
  KeycloakTokenProvider& operator=(const KeycloakTokenProvider&) = delete;

  ~KeycloakTokenProvider() { Stop(); }

  // ("authorization", "Bearer <access token>") for the next call, refreshing first when the token
  // is inside the leeway and no backoff is running. Serves a still-valid token when its refresh
  // fails; throws KeycloakAuthenticationError only for an expired one. Concurrent callers are
  // single-flight: one request at a time, everyone else waits for it and reads its outcome.
  std::pair<std::string, std::string> AuthorizationMetadata() {
    std::lock_guard<std::mutex> lock(mutex_);
    RefreshIfWithinWindow(time_fn_());
    return {"authorization", "Bearer " + access_token_};
  }

  // Ends the background refresh: idempotent, safe from any state and from the refresh thread
  // itself, interrupts a pending (backoff) wait so no further request is sent, and waits for the
  // thread at most KeycloakTimings::thread_join_timeout_s. The per-call path keeps working.
  void Stop() {
    stop_->Stop();
    std::thread thread;
    {
      std::lock_guard<std::mutex> lock(thread_mutex_);
      thread.swap(thread_);
    }
    if (!thread.joinable()) return;
    if (thread.get_id() == std::this_thread::get_id() ||
        !stop_->WaitUntilFinished(timings_.thread_join_timeout_s)) {
      thread.detach();
      return;
    }
    thread.join();
  }

  void Close() { Stop(); }

  bool stopped() const { return stop_->IsStopped(); }

  // True while the background thread runs.
  bool background_refresh_running() const {
    std::lock_guard<std::mutex> lock(thread_mutex_);
    return thread_.joinable() && !stop_->IsFinished();
  }

  // Read-only views of the state, for diagnostics and tests. Never log access_token().
  std::string access_token() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return access_token_;
  }
  double access_token_expires_at() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return access_token_expires_at_;
  }
  std::optional<double> login_deadline() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return login_deadline_;
  }
  int consecutive_refresh_failures() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return consecutive_refresh_failures_;
  }
  double next_refresh_attempt_at() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_refresh_attempt_at_;
  }
  const std::string& token_endpoint() const { return token_endpoint_; }
  const std::string& client_id() const { return credentials_.client_id; }
  const std::string& username() const { return credentials_.username; }

 private:
  friend void internal::RunKeycloakRefreshLoop(const std::weak_ptr<KeycloakTokenProvider>& weak_provider);

  static double DefaultTime() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  void Log(KeycloakLogLevel level, const std::string& message) const { log_sink_(level, message); }

  void StartBackgroundRefresh(const std::shared_ptr<KeycloakTokenProvider>& self) {
    const std::weak_ptr<KeycloakTokenProvider> weak = self;
    const std::shared_ptr<internal::KeycloakStopSignal> stop = stop_;
    const std::string name = "keycloak-token-refresh-" + credentials_.client_id;
    std::lock_guard<std::mutex> lock(thread_mutex_);
    thread_ = std::thread([weak, stop, name] {
#if defined(__linux__)
      // The kernel keeps 15 characters of a thread name.
      pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
#endif
      internal::RunKeycloakRefreshLoop(weak);
      stop->MarkFinished();
    });
  }

  void AcquireInitialToken() {
    const bool offline = !refresh_token_.empty();
    try {
      if (offline) {
        Refresh();
      } else {
        Login();
      }
    } catch (const KeycloakAuthenticationError&) {
      throw;
    } catch (...) {
      const std::exception_ptr error = std::current_exception();
      throw KeycloakAuthenticationError(std::string("Keycloak token ") + (offline ? "refresh" : "login") +
                                            " failed (" + internal::DescribeException(error) + ")",
                                        error);
    }
  }

  // The decision table every SDK shares. Called with mutex_ held.
  void RefreshIfWithinWindow(double now) {
    if (login_deadline_ && now >= *login_deadline_) return;
    if (now < access_token_expires_at_ - timings_.expiry_leeway_s) return;
    if (now < next_refresh_attempt_at_) {
      if (now < access_token_expires_at_) return;
      throw KeycloakAuthenticationError(
          "The Keycloak access token has expired and the refresh is backing off after " +
          std::to_string(consecutive_refresh_failures_) + " consecutive failure(s); the next attempt is due in " +
          internal::FormatSeconds(next_refresh_attempt_at_ - now) + "s");
    }
    std::exception_ptr failure;
    try {
      Refresh();
    } catch (...) {
      failure = std::current_exception();
    }
    if (!failure) {
      RecordRefreshSuccess();
      return;
    }
    RecordRefreshFailure(failure);
    if (time_fn_() < access_token_expires_at_) return;
    try {
      std::rethrow_exception(failure);
    } catch (const KeycloakAuthenticationError&) {
      throw;
    } catch (...) {
      throw KeycloakAuthenticationError(
          "Keycloak token refresh failed (" + internal::DescribeException(failure) + ")", failure);
    }
  }

  void RecordRefreshFailure(const std::exception_ptr& failure) {
    const double now = time_fn_();
    ++consecutive_refresh_failures_;
    const double delay = internal::KeycloakBackoffDelay(consecutive_refresh_failures_, random_fn_(), timings_);
    next_refresh_attempt_at_ = now + delay;
    const double remaining = access_token_expires_at_ - now;
    const std::string validity =
        remaining > 0 ? "the current access token stays valid for " + internal::FormatSeconds(remaining) + "s"
                      : std::string("the current access token has expired");
    Log(KeycloakLogLevel::kWarning, "Keycloak token refresh failed (attempt " +
                                        std::to_string(consecutive_refresh_failures_) + "); retrying in " +
                                        internal::FormatSeconds(delay) + "s, " + validity + " (" +
                                        internal::DescribeException(failure) + ")");
  }

  void RecordRefreshSuccess() {
    if (consecutive_refresh_failures_ > 0) {
      Log(KeycloakLogLevel::kInfo, "Keycloak token refresh recovered after " +
                                       std::to_string(consecutive_refresh_failures_) + " failed attempt(s)");
    }
    consecutive_refresh_failures_ = 0;
    next_refresh_attempt_at_ = 0.0;
  }

  void Login() {
    StoreTokens(PostTokenRequest({{"grant_type", "password"},
                                  {"client_id", credentials_.client_id},
                                  {"username", credentials_.username},
                                  {"password", credentials_.password},
                                  {"scope", "offline_access"}},
                                 "login"));
  }

  void Refresh() {
    StoreTokens(PostTokenRequest(
        {{"grant_type", "refresh_token"}, {"client_id", credentials_.client_id}, {"refresh_token", refresh_token_}},
        "refresh"));
  }

  google::protobuf::Struct PostTokenRequest(std::vector<std::pair<std::string, std::string>> form,
                                            const std::string& action) {
    KeycloakHttpRequest request;
    request.url = token_endpoint_;
    request.form = std::move(form);
    request.timeout_s = timings_.http_timeout_s;
    request.verify_ssl = credentials_.verify_ssl;
    KeycloakHttpResponse response;
    try {
      response = transport_(request);
    } catch (const KeycloakTransportError&) {
      const std::exception_ptr error = std::current_exception();
      throw KeycloakAuthenticationError("Keycloak token " + action + " could not reach the token endpoint (" +
                                            internal::DescribeException(error) + ")",
                                        error);
    }
    const std::string status = std::to_string(response.status_code);
    if (response.status_code < 200 || response.status_code >= 300) {
      throw KeycloakAuthenticationError("Keycloak token " + action + " failed with status " + status + ": " +
                                        internal::TruncateBody(response.body, timings_.max_error_body_chars));
    }
    google::protobuf::Value body;
    if (response.body.empty() || !google::protobuf::util::JsonStringToMessage(response.body, &body).ok()) {
      throw KeycloakAuthenticationError("Keycloak token " + action + " returned status " + status +
                                        " with a body that is not JSON");
    }
    if (body.kind_case() != google::protobuf::Value::kStructValue) {
      throw KeycloakAuthenticationError("Keycloak token " + action + " returned a JSON " +
                                        internal::JsonTypeName(body) + " instead of an object");
    }
    return body.struct_value();
  }

  // Validates the whole answer first, then assigns: a malformed answer changes no state.
  void StoreTokens(const google::protobuf::Struct& payload) {
    const auto& fields = payload.fields();
    const auto access = fields.find("access_token");
    if (access == fields.end() || access->second.kind_case() != google::protobuf::Value::kStringValue ||
        access->second.string_value().empty()) {
      std::vector<std::string> names;
      for (const auto& field : fields) names.push_back(field.first);
      std::sort(names.begin(), names.end());
      std::string rendered;
      for (const std::string& name : names) rendered += (rendered.empty() ? "'" : ", '") + name + "'";
      throw KeycloakAuthenticationError(
          "Keycloak token response did not contain an access_token; the response carried the fields [" + rendered +
          "] (values withheld: a token response is credential material)");
    }
    double expires_in = 0.0;
    const auto expires = fields.find("expires_in");
    if (expires != fields.end()) {
      std::optional<double> parsed;
      switch (expires->second.kind_case()) {
        case google::protobuf::Value::kNumberValue:
          parsed = expires->second.number_value();
          break;
        case google::protobuf::Value::kStringValue:
          parsed = internal::ParseNumericString(expires->second.string_value());
          break;
        case google::protobuf::Value::kBoolValue:
          parsed = expires->second.bool_value() ? 1.0 : 0.0;
          break;
        default:
          break;
      }
      if (!parsed || std::isnan(*parsed)) {
        throw KeycloakAuthenticationError("Keycloak token response carried a non-numeric expires_in (value withheld)");
      }
      expires_in = *parsed;
    }
    access_token_ = access->second.string_value();
    const auto refresh = fields.find("refresh_token");
    if (refresh != fields.end() && refresh->second.kind_case() == google::protobuf::Value::kStringValue &&
        !refresh->second.string_value().empty()) {
      refresh_token_ = refresh->second.string_value();
    }
    access_token_expires_at_ = time_fn_() + std::max(expires_in, 0.0);
  }

  KeycloakCredentials credentials_;
  KeycloakTransport transport_;
  std::function<double()> time_fn_;
  std::function<double()> random_fn_;
  std::function<bool(double)> wait_fn_;
  KeycloakLogSink log_sink_;
  KeycloakTimings timings_;
  bool start_background_refresh_;
  std::shared_ptr<internal::KeycloakStopSignal> stop_;
  std::string token_endpoint_;

  // Guarded by mutex_.
  mutable std::mutex mutex_;
  std::string access_token_;
  std::string refresh_token_;
  double access_token_expires_at_ = 0.0;
  std::optional<double> login_deadline_;
  int consecutive_refresh_failures_ = 0;
  double next_refresh_attempt_at_ = 0.0;

  mutable std::mutex thread_mutex_;
  std::thread thread_;
};

namespace internal {

inline void RunKeycloakRefreshLoop(const std::weak_ptr<KeycloakTokenProvider>& weak_provider) {
  std::shared_ptr<KeycloakStopSignal> stop;
  {
    const std::shared_ptr<KeycloakTokenProvider> provider = weak_provider.lock();
    if (!provider) return;
    stop = provider->stop_;
  }
  while (!stop->IsStopped()) {
    double delay = 0.0;
    std::function<bool(double)> wait;
    {
      // Held only while the state is read: the thread must never keep the provider alive.
      const std::shared_ptr<KeycloakTokenProvider> provider = weak_provider.lock();
      if (!provider) return;
      std::lock_guard<std::mutex> lock(provider->mutex_);
      const double now = provider->time_fn_();
      const std::optional<double> deadline = provider->login_deadline_;
      if (deadline && now >= *deadline) {
        stop->Stop();
        return;
      }
      const KeycloakTimings& timings = provider->timings_;
      delay = provider->access_token_expires_at_ - timings.expiry_leeway_s - now;
      if (provider->consecutive_refresh_failures_ > 0) delay = provider->next_refresh_attempt_at_ - now;
      delay = std::max(delay, timings.min_refresh_delay_s);
      if (deadline) delay = std::min(delay, *deadline - now);
      wait = provider->wait_fn_;
    }
    const bool stopped = wait ? wait(delay) : stop->WaitFor(delay);
    if (stopped || stop->IsStopped()) return;
    const std::shared_ptr<KeycloakTokenProvider> provider = weak_provider.lock();
    if (!provider) return;
    std::lock_guard<std::mutex> lock(provider->mutex_);
    try {
      provider->RefreshIfWithinWindow(provider->time_fn_());
    } catch (const std::exception& error) {
      // Already recorded and warned about by RecordRefreshFailure; a failure never ends the loop.
      provider->Log(KeycloakLogLevel::kDebug,
                    std::string("Keycloak background refresh will be retried (") + error.what() + ")");
    }
  }
}

// One provider per credential set: SHA-256 over the fields joined by NUL, in the cross-SDK order,
// with token_expiration_in_s "" when unset and verify_ssl rendered "True" / "False" (as python's
// str(bool)). Never an address, never a raw value.
inline std::string KeycloakProviderRegistryKey(const KeycloakCredentials& credentials) {
  const std::string separator(1, '\0');
  return Sha256Hex(credentials.keycloak_url + separator + credentials.realm + separator + credentials.client_id +
                   separator + credentials.username + separator + credentials.password + separator +
                   (credentials.token_expiration_in_s ? std::to_string(*credentials.token_expiration_in_s) : "") +
                   separator + (credentials.verify_ssl ? "True" : "False") + separator + credentials.refresh_token);
}

struct KeycloakProviderRegistry {
  std::mutex mutex;
  std::unordered_map<std::string, std::weak_ptr<KeycloakTokenProvider>> providers;

  static KeycloakProviderRegistry& Get() {
    static KeycloakProviderRegistry registry;
    return registry;
  }
};

}  // namespace internal

// The shared provider of config's credentials: the live one when a channel already holds it, else
// a new one (logged in under the registry lock, so concurrent first use logs in once). Held weakly:
// it lives as long as a channel or a caller holds it, and a stopped provider is never handed out
// again. Throws like KeycloakTokenProvider::Create(). The options of the call that CREATES the
// provider apply; later callers share it as it is.
inline std::shared_ptr<KeycloakTokenProvider> GetKeycloakTokenProvider(const ClientConfig& config,
                                                                       const KeycloakProviderOptions& options) {
  const KeycloakCredentials credentials = KeycloakCredentialsFrom(config);
  const std::string key = internal::KeycloakProviderRegistryKey(credentials);
  internal::KeycloakProviderRegistry& registry = internal::KeycloakProviderRegistry::Get();
  std::lock_guard<std::mutex> lock(registry.mutex);
  for (auto entry = registry.providers.begin(); entry != registry.providers.end();) {
    entry = entry->second.expired() ? registry.providers.erase(entry) : std::next(entry);
  }
  const auto found = registry.providers.find(key);
  if (found != registry.providers.end()) {
    std::shared_ptr<KeycloakTokenProvider> provider = found->second.lock();
    if (provider && !provider->stopped()) return provider;
  }
  std::shared_ptr<KeycloakTokenProvider> provider = KeycloakTokenProvider::Create(credentials, options);
  registry.providers[key] = provider;
  return provider;
}

// ------------------------------------------------------------------------------------------
// gRPC integration

// Attaches "authorization: Bearer <token>" to every call of a channel built with it. A failure
// fails that call with the KeycloakAuthenticationError text (UNAUTHENTICATED here; gRPC 1.51 turns
// a plugin failure into UNAVAILABLE for the caller); the call is never sent without a token.
class KeycloakMetadataPlugin : public grpc::MetadataCredentialsPlugin {
 public:
  explicit KeycloakMetadataPlugin(std::shared_ptr<KeycloakTokenProvider> provider) : provider_(std::move(provider)) {}

  // A refresh may wait for the token endpoint, so gRPC runs this on a thread of its own.
  bool IsBlocking() const override { return true; }

  const char* GetType() const override { return "ondewo-keycloak"; }

  grpc::Status GetMetadata(grpc::string_ref /*service_url*/, grpc::string_ref /*method_name*/,
                           const grpc::AuthContext& /*channel_auth_context*/,
                           std::multimap<grpc::string, grpc::string>* metadata) override {
    try {
      const std::pair<std::string, std::string> header = provider_->AuthorizationMetadata();
      metadata->insert(header);
      return grpc::Status::OK;
    } catch (const std::exception& error) {
      return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, error.what());
    }
  }

 private:
  std::shared_ptr<KeycloakTokenProvider> provider_;
};

inline std::shared_ptr<grpc::CallCredentials> KeycloakCallCredentials(std::shared_ptr<KeycloakTokenProvider> provider) {
  return grpc::MetadataCredentialsFromPlugin(std::make_unique<KeycloakMetadataPlugin>(std::move(provider)));
}

// Adds the bearer header to ONE call by hand, e.g. for a plaintext channel, which gRPC never
// lets carry call credentials. Throws KeycloakAuthenticationError for an expired token.
inline void AddKeycloakAuthorization(KeycloakTokenProvider& provider, grpc::ClientContext* context) {
  const std::pair<std::string, std::string> header = provider.AuthorizationMetadata();
  context->AddMetadata(header.first, header.second);
}

// Like CreateChannel(), plus the bearer token when config.KeycloakConfigured(): one shared provider
// per credential set (GetKeycloakTokenProvider), attached as call credentials. Without Keycloak it
// is exactly CreateChannel() - no provider, no header. Refuses a plaintext channel with Keycloak
// (a bearer token is never sent in clear). Returns INVALID_ARGUMENT for a bad config or options,
// UNAUTHENTICATED when the first token cannot be acquired; on error nothing is opened. *provider,
// when given, receives the provider (nullptr without Keycloak).
inline grpc::Status CreateAuthenticatedChannel(const ClientConfig& config, const KeycloakProviderOptions& options,
                                               const grpc::ChannelArguments& args,
                                               std::shared_ptr<grpc::Channel>* channel,
                                               std::shared_ptr<KeycloakTokenProvider>* provider = nullptr) {
  if (!config.KeycloakConfigured()) return CreateChannel(config, args, channel);
  const grpc::Status status = ValidateConfig(config);
  if (!status.ok()) return status;
  if (!config.use_secure_channel) {
    return internal::Invalid("ClientConfig for " + config.HostAndPort() +
                             " configures Keycloak, but use_secure_channel is false and a bearer token is"
                             " never sent over a plaintext channel; use a secure channel, or attach the token"
                             " to each call with AddKeycloakAuthorization().");
  }
  std::shared_ptr<grpc::ChannelCredentials> credentials;
  CreateChannelCredentials(config, &credentials);  // validated above, so it cannot fail
  std::shared_ptr<KeycloakTokenProvider> shared;
  try {
    shared = GetKeycloakTokenProvider(config, options);
  } catch (const KeycloakAuthenticationError& error) {
    return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, error.what());
  } catch (const std::invalid_argument& error) {
    return internal::Invalid(error.what());
  }
  *channel = grpc::CreateCustomChannel(
      config.HostAndPort(), grpc::CompositeChannelCredentials(credentials, KeycloakCallCredentials(shared)), args);
  if (provider != nullptr) *provider = shared;
  return grpc::Status::OK;
}

inline grpc::Status CreateAuthenticatedChannel(const ClientConfig& config, const KeycloakProviderOptions& options,
                                               std::shared_ptr<grpc::Channel>* channel,
                                               std::shared_ptr<KeycloakTokenProvider>* provider = nullptr) {
  return CreateAuthenticatedChannel(config, options, DefaultChannelArguments(), channel, provider);
}

}  // namespace client
}  // namespace ondewo
