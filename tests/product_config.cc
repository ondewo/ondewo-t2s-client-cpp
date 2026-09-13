#include "product_config.h"

namespace ondewo_client_test {

// Mirrors the #include list of the generated public-api.h, one .proto per pair of headers:
//   sed -n 's|^#include "\(.*\)\.pb\.h"$|\1|p' public-api.h | sed 's|\.grpc$||' | sort -u
//
// The ONDEWO T2S API is a single .proto file. Its imports (google/protobuf/empty.proto,
// struct.proto) are well-known types that live inside libprotobuf, so no google/* code is
// generated into api/ and none is listed here.
const std::vector<std::string> kProtoFileNames = {
    "ondewo/t2s/text-to-speech.proto",
};

const std::vector<std::string> kServiceFullNames = {
    "ondewo.t2s.Text2Speech",
};

const std::vector<ExpectedMethod> kExpectedMethods = {
    // The three RPCs the product exists for, one of them bidirectionally streaming ...
    {"ondewo.t2s.Text2Speech", "Synthesize"},
    {"ondewo.t2s.Text2Speech", "BatchSynthesize"},
    {"ondewo.t2s.Text2Speech", "StreamingSynthesize"},
    // ... the complete CRUD surface over the pipeline configurations ...
    {"ondewo.t2s.Text2Speech", "GetT2sPipeline"},
    {"ondewo.t2s.Text2Speech", "CreateT2sPipeline"},
    {"ondewo.t2s.Text2Speech", "UpdateT2sPipeline"},
    {"ondewo.t2s.Text2Speech", "DeleteT2sPipeline"},
    {"ondewo.t2s.Text2Speech", "ListT2sPipelines"},
    // ... the listing RPCs ...
    {"ondewo.t2s.Text2Speech", "ListT2sLanguages"},
    {"ondewo.t2s.Text2Speech", "ListT2sDomains"},
    {"ondewo.t2s.Text2Speech", "ListT2sNormalizationPipelines"},
    // ... the second CRUD surface, over the custom phonemizers ...
    {"ondewo.t2s.Text2Speech", "GetCustomPhonemizer"},
    {"ondewo.t2s.Text2Speech", "CreateCustomPhonemizer"},
    {"ondewo.t2s.Text2Speech", "UpdateCustomPhonemizer"},
    {"ondewo.t2s.Text2Speech", "DeleteCustomPhonemizer"},
    {"ondewo.t2s.Text2Speech", "ListCustomPhonemizer"},
    // ... text normalization and voice cloning ...
    {"ondewo.t2s.Text2Speech", "NormalizeText"},
    {"ondewo.t2s.Text2Speech", "VoiceCloning"},
    // ... and the RPC whose request type comes from google.protobuf rather than from T2S.
    {"ondewo.t2s.Text2Speech", "GetServiceInfo"},
};

// RequestConfig carries a plain string, oneof-wrapped floats / an int32 / a bool / two enums
// and a proto3 `optional` string - the widest singular-scalar mix in the API.
const std::string kScalarMessageFullName = "ondewo.t2s.RequestConfig";

const std::string kEnumFullName = "ondewo.t2s.AudioFormat";

// ONDEWO T2S API 6.6.0 generates 60 messages, 3 enums and 161 singular scalar fields
// across the file listed above (text-to-speech.proto declares no map<> field, so there is
// no synthetic entry type to exclude). The floors sit just below that; two of the three
// enums are declared at file scope and UpdateMethod is nested inside CustomPhonemizerProto.
const int kMinimumMessageCount = 55;
const int kMinimumEnumCount = 3;
const int kMinimumScalarFieldCount = 155;

}  // namespace ondewo_client_test
