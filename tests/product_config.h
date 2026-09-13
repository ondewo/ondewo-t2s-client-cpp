// What the generated stubs of THIS product must contain.
//
// This is the only per-product file in the suite. descriptor_probe.* and
// test_generated_stubs.cc are product-agnostic and can be copied verbatim into the other
// ONDEWO C++ clients; replicating the suite means rewriting the tables below (and
// test_typed_api.cc, which names concrete C++ types) for that product's .proto surface.

#pragma once

#include <string>
#include <vector>

namespace ondewo_client_test {

// One expected RPC of one expected service.
struct ExpectedMethod {
  std::string service_full_name;
  std::string method_name;
};

// Every .proto file whose generated code is committed under api/. Each one must be
// registered in the descriptor pool, i.e. the generator must have emitted it AND the
// archive must actually carry it.
extern const std::vector<std::string> kProtoFileNames;

// Fully-qualified name of every gRPC service the product declares.
extern const std::vector<std::string> kServiceFullNames;

// A representative slice of the RPC surface. Not the full API mirror on purpose - this
// catches a generator that drops or renames methods without turning every API release
// into a test-data rewrite.
extern const std::vector<ExpectedMethod> kExpectedMethods;

// A message that is guaranteed to carry at least one singular scalar field, used by the
// tests that need a concrete non-empty payload.
extern const std::string kScalarMessageFullName;

// An enum that must exist and, per proto3, must declare 0 as its first value.
extern const std::string kEnumFullName;

// Floors for the pool-wide sweeps. They exist so the sweeps cannot quietly shrink to a
// handful of types when a .proto stops being generated or linked; raise them when the API
// grows, never lower them to make a red build green.
extern const int kMinimumMessageCount;
extern const int kMinimumEnumCount;
extern const int kMinimumScalarFieldCount;

}  // namespace ondewo_client_test
