// Reflection helpers shared by the ondewo-t2s-client-cpp test suite.
//
// Everything here works through the protobuf descriptor pool that the generated stubs
// register into at static-initialisation time. That is deliberate: a probe written
// against the descriptors stays identical for every ONDEWO product, so the same file can
// be copied verbatim into the s2t / t2s / sip / csi / vtsi / survey clients - only
// product_config.cc, which lists what the pool must contain, differs between them.
//
// This is the only hand-written non-test logic in the repository and is therefore held to
// 100 % line coverage by `make coverage`.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

namespace ondewo_client_test {

// Descriptor of a .proto file by the path protoc recorded for it
// ("ondewo/nlu/context.proto"), or nullptr when its generated code was never linked in.
const google::protobuf::FileDescriptor* FindFile(const std::string& file_name);

// Descriptor of a message by its fully-qualified proto name ("ondewo.nlu.Context"),
// or nullptr when the generated stubs never registered it.
const google::protobuf::Descriptor* FindMessage(const std::string& full_name);

// Descriptor of a gRPC service by its fully-qualified proto name ("ondewo.nlu.Contexts"),
// or nullptr when the generated stubs never registered it.
const google::protobuf::ServiceDescriptor* FindService(const std::string& full_name);

// Method names of a service, in declaration order. Empty for an unknown service.
std::vector<std::string> MethodNames(const std::string& service_full_name);

// Every message a .proto file declares, nested types included but the synthetic map-entry
// types of map<> fields left out, in an unspecified order. Empty when the file was never
// linked into the binary.
std::vector<const google::protobuf::Descriptor*> MessagesInFile(const std::string& file_name);

// Every enum a .proto file declares, both top-level and nested inside its messages.
// Empty when the file was never linked into the binary.
std::vector<const google::protobuf::EnumDescriptor*> EnumsInFile(const std::string& file_name);

// A default-constructed instance of a message, or nullptr for an unknown name. The caller
// owns the result.
std::unique_ptr<google::protobuf::Message> NewMessage(const std::string& full_name);

// Sets every singular scalar field of `message` to a deterministic non-default value
// derived from the field number, so that the field actually reaches the wire.
//
// Message-typed fields are left untouched - a sub-message is covered on its own by the
// per-message sweep, which fills and round-trips every generated message in the pool.
//
// Returns the number of fields it set.
int FillScalarFields(google::protobuf::Message* message);

// True when `message` survives a serialize -> parse -> compare cycle: the bytes parse back
// into a message of the same type that compares equal to the original. This is the check
// that catches a generator emitting a field the writer never puts on the wire or the
// reader never picks up again.
bool RoundTrips(const google::protobuf::Message& message);

}  // namespace ondewo_client_test
