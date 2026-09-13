# Test suite

GoogleTest/CTest suite over the **committed** stubs in `api/`. It needs no Docker, no proto
compiler and neither submodule — it builds and exercises the code that is in this repository.

```bash
make build_library   # compile + install the client package into the repo root
make unit_test       # build and run the suite
make coverage        # same, under gcov, failing below COVERAGE_MIN % line coverage
make test            # check_stubs -> check_build -> unit_test -> smoke_test
```

## Layout

| File | Product-specific? | What it is |
| --- | --- | --- |
| `product_config.{h,cc}` | **yes** | The expectation tables: proto files, services, a slice of the RPC surface, the sweep floors |
| `descriptor_probe.{h,cc}` | no | Reflection helpers over the generated descriptor pool |
| `test_generated_stubs.cc` | no | Pool-wide assertions driven entirely by `product_config.cc` |
| `test_typed_api.cc` | **yes** | Assertions against concrete `ondewo::t2s` C++ types |
| `CMakeLists.txt` | no | Standalone project consuming the installed CMake package |

Replicating the suite to another ONDEWO C++ client means rewriting the two product-specific
files; the rest is copied verbatim.

## What it actually checks

- Every `.proto` listed in `product_config.cc` is registered in the descriptor pool, every
  service exists and declares RPCs, and a representative slice of method names is present.
- **Every** generated message is instantiated, has each of its singular scalar fields set to a
  non-default value, and is pushed through `SerializeToString` → `ParseFromString` → compare.
  A writer that drops a field, a reader that ignores one, or a field-number mismatch between
  the two fails here.
- Every generated enum declares `0` as its first value, as proto3 requires.
- `FillScalarFields` handles every protobuf scalar type. No single product uses all of them —
  the sip protos declare no `float` and no `uint64` — so the branches are pinned against
  `google.protobuf`'s wrapper types, which libprotobuf registers into the same pool. That is
  what lets `descriptor_probe.cc` be copied between products untouched.
- `MessagesInFile` / `EnumsInFile` handle every *shape* a `.proto` can have. No single product
  has all three — t2s declares no `map<>`, s2t no nested enum, sip no file-scope enum — so the
  branches are pinned against `google/protobuf/struct.proto` (a map entry plus a file-scope enum)
  and `google/protobuf/descriptor.proto` (enums nested inside messages), both of which libprotobuf
  registers into the same pool.
- `optional string instruction` — a proto3 *explicit presence* field — still reports
  `has_...()` after an empty-string round trip, while the plain `string t2s_pipeline_id` correctly
  keeps its zero value off the wire. This is the bug class that broke the Angular client.
- Service stubs are constructed against a channel, and a unary and a bidirectional-streaming
  RPC are actually issued against a dead endpoint: the call must come back as `UNAVAILABLE` /
  `DEADLINE_EXCEEDED`, which proves the stub, the request/response types and the generated
  method descriptors all link and dispatch.

## Notes on the build

- The suite is a **standalone** CMake project and is not `add_subdirectory()`-ed from the root
  `CMakeLists.txt`: that file is shipped verbatim by the compiler image and is overwritten on
  every regeneration. Consuming the installed package instead is also the stronger test.
- It links the client archive with `--whole-archive` (`-force_load` on macOS). A static archive
  otherwise contributes only the objects needed to resolve a referenced symbol, and the
  descriptor registration of a generated `*.pb.o` is a static initialiser nothing references —
  without it the pool-wide sweeps would silently check only the handful of files
  `test_typed_api.cc` names.
- It compiles with `-fno-exceptions`. Nothing here throws, and the compiler-generated unwind
  blocks otherwise land on every closing brace as lines no test can reach.

## Coverage

`make coverage` measures **hand-written** code only: the gcovr filter is `tests/`, and the
client archive is compiled without `--coverage`, so no generated `*.pb.cc` can contribute a
line either way. The floor is 100 % lines (`COVERAGE_MIN`) and CI enforces it. The generated
stubs are excluded from the *metric* but are the *subject* of every test above.
