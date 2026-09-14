// The downstream application's point of view on the release archive.
//
// Nothing here names a generated type - what the stubs contain is what tests/ asserts. The
// point of this program is that it was compiled and linked out of the EXTRACTED release
// tarball: it includes the umbrella header the archive installs and links the exported
// ondewo:: target, and its CMakeLists.txt refuses to resolve the package anywhere else.

#include <cstdio>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/stubs/common.h>

#include "public-api.h"

int main() {
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  // Every generated *.pb.cc registers its descriptors into the generated pool from a static
  // initialiser, and the CMakeLists.txt beside this file links the WHOLE archive, so this
  // lookup fails if - and only if - the archive shipped no compiled stubs. It is the one
  // assertion that keeps the check from passing against an empty lib<name>.a.
  const google::protobuf::FileDescriptor* file =
      google::protobuf::DescriptorPool::generated_pool()->FindFileByName(ONDEWO_PROBE_PROTO_FILE);
  if (file == nullptr) {
    std::fprintf(stderr, "%s is not in the descriptor pool - the release archive ships no stubs\n",
                 ONDEWO_PROBE_PROTO_FILE);
    return 1;
  }

  std::printf("consumed %s from the extracted release archive (%d message types, %d services)\n",
              ONDEWO_PROBE_PROTO_FILE, file->message_type_count(), file->service_count());

  google::protobuf::ShutdownProtobufLibrary();
  return 0;
}
