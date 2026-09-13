// Assertions against the concrete C++ types the T2S stubs generate.
//
// This is the per-product half of the suite: it names ondewo::t2s types, so replicating
// the suite to another ONDEWO client means rewriting this file against that product's
// messages and services. Everything generic lives in test_generated_stubs.cc.

#include <chrono>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include "ondewo/t2s/text-to-speech.grpc.pb.h"
#include "ondewo/t2s/text-to-speech.pb.h"

namespace ondewo_client_test {
namespace {

// A channel to a port nothing listens on. gRPC connects lazily, so constructing stubs
// against it touches no network at all; the one test that does issue an RPC gives it a
// short deadline and asserts only that the call comes back as a failure.
std::shared_ptr<grpc::Channel> DeadChannel() {
  return grpc::CreateChannel("127.0.0.1:1", grpc::InsecureChannelCredentials());
}

TEST(TypedApi, MessageSurvivesSerializeAndParse) {
  ondewo::t2s::SynthesizeRequest original;
  original.set_text("Guten Tag, wie geht es Ihnen?");

  ondewo::t2s::RequestConfig* config = original.mutable_config();
  config->set_t2s_pipeline_id("pipeline-de-1");
  config->set_pcm(ondewo::t2s::Pcm::PCM_24);
  config->set_audio_format(ondewo::t2s::AudioFormat::flac);
  config->set_sample_rate(22050);
  config->set_length_scale(1.25F);

  std::string bytes;
  ASSERT_TRUE(original.SerializeToString(&bytes));
  EXPECT_FALSE(bytes.empty());

  ondewo::t2s::SynthesizeRequest parsed;
  ASSERT_TRUE(parsed.ParseFromString(bytes));

  EXPECT_EQ(parsed.text(), "Guten Tag, wie geht es Ihnen?");
  ASSERT_TRUE(parsed.has_config());
  EXPECT_EQ(parsed.config().t2s_pipeline_id(), "pipeline-de-1");
  EXPECT_EQ(parsed.config().pcm(), ondewo::t2s::Pcm::PCM_24);
  EXPECT_EQ(parsed.config().audio_format(), ondewo::t2s::AudioFormat::flac);
  EXPECT_EQ(parsed.config().sample_rate(), 22050);
  EXPECT_FLOAT_EQ(parsed.config().length_scale(), 1.25F);
  // A oneof member reports presence of its own, which is what distinguishes "the caller
  // asked for 22050 Hz" from "the caller said nothing about the sample rate".
  EXPECT_TRUE(parsed.config().has_sample_rate());
  EXPECT_EQ(parsed.SerializeAsString(), bytes);
}

// `optional string instruction = 13` has proto3 explicit presence. Set to "" - the type's
// default - it must still reach the wire and still read back as *present*; a generator
// that drops the presence bit makes "" unsendable, which is exactly the class of bug that
// hit the Angular client.
TEST(TypedApi, ExplicitPresenceFieldSurvivesItsZeroValue) {
  ondewo::t2s::RequestConfig original;
  EXPECT_FALSE(original.has_instruction());

  original.set_instruction("");
  ASSERT_TRUE(original.has_instruction());

  const std::string bytes = original.SerializeAsString();
  EXPECT_FALSE(bytes.empty()) << "an explicitly present empty string was not written to the wire";

  ondewo::t2s::RequestConfig parsed;
  ASSERT_TRUE(parsed.ParseFromString(bytes));
  EXPECT_TRUE(parsed.has_instruction()) << "presence of an empty value was lost on the wire";
  EXPECT_EQ(parsed.instruction(), "");

  original.clear_instruction();
  EXPECT_FALSE(original.has_instruction());
  EXPECT_TRUE(original.SerializeAsString().empty());
}

// A plain (non-optional) proto3 scalar has the opposite contract: its zero value is the
// default and must NOT be written. Asserting both directions is what proves the two field
// kinds really are generated differently.
TEST(TypedApi, PlainScalarZeroValueStaysOffTheWire) {
  ondewo::t2s::RequestConfig config;
  config.set_t2s_pipeline_id("");
  EXPECT_TRUE(config.SerializeAsString().empty());

  config.set_t2s_pipeline_id("pipeline-de-1");
  EXPECT_FALSE(config.SerializeAsString().empty());
}

TEST(TypedApi, EnumZeroValueIsTheUnspecifiedOne) {
  EXPECT_EQ(static_cast<int>(ondewo::t2s::AudioFormat::wav), 0);
  EXPECT_EQ(ondewo::t2s::AudioFormat_Name(ondewo::t2s::AudioFormat::wav), "wav");
  EXPECT_EQ(static_cast<int>(ondewo::t2s::Pcm::PCM_16), 0);

  ondewo::t2s::AudioFormat parsed = ondewo::t2s::AudioFormat::mp3;
  ASSERT_TRUE(ondewo::t2s::AudioFormat_Parse("wav", &parsed));
  EXPECT_EQ(parsed, ondewo::t2s::AudioFormat::wav);

  // A request defaults to the zero format, so the zero value has to be requestable.
  ondewo::t2s::RequestConfig config;
  EXPECT_EQ(config.audio_format(), ondewo::t2s::AudioFormat::wav);
  EXPECT_EQ(config.pcm(), ondewo::t2s::Pcm::PCM_16);
}

TEST(TypedApi, ServiceStubsAreConstructibleAgainstAChannel) {
  const std::shared_ptr<grpc::Channel> channel = DeadChannel();
  ASSERT_NE(channel, nullptr);

  std::unique_ptr<ondewo::t2s::Text2Speech::Stub> text_to_speech =
      ondewo::t2s::Text2Speech::NewStub(channel);

  EXPECT_NE(text_to_speech, nullptr);
}

TEST(TypedApi, ServicesKeepTheirFullyQualifiedNames) {
  EXPECT_STREQ(ondewo::t2s::Text2Speech::service_full_name(), "ondewo.t2s.Text2Speech");
}

// Actually issue an RPC. Nothing is listening, so the only correct outcome is a failure -
// but reaching a transport-level failure means the stub, the request/response types and
// the generated method descriptor all linked and dispatched. A crash or an OK here would
// mean the generated client is broken.
TEST(TypedApi, UnaryRpcAgainstADeadEndpointFailsCleanly) {
  std::unique_ptr<ondewo::t2s::Text2Speech::Stub> text_to_speech =
      ondewo::t2s::Text2Speech::NewStub(DeadChannel());

  grpc::ClientContext client_context;
  client_context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

  ondewo::t2s::T2sPipelineId request;
  request.set_id("pipeline-de-1");
  ondewo::t2s::Text2SpeechConfig response;

  const grpc::Status status =
      text_to_speech->GetT2sPipeline(&client_context, request, &response);

  EXPECT_FALSE(status.ok()) << "an RPC to a dead endpoint reported success";
  EXPECT_TRUE(status.error_code() == grpc::StatusCode::UNAVAILABLE ||
              status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED)
      << "unexpected status " << status.error_code() << ": " << status.error_message();
}

// StreamingSynthesize is bidirectional, so it gets its own generated ClientReaderWriter
// type. Driving one proves that half of the generated service compiled and dispatches too.
TEST(TypedApi, BidiStreamingRpcStubIsUsable) {
  std::unique_ptr<ondewo::t2s::Text2Speech::Stub> text_to_speech =
      ondewo::t2s::Text2Speech::NewStub(DeadChannel());

  grpc::ClientContext client_context;
  client_context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

  std::unique_ptr<grpc::ClientReaderWriterInterface<ondewo::t2s::StreamingSynthesizeRequest,
                                                    ondewo::t2s::StreamingSynthesizeResponse>>
      stream(text_to_speech->StreamingSynthesize(&client_context));
  ASSERT_NE(stream, nullptr);

  ondewo::t2s::StreamingSynthesizeRequest request;
  request.set_text("Guten Tag");
  request.mutable_config()->set_t2s_pipeline_id("pipeline-de-1");
  stream->Write(request);
  stream->WritesDone();

  ondewo::t2s::StreamingSynthesizeResponse response;
  EXPECT_FALSE(stream->Read(&response)) << "a dead endpoint returned a streamed response";

  const grpc::Status status = stream->Finish();
  EXPECT_FALSE(status.ok()) << "a stream to a dead endpoint reported success";
}

}  // namespace
}  // namespace ondewo_client_test
