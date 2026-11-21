// Keycloak bearer-token authentication through ondewo/client/keycloak.h.
//
// Product-agnostic: nothing here names a generated type, so the file is copied verbatim between
// the ONDEWO C++ clients. It implements the test catalogue of the cross-language ONDEWO SDK
// Keycloak specification; the comment above each TEST names its catalogue id (Txx), the same ids
// the python SDKs' tests/unit/utils/test_keycloak_refresh_resilience.py and
// test_client_config_refresh_token.py carry.
//
// Everything runs without network: the token endpoint is a FakeTransport (scripted answers, every
// form recorded), time is a fake monotonic clock, the jitter fraction is fixed and the background
// loop is driven through a scripted wait_fn. The two exceptions are deliberate and stay on
// loopback: the real-thread test (T41) uses the real clock, and the libcurl transport and the gRPC
// integration are exercised against in-process servers on 127.0.0.1.
//
// Compiled into an executable of its own WITH exceptions: the typed KeycloakAuthenticationError is
// part of the contract (the rest of the suite builds with -fno-exceptions).

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/generic/async_generic_service.h>
#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <ondewo/client/keycloak.h>
#include <ondewo/client/keycloak_curl.h>

namespace ondewo_client_test {
namespace {

using ondewo::client::ClientConfig;
using ondewo::client::KeycloakAuthenticationError;
using ondewo::client::KeycloakCredentials;
using ondewo::client::KeycloakHttpRequest;
using ondewo::client::KeycloakHttpResponse;
using ondewo::client::KeycloakLogLevel;
using ondewo::client::KeycloakProviderOptions;
using ondewo::client::KeycloakTimings;
using ondewo::client::KeycloakTokenProvider;
using ondewo::client::KeycloakTransportError;
using ondewo::client::internal::KeycloakBackoffDelay;

const std::string kKeycloakUrl = "https://keycloak.example/auth";
const std::string kRealm = "ondewo-test-realm";
const std::string kClientId = "ondewo-test-sdk-public";
const std::string kUsername = "technical-user@example.com";
const std::string kPassword = "pw-never-logged-5d1c";
const std::string kDeadSession = R"({"error":"invalid_grant","error_description":"Offline user session not found"})";
const std::string kTokenUrl = kKeycloakUrl + "/realms/" + kRealm + "/protocol/openid-connect/token";

// ------------------------------------------------------------------------------------------
// Fakes

// One scripted answer of the token endpoint: a response, or an exception the transport throws.
struct Step {
  KeycloakHttpResponse response;
  std::exception_ptr error;
};

Step Answer(long status, const std::string& body) { return Step{KeycloakHttpResponse{status, body}, nullptr}; }

// A successful token response. expires_in is a JSON literal, so a string or a float can be scripted.
Step Token(const std::string& access, const std::string& expires_in = "300",
           const std::optional<std::string>& refresh = std::string("off-x")) {
  std::string body = R"({"access_token":")" + access + R"(","expires_in":)" + expires_in;
  if (refresh) body += R"(,"refresh_token":")" + *refresh + "\"";
  return Answer(200, body + "}");
}

Step Refused(long status = 400, const std::string& body = kDeadSession) { return Answer(status, body); }

template <typename Exception>
Step Throws(const Exception& exception) {
  return Step{{}, std::make_exception_ptr(exception)};
}

// Replays scripted steps (the last one repeats once the script runs out) and records every request.
class FakeTransport : public std::enable_shared_from_this<FakeTransport> {
 public:
  explicit FakeTransport(std::vector<Step> steps, std::chrono::milliseconds delay = std::chrono::milliseconds(0))
      : steps_(steps.begin(), steps.end()), delay_(delay) {}

  ondewo::client::KeycloakTransport AsTransport() {
    std::shared_ptr<FakeTransport> self = shared_from_this();
    return [self](const KeycloakHttpRequest& request) { return self->Post(request); };
  }

  KeycloakHttpResponse Post(const KeycloakHttpRequest& request) {
    Step step;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      calls_.push_back(request);
      step = steps_.front();
      if (steps_.size() > 1) steps_.pop_front();
    }
    if (on_post) on_post(request);
    if (delay_.count() > 0) std::this_thread::sleep_for(delay_);
    if (step.error) std::rethrow_exception(step.error);
    return step.response;
  }

  std::vector<KeycloakHttpRequest> calls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return calls_;
  }

  std::vector<std::string> Grants() const {
    std::vector<std::string> grants;
    for (const KeycloakHttpRequest& call : calls()) grants.push_back(Field(call, "grant_type").value_or(""));
    return grants;
  }

  static std::optional<std::string> Field(const KeycloakHttpRequest& request, const std::string& name) {
    for (const auto& [key, value] : request.form)
      if (key == name) return value;
    return std::nullopt;
  }

  // Runs inside Post(), before the step is played.
  std::function<void(const KeycloakHttpRequest&)> on_post;

 private:
  mutable std::mutex mutex_;
  std::deque<Step> steps_;
  std::chrono::milliseconds delay_;
  std::vector<KeycloakHttpRequest> calls_;
};

std::shared_ptr<FakeTransport> Fake(std::vector<Step> steps,
                                    std::chrono::milliseconds delay = std::chrono::milliseconds(0)) {
  return std::make_shared<FakeTransport>(std::move(steps), delay);
}

struct Clock {
  double now = 1000.0;
};

// Drives the background loop deterministically: each wait records its delay, advances the fake
// clock by it (or by a scripted amount) and replays a scripted result (true = stopped, false =
// timed out, i.e. refresh). Once the script runs out it answers "stopped".
struct ScriptedWait {
  std::shared_ptr<Clock> clock;
  std::deque<bool> returns;
  std::deque<double> advance_by;
  std::vector<double> delays;

  bool operator()(double delay) {
    delays.push_back(delay);
    if (!advance_by.empty()) {
      clock->now += advance_by.front();
      advance_by.pop_front();
    } else {
      clock->now += delay;
    }
    if (returns.empty()) return true;
    const bool result = returns.front();
    returns.pop_front();
    return result;
  }
};

std::shared_ptr<ScriptedWait> Waits(const std::shared_ptr<Clock>& clock, std::vector<bool> returns,
                                    std::vector<double> advance_by = {}) {
  auto wait = std::make_shared<ScriptedWait>();
  wait->clock = clock;
  wait->returns.assign(returns.begin(), returns.end());
  wait->advance_by.assign(advance_by.begin(), advance_by.end());
  return wait;
}

// Every log line, rendered "<LEVEL> <message>".
struct Logs {
  std::mutex mutex;
  std::vector<std::string> lines;

  ondewo::client::KeycloakLogSink Sink() {
    return [this](KeycloakLogLevel level, const std::string& message) {
      std::lock_guard<std::mutex> lock(mutex);
      lines.push_back(std::string(level == KeycloakLogLevel::kWarning ? "WARNING"
                                  : level == KeycloakLogLevel::kInfo  ? "INFO"
                                                                      : "DEBUG") +
                      " " + message);
    };
  }

  std::string All() {
    std::lock_guard<std::mutex> lock(mutex);
    std::string all;
    for (const std::string& line : lines) all += line + "\n";
    return all;
  }

  int Count(const std::string& level, const std::string& fragment) {
    std::lock_guard<std::mutex> lock(mutex);
    int count = 0;
    for (const std::string& line : lines)
      if (line.rfind(level + " ", 0) == 0 && line.find(fragment) != std::string::npos) ++count;
    return count;
  }
};

KeycloakCredentials Credentials(const std::string& password = kPassword, const std::string& refresh_token = "",
                                std::optional<int> token_expiration_in_s = std::nullopt) {
  return KeycloakCredentials{kKeycloakUrl, kRealm, kClientId, kUsername, password, token_expiration_in_s, true,
                             refresh_token};
}

struct Built {
  std::shared_ptr<FakeTransport> transport;
  std::shared_ptr<Clock> clock;
  std::shared_ptr<ScriptedWait> wait;
  std::shared_ptr<Logs> logs = std::make_shared<Logs>();
  std::shared_ptr<KeycloakTokenProvider> provider;
};

KeycloakProviderOptions Options(Built& built, double jitter) {
  KeycloakProviderOptions options;
  options.transport = built.transport->AsTransport();
  const std::shared_ptr<Clock> clock = built.clock;
  options.time_fn = [clock] { return clock->now; };
  options.random_fn = [jitter] { return jitter; };
  if (built.wait) {
    const std::shared_ptr<ScriptedWait> wait = built.wait;
    options.wait_fn = [wait](double delay) { return (*wait)(delay); };
  }
  options.log_sink = built.logs->Sink();
  options.start_background_refresh = false;
  return options;
}

// A logged-in provider on a fake clock with a fixed jitter and no background thread.
Built Build(std::vector<Step> steps, std::shared_ptr<Clock> clock = std::make_shared<Clock>(),
            std::vector<bool> wait_returns = {}, double jitter = 0.0, KeycloakCredentials credentials = Credentials(),
            std::vector<double> advance_by = {}) {
  Built built;
  built.transport = Fake(std::move(steps));
  built.clock = clock;
  built.wait = Waits(clock, std::move(wait_returns), std::move(advance_by));
  built.provider = KeycloakTokenProvider::Create(credentials, Options(built, jitter));
  return built;
}

void RunLoop(const Built& built) { ondewo::client::internal::RunKeycloakRefreshLoop(built.provider); }

// The KeycloakAuthenticationError a callable throws; the test fails when it throws nothing or
// anything else. (The capture is one line on purpose: its failure path is a line no passing run
// executes, and the coverage floor counts lines.)
template <typename Callable>
std::optional<KeycloakAuthenticationError> AuthErrorOf(Callable&& callable) {
  std::optional<KeycloakAuthenticationError> caught;
  const auto capture = [&] { try { callable(); } catch (const KeycloakAuthenticationError& error) { caught = error; throw; } };
  EXPECT_THROW(capture(), KeycloakAuthenticationError);
  return caught;
}

bool Contains(const std::string& text, const std::string& fragment) { return text.find(fragment) != std::string::npos; }

std::vector<std::string> Repeat(const std::string& value, std::size_t count) {
  return std::vector<std::string>(count, value);
}

std::vector<std::string> Concat(std::vector<std::string> head, const std::vector<std::string>& tail) {
  head.insert(head.end(), tail.begin(), tail.end());
  return head;
}

// ------------------------------------------------------------------------------------------
// A. The backoff ladder

// T01
TEST(KeycloakBackoff, TheCeilingDoublesFromTheBaseAndStopsAtTheMaximum) {
  std::vector<double> delays;
  for (int n = 1; n <= 10; ++n) delays.push_back(KeycloakBackoffDelay(n, 1.0));
  EXPECT_EQ(delays, (std::vector<double>{5, 10, 20, 40, 80, 160, 300, 300, 300, 300}));
}

// T02
TEST(KeycloakBackoff, AZeroFractionAlwaysWaitsTheBase) {
  for (int n = 1; n < 20; ++n) EXPECT_EQ(KeycloakBackoffDelay(n, 0.0), 5.0) << n;
}

// T03
TEST(KeycloakBackoff, TheDelayIsDrawnInsideTheWindow) {
  EXPECT_EQ(KeycloakBackoffDelay(2, 0.5), 7.5);
  EXPECT_EQ(KeycloakBackoffDelay(3, 0.25), 8.75);
}

// T04, plus NaN: a faulty jitter source is clamped into [base, ceiling].
TEST(KeycloakBackoff, AFaultyJitterSourceIsClampedIntoTheWindow) {
  for (const double fraction : {-1.0, -0.001, 1.5, 1e9, std::nan("")}) {
    const double delay = KeycloakBackoffDelay(4, fraction);
    EXPECT_GE(delay, 5.0) << fraction;
    EXPECT_LE(delay, 40.0) << fraction;
  }
  EXPECT_EQ(KeycloakBackoffDelay(4, std::nan("")), 5.0);
}

// T05
TEST(KeycloakBackoff, ANonPositiveFailureCountIsTheFirstFailure) {
  EXPECT_EQ(KeycloakBackoffDelay(0, 1.0), 5.0);
  EXPECT_EQ(KeycloakBackoffDelay(-3, 1.0), 5.0);
}

// T06
TEST(KeycloakBackoff, AHugeFailureCountNeitherOverflowsNorExceedsTheCeiling) {
  EXPECT_EQ(KeycloakBackoffDelay(1000000000LL, 1.0), 300.0);
  EXPECT_EQ(KeycloakBackoffDelay(std::numeric_limits<long long>::max(), 1.0), 300.0);
}

// T07
TEST(KeycloakBackoff, TheConstantsKeepTheirDocumentedValuesAndRelations) {
  const KeycloakTimings timings;
  EXPECT_EQ(timings.expiry_leeway_s, 30.0);
  EXPECT_EQ(timings.min_refresh_delay_s, 1.0);
  EXPECT_EQ(timings.retry_base_delay_s, 5.0);
  EXPECT_EQ(timings.retry_max_delay_s, 300.0);
  EXPECT_EQ(timings.max_retry_exponent, 6);
  EXPECT_EQ(timings.http_timeout_s, 10.0);
  EXPECT_EQ(timings.max_error_body_chars, 500u);
  EXPECT_EQ(timings.thread_join_timeout_s, 5.0);
  EXPECT_GT(timings.retry_base_delay_s, timings.min_refresh_delay_s);
  EXPECT_GE(timings.retry_base_delay_s * (1 << timings.max_retry_exponent), timings.retry_max_delay_s);
  EXPECT_LT(timings.http_timeout_s, timings.expiry_leeway_s);
}

// ------------------------------------------------------------------------------------------
// B. The background loop survives every failure

// T08
TEST(KeycloakLoopSurvives, ARefusedRefreshIsRetriedAndTheProviderRecovers) {
  const Built built = Build({Token("acc-1"), Refused(), Token("acc-3")}, std::make_shared<Clock>(), {false, false});
  RunLoop(built);
  EXPECT_EQ(built.transport->Grants(), (std::vector<std::string>{"password", "refresh_token", "refresh_token"}));
  EXPECT_EQ(built.provider->access_token(), "acc-3");
  EXPECT_EQ(built.provider->consecutive_refresh_failures(), 0);
}

// T09
TEST(KeycloakLoopSurvives, NoKindOfFailureEndsTheLoop) {
  const std::vector<std::pair<std::string, Step>> failures = {
      {"network", Throws(KeycloakTransportError("OSError", "network unreachable"))},
      {"connection-refused", Throws(KeycloakTransportError("ConnectionError", "connection refused"))},
      {"timeout", Throws(KeycloakTransportError("Timeout", "read timed out"))},
      {"transport-bug", Throws(std::runtime_error("a bug in a custom transport"))},
      {"503", Refused(503, R"({"error":"temporarily_unavailable"})")},
      {"502-empty", Refused(502, "")},
      {"200-non-json", Answer(200, "<html>bad gateway</html>")},
      {"200-json-array", Answer(200, R"(["not","an","object"])")},
      {"200-no-access-token", Answer(200, R"({"expires_in":300})")},
      {"200-bad-expires-in", Answer(200, R"({"access_token":"acc","expires_in":"soon"})")},
  };
  for (const auto& [name, failure] : failures) {
    const Built built = Build({Token("acc-1"), failure, Token("acc-ok")}, std::make_shared<Clock>(), {false, false});
    RunLoop(built);
    EXPECT_EQ(built.transport->calls().size(), 3u) << name << ": the loop stopped at the failure";
    EXPECT_EQ(built.provider->access_token(), "acc-ok") << name;
  }
}

// T10
TEST(KeycloakLoopSurvives, APersistentFailureKeepsTheLoopAliveWithoutRepairingTheProvider) {
  const Built built = Build({Token("acc-1"), Refused()}, std::make_shared<Clock>(), std::vector<bool>(6, false));
  RunLoop(built);
  EXPECT_EQ(built.transport->Grants(), Concat({"password"}, Repeat("refresh_token", 6)));
  EXPECT_EQ(built.provider->access_token(), "acc-1");
  EXPECT_EQ(built.provider->consecutive_refresh_failures(), 6);
}

// T11
TEST(KeycloakLoopSurvives, NoFailureIsEverAnsweredWithAPasswordGrant) {
  const Built built = Build({Token("acc-1"), Refused(401, R"({"error":"invalid_client"})")},
                            std::make_shared<Clock>(), std::vector<bool>(10, false));
  RunLoop(built);
  const std::vector<std::string> grants = built.transport->Grants();
  EXPECT_EQ(std::count(grants.begin(), grants.end(), "password"), 1) << "the loop fell back to a password grant";
  EXPECT_EQ(grants.size(), 11u);
}

// ------------------------------------------------------------------------------------------
// C. The background loop backs off

// T12
TEST(KeycloakLoopBacksOff, TheFirstRetryWaitsTheBaseNotTheFloor) {
  const Built built = Build({Token("acc-1"), Refused(503), Token("acc-2")}, std::make_shared<Clock>(), {false, false});
  RunLoop(built);
  ASSERT_GE(built.wait->delays.size(), 2u);
  EXPECT_EQ(built.wait->delays[0], 270.0);
  EXPECT_EQ(built.wait->delays[1], 5.0);
}

// T13
TEST(KeycloakLoopBacksOff, ConsecutiveFailuresClimbTheLadderToTheCeiling) {
  const Built built =
      Build({Token("acc-1"), Refused(503)}, std::make_shared<Clock>(), std::vector<bool>(9, false), 1.0);
  RunLoop(built);
  const std::vector<double> after_first(built.wait->delays.begin() + 1, built.wait->delays.end());
  EXPECT_EQ(after_first, (std::vector<double>{5, 10, 20, 40, 80, 160, 300, 300, 300}));
}

// T14
TEST(KeycloakLoopBacksOff, ASuccessResetsTheLadderToTheHealthySchedule) {
  const Built built = Build({Token("acc-1"), Refused(503), Refused(503), Token("acc-4", "120"), Refused(503)},
                            std::make_shared<Clock>(), std::vector<bool>(4, false), 1.0);
  RunLoop(built);
  EXPECT_EQ(built.wait->delays, (std::vector<double>{270, 5, 10, 90, 5}));
  EXPECT_EQ(built.logs->Count("INFO", "recovered after 2 failed attempt(s)"), 1) << built.logs->All();
}

// T15
TEST(KeycloakLoopBacksOff, TheBackoffIsMeasuredFromWhenTheFailureWasRecorded) {
  auto clock = std::make_shared<Clock>();
  Built built;
  built.transport = Fake({Token("acc-1"), Refused(503)});
  built.transport->on_post = [clock](const KeycloakHttpRequest& request) {
    if (FakeTransport::Field(request, "grant_type") == "refresh_token") clock->now += 10.0;
  };
  built.clock = clock;
  built.wait = Waits(clock, {false, false});
  built.provider = KeycloakTokenProvider::Create(Credentials(), Options(built, 0.0));
  RunLoop(built);
  ASSERT_GE(built.wait->delays.size(), 2u);
  EXPECT_EQ(built.wait->delays[1], 5.0);
}

// T16
TEST(KeycloakLoopBacksOff, TheDeadlineStillEndsTheLoopWhileItBacksOff) {
  const Built built = Build({Token("acc-1", "30"), Refused(503)}, std::make_shared<Clock>(),
                            std::vector<bool>(50, false), 1.0, Credentials(kPassword, "", 100));
  RunLoop(built);
  EXPECT_LE(built.clock->now, 1000.0 + 100.0 + 1.0);
  EXPECT_TRUE(built.provider->stopped()) << "the loop did not stop at the deadline";
  for (const double delay : built.wait->delays) EXPECT_LE(delay, 100.0);
  EXPECT_LT(built.wait->delays.size(), 50u);
}

// T17
TEST(KeycloakLoopBacksOff, StopDuringABackoffWaitEndsTheLoopWithoutAnotherRequest) {
  const Built built = Build({Token("acc-1"), Refused(503)}, std::make_shared<Clock>(), {false, true}, 0.0,
                            Credentials(), {270.0, 1.0});
  RunLoop(built);
  EXPECT_EQ(built.transport->Grants(), (std::vector<std::string>{"password", "refresh_token"}));
}

// T17, through Stop(): a stop that fires while the loop waits ends it with no further request.
TEST(KeycloakLoopBacksOff, StopFiredDuringTheWaitEndsTheLoop) {
  Built built;
  built.transport = Fake({Token("acc-1"), Refused(503)});
  built.clock = std::make_shared<Clock>();
  KeycloakProviderOptions options = Options(built, 0.0);
  auto holder = std::make_shared<std::weak_ptr<KeycloakTokenProvider>>();
  options.wait_fn = [holder](double) {
    if (auto provider = holder->lock()) provider->Stop();
    return false;  // "timed out" - only the stop flag can end the loop now
  };
  built.provider = KeycloakTokenProvider::Create(Credentials(), options);
  *holder = built.provider;
  RunLoop(built);
  EXPECT_EQ(built.transport->Grants(), (std::vector<std::string>{"password"}));
}

// T18
TEST(KeycloakLoopBacksOff, AProviderThatIsGoneOrClosedEndsTheLoop) {
  Built built = Build({Token("acc-1")}, std::make_shared<Clock>(), {false, false, false});
  const std::weak_ptr<KeycloakTokenProvider> weak = built.provider;
  const std::shared_ptr<FakeTransport> transport = built.transport;
  built.provider.reset();
  EXPECT_TRUE(weak.expired());
  ondewo::client::internal::RunKeycloakRefreshLoop(weak);
  EXPECT_EQ(transport->calls().size(), 1u);

  const Built closed = Build({Token("acc-1")}, std::make_shared<Clock>(), {false, false, false});
  closed.provider->Close();
  RunLoop(closed);
  EXPECT_TRUE(closed.wait->delays.empty());
  EXPECT_EQ(closed.transport->calls().size(), 1u);
}

// The provider disappearing WHILE the loop waits ends it at the next look, without a request.
TEST(KeycloakLoopBacksOff, AProviderDroppedDuringTheWaitEndsTheLoop) {
  auto clock = std::make_shared<Clock>();
  Built built;
  built.transport = Fake({Token("acc-1")});
  built.clock = clock;
  KeycloakProviderOptions options = Options(built, 0.0);
  auto holder = std::make_shared<std::shared_ptr<KeycloakTokenProvider>>();
  options.wait_fn = [holder](double) {
    holder->reset();
    return false;
  };
  *holder = KeycloakTokenProvider::Create(Credentials(), options);
  const std::weak_ptr<KeycloakTokenProvider> weak = *holder;
  ondewo::client::internal::RunKeycloakRefreshLoop(weak);
  EXPECT_TRUE(weak.expired());
  EXPECT_EQ(built.transport->calls().size(), 1u);
}

// ------------------------------------------------------------------------------------------
// D. The per-call path shares the backoff

// T19
TEST(KeycloakLazyPath, AStillValidTokenIsServedWhenItsRefreshFails) {
  const Built built = Build({Token("acc-1", "300"), Refused(503)});
  built.clock->now += 271.0;  // inside the leeway, 29 s still valid
  EXPECT_EQ(built.provider->AuthorizationMetadata(),
            (std::pair<std::string, std::string>{"authorization", "Bearer acc-1"}));
  EXPECT_EQ(built.logs->Count("WARNING", "refresh failed (attempt 1)"), 1) << built.logs->All();
  EXPECT_EQ(built.logs->Count("WARNING", "stays valid for 29.0s"), 1) << built.logs->All();
}

// T20
TEST(KeycloakLazyPath, NoRequestIsSentWhileTheBackoffRuns) {
  const Built built = Build({Token("acc-1", "300"), Refused(503)});
  built.clock->now += 275.0;
  for (int i = 0; i < 100; ++i) {
    built.provider->AuthorizationMetadata();
    built.clock->now += 0.04;
  }
  EXPECT_EQ(built.transport->Grants(), (std::vector<std::string>{"password", "refresh_token"}));
}

// T21
TEST(KeycloakLazyPath, AnExpiredTokenRaisesDuringTheBackoffWithoutARequest) {
  const Built built = Build({Token("acc-1", "300"), Refused(503)}, std::make_shared<Clock>(), {}, 1.0);
  built.clock->now += 299.0;
  built.provider->AuthorizationMetadata();  // fails, backs off 5 s, 1 s of validity left
  built.clock->now += 2.0;
  const auto error = AuthErrorOf([&] { built.provider->AuthorizationMetadata(); });
  ASSERT_TRUE(error);
  EXPECT_TRUE(Contains(error->what(), "backing off after 1 consecutive failure")) << error->what();
  EXPECT_TRUE(Contains(error->what(), "the next attempt is due in 3.0s")) << error->what();
  EXPECT_EQ(built.transport->calls().size(), 2u);
}

// T22
TEST(KeycloakLazyPath, AnExpiredTokenWhoseRefreshFailsRaises) {
  const Built built = Build({Token("acc-1", "300"), Refused()});
  built.clock->now += 400.0;
  const auto error = AuthErrorOf([&] { built.provider->AuthorizationMetadata(); });
  ASSERT_TRUE(error);
  EXPECT_TRUE(Contains(error->what(), "status 400")) << error->what();
  EXPECT_EQ(built.logs->Count("WARNING", "the current access token has expired"), 1) << built.logs->All();
}

// T23
TEST(KeycloakLazyPath, ANonKeycloakExceptionReachesTheCallerAsAKeycloakErrorWithItsCause) {
  const Built built = Build({Token("acc-1", "300"), Throws(std::runtime_error("transport bug"))});
  built.clock->now += 400.0;
  const auto error = AuthErrorOf([&] { built.provider->AuthorizationMetadata(); });
  ASSERT_TRUE(error);
  EXPECT_TRUE(Contains(error->what(), "Keycloak token refresh failed (std::runtime_error: transport bug)"))
      << error->what();
  ASSERT_TRUE(error->cause());
  try {
    std::rethrow_exception(error->cause());
  } catch (const std::runtime_error& cause) {
    EXPECT_STREQ(cause.what(), "transport bug");
  }
}

// T23, for an exception that is not even a std::exception.
TEST(KeycloakLazyPath, AForeignExceptionIsWrappedToo) {
  const Built built = Build({Token("acc-1", "300"), Step{{}, std::make_exception_ptr(42)}});
  built.clock->now += 400.0;
  const auto error = AuthErrorOf([&] { built.provider->AuthorizationMetadata(); });
  ASSERT_TRUE(error);
  EXPECT_TRUE(Contains(error->what(), "UnknownException")) << error->what();
}

// T24
TEST(KeycloakLazyPath, TheLazyPathRetriesOnceTheBackoffElapsesAndRecovers) {
  const Built built = Build({Token("acc-1", "300"), Refused(503), Token("acc-3", "300")});
  built.clock->now += 280.0;
  built.provider->AuthorizationMetadata();
  built.clock->now += 5.0;
  EXPECT_EQ(built.provider->AuthorizationMetadata().second, "Bearer acc-3");
  EXPECT_EQ(built.provider->consecutive_refresh_failures(), 0);
  EXPECT_EQ(built.provider->next_refresh_attempt_at(), 0.0);
  EXPECT_EQ(built.logs->Count("INFO", "recovered after 1 failed attempt"), 1) << built.logs->All();
}

// T25
TEST(KeycloakLazyPath, ALazyFailureSchedulesTheBackgroundLoopsNextWake) {
  const Built built =
      Build({Token("acc-1", "300"), Refused(503), Token("acc-3", "300")}, std::make_shared<Clock>(), {false}, 1.0);
  built.clock->now += 280.0;
  built.provider->AuthorizationMetadata();  // next attempt at +5 s
  built.clock->now += 2.0;
  RunLoop(built);
  ASSERT_FALSE(built.wait->delays.empty());
  EXPECT_NEAR(built.wait->delays[0], 3.0, 1e-9) << "the loop ignored the lazy path's backoff";
  EXPECT_EQ(built.provider->access_token(), "acc-3");
}

// T26: 16 threads hammer a provider whose token is inside the leeway while Keycloak is down.
TEST(KeycloakLazyPath, ConcurrentCallsDuringAnOutageSendOneRequestAndNeverRaise) {
  const auto transport =
      Fake({Token("acc-1", "20"), Throws(KeycloakTransportError("OSError", "down"))}, std::chrono::milliseconds(50));
  KeycloakProviderOptions options;
  options.transport = transport->AsTransport();
  options.start_background_refresh = false;
  options.log_sink = [](KeycloakLogLevel, const std::string&) {};
  const auto provider = KeycloakTokenProvider::Create(Credentials(), options);
  std::mutex mutex;
  std::vector<std::string> values;
  std::atomic<int> errors{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 16; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 20; ++i) {
        try {
          const std::string value = provider->AuthorizationMetadata().second;
          std::lock_guard<std::mutex> lock(mutex);
          values.push_back(value);
        } catch (const std::exception&) { errors.fetch_add(1); }
      }
    });
  }
  for (std::thread& thread : threads) thread.join();
  EXPECT_EQ(errors.load(), 0) << "calls failed while the token was still valid";
  EXPECT_EQ(values, Repeat("Bearer acc-1", 320));
  EXPECT_EQ(transport->Grants(), (std::vector<std::string>{"password", "refresh_token"}));
}

// ------------------------------------------------------------------------------------------
// E. The token endpoint's answer is validated

// T27
TEST(KeycloakValidation, ANetworkFailureAtLoginIsAKeycloakErrorWithItsCause) {
  for (const auto& [kind, message] : std::vector<std::pair<std::string, std::string>>{
           {"OSError", "unreachable"}, {"ConnectionError", "refused"}, {"Timeout", "timed out"}}) {
    const auto error = AuthErrorOf([&] { Build({Throws(KeycloakTransportError(kind, message))}); });
    ASSERT_TRUE(error);
    EXPECT_TRUE(Contains(error->what(), "Keycloak token login could not reach the token endpoint (" + kind + ": " +
                                            message + ")"))
        << error->what();
    try {
      std::rethrow_exception(error->cause());
    } catch (const KeycloakTransportError& cause) {
      EXPECT_EQ(cause.kind(), kind);
    }
  }
}

// T28
TEST(KeycloakValidation, ANonJsonBodyIsAKeycloakError) {
  for (const std::string& body : {std::string("<html>bad gateway</html>"), std::string("")}) {
    const auto error = AuthErrorOf([&] { Build({Answer(200, body)}); });
    ASSERT_TRUE(error);
    EXPECT_TRUE(Contains(error->what(), "Keycloak token login returned status 200 with a body that is not JSON"))
        << error->what();
  }
}

// T29, for every JSON type that is not an object.
TEST(KeycloakValidation, AJsonBodyThatIsNotAnObjectIsAKeycloakError) {
  for (const auto& [body, type] : std::vector<std::pair<std::string, std::string>>{
           {R"(["access_token"])", "array"}, {R"("x")", "string"}, {"5", "number"}, {"true", "boolean"},
           {"null", "null"}}) {
    const auto error = AuthErrorOf([&] { Build({Answer(200, body)}); });
    ASSERT_TRUE(error);
    EXPECT_TRUE(Contains(error->what(), "returned a JSON " + type + " instead of an object")) << error->what();
  }
}

// T30
TEST(KeycloakValidation, AHugeErrorPageIsTruncatedInTheMessage) {
  const std::string page = "<html>" + std::string(10000, 'x') + "</html>";
  const auto error = AuthErrorOf([&] { Build({Answer(502, page)}); });
  ASSERT_TRUE(error);
  EXPECT_LT(std::string(error->what()).size(), 500u + 200u);
  EXPECT_TRUE(Contains(error->what(), "Keycloak token login failed with status 502: <html>xxx")) << error->what();
}

// T30, at a UTF-8 boundary: a multi-byte character is never cut in half.
TEST(KeycloakValidation, TheTruncationNeverSplitsAUtf8Character) {
  const std::string body = std::string(499, 'x') + "\xc3\xa9" + "tail";
  EXPECT_EQ(ondewo::client::internal::TruncateBody(body, 500), std::string(499, 'x'));
  EXPECT_EQ(ondewo::client::internal::TruncateBody(body, 501), std::string(499, 'x') + "\xc3\xa9");
  EXPECT_EQ(ondewo::client::internal::TruncateBody("short", 500), "short");
}

// T31, plus an empty and a non-string access_token.
TEST(KeycloakValidation, AResponseWithoutAnAccessTokenNamesFieldsNeverValues) {
  for (const std::string& body : {std::string(R"({"refresh_token":"secret-offline-token-77","expires_in":300})"),
                                  std::string(R"({"access_token":"","refresh_token":"secret-offline-token-77"})"),
                                  std::string(R"({"access_token":7,"refresh_token":"secret-offline-token-77"})")}) {
    const auto error = AuthErrorOf([&] { Build({Answer(200, body)}); });
    ASSERT_TRUE(error);
    EXPECT_TRUE(Contains(error->what(), "did not contain an access_token")) << error->what();
    EXPECT_TRUE(Contains(error->what(), "'refresh_token'")) << error->what();
    EXPECT_FALSE(Contains(error->what(), "secret-offline-token-77")) << error->what();
  }
  const auto sorted = AuthErrorOf([&] { Build({Answer(200, R"({"zeta":1,"alpha":2})")}); });
  ASSERT_TRUE(sorted);
  EXPECT_TRUE(Contains(sorted->what(), "the response carried the fields ['alpha', 'zeta'] (values withheld"))
      << sorted->what();
}

// T32, plus every other non-numeric expires_in.
TEST(KeycloakValidation, ANonNumericExpiresInLeavesThePreviousTokenUntouched) {
  for (const std::string& expires_in : {std::string(R"("soon")"), std::string("null"), std::string("{}"),
                                        std::string("[]"), std::string(R"("0x10")"), std::string(R"("nan")"),
                                        std::string(R"("12abc")"), std::string(R"("")")}) {
    const Built built = Build({Token("acc-1", "300"), Token("acc-bad", expires_in)});
    built.clock->now += 280.0;
    built.provider->AuthorizationMetadata();
    EXPECT_EQ(built.provider->access_token(), "acc-1") << expires_in;
    EXPECT_EQ(built.provider->access_token_expires_at(), 1300.0) << expires_in;
    EXPECT_EQ(built.logs->Count("WARNING", "non-numeric expires_in (value withheld)"), 1) << expires_in;
  }
}

// T33, plus the other numeric forms.
TEST(KeycloakValidation, ANumericStringExpiresInIsAccepted) {
  struct Case {
    const char* expires_in;
    double expected;
  };
  static constexpr Case kCases[] = {{R"("300")", 1300.0}, {R"(" 300 ")", 1300.0}, {R"("+1.5e2")", 1150.0},
                                    {"299.5", 1299.5},    {"true", 1001.0},       {"false", 1000.0}};
  for (const Case& c : kCases) {
    const Built built = Build({Token("acc-1", c.expires_in)});
    EXPECT_EQ(built.provider->access_token_expires_at(), c.expected) << c.expires_in;
  }
  const Built missing = Build({Answer(200, R"({"access_token":"acc-1"})")});
  EXPECT_EQ(missing.provider->access_token_expires_at(), 1000.0);
}

// T34
TEST(KeycloakValidation, ANegativeExpiresInCountsAsAlreadyExpired) {
  const Built built = Build({Token("acc-1", "-50")});
  EXPECT_EQ(built.provider->access_token_expires_at(), 1000.0);
}

// T35
TEST(KeycloakValidation, ARefreshWithoutANewRefreshTokenKeepsThePreviousOne) {
  const Built built = Build({Token("acc-1", "300", std::string("off-1")), Token("acc-2", "300", std::nullopt),
                             Token("acc-3", "300", std::string(""))});
  built.clock->now += 280.0;
  built.provider->AuthorizationMetadata();
  built.clock->now += 280.0;
  built.provider->AuthorizationMetadata();
  const auto calls = built.transport->calls();
  ASSERT_EQ(calls.size(), 3u);
  EXPECT_EQ(FakeTransport::Field(calls[1], "refresh_token"), "off-1");
  EXPECT_EQ(FakeTransport::Field(calls[2], "refresh_token"), "off-1");
}

// T36
TEST(KeycloakValidation, TheGrantFormsTheUrlAndTheTimeoutAreExact) {
  KeycloakCredentials credentials = Credentials();
  credentials.keycloak_url = kKeycloakUrl + "//";
  credentials.verify_ssl = false;
  const Built built =
      Build({Token("acc-1", "300", std::string("off-1"))}, std::make_shared<Clock>(), {}, 0.0, credentials);
  built.clock->now += 280.0;
  built.provider->AuthorizationMetadata();
  const auto calls = built.transport->calls();
  ASSERT_EQ(calls.size(), 2u);
  using Form = std::vector<std::pair<std::string, std::string>>;
  EXPECT_EQ(calls[0].form, (Form{{"grant_type", "password"},
                                 {"client_id", kClientId},
                                 {"username", kUsername},
                                 {"password", kPassword},
                                 {"scope", "offline_access"}}));
  EXPECT_EQ(calls[1].form, (Form{{"grant_type", "refresh_token"}, {"client_id", kClientId}, {"refresh_token", "off-1"}}));
  for (const KeycloakHttpRequest& call : calls) {
    EXPECT_EQ(call.url, kTokenUrl);
    EXPECT_EQ(call.timeout_s, 10.0);
    EXPECT_FALSE(call.verify_ssl);
  }
  EXPECT_EQ(built.provider->token_endpoint(), kTokenUrl);
  EXPECT_EQ(built.provider->client_id(), kClientId);
  EXPECT_EQ(built.provider->username(), kUsername);
}

// A generic exception at construction is wrapped, never let through raw.
TEST(KeycloakValidation, ATransportBugAtLoginIsAKeycloakError) {
  const auto error = AuthErrorOf([&] { Build({Throws(std::logic_error("bug"))}); });
  ASSERT_TRUE(error);
  EXPECT_TRUE(Contains(error->what(), "Keycloak token login failed (std::logic_error: bug)")) << error->what();
  const auto offline =
      AuthErrorOf([&] { Build({Throws(std::logic_error("bug"))}, std::make_shared<Clock>(), {}, 0.0,
                              Credentials("", "off-handed-0")); });
  ASSERT_TRUE(offline);
  EXPECT_TRUE(Contains(offline->what(), "Keycloak token refresh failed (std::logic_error: bug)")) << offline->what();
}

TEST(KeycloakValidation, AProviderWithoutATransportIsRefused) {
  EXPECT_THROW(KeycloakTokenProvider::Create(Credentials(), KeycloakProviderOptions()), std::invalid_argument);
}

TEST(KeycloakValidation, TheNumericStringParserMatchesPythonsFloatForKeycloaksForms) {
  using ondewo::client::internal::ParseNumericString;
  EXPECT_EQ(ParseNumericString("300"), 300.0);
  EXPECT_EQ(ParseNumericString("\t300\n"), 300.0);
  EXPECT_EQ(ParseNumericString("-2.5"), -2.5);
  for (const std::string text : {"", " ", "abc", "1 2", "0x1p3", "inf", "nan", "1e"})
    EXPECT_FALSE(ParseNumericString(text)) << text;
}

// ------------------------------------------------------------------------------------------
// F. No credential in logs or errors

// T37
TEST(KeycloakNoCredentialLeaks, TheFailureLinesAndErrorsCarryNoCredential) {
  const Built built = Build({Token("acc-secret-1", "300", std::string("off-secret-1")), Refused(503),
                             Token("acc-secret-2", "300")},
                            std::make_shared<Clock>(), {false, false});
  RunLoop(built);
  const std::string rendered = built.logs->All();
  EXPECT_TRUE(Contains(rendered, "refresh failed (attempt 1)")) << rendered;
  EXPECT_TRUE(Contains(rendered, "recovered")) << rendered;
  const std::vector<std::string> secrets = {kPassword, "acc-secret-1", "off-secret-1", "acc-secret-2"};
  for (const std::string& secret : secrets) EXPECT_FALSE(Contains(rendered, secret)) << secret;

  // Every error text of the catalogue, produced with the same secrets in play.
  std::vector<std::string> errors;
  const std::vector<Step> failures = {
      Throws(KeycloakTransportError("OSError", "down")), Refused(401, R"({"error":"invalid_client"})"),
      Answer(200, "<html>"), Answer(200, "[1]"), Answer(200, R"({"refresh_token":"off-secret-1"})"),
      Answer(200, R"({"access_token":"acc-secret-2","expires_in":"soon"})"), Throws(std::runtime_error("bug"))};
  for (const Step& failure : failures) {
    const Built lazy = Build({Token("acc-secret-1", "300", std::string("off-secret-1")), failure});
    lazy.clock->now += 400.0;
    const auto error = AuthErrorOf([&] { lazy.provider->AuthorizationMetadata(); });
    ASSERT_TRUE(error);
    errors.push_back(error->what());
    errors.push_back(lazy.logs->All());
    lazy.clock->now += 1.0;
    const auto backing_off = AuthErrorOf([&] { lazy.provider->AuthorizationMetadata(); });
    ASSERT_TRUE(backing_off);
    errors.push_back(backing_off->what());
  }
  for (const std::string& text : errors)
    for (const std::string& secret : secrets) EXPECT_FALSE(Contains(text, secret)) << secret << " in " << text;
}

TEST(KeycloakNoCredentialLeaks, TheDefaultSinkWritesLevelTokensAndDropsDebug) {
  std::ostringstream captured;
  std::streambuf* const original = std::clog.rdbuf(captured.rdbuf());
  ondewo::client::DefaultKeycloakLogSink(KeycloakLogLevel::kWarning, "w-line");
  ondewo::client::DefaultKeycloakLogSink(KeycloakLogLevel::kInfo, "i-line");
  ondewo::client::DefaultKeycloakLogSink(KeycloakLogLevel::kDebug, "d-line");
  std::clog.rdbuf(original);
  EXPECT_EQ(captured.str(), "WARNING: ondewo::client: w-line\nINFO: ondewo::client: i-line\n");
}

TEST(KeycloakNoCredentialLeaks, TheDefaultSinkIsUsedWithoutAnInjectedOne) {
  const auto transport = Fake({Token("acc-1", "300"), Refused(503)});
  KeycloakProviderOptions options;
  options.transport = transport->AsTransport();
  options.start_background_refresh = false;
  auto clock = std::make_shared<Clock>();
  options.time_fn = [clock] { return clock->now; };
  const auto provider = KeycloakTokenProvider::Create(Credentials(), options);
  clock->now += 280.0;
  std::ostringstream captured;
  std::streambuf* const original = std::clog.rdbuf(captured.rdbuf());
  provider->AuthorizationMetadata();
  std::clog.rdbuf(original);
  EXPECT_TRUE(Contains(captured.str(), "WARNING: ondewo::client: Keycloak token refresh failed (attempt 1)"))
      << captured.str();
  EXPECT_FALSE(Contains(captured.str(), kPassword));
}

// ------------------------------------------------------------------------------------------
// G. Stop / close

// T38
TEST(KeycloakStop, StopIsIdempotentAndSafeWithoutAThread) {
  const Built built = Build({Token("acc-1")});
  EXPECT_FALSE(built.provider->stopped());
  EXPECT_FALSE(built.provider->background_refresh_running());
  built.provider->Stop();
  built.provider->Stop();
  built.provider->Close();
  EXPECT_TRUE(built.provider->stopped());
  // The per-call path keeps working after a stop.
  EXPECT_EQ(built.provider->AuthorizationMetadata().second, "Bearer acc-1");
}

// T39: Stop() from inside the refresh thread never joins itself.
TEST(KeycloakStop, StopFromInsideTheRefreshThreadDoesNotJoinItself) {
  const auto transport = Fake({Token("acc-1")});
  // Handed to the thread once Create() returned; guarded, the thread reads it while it is set.
  struct Holder {
    std::mutex mutex;
    std::condition_variable set;
    std::weak_ptr<KeycloakTokenProvider> provider;
    std::atomic<bool> stopped_inside{false};
  };
  const auto holder = std::make_shared<Holder>();
  KeycloakProviderOptions options;
  options.transport = transport->AsTransport();
  options.wait_fn = [holder](double) {
    std::shared_ptr<KeycloakTokenProvider> provider;
    {
      std::unique_lock<std::mutex> lock(holder->mutex);
      holder->set.wait_for(lock, std::chrono::seconds(5), [&] { return !holder->provider.expired(); });
      provider = holder->provider.lock();
    }
    if (provider) provider->Stop();
    holder->stopped_inside.store(true);
    return false;
  };
  const auto provider = KeycloakTokenProvider::Create(Credentials(), options);
  {
    std::lock_guard<std::mutex> lock(holder->mutex);
    holder->provider = provider;
  }
  holder->set.notify_all();
  for (int i = 0; i < 1000 && !holder->stopped_inside.load(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  EXPECT_TRUE(holder->stopped_inside.load());
  EXPECT_TRUE(provider->stopped());
  EXPECT_FALSE(provider->background_refresh_running());
  EXPECT_EQ(transport->Grants(), (std::vector<std::string>{"password"}));
}

// T39: the last reference dropped ON the refresh thread runs the destructor there, which must
// detach instead of joining itself.
TEST(KeycloakStop, TheDestructorRunningOnTheRefreshThreadDoesNotJoinItself) {
  auto release = std::make_shared<std::promise<void>>();
  std::shared_future<void> released = release->get_future().share();
  auto in_refresh = std::make_shared<std::promise<void>>();
  const auto transport = Fake({Token("acc-1", "20"), Token("acc-2", "300")});
  transport->on_post = [released, in_refresh](const KeycloakHttpRequest& request) {
    if (FakeTransport::Field(request, "grant_type") == "refresh_token") {
      in_refresh->set_value();
      released.wait();
    }
  };
  KeycloakProviderOptions options;
  options.transport = transport->AsTransport();
  options.wait_fn = [](double) { return false; };  // refresh at once: the token is inside the leeway
  options.log_sink = [](KeycloakLogLevel, const std::string&) {};
  auto provider = KeycloakTokenProvider::Create(Credentials(), options);
  const std::weak_ptr<KeycloakTokenProvider> weak = provider;
  in_refresh->get_future().wait();  // the thread now holds the provider
  provider.reset();
  EXPECT_FALSE(weak.expired());
  release->set_value();
  for (int i = 0; i < 2500 && !weak.expired(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  EXPECT_TRUE(weak.expired());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let the detached thread leave
  EXPECT_EQ(transport->Grants(), (std::vector<std::string>{"password", "refresh_token"}));
}

// T40: a thread that does not finish inside the join timeout is detached; nothing escapes.
TEST(KeycloakStop, AThreadThatOutlivesTheJoinTimeoutIsDetachedQuietly) {
  const auto transport = Fake({Token("acc-1")});
  struct Progress {
    std::atomic<bool> entered{false};
    std::atomic<bool> left{false};
  };
  const auto progress = std::make_shared<Progress>();
  KeycloakProviderOptions options;
  options.transport = transport->AsTransport();
  options.timings.thread_join_timeout_s = 0.02;
  options.wait_fn = [progress](double) {
    progress->entered.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // ignores the stop
    progress->left.store(true);
    return false;
  };
  const auto provider = KeycloakTokenProvider::Create(Credentials(), options);
  EXPECT_TRUE(provider->background_refresh_running());
  for (int i = 0; i < 1000 && !progress->entered.load(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_TRUE(progress->entered.load());
  const auto started = std::chrono::steady_clock::now();
  EXPECT_NO_THROW(provider->Stop());
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(250));
  EXPECT_FALSE(provider->background_refresh_running());
  for (int i = 0; i < 500 && !progress->left.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  EXPECT_TRUE(progress->left.load());
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(transport->Grants(), (std::vector<std::string>{"password"}));
}

// A stop interrupts a real (condition variable) wait at once.
TEST(KeycloakStop, StopInterruptsARealWaitImmediately) {
  const auto transport = Fake({Token("acc-1", "3600")});
  KeycloakProviderOptions options;
  options.transport = transport->AsTransport();
  const auto provider = KeycloakTokenProvider::Create(Credentials(), options);
  EXPECT_TRUE(provider->background_refresh_running());
  const auto started = std::chrono::steady_clock::now();
  provider->Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
  EXPECT_FALSE(provider->background_refresh_running());
  EXPECT_EQ(transport->calls().size(), 1u);
}

// ------------------------------------------------------------------------------------------
// H. The real thread, end to end on the real clock with the timings shrunk

// T41
TEST(KeycloakRealThread, TheThreadSurvivesFailuresAndTheTokenRecovers) {
  const auto transport =
      Fake({Token("acc-1", "0.1"), Throws(KeycloakTransportError("OSError", "down")), Refused(503),
            Throws(std::runtime_error("bug")), Token("acc-ok", "3600")});
  KeycloakProviderOptions options;
  options.transport = transport->AsTransport();
  options.timings.expiry_leeway_s = 0.05;
  options.timings.min_refresh_delay_s = 0.01;
  options.timings.retry_base_delay_s = 0.05;
  options.timings.retry_max_delay_s = 0.05;
  options.log_sink = [](KeycloakLogLevel, const std::string&) {};
  const auto provider = KeycloakTokenProvider::Create(Credentials(), options);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (provider->access_token() != "acc-ok" && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_EQ(provider->access_token(), "acc-ok");
  EXPECT_TRUE(provider->background_refresh_running()) << "the refresh thread died";
  EXPECT_EQ(transport->Grants(), Concat({"password"}, Repeat("refresh_token", 4)));
  provider->Stop();
  EXPECT_FALSE(provider->background_refresh_running());
}

// ------------------------------------------------------------------------------------------
// I. The offline-token start-up

Built HandedOff(std::vector<Step> steps, std::shared_ptr<Clock> clock = std::make_shared<Clock>(),
                const std::string& password = "", std::optional<int> token_expiration_in_s = std::nullopt,
                std::vector<bool> wait_returns = {}) {
  return Build(std::move(steps), std::move(clock), std::move(wait_returns), 0.0,
               Credentials(password, "off-handed-0", token_expiration_in_s));
}

// T42
TEST(KeycloakOfflineToken, TheStartUpIsARefreshGrantWithTheHandedOffToken) {
  const Built built = HandedOff({Token("acc-1", "300", std::string("off-1"))});
  const auto calls = built.transport->calls();
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(built.transport->Grants(), (std::vector<std::string>{"refresh_token"}));
  EXPECT_EQ(FakeTransport::Field(calls[0], "refresh_token"), "off-handed-0");
  EXPECT_FALSE(FakeTransport::Field(calls[0], "password"));
  EXPECT_FALSE(FakeTransport::Field(calls[0], "scope"));
  EXPECT_EQ(built.provider->access_token(), "acc-1");
}

// T43
TEST(KeycloakOfflineToken, APasswordAlongsideTheTokenIsNeverSent) {
  const Built built = HandedOff({Token("acc-1", "300"), Refused(), Token("acc-3", "300")},
                                std::make_shared<Clock>(), kPassword);
  built.clock->now += 280.0;
  built.provider->AuthorizationMetadata();
  built.clock->now += 5.0;
  built.provider->AuthorizationMetadata();
  EXPECT_EQ(built.transport->Grants(), Repeat("refresh_token", 3));
  for (const KeycloakHttpRequest& call : built.transport->calls()) EXPECT_FALSE(FakeTransport::Field(call, "password"));
}

// T44
TEST(KeycloakOfflineToken, ARefusedStartUpTokenRaisesWithoutAPasswordFallback) {
  const auto transport = Fake({Refused(), Token("acc-never", "300")});
  KeycloakProviderOptions options;
  options.transport = transport->AsTransport();
  const auto error = AuthErrorOf([&] { KeycloakTokenProvider::Create(Credentials(kPassword, "off-handed-0"), options); });
  ASSERT_TRUE(error);
  EXPECT_TRUE(Contains(error->what(), "Keycloak token refresh failed with status 400")) << error->what();
  EXPECT_EQ(transport->Grants(), (std::vector<std::string>{"refresh_token"}));
}

// T45
TEST(KeycloakOfflineToken, TheRotatedRefreshTokenIsUsedFromThenOn) {
  const Built built = HandedOff({Token("acc-1", "300", std::string("off-rotated")), Token("acc-2", "300")});
  built.clock->now += 280.0;
  built.provider->AuthorizationMetadata();
  EXPECT_EQ(FakeTransport::Field(built.transport->calls()[1], "refresh_token"), "off-rotated");
}

// T46
TEST(KeycloakOfflineToken, TheAutoRefreshDeadlineIsArmedOnTheOfflineTokenPath) {
  const Built built =
      HandedOff({Token("acc-1", "60")}, std::make_shared<Clock>(), "", 200, std::vector<bool>(50, false));
  EXPECT_EQ(built.provider->login_deadline(), 1200.0);
  RunLoop(built);
  EXPECT_TRUE(built.provider->stopped());
  EXPECT_LE(built.clock->now, 1200.0 + 1.0);
  // Past the deadline the per-call path no longer refreshes and serves the token as it is.
  built.clock->now = 5000.0;
  EXPECT_EQ(built.provider->AuthorizationMetadata().second, "Bearer acc-1");
}

// T47
TEST(KeycloakOfflineToken, TheBackgroundLoopSurvivesAFailingOfflineToken) {
  const Built built = HandedOff({Token("acc-1", "300"), Refused(), Refused(), Token("acc-4", "300")},
                                std::make_shared<Clock>(), "", std::nullopt, {false, false, false});
  RunLoop(built);
  EXPECT_EQ(built.transport->Grants(), Repeat("refresh_token", 4));
  EXPECT_EQ(built.provider->access_token(), "acc-4");
}

// ------------------------------------------------------------------------------------------
// J. The provider registry is keyed by credentials

ClientConfig KeycloakConfig(const std::string& tag) {
  ClientConfig config;
  config.host = "t2s.example.com";
  config.port = "443";
  config.keycloak_url = kKeycloakUrl;
  config.realm = kRealm;
  config.client_id = kClientId;
  config.username = tag + "@example.com";  // unique per test: the registry is process-wide
  config.password = kPassword;
  return config;
}

KeycloakProviderOptions RegistryOptions(const std::shared_ptr<FakeTransport>& transport) {
  KeycloakProviderOptions options;
  options.transport = transport->AsTransport();
  options.start_background_refresh = false;
  return options;
}

// T48
TEST(KeycloakRegistry, TheSameCredentialsShareOneProvider) {
  const auto transport = Fake({Token("acc-1")});
  const auto first = ondewo::client::GetKeycloakTokenProvider(KeycloakConfig("t48"), RegistryOptions(transport));
  const auto second = ondewo::client::GetKeycloakTokenProvider(KeycloakConfig("t48"), RegistryOptions(transport));
  EXPECT_EQ(first, second);
  EXPECT_EQ(transport->calls().size(), 1u);
}

// T48, under concurrency: a concurrent first use still logs in once.
TEST(KeycloakRegistry, ConcurrentFirstUseLogsInOnce) {
  const auto transport = Fake({Token("acc-1")}, std::chrono::milliseconds(20));
  std::vector<std::shared_ptr<KeycloakTokenProvider>> providers(8);
  std::vector<std::thread> threads;
  for (std::size_t i = 0; i < providers.size(); ++i) {
    threads.emplace_back([&, i] {
      providers[i] = ondewo::client::GetKeycloakTokenProvider(KeycloakConfig("t48c"), RegistryOptions(transport));
    });
  }
  for (std::thread& thread : threads) thread.join();
  for (const auto& provider : providers) EXPECT_EQ(provider, providers[0]);
  EXPECT_EQ(transport->calls().size(), 1u);
}

// T49
TEST(KeycloakRegistry, DifferentCredentialsNeverShareAProvider) {
  const std::vector<std::function<void(ClientConfig&)>> changes = {
      [](ClientConfig& c) { c.username = "other@example.com"; },
      [](ClientConfig& c) { c.password = "other-pw"; },
      [](ClientConfig& c) { c.realm = "other"; },
      [](ClientConfig& c) { c.keycloak_url = "https://x"; },
      [](ClientConfig& c) { c.client_id = "other-client"; },
      [](ClientConfig& c) { c.token_expiration_in_s = 60; },
      [](ClientConfig& c) { c.verify_ssl = false; },
      [](ClientConfig& c) { c.refresh_token = "off-other"; },
  };
  for (std::size_t i = 0; i < changes.size(); ++i) {
    const auto transport = Fake({Token("acc-1")});
    const ClientConfig base = KeycloakConfig("t49-" + std::to_string(i));
    ClientConfig changed = base;
    changes[i](changed);
    const auto first = ondewo::client::GetKeycloakTokenProvider(base, RegistryOptions(transport));
    const auto second = ondewo::client::GetKeycloakTokenProvider(changed, RegistryOptions(transport));
    EXPECT_NE(first, second) << i;
    EXPECT_EQ(transport->calls().size(), 2u) << i;
  }
}

// T50
TEST(KeycloakRegistry, FiftyConfigsCreatedAndDroppedLogInAsThemselves) {
  const auto transport = Fake({Token("acc-1")});
  std::vector<std::shared_ptr<KeycloakTokenProvider>> keep;
  std::vector<std::string> expected;
  for (int index = 0; index < 50; ++index) {
    auto config = std::make_unique<ClientConfig>(KeycloakConfig("t50-user-" + std::to_string(index)));
    keep.push_back(ondewo::client::GetKeycloakTokenProvider(*config, RegistryOptions(transport)));
    expected.push_back(config->username);
  }
  std::vector<std::string> usernames;
  for (const KeycloakHttpRequest& call : transport->calls())
    usernames.push_back(FakeTransport::Field(call, "username").value_or(""));
  EXPECT_EQ(usernames, expected);
}

// T51
TEST(KeycloakRegistry, TwoOfflineTokensWithNoPasswordNeverShareAProvider) {
  const auto transport = Fake({Token("acc-1")});
  ClientConfig tenant_a = KeycloakConfig("t51");
  tenant_a.password = "";
  tenant_a.refresh_token = "off-tenant-a";
  ClientConfig tenant_b = tenant_a;
  tenant_b.refresh_token = "off-tenant-b";
  const auto first = ondewo::client::GetKeycloakTokenProvider(tenant_a, RegistryOptions(transport));
  const auto second = ondewo::client::GetKeycloakTokenProvider(tenant_b, RegistryOptions(transport));
  EXPECT_NE(first, second);
  std::vector<std::string> tokens;
  for (const KeycloakHttpRequest& call : transport->calls())
    tokens.push_back(FakeTransport::Field(call, "refresh_token").value_or(""));
  EXPECT_EQ(tokens, (std::vector<std::string>{"off-tenant-a", "off-tenant-b"}));
}

// T52, T53, and the key derivation pinned against the python SDKs' hashlib output.
TEST(KeycloakRegistry, TheKeyIsAShaOfTheFieldsAndHoldsNoSecret) {
  using ondewo::client::internal::KeycloakProviderRegistryKey;
  const std::string with_token = KeycloakProviderRegistryKey(Credentials(kPassword, "off-secret-key-9"));
  EXPECT_FALSE(Contains(with_token, "off-secret-key-9"));
  const std::string key = KeycloakProviderRegistryKey(Credentials());
  EXPECT_FALSE(Contains(key, kPassword));
  EXPECT_EQ(key.size(), 64u);
  EXPECT_EQ(key.find_first_not_of("0123456789abcdef"), std::string::npos);
  // hashlib.sha256("\0".join([url, realm, client_id, username, password, "", "True", ""]))
  EXPECT_EQ(key, "6aaf58506471e4f9ce6bf77b4c52e196cf31143b750b00bf8d732ccb87d8a09b");
  KeycloakCredentials offline = Credentials("", "off-handed-0", 3600);
  offline.verify_ssl = false;
  EXPECT_EQ(KeycloakProviderRegistryKey(offline), "5be16bcc95982bd5fa693f1d2d790fc8ed5973b095f8a4ae771b87314943112c");
}

TEST(KeycloakRegistry, Sha256MatchesTheFipsVectors) {
  using ondewo::client::internal::Sha256Hex;
  EXPECT_EQ(Sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(Sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  EXPECT_EQ(Sha256Hex(std::string(1000000, 'a')), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// T54, and a dropped provider is rebuilt rather than resurrected.
TEST(KeycloakRegistry, AStoppedOrDroppedProviderIsNeverHandedToANewClient) {
  const auto transport = Fake({Token("acc-1")});
  const ClientConfig config = KeycloakConfig("t54");
  auto first = ondewo::client::GetKeycloakTokenProvider(config, RegistryOptions(transport));
  first->Stop();
  auto second = ondewo::client::GetKeycloakTokenProvider(config, RegistryOptions(transport));
  EXPECT_NE(first, second);
  EXPECT_FALSE(second->stopped());
  first.reset();
  std::weak_ptr<KeycloakTokenProvider> weak = second;
  second.reset();
  EXPECT_TRUE(weak.expired());
  const auto third = ondewo::client::GetKeycloakTokenProvider(config, RegistryOptions(transport));
  EXPECT_FALSE(third->stopped());
  EXPECT_EQ(transport->calls().size(), 3u);
}

// ------------------------------------------------------------------------------------------
// K. The config

// T55: refresh_token is the LAST member of both positional surfaces. A structured binding must
// name every member, so these lines stop compiling when a member is added, and the address check
// fails when one is inserted before refresh_token.
TEST(KeycloakConfig, RefreshTokenIsTheLastFieldOfEveryPositionalSurface) {
  ClientConfig config{"host", "50051", "", "", "", true, "url", "realm", "cid", "user", "pw", 60, false, "off"};
  [[maybe_unused]] auto& [host, port, cert, client_cert, client_key, secure, url, realm, client_id, username,
                          password, expiration, verify, last] = config;
  EXPECT_EQ(&last, &config.refresh_token);
  EXPECT_EQ(config.refresh_token, "off");
  EXPECT_EQ(config.token_expiration_in_s, 60);
  EXPECT_FALSE(config.verify_ssl);

  KeycloakCredentials credentials{"url", "realm", "cid", "user", "pw", std::nullopt, true, "off"};
  [[maybe_unused]] auto& [c_url, c_realm, c_client, c_user, c_password, c_expiration, c_verify, c_last] = credentials;
  EXPECT_EQ(&c_last, &credentials.refresh_token);

  // The old six-field initialisation still compiles and means what it meant.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
  const ClientConfig legacy{"host", "50051", "ca", "", "", false};
#pragma GCC diagnostic pop
  EXPECT_EQ(legacy.grpc_cert, "ca");
  EXPECT_FALSE(legacy.use_secure_channel);
  EXPECT_FALSE(legacy.KeycloakConfigured());
  EXPECT_TRUE(legacy.verify_ssl);
}

// T56
TEST(KeycloakConfig, ATokenOnlyConfigIsValidAndKeycloakConfigured) {
  ClientConfig config = KeycloakConfig("t56");
  config.password = "";
  config.refresh_token = "off-handed-0";
  EXPECT_TRUE(ondewo::client::ValidateConfig(config).ok());
  EXPECT_TRUE(config.KeycloakConfigured());
}

// T57
TEST(KeycloakConfig, TheTripleWithNeitherPasswordNorTokenIsRefused) {
  ClientConfig config = KeycloakConfig("t57");
  config.password = "";
  const grpc::Status status = ondewo::client::ValidateConfig(config);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(Contains(status.error_message(), "`refresh_token`")) << status.error_message();
  EXPECT_TRUE(Contains(status.error_message(), "`password`")) << status.error_message();
}

// T58
TEST(KeycloakConfig, APasswordConfigIsUnchanged) {
  const ClientConfig config = KeycloakConfig("t58");
  EXPECT_TRUE(ondewo::client::ValidateConfig(config).ok());
  EXPECT_TRUE(config.KeycloakConfigured());
}

// T59
TEST(KeycloakConfig, ToStringRedactsThePasswordAndTheRefreshToken) {
  ClientConfig config = KeycloakConfig("t59");
  config.refresh_token = "off-very-secret-1";
  config.token_expiration_in_s = 120;
  const std::string text = config.ToString();
  EXPECT_TRUE(Contains(text, "password=***REDACTED***")) << text;
  EXPECT_TRUE(Contains(text, "refresh_token=***REDACTED***")) << text;
  EXPECT_FALSE(Contains(text, kPassword)) << text;
  EXPECT_FALSE(Contains(text, "off-very-secret-1")) << text;
  EXPECT_TRUE(Contains(text, "keycloak_url=" + kKeycloakUrl)) << text;
  EXPECT_TRUE(Contains(text, "username=t59@example.com")) << text;
  EXPECT_TRUE(Contains(text, "token_expiration_in_s=120")) << text;
  EXPECT_TRUE(Contains(text, "verify_ssl=true")) << text;

  const ClientConfig empty;
  const std::string blank = empty.ToString();
  EXPECT_TRUE(Contains(blank, "password=,")) << blank;
  EXPECT_TRUE(Contains(blank, "refresh_token=}")) << blank;
  EXPECT_TRUE(Contains(blank, "token_expiration_in_s=,")) << blank;
  EXPECT_FALSE(Contains(blank, "REDACTED")) << blank;
}

// T60
TEST(KeycloakConfig, ATokenOnlyConfigReachesTheProviderWithThatTokenAndNoPassword) {
  const auto transport = Fake({Token("acc-1")});
  ClientConfig config = KeycloakConfig("t60");
  config.password = "";
  config.refresh_token = "off-t60";
  const auto provider = ondewo::client::GetKeycloakTokenProvider(config, RegistryOptions(transport));
  const auto calls = transport->calls();
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(FakeTransport::Field(calls[0], "grant_type"), "refresh_token");
  EXPECT_EQ(FakeTransport::Field(calls[0], "refresh_token"), "off-t60");
  EXPECT_FALSE(FakeTransport::Field(calls[0], "password"));
  const KeycloakCredentials credentials = ondewo::client::KeycloakCredentialsFrom(config);
  EXPECT_EQ(credentials.password, "");
  EXPECT_EQ(credentials.refresh_token, "off-t60");
}

// T61
TEST(KeycloakConfig, APartialTripleIsRefusedNamingTheThreeFields) {
  for (int mask = 1; mask < 7; ++mask) {
    ClientConfig config;
    config.host = "t2s.example.com";
    config.port = "443";
    config.password = kPassword;
    if (mask & 1) config.keycloak_url = kKeycloakUrl;
    if (mask & 2) config.realm = kRealm;
    if (mask & 4) config.client_id = kClientId;
    const grpc::Status status = ondewo::client::ValidateConfig(config);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT) << mask;
    EXPECT_TRUE(Contains(status.error_message(), "`keycloak_url`, `realm`, and `client_id` must be provided together"))
        << status.error_message();
    EXPECT_FALSE(config.KeycloakConfigured());
  }
  // Without Keycloak nothing new is demanded: no username, no password.
  ClientConfig plain;
  plain.host = "t2s.example.com";
  plain.port = "443";
  EXPECT_TRUE(ondewo::client::ValidateConfig(plain).ok());
}

// ------------------------------------------------------------------------------------------
// gRPC integration (T62 and the bearer header on the wire)

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct ServerPki {
  std::string ca;
  std::string key;
};

// A self-signed certificate for 127.0.0.1, generated by the openssl CLI. No key is committed.
const ServerPki& Pki() {
  static const ServerPki pki = [] {
    char dir_template[] = "/tmp/ondewo-keycloak-test-XXXXXX";
    const std::filesystem::path dir = mkdtemp(dir_template);
    const std::string command =
        "cd " + dir.string() +
        " && openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 2 -subj /CN=localhost"
        " -addext subjectAltName=DNS:localhost,IP:127.0.0.1 -keyout server.key -out server.pem > pki.log 2>&1";
    EXPECT_EQ(std::system(command.c_str()), 0) << ReadFile(dir / "pki.log");
    ServerPki result{ReadFile(dir / "server.pem"), ReadFile(dir / "server.key")};
    std::filesystem::remove_all(dir);
    return result;
  }();
  return pki;
}

// Answers every method with OK and records the authorization header of each call ("" when absent).
class RecordAuthorization : public grpc::CallbackGenericService {
 public:
  grpc::ServerGenericBidiReactor* CreateReactor(grpc::GenericCallbackServerContext* context) override {
    std::string header;
    const auto found = context->client_metadata().find("authorization");
    if (found != context->client_metadata().end()) header.assign(found->second.data(), found->second.size());
    {
      std::lock_guard<std::mutex> lock(mutex_);
      headers_.push_back(header);
    }
    class Reactor : public grpc::ServerGenericBidiReactor {
     public:
      Reactor() { StartWriteAndFinish(&response_, grpc::WriteOptions(), grpc::Status::OK); }
      void OnDone() override { delete this; }

     private:
      grpc::Slice empty_{""};
      grpc::ByteBuffer response_{&empty_, 1};
    };
    return new Reactor;
  }

  std::vector<std::string> headers() {
    std::lock_guard<std::mutex> lock(mutex_);
    return headers_;
  }

 private:
  std::mutex mutex_;
  std::vector<std::string> headers_;
};

class AuthServer {
 public:
  explicit AuthServer(bool tls) {
    grpc::SslServerCredentialsOptions options(GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE);
    if (tls) options.pem_key_cert_pairs.push_back({Pki().key, Pki().ca});
    grpc::ServerBuilder builder;
    builder.RegisterCallbackGenericService(&service);
    builder.AddListeningPort("127.0.0.1:0",
                             tls ? grpc::SslServerCredentials(options) : grpc::InsecureServerCredentials(), &port);
    server = builder.BuildAndStart();
  }
  ~AuthServer() {
    if (server) server->Shutdown(std::chrono::system_clock::now());
  }

  RecordAuthorization service;
  int port = 0;
  std::unique_ptr<grpc::Server> server;
};

grpc::Status Call(const std::shared_ptr<grpc::Channel>& channel, KeycloakTokenProvider* per_call = nullptr) {
  grpc::GenericStub stub(channel);
  grpc::CompletionQueue queue;
  grpc::ClientContext context;
  if (per_call != nullptr) ondewo::client::AddKeycloakAuthorization(*per_call, &context);
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
  grpc::Slice empty("");
  const grpc::ByteBuffer request(&empty, 1);
  grpc::ByteBuffer response;
  grpc::Status status;
  auto call = stub.PrepareUnaryCall(&context, "/ondewo.test.Auth/Check", request, &queue);
  call->StartCall();
  call->Finish(&response, &status, nullptr);
  void* tag = nullptr;
  bool ok = false;
  EXPECT_TRUE(queue.Next(&tag, &ok));
  queue.Shutdown();
  while (queue.Next(&tag, &ok)) continue;
  return status;
}

ClientConfig ServerConfig(const AuthServer& server, const std::string& tag, bool tls = true) {
  ClientConfig config = KeycloakConfig(tag);
  config.host = "127.0.0.1";
  config.port = std::to_string(server.port);
  config.grpc_cert = tls ? Pki().ca : "";
  config.use_secure_channel = tls;
  return config;
}

TEST(KeycloakGrpc, EveryCallCarriesTheBearerToken) {
  AuthServer server(true);
  const auto transport = Fake({Token("acc-wire-1", "3600")});
  std::shared_ptr<grpc::Channel> channel;
  std::shared_ptr<KeycloakTokenProvider> provider;
  const grpc::Status created = ondewo::client::CreateAuthenticatedChannel(
      ServerConfig(server, "grpc-wire"), RegistryOptions(transport), &channel, &provider);
  ASSERT_TRUE(created.ok()) << created.error_message();
  ASSERT_NE(provider, nullptr);
  EXPECT_TRUE(Call(channel).ok());
  EXPECT_TRUE(Call(channel).ok());
  EXPECT_EQ(server.service.headers(), Repeat("Bearer acc-wire-1", 2));
  EXPECT_EQ(transport->calls().size(), 1u);
}

// The plugin answers UNAUTHENTICATED; gRPC 1.51 reports any plugin failure to the caller as
// UNAVAILABLE ("Getting metadata from plugin failed with error: <text>"), newer releases keep the
// code. Either way the call fails with the error text and is never sent.
TEST(KeycloakGrpc, AnExpiredTokenFailsTheCallAndNothingIsSent) {
  AuthServer server(true);
  const auto transport = Fake({Token("acc-1", "300"), Refused()});
  auto clock = std::make_shared<Clock>();
  KeycloakProviderOptions options = RegistryOptions(transport);
  options.time_fn = [clock] { return clock->now; };
  options.log_sink = [](KeycloakLogLevel, const std::string&) {};
  std::shared_ptr<grpc::Channel> channel;
  ASSERT_TRUE(ondewo::client::CreateAuthenticatedChannel(ServerConfig(server, "grpc-expired"), options,
                                                         ondewo::client::DefaultChannelArguments(), &channel)
                  .ok());
  clock->now += 400.0;
  const grpc::Status status = Call(channel);
  EXPECT_TRUE(status.error_code() == grpc::StatusCode::UNAUTHENTICATED ||
              status.error_code() == grpc::StatusCode::UNAVAILABLE)
      << status.error_code() << " " << status.error_message();
  EXPECT_TRUE(Contains(status.error_message(), "status 400")) << status.error_message();
  EXPECT_TRUE(server.service.headers().empty());
}

TEST(KeycloakGrpc, TheMetadataPluginReportsAFailureAsUnauthenticated) {
  const Built built = Build({Token("acc-1", "300"), Refused()});
  ondewo::client::KeycloakMetadataPlugin plugin(built.provider);
  EXPECT_TRUE(plugin.IsBlocking());
  EXPECT_STREQ(plugin.GetType(), "ondewo-keycloak");
  std::multimap<grpc::string, grpc::string> metadata;
  // An AuthContext is only needed for the signature; the plugin never reads it.
  class NoProperties : public grpc::AuthPropertyIterator {
   public:
    NoProperties() = default;
  };
  class NoAuthContext : public grpc::AuthContext {
   public:
    bool IsPeerAuthenticated() const override { return false; }
    std::vector<grpc::string_ref> GetPeerIdentity() const override { return {}; }
    std::string GetPeerIdentityPropertyName() const override { return ""; }
    std::vector<grpc::string_ref> FindPropertyValues(const std::string&) const override { return {}; }
    grpc::AuthPropertyIterator begin() const override { return NoProperties(); }
    grpc::AuthPropertyIterator end() const override { return NoProperties(); }
    void AddProperty(const std::string&, const grpc::string_ref&) override {}
    bool SetPeerIdentityPropertyName(const std::string&) override { return false; }
  } context;
  EXPECT_FALSE(context.IsPeerAuthenticated());
  EXPECT_TRUE(context.GetPeerIdentity().empty());
  EXPECT_EQ(context.GetPeerIdentityPropertyName(), "");
  EXPECT_TRUE(context.FindPropertyValues("x").empty());
  EXPECT_TRUE(context.begin() == context.end());
  context.AddProperty("x", "y");
  EXPECT_FALSE(context.SetPeerIdentityPropertyName("x"));
  EXPECT_TRUE(plugin.GetMetadata("", "", context, &metadata).ok());
  EXPECT_EQ(metadata, (std::multimap<grpc::string, grpc::string>{{"authorization", "Bearer acc-1"}}));
  built.clock->now += 400.0;
  const grpc::Status failed = plugin.GetMetadata("", "", context, &metadata);
  EXPECT_EQ(failed.error_code(), grpc::StatusCode::UNAUTHENTICATED);
  EXPECT_TRUE(Contains(failed.error_message(), "status 400")) << failed.error_message();
}

// T62
TEST(KeycloakGrpc, WithoutTheTripleNoProviderIsBuiltAndNoHeaderSent) {
  AuthServer server(false);
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = std::to_string(server.port);
  config.use_secure_channel = false;
  config.username = kUsername;
  config.password = kPassword;
  const auto transport = Fake({Token("acc-1")});
  std::shared_ptr<grpc::Channel> channel;
  std::shared_ptr<KeycloakTokenProvider> provider;
  std::ostringstream captured;
  std::streambuf* const original = std::clog.rdbuf(captured.rdbuf());
  const grpc::Status created =
      ondewo::client::CreateAuthenticatedChannel(config, RegistryOptions(transport), &channel, &provider);
  std::clog.rdbuf(original);
  ASSERT_TRUE(created.ok()) << created.error_message();
  EXPECT_EQ(provider, nullptr);
  EXPECT_TRUE(Call(channel).ok());
  EXPECT_EQ(server.service.headers(), (std::vector<std::string>{""}));
  EXPECT_TRUE(transport->calls().empty());
}

TEST(KeycloakGrpc, APlaintextChannelWithKeycloakIsRefusedButPerCallMetadataWorks) {
  AuthServer server(false);
  const auto transport = Fake({Token("acc-plain", "3600")});
  const ClientConfig config = ServerConfig(server, "grpc-plain", false);
  std::shared_ptr<grpc::Channel> channel;
  const grpc::Status refused = ondewo::client::CreateAuthenticatedChannel(config, RegistryOptions(transport), &channel);
  EXPECT_EQ(refused.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(Contains(refused.error_message(), "never sent over a plaintext channel")) << refused.error_message();
  EXPECT_EQ(channel, nullptr);
  EXPECT_TRUE(transport->calls().empty());

  std::ostringstream captured;
  std::streambuf* const original = std::clog.rdbuf(captured.rdbuf());
  ASSERT_TRUE(ondewo::client::CreateChannel(config, &channel).ok());
  std::clog.rdbuf(original);
  const auto provider = ondewo::client::GetKeycloakTokenProvider(config, RegistryOptions(transport));
  EXPECT_TRUE(Call(channel, provider.get()).ok());
  EXPECT_EQ(server.service.headers(), (std::vector<std::string>{"Bearer acc-plain"}));
}

TEST(KeycloakGrpc, ConfigAndLoginFailuresOpenNothing) {
  AuthServer server(true);
  std::shared_ptr<grpc::Channel> channel;
  // An invalid config.
  ClientConfig invalid = ServerConfig(server, "grpc-invalid");
  invalid.password = "";
  EXPECT_EQ(ondewo::client::CreateAuthenticatedChannel(invalid, RegistryOptions(Fake({Token("a")})), &channel)
                .error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  // No transport.
  const grpc::Status no_transport = ondewo::client::CreateAuthenticatedChannel(
      ServerConfig(server, "grpc-no-transport"), KeycloakProviderOptions(), &channel);
  EXPECT_EQ(no_transport.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(Contains(no_transport.error_message(), "transport is empty")) << no_transport.error_message();
  // A refused login.
  const grpc::Status refused = ondewo::client::CreateAuthenticatedChannel(
      ServerConfig(server, "grpc-refused"), RegistryOptions(Fake({Refused(401)})), &channel);
  EXPECT_EQ(refused.error_code(), grpc::StatusCode::UNAUTHENTICATED);
  EXPECT_TRUE(Contains(refused.error_message(), "Keycloak token login failed with status 401"))
      << refused.error_message();
  EXPECT_FALSE(Contains(refused.error_message(), kPassword));
  EXPECT_EQ(channel, nullptr);
}

// ------------------------------------------------------------------------------------------
// The libcurl transport, against an HTTP server on loopback

// Accepts ONE connection, records the raw request and answers with the given status and body,
// or never answers when status is 0 (for the timeout case).
class OneShotHttpServer {
 public:
  OneShotHttpServer(int status, std::string body) : status_(status), body_(std::move(body)) {
    listener_ = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    socklen_t length = sizeof(address);
    getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = ntohs(address.sin_port);
    listen(listener_, 1);
    thread_ = std::thread([this] { Serve(); });
  }

  ~OneShotHttpServer() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closing_ = true;
    }
    closed_.notify_all();
    shutdown(listener_, SHUT_RDWR);
    close(listener_);
    thread_.join();
  }

  std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/auth/realms/r/token"; }

  std::string request() {
    std::lock_guard<std::mutex> lock(mutex_);
    return request_;
  }

 private:
  void Serve() {
    const int connection = accept(listener_, nullptr, nullptr);
    if (connection < 0) return;
    std::string received;
    char buffer[4096];
    while (true) {
      const std::size_t header_end = received.find("\r\n\r\n");
      if (header_end != std::string::npos) {
        const std::size_t length_at = received.find("Content-Length: ");
        const std::size_t length =
            length_at == std::string::npos ? 0 : std::stoul(received.substr(length_at + 16));
        if (received.size() >= header_end + 4 + length) break;
      }
      const ssize_t read = recv(connection, buffer, sizeof(buffer), 0);
      if (read <= 0) break;
      received.append(buffer, static_cast<std::size_t>(read));
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      request_ = received;
    }
    if (status_ == 0) {
      std::unique_lock<std::mutex> lock(mutex_);
      closed_.wait(lock, [this] { return closing_; });
    } else {
      const std::string answer = "HTTP/1.1 " + std::to_string(status_) + " X\r\nContent-Type: application/json\r\n" +
                                 "Content-Length: " + std::to_string(body_.size()) + "\r\nConnection: close\r\n\r\n" +
                                 body_;
      send(connection, answer.data(), answer.size(), 0);
    }
    close(connection);
  }

  int status_;
  std::string body_;
  int listener_ = -1;
  int port_ = 0;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable closed_;
  bool closing_ = false;
  std::string request_;
};

KeycloakHttpRequest CurlRequest(const std::string& url) {
  KeycloakHttpRequest request;
  request.url = url;
  request.form = {{"grant_type", "password"}, {"username", "user@example.com"}, {"password", "p&w =x"}};
  request.timeout_s = 5.0;
  return request;
}

TEST(KeycloakCurlTransport, PostsAFormAndReturnsTheAnswer) {
  OneShotHttpServer server(200, R"({"access_token":"acc-curl"})");
  const KeycloakHttpResponse response = ondewo::client::CurlKeycloakTransport()(CurlRequest(server.url()));
  EXPECT_EQ(response.status_code, 200);
  EXPECT_EQ(response.body, R"({"access_token":"acc-curl"})");
  const std::string request = server.request();
  EXPECT_EQ(request.rfind("POST /auth/realms/r/token HTTP/1.1\r\n", 0), 0u) << request;
  EXPECT_TRUE(Contains(request, "Content-Type: application/x-www-form-urlencoded\r\n")) << request;
  EXPECT_TRUE(Contains(request, "\r\n\r\ngrant_type=password&username=user%40example.com&password=p%26w%20%3Dx"))
      << request;
}

TEST(KeycloakCurlTransport, AnErrorStatusIsReturnedNotThrown) {
  OneShotHttpServer server(503, R"({"error":"temporarily_unavailable"})");
  KeycloakHttpRequest request = CurlRequest(server.url());
  request.verify_ssl = false;
  const KeycloakHttpResponse response = ondewo::client::CurlKeycloakTransport()(request);
  EXPECT_EQ(response.status_code, 503);
  EXPECT_TRUE(Contains(response.body, "temporarily_unavailable"));
}

KeycloakTransportError CurlErrorOf(const KeycloakHttpRequest& request) {
  KeycloakTransportError caught("", "");
  const auto capture = [&] { try { ondewo::client::CurlKeycloakTransport()(request); } catch (const KeycloakTransportError& error) { caught = error; throw; } };
  EXPECT_THROW(capture(), KeycloakTransportError);
  return caught;
}

TEST(KeycloakCurlTransport, NoAnswerThrowsATransportErrorOfTheRightKind) {
  // A port nothing listens on.
  const int probe = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  socklen_t length = sizeof(address);
  getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length);
  close(probe);
  const KeycloakTransportError refused =
      CurlErrorOf(CurlRequest("http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/token"));
  EXPECT_EQ(refused.kind(), "ConnectionError") << refused.what();

  OneShotHttpServer silent(0, "");
  KeycloakHttpRequest slow = CurlRequest(silent.url());
  slow.timeout_s = 0.3;
  const KeycloakTransportError timeout = CurlErrorOf(slow);
  EXPECT_EQ(timeout.kind(), "Timeout") << timeout.what();
  EXPECT_FALSE(Contains(timeout.what(), "p&w")) << timeout.what();

  const KeycloakTransportError other = CurlErrorOf(CurlRequest("unsupported-scheme://127.0.0.1/token"));
  EXPECT_EQ(other.kind(), "TransportError") << other.what();
}

TEST(KeycloakCurlTransport, TheCurlOptionsBuildAWorkingProvider) {
  OneShotHttpServer server(200, R"({"access_token":"acc-curl","expires_in":300,"refresh_token":"off-curl"})");
  ClientConfig config = KeycloakConfig("curl");
  config.keycloak_url = server.url().substr(0, server.url().find("/auth/") + 5);
  KeycloakProviderOptions options = ondewo::client::CurlKeycloakProviderOptions();
  options.start_background_refresh = false;
  const auto provider = KeycloakTokenProvider::Create(ondewo::client::KeycloakCredentialsFrom(config), options);
  EXPECT_EQ(provider->AuthorizationMetadata().second, "Bearer acc-curl");
  EXPECT_TRUE(Contains(server.request(), "POST /auth/realms/" + kRealm + "/protocol/openid-connect/token"))
      << server.request();
  EXPECT_TRUE(Contains(server.request(), "scope=offline_access")) << server.request();
}

}  // namespace
}  // namespace ondewo_client_test
