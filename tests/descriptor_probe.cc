#include "descriptor_probe.h"

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/util/message_differencer.h>

namespace ondewo_client_test {
namespace {

using ::google::protobuf::Descriptor;
using ::google::protobuf::DescriptorPool;
using ::google::protobuf::EnumDescriptor;
using ::google::protobuf::FieldDescriptor;
using ::google::protobuf::FileDescriptor;
using ::google::protobuf::Message;
using ::google::protobuf::MessageFactory;
using ::google::protobuf::Reflection;
using ::google::protobuf::ServiceDescriptor;

// The pool the generated *.pb.cc files register themselves into. Reading it proves the
// stubs were linked in and their static initialisers ran.
const DescriptorPool* GeneratedPool() { return DescriptorPool::generated_pool(); }

// Appends `descriptor` and, recursively, every message nested inside it. Synthetic
// map-entry types are left out: they are an implementation detail of map<> fields and have
// no generated C++ class of their own to instantiate.
void CollectMessages(const Descriptor* descriptor, std::vector<const Descriptor*>* out) {
  if (descriptor->options().map_entry()) {
    return;
  }
  out->push_back(descriptor);
  for (int i = 0; i < descriptor->nested_type_count(); ++i) {
    CollectMessages(descriptor->nested_type(i), out);
  }
}

// Appends every enum declared inside `descriptor`, recursing through nested messages.
void CollectEnums(const Descriptor* descriptor, std::vector<const EnumDescriptor*>* out) {
  for (int i = 0; i < descriptor->enum_type_count(); ++i) {
    out->push_back(descriptor->enum_type(i));
  }
  for (int i = 0; i < descriptor->nested_type_count(); ++i) {
    CollectEnums(descriptor->nested_type(i), out);
  }
}

}  // namespace

const FileDescriptor* FindFile(const std::string& file_name) {
  return GeneratedPool()->FindFileByName(file_name);
}

const Descriptor* FindMessage(const std::string& full_name) {
  return GeneratedPool()->FindMessageTypeByName(full_name);
}

const ServiceDescriptor* FindService(const std::string& full_name) {
  return GeneratedPool()->FindServiceByName(full_name);
}

std::vector<std::string> MethodNames(const std::string& service_full_name) {
  std::vector<std::string> names;
  const ServiceDescriptor* service = FindService(service_full_name);
  if (service == nullptr) {
    return names;
  }
  for (int i = 0; i < service->method_count(); ++i) {
    names.push_back(service->method(i)->name());
  }
  return names;
}

std::vector<const Descriptor*> MessagesInFile(const std::string& file_name) {
  std::vector<const Descriptor*> messages;
  const FileDescriptor* file = FindFile(file_name);
  if (file == nullptr) {
    return messages;
  }
  for (int i = 0; i < file->message_type_count(); ++i) {
    CollectMessages(file->message_type(i), &messages);
  }
  return messages;
}

std::vector<const EnumDescriptor*> EnumsInFile(const std::string& file_name) {
  std::vector<const EnumDescriptor*> enums;
  const FileDescriptor* file = FindFile(file_name);
  if (file == nullptr) {
    return enums;
  }
  for (int i = 0; i < file->enum_type_count(); ++i) {
    enums.push_back(file->enum_type(i));
  }
  for (int i = 0; i < file->message_type_count(); ++i) {
    CollectEnums(file->message_type(i), &enums);
  }
  return enums;
}

std::unique_ptr<Message> NewMessage(const std::string& full_name) {
  const Descriptor* descriptor = FindMessage(full_name);
  if (descriptor == nullptr) {
    return nullptr;
  }
  return std::unique_ptr<Message>(
      MessageFactory::generated_factory()->GetPrototype(descriptor)->New());
}

int FillScalarFields(Message* message) {
  const Descriptor* descriptor = message->GetDescriptor();
  const Reflection* reflection = message->GetReflection();
  int filled = 0;
  for (int i = 0; i < descriptor->field_count(); ++i) {
    const FieldDescriptor* field = descriptor->field(i);
    // Repeated fields (map entries included) stay empty - an empty repeated field is
    // indistinguishable from an absent one on the wire, so it adds nothing to the
    // round-trip assertion.
    if (field->is_repeated()) {
      continue;
    }
    const int seed = field->number();
    switch (field->cpp_type()) {
      case FieldDescriptor::CPPTYPE_INT32:
        reflection->SetInt32(message, field, seed);
        break;
      case FieldDescriptor::CPPTYPE_INT64:
        reflection->SetInt64(message, field, seed);
        break;
      case FieldDescriptor::CPPTYPE_UINT32:
        reflection->SetUInt32(message, field, static_cast<uint32_t>(seed));
        break;
      case FieldDescriptor::CPPTYPE_UINT64:
        reflection->SetUInt64(message, field, static_cast<uint64_t>(seed));
        break;
      case FieldDescriptor::CPPTYPE_DOUBLE:
        reflection->SetDouble(message, field, seed + 0.5);
        break;
      case FieldDescriptor::CPPTYPE_FLOAT:
        reflection->SetFloat(message, field, seed + 0.25F);
        break;
      case FieldDescriptor::CPPTYPE_BOOL:
        reflection->SetBool(message, field, true);
        break;
      case FieldDescriptor::CPPTYPE_ENUM:
        // The LAST declared value, never value(0): a proto3 zero value is the field
        // default and would be skipped by the writer, so it would not exercise the wire.
        reflection->SetEnumValue(
            message, field,
            field->enum_type()->value(field->enum_type()->value_count() - 1)->number());
        break;
      case FieldDescriptor::CPPTYPE_STRING:
        // Valid UTF-8 for a `string` field and arbitrary bytes for a `bytes` field alike.
        reflection->SetString(message, field, "ondewo-" + field->name());
        break;
      default:
        continue;
    }
    ++filled;
  }
  return filled;
}

bool RoundTrips(const Message& message) {
  // No early returns: every line below runs on every call, which keeps this helper
  // honestly measurable at 100 % line coverage.
  std::string bytes;
  const bool serialized = message.SerializeToString(&bytes);
  std::unique_ptr<Message> parsed(message.New());
  const bool reparsed = parsed->ParseFromString(bytes);
  const bool equal = google::protobuf::util::MessageDifferencer::Equals(message, *parsed);
  return serialized && reparsed && equal;
}

}  // namespace ondewo_client_test
