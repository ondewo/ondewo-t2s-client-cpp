export

# =====================================================================================
# ondewo-t2s-client-cpp - Makefile
#
# The ONDEWO T2S (Text-to-Speech) gRPC client library for C++.
#
# There is no hand-written client code in this repository: everything under api/ is
# generated from the .proto files of the ondewo-t2s-api submodule by the
# ondewo-cpp-proto-compiler docker image (the ondewo-proto-compiler submodule), which
# also packages the result as a consumable CMake package.
#
# Quick start:
#   make help                    # list every documented target
#   make makefile_chapters       # list the section headers below
#   make build                   # submodules -> compiler image -> stubs -> library
#   make test                    # verify the generated code and consume the CMake package
#   make publish_dry_run         # build the release archive and consume it - no credentials
#
# Versioning: ONDEWO_T2S_VERSION (below) is the single source of truth and MUST
# match the ONDEWO T2S API in major and minor version.
#
# Overriding variables: pass on the command line, e.g. `make build LIBRARY_NAME=my_client`,
# or export in the environment. Credentials (GITHUB_GH_TOKEN) are only ever read at runtime
# and must never be committed.
# =====================================================================================

# ---------------- BEFORE RELEASE ----------------
# 1 - Update Version Number
# 2 - Update RELEASE.md
# 3 - make build
# -------------- Release Process Steps --------------
# 1 - Get Credentials from devops-accounts repo
# 2 - Create Release Branch and push
# 3 - Create Release Tag and push
# 4 - GitHub Release
# 5 - Attach the release archive to the GitHub Release (there is no C++ package registry)

########################################################
# 		Variables
########################################################

# MUST BE THE SAME AS THE API IN MAJOR AND MINOR VERSION NUMBER
# example: API 2.9.0 --> Client 2.9.X
# CMake's project(VERSION ...) and write_basic_package_version_file() only accept a dotted
# numeric MAJOR.MINOR.PATCH here - a pre-release suffix such as 1.2.0-rc1 aborts the configure
# step, so the compiler image rejects it up front.
ONDEWO_T2S_VERSION=6.6.0

# Submodule pins. Both are checked out by `make checkout_defined_submodule_versions`, which is
# part of `make build`, so a build is always reproducible from these two lines alone.
ONDEWO_T2S_API_GIT_BRANCH=tags/6.6.0
ONDEWO_PROTO_COMPILER_GIT_BRANCH=tags/5.15.0

# You need to setup an access token at https://github.com/settings/tokens - permissions are important
GITHUB_GH_TOKEN?=ENTER_YOUR_TOKEN_HERE

ONDEWO_API_DIR=ondewo-t2s-api
ONDEWO_PROTO_COMPILER_DIR=ondewo-proto-compiler

# The image tag is the ONLY contract with the proto compiler - `make build_compiler` rebuilds it
# from the submodule, but any locally built ondewo-cpp-proto-compiler image is used as-is.
PROTO_COMPILER_IMAGE=ondewo-cpp-proto-compiler

# Entrypoint argument 1: the proto root, relative to the mounted /input-volume (the repo root).
# Entrypoint argument 2: the sub-directory of that root whose protos are the compilation entry
# points; their transitive imports (google/**, minus the well-known types already inside
# libprotobuf) are pulled in automatically, so google/ must NOT be listed here.
# Entrypoint argument 3: the CMake target / package / archive name. C++ has no package manifest
# the image could read a name from, so it arrives positionally. Letters, digits, '_' and '-' only.
ONDEWO_PROTOS_SUBDIR=ondewo
LIBRARY_NAME=ondewo_t2s_client

# Host-side CMake build of the generated stubs. BUILD_DIR is throwaway; the install tree lands at
# the repo root so include/, lib/lib$(LIBRARY_NAME).a and lib/cmake/$(LIBRARY_NAME)/ sit exactly
# where the compiler image writes them (all three are gitignored build output).
BUILD_DIR=build
INSTALL_PREFIX=$(CURDIR)
# getconf, not nproc: nproc is a GNU coreutils extension and does not exist on macOS. Lower this
# on a small machine - each cc1plus on a large generated .pb.cc needs a few hundred MB of RAM.
BUILD_JOBS?=$(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
SMOKE_TEST_DIR=.smoke-test

# The release archive - this client's ONLY distribution channel for a prebuilt consumer. C++ has
# no package registry, so `make publish` stages the CMake install tree into a versioned directory,
# tars it and attaches it to the GitHub release; `find_package(<library> CONFIG REQUIRED)` against
# the extracted archive is what the generated *-config.cmake files exist for.
# The archive is a BINARY artifact (static archive + headers + CMake package files), so the name
# carries the platform it was built on and PACKAGE-INFO.txt inside records the exact protobuf/gRPC
# it was compiled against - C++ gencode and runtime have no cross-version guarantee.
PACKAGE_DIR=dist
PACKAGE_PLATFORM?=$(shell uname -s | tr '[:upper:]' '[:lower:]')-$(shell uname -m)
PACKAGE_NAME=$(LIBRARY_NAME)-$(ONDEWO_T2S_VERSION)-$(PACKAGE_PLATFORM)
PACKAGE_STAGE_DIR=$(PACKAGE_DIR)/$(PACKAGE_NAME)
PACKAGE_ARCHIVE=$(PACKAGE_DIR)/$(PACKAGE_NAME).tar.gz
# Throwaway tree for verify_package: the archive is extracted here and consumed from
# tests/package-consume, exactly the way a downstream project consumes it.
PACKAGE_TEST_DIR=.package-test
PACKAGE_CONSUME_DIR=tests/package-consume

# GoogleTest/CTest suite over the committed stubs. tests/ is a STANDALONE CMake project that
# consumes the installed package (see tests/CMakeLists.txt for why it is not a subdirectory of
# the generated root CMakeLists.txt), so it needs its own build tree.
TEST_DIR=tests
TEST_BUILD_DIR=build-tests
# Line-coverage floor for the HAND-WRITTEN sources under tests/. The generated stubs are
# excluded by construction: the client archive is compiled without --coverage, so no .gcno for
# a *.pb.cc exists at all, and the gcovr filter below keeps the metric inside tests/.
COVERAGE_MIN?=100

# Terminate on the ***** separator that delimits release entries, NOT on /\*\*/ - that matches the
# first markdown **bold** span inside the entry and silently truncates the notes there, with no
# error from `gh release create`. The '+' of "C++" is escaped because an unescaped one is a perl
# regex quantifier ("Nested quantifiers in regex") - keep the escaping if you rename the heading.
CURRENT_RELEASE_NOTES=`cat RELEASE.md \
	| perl -ne 'print if /Release ONDEWO T2S C\+\+ Client ${ONDEWO_T2S_VERSION}/../^\*{5}/'`

GH_REPO="https://github.com/ondewo/ondewo-t2s-client-cpp"
DEVOPS_ACCOUNT_GIT="ondewo-devops-accounts"
DEVOPS_ACCOUNT_DIR="./${DEVOPS_ACCOUNT_GIT}"

# `make` with no target prints the help listing.
.DEFAULT_GOAL := help

# Define colors globally (reused for [INFO]/[SUCCESS]/[WARN]/[ERROR] log lines in recipes)
BLUE   := \033[1;34m
GREEN  := \033[0;32m
YELLOW := \033[1;33m
RED    := \033[0;31m
NC     := \033[0m

########################################################
#       ONDEWO Standard Make Targets
########################################################

setup_developer_environment_locally: update_submodules install_precommit_hooks ## Ready a fresh checkout: initialize the submodules and install the pre-commit hooks

install_precommit_hooks: ## Installs pre-commit hooks and sets them up for the ondewo-t2s-client-cpp repo
	pip install pre-commit
	pre-commit install
	pre-commit install --hook-type commit-msg

precommit_hooks_run_all_files: ## Runs all pre-commit hooks on all files and not just the changed ones
	pre-commit run --all-files

help: ## Print usage info about help targets
	# (first comment after target starting with double hashes ##)
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' Makefile | sort | awk 'BEGIN {FS = ":.*?## "}; {printf "\033[36m%-40s\033[0m %s\n", $$1, $$2}'

makefile_chapters: ## Shows all sections of Makefile
	@echo `cat Makefile| grep "########################################################" -A 1 | grep -v "########################################################"`

TEST: ## Diagnostics - report whether release credentials are set and print the current release notes
# The placeholder counts as unset - `$(if $(GITHUB_GH_TOKEN),...)` alone reports "yes" for the
# default ENTER_YOUR_TOKEN_HERE and hides the one thing this line exists to tell you.
	@if [ -z "${GITHUB_GH_TOKEN}" ] || [ "${GITHUB_GH_TOKEN}" = "ENTER_YOUR_TOKEN_HERE" ]; then \
		echo "GITHUB_GH_TOKEN is set: no"; \
	else \
		echo "GITHUB_GH_TOKEN is set: yes"; \
	fi
	@echo "Library name:           $(LIBRARY_NAME)"
	@echo "Client version:         $(ONDEWO_T2S_VERSION)"
	@echo "Release archive:        $(PACKAGE_ARCHIVE)"
	@printf '\n%s\n' "${CURRENT_RELEASE_NOTES}"

########################################################
#       Repo Specific Make Targets
########################################################
#		Build

build: checkout_defined_submodule_versions build_compiler generate_ondewo_protos build_library ## Build source code: submodules -> compiler image -> generated stubs -> CMake library

build_compiler: ## Build the ondewo-cpp-proto-compiler docker image from the submodule
	@echo "$(BLUE)[INFO]$(NC) Building $(PROTO_COMPILER_IMAGE) from $(ONDEWO_PROTO_COMPILER_DIR)/cpp ..."
	cd $(ONDEWO_PROTO_COMPILER_DIR)/cpp && sh build.sh
	@echo "$(GREEN)[SUCCESS]$(NC) $(PROTO_COMPILER_IMAGE) built"

# Derived from ondewo-proto-compiler/cpp/example/run-compile.sh: same image tag, same three
# positional arguments. There is deliberately NO `-it` - it breaks every non-interactive caller
# with "cannot attach stdin to a TTY-enabled container because stdin is not a terminal" - and no
# `--user` either, because the image's scripts write into the root-owned /image-data tree
# (fix_file_ownership below hands the results back afterwards).
#
# Both volumes are the repo root: the input volume must contain the ondewo-t2s-api proto tree, and the
# output volume is where the generated library belongs. The image only ever deletes what it owns
# there (api/, include/$(LIBRARY_NAME)/, lib/lib$(LIBRARY_NAME).a, lib/cmake/$(LIBRARY_NAME)/) and
# overwrites the wholly generated public-api.h - never the whole directory - and it compiles inside
# an internal copy, so the mounted .proto sources are never mutated.
generate_ondewo_protos: ## Generate the C++ gRPC client stubs and the CMake library from the .proto files
	@echo "$(BLUE)[INFO]$(NC) Generating C++ stubs from $(ONDEWO_API_DIR)/$(ONDEWO_PROTOS_SUBDIR) into api/ ..."
	docker run \
		-v ${shell pwd}:/input-volume \
		-v ${shell pwd}:/output-volume \
		$(PROTO_COMPILER_IMAGE) $(ONDEWO_API_DIR) $(ONDEWO_PROTOS_SUBDIR) $(LIBRARY_NAME)
	-make fix_file_ownership
	@echo "$(GREEN)[SUCCESS]$(NC) Stubs generated"

# `-` prefixed wherever it is called: a machine without sudo, or a Docker setup that already maps
# ownership to the calling user (Docker Desktop on macOS), must not fail the build here.
fix_file_ownership: ## Take back ownership of the files the (root) compiler container wrote into the repo
	@if [ "`id -u`" = "0" ]; then \
		echo "$(YELLOW)[NOOP]$(NC) running as root - nothing to restore"; \
	elif command -v sudo >/dev/null 2>&1; then \
		echo "$(BLUE)[INFO]$(NC) Restoring ownership of root-owned files (sudo may prompt) ..."; \
		find . -user 0 2>/dev/null | while IFS= read -r f; do \
			sudo chown -R "`id -u`:`id -g`" "$$f" && echo "  $$f"; \
		done; \
	else \
		echo "$(YELLOW)[NOOP]$(NC) sudo not available - skipping ownership restore"; \
	fi

# The host-native rebuild of the very same sources the image compiled. It is not redundant: the
# archive the image produces is linked against Debian's glibc/libstdc++ and will not necessarily
# link on this host, and this is the build a consumer of the repository actually performs.
build_library: ## Configure, compile and install the CMake library from the generated stubs
	@test -f CMakeLists.txt || { \
		echo "$(RED)[ERROR]$(NC) no CMakeLists.txt in the repository root - run 'make generate_ondewo_protos' first"; \
		exit 1; \
	}
	@echo "$(BLUE)[INFO]$(NC) Building $(LIBRARY_NAME) $(ONDEWO_T2S_VERSION) with $(BUILD_JOBS) parallel jobs ..."
	cmake -S . -B $(BUILD_DIR) \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$(INSTALL_PREFIX)" \
		-DONDEWO_LIBRARY_NAME=$(LIBRARY_NAME) \
		-DONDEWO_LIBRARY_VERSION=$(ONDEWO_T2S_VERSION)
	cmake --build $(BUILD_DIR) --parallel $(BUILD_JOBS)
	cmake --install $(BUILD_DIR)
	@echo "$(GREEN)[SUCCESS]$(NC) lib/lib$(LIBRARY_NAME).a + lib/cmake/$(LIBRARY_NAME)/ installed into $(INSTALL_PREFIX)"

clean: ## Remove the CMake build trees, the installed artifacts, the release archive and the scratch directories
	rm -rf $(BUILD_DIR) $(TEST_BUILD_DIR) $(SMOKE_TEST_DIR) $(PACKAGE_DIR) $(PACKAGE_TEST_DIR) include lib
	rm -f build_check.txt build_check_tmp.txt

clean_generated_stubs: ## Remove the generated stub sources (api/ and public-api.h) - they are regenerated by `make build`
	rm -rf api
	rm -f public-api.h

########################################################
#		Test

test: check_stubs check_build unit_test smoke_test publish_dry_run ## Full local gate: stubs present -> stubs complete -> unit tests -> package consumable -> release archive consumable

# The gate that must NEVER be satisfiable by an empty repository. It reads only committed
# files, so it needs neither Docker nor the submodules and cannot degrade into a skip: if the
# stubs are missing or the umbrella header points at a file that is not there, this fails.
check_stubs: ## Checks that the committed stubs are present and that public-api.h resolves against them
	@test -f public-api.h || { \
		echo "$(RED)[ERROR]$(NC) public-api.h is missing - the generated stubs are not committed"; \
		exit 1; \
	}
	@test -d api || { \
		echo "$(RED)[ERROR]$(NC) api/ is missing - the generated stubs are not committed"; \
		exit 1; \
	}
	@sources=`find api -type f -name "*.pb.cc" | grep -c . || true`; \
	headers=`find api -type f -name "*.pb.h" | grep -c . || true`; \
	includes=`sed -n 's|^#include "\(.*\.pb\.h\)"$$|\1|p' public-api.h | grep -c . || true`; \
	if [ "$$sources" -lt 1 ] || [ "$$headers" -lt 1 ]; then \
		echo "$(RED)[ERROR]$(NC) api/ holds no generated sources ($$sources .pb.cc / $$headers .pb.h)"; \
		exit 1; \
	fi; \
	if [ "$$includes" -lt 1 ]; then \
		echo "$(RED)[ERROR]$(NC) public-api.h includes no generated header"; \
		exit 1; \
	fi; \
	missing=0; \
	for header in `sed -n 's|^#include "\(.*\.pb\.h\)"$$|\1|p' public-api.h`; do \
		test -f "api/$$header" || { \
			echo "$(RED)[ERROR]$(NC) public-api.h includes $$header, but api/$$header does not exist"; \
			missing=1; \
		}; \
	done; \
	if [ "$$missing" != "0" ]; then exit 1; fi; \
	echo "$(GREEN)[SUCCESS]$(NC) $$sources generated sources, $$headers headers, all $$includes public-api.h includes resolve"

# Every .proto under ondewo-t2s-api/ondewo must have produced a <name>.pb.h. Degrades to a labelled
# skip when the API submodule is not checked out, so a `make test` on a bare checkout (CI without
# submodule access) still runs the smoke test instead of dying on a missing directory.
check_build: ## Checks that every ONDEWO .proto produced generated C++ code
	@if [ ! -d "$(ONDEWO_API_DIR)/$(ONDEWO_PROTOS_SUBDIR)" ]; then \
		echo "$(YELLOW)[NOOP]$(NC) $(ONDEWO_API_DIR)/$(ONDEWO_PROTOS_SUBDIR) is not checked out - run 'make update_submodules'; skipping check_build"; \
		exit 0; \
	fi; \
	if [ ! -d api ]; then \
		echo "$(RED)[ERROR]$(NC) no api/ directory - run 'make generate_ondewo_protos' first"; \
		exit 1; \
	fi; \
	missing=0; \
	for proto in `find $(ONDEWO_API_DIR)/$(ONDEWO_PROTOS_SUBDIR) -type f -name "*.proto"`; do \
		header="`basename "$$proto" .proto`.pb.h"; \
		find api -type f -name "$$header" | grep -q . || { \
			echo "$(RED)[ERROR]$(NC) No C++ code generated for $$proto (expected api/**/$$header)"; \
			missing=1; \
		}; \
	done; \
	if [ "$$missing" != "0" ]; then exit 1; fi; \
	echo "$(GREEN)[SUCCESS]$(NC) Generated C++ code found for every .proto"

# Consumes the installed CMake package exactly the way a downstream application does - the one
# check that proves find_package(), the exported target, the umbrella header and the link line all
# work together. Everything is written into a throwaway directory; nothing is added to the repo.
smoke_test: ## Compile and run a tiny program against the installed CMake package
	@test -d "lib/cmake/$(LIBRARY_NAME)" || { \
		echo "$(RED)[ERROR]$(NC) lib/cmake/$(LIBRARY_NAME)/ is missing - run 'make build_library' first"; \
		exit 1; \
	}
	@rm -rf $(SMOKE_TEST_DIR)
	@mkdir -p $(SMOKE_TEST_DIR)
	@printf '%s\n' \
		'#include "public-api.h"' \
		'#include <google/protobuf/stubs/common.h>' \
		'#include <cstdio>' \
		'int main() {' \
		'  GOOGLE_PROTOBUF_VERIFY_VERSION;' \
		'  std::puts("ondewo-t2s-client-cpp smoke test OK");' \
		'  google::protobuf::ShutdownProtobufLibrary();' \
		'  return 0;' \
		'}' > $(SMOKE_TEST_DIR)/main.cpp
	@printf '%s\n' \
		'cmake_minimum_required(VERSION 3.22)' \
		'project(ondewo_client_smoke_test LANGUAGES CXX)' \
		'set(CMAKE_CXX_STANDARD 17)' \
		'set(CMAKE_CXX_STANDARD_REQUIRED ON)' \
		'find_package($(LIBRARY_NAME) CONFIG REQUIRED)' \
		'add_executable(smoke_test main.cpp)' \
		'target_link_libraries(smoke_test PRIVATE ondewo::$(LIBRARY_NAME))' \
		> $(SMOKE_TEST_DIR)/CMakeLists.txt
	cmake -S $(SMOKE_TEST_DIR) -B $(SMOKE_TEST_DIR)/build -DCMAKE_PREFIX_PATH="$(INSTALL_PREFIX)"
	cmake --build $(SMOKE_TEST_DIR)/build --parallel $(BUILD_JOBS)
	./$(SMOKE_TEST_DIR)/build/smoke_test
	@rm -rf $(SMOKE_TEST_DIR)
	@echo "$(GREEN)[SUCCESS]$(NC) The installed CMake package is consumable"

# The GoogleTest suite. It links against the INSTALLED package, so `make build_library` (or a
# `make build`) has to have run first - that is also what makes it a consumer-level test.
unit_test: ## Build and run the GoogleTest/CTest suite against the installed package
	@test -d "lib/cmake/$(LIBRARY_NAME)" || { \
		echo "$(RED)[ERROR]$(NC) lib/cmake/$(LIBRARY_NAME)/ is missing - run 'make build_library' first"; \
		exit 1; \
	}
	@echo "$(BLUE)[INFO]$(NC) Building the test suite ..."
	cmake -S $(TEST_DIR) -B $(TEST_BUILD_DIR) \
		-DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_PREFIX_PATH="$(INSTALL_PREFIX)" \
		-DONDEWO_LIBRARY_NAME=$(LIBRARY_NAME) \
		-DONDEWO_ENABLE_COVERAGE=$(ONDEWO_ENABLE_COVERAGE)
	cmake --build $(TEST_BUILD_DIR) --parallel $(BUILD_JOBS)
	ctest --test-dir $(TEST_BUILD_DIR) --output-on-failure
	@echo "$(GREEN)[SUCCESS]$(NC) The test suite passed"

# Default OFF so a plain `make unit_test` builds optimised, uninstrumented binaries; the
# coverage target below turns it on.
ONDEWO_ENABLE_COVERAGE?=OFF

# Measures HAND-WRITTEN code only. The gcovr filter is tests/ and nothing else, and the client
# archive was compiled without --coverage, so no generated *.pb.cc can contribute a line either
# way. Anything below $(COVERAGE_MIN) % lines fails the build.
coverage: ## Run the test suite under gcov and fail if hand-written line coverage drops below COVERAGE_MIN
	@command -v gcovr >/dev/null 2>&1 || { \
		echo "$(RED)[ERROR]$(NC) gcovr is not installed - 'pip install gcovr' or 'apt-get install gcovr'"; \
		exit 1; \
	}
	@rm -rf $(TEST_BUILD_DIR)
	@$(MAKE) unit_test ONDEWO_ENABLE_COVERAGE=ON
	@echo "$(BLUE)[INFO]$(NC) Measuring coverage of the hand-written sources under $(TEST_DIR)/ ..."
# The report is printed even when the threshold is not met - the table naming the uncovered
# lines is exactly what is needed then, so the exit code is captured and re-raised afterwards.
	@gcovr --root . --filter '$(TEST_DIR)/' --print-summary --txt coverage.txt \
		--fail-under-line $(COVERAGE_MIN) $(TEST_BUILD_DIR); \
	status=$$?; cat coverage.txt; exit $$status
	@echo "$(GREEN)[SUCCESS]$(NC) Hand-written line coverage is at least $(COVERAGE_MIN) %"

########################################################
#		Submodules

update_submodules: ## Initialize and update all submodules
	@echo "$(BLUE)[INFO]$(NC) START initializing submodules ..."
	git submodule update --init --recursive
	@echo "$(GREEN)[SUCCESS]$(NC) DONE initializing submodules"

checkout_defined_submodule_versions: update_submodules ## Check out the submodule versions pinned in the Variables chapter
	@echo "$(BLUE)[INFO]$(NC) START checking out submodules ..."
	git -C $(ONDEWO_API_DIR) fetch --all
	git -C $(ONDEWO_API_DIR) checkout ${ONDEWO_T2S_API_GIT_BRANCH}
	git -C $(ONDEWO_PROTO_COMPILER_DIR) fetch --all
	git -C $(ONDEWO_PROTO_COMPILER_DIR) checkout ${ONDEWO_PROTO_COMPILER_GIT_BRANCH}
	@echo "$(GREEN)[SUCCESS]$(NC) DONE checking out submodules"

########################################################
#		Release

release: ## Automate the entire release process
	@echo "$(BLUE)[INFO]$(NC) Start Release"
	make build
	-make precommit_hooks_run_all_files
	make test
	git status
# api/, public-api.h and the two generated build files are COMMITTED on purpose: C++ has no
# package registry, so the git tag is this client's only distribution channel and a consumer must
# get a buildable CMake project from a plain clone. See the note in .gitignore.
	git add api
	git add public-api.h
	-git add CMakeLists.txt
	-git add ondewo-client-config.cmake.in
	git add Makefile
	git add README.md
	git add RELEASE.md
	git add $(ONDEWO_PROTO_COMPILER_DIR)
	git add $(ONDEWO_API_DIR)
	git status
	-git commit --no-verify -m "Preparing for Release ${ONDEWO_T2S_VERSION}"
	git push
	make create_release_branch
	make create_release_tag
	make push_to_gh
# Strictly after push_to_gh: `gh release upload` attaches assets to a release that must already
# exist. The release workflow does the same upload from the tag push, so whichever gets there
# first wins and the other one clobbers its own identical asset.
	make publish
	@echo "$(GREEN)[SUCCESS]$(NC) Release Finished"

create_release_branch: ## Create Release Branch and push it to origin
	git checkout -b "release/${ONDEWO_T2S_VERSION}"
	git push -u origin "release/${ONDEWO_T2S_VERSION}"

create_release_tag: ## Create Release Tag and push it to origin
	git tag -a ${ONDEWO_T2S_VERSION} -m "release/${ONDEWO_T2S_VERSION}"
	git push origin ${ONDEWO_T2S_VERSION}

login_to_gh: ## Login to Github CLI with Access Token
	@if [ -z "${GITHUB_GH_TOKEN}" ] || [ "${GITHUB_GH_TOKEN}" = "ENTER_YOUR_TOKEN_HERE" ]; then \
		echo "$(RED)[ERROR]$(NC) GITHUB_GH_TOKEN is not set - create one at https://github.com/settings/tokens"; \
		exit 1; \
	fi
	@echo "${GITHUB_GH_TOKEN}" | gh auth login -p ssh --with-token

build_gh_release: ## Generate Github Release with CLI
	gh release create --repo $(GH_REPO) "$(ONDEWO_T2S_VERSION)" -n "$(CURRENT_RELEASE_NOTES)" -t "Release ${ONDEWO_T2S_VERSION}"

########################################################
#		PACKAGE

# C++ has no registry to push to, so "publishing" is: stage -> archive -> verify -> attach to the
# GitHub release. The GitHub release asset IS the package; the sibling clients' PyPI/npm chapters
# sit in exactly this slot of their Makefiles.
#
#   make publish_dry_run   everything except the upload - no credentials, runs in CI on every push
#   make publish           the same, then attaches the archive to the GitHub release
#
# The only credential involved is GITHUB_GH_TOKEN, which the release flow already reads from the
# devops-accounts repo (see the DEVOPS-ACCOUNTS chapter) - there is no registry account to add.

publish: build_package verify_package login_to_gh upload_package ## Build, verify and attach the release archive to the GitHub release
	@echo "$(GREEN)[SUCCESS]$(NC) $(PACKAGE_NAME).tar.gz published as a GitHub release asset"

# The credential-free half of `publish`, and the packaging gate CI runs on every push: if the
# archive cannot be built, or cannot be consumed once extracted, that fails here rather than
# during a release.
publish_dry_run: build_package verify_package ## Everything `make publish` does except the upload - no credentials needed
	@echo "$(BLUE)[INFO]$(NC) would upload: gh release upload --repo $(GH_REPO) --clobber $(ONDEWO_T2S_VERSION) $(PACKAGE_ARCHIVE) $(PACKAGE_ARCHIVE).sha256"
	@echo "$(GREEN)[SUCCESS]$(NC) Packaging dry-run complete - nothing was uploaded"

build_package: ## Stage the install tree into dist/<library>-<version>-<platform>.tar.gz and checksum it
	@test -d $(BUILD_DIR) || { \
		echo "$(RED)[ERROR]$(NC) no $(BUILD_DIR)/ build tree - run 'make build_library' first"; \
		exit 1; \
	}
	@rm -rf $(PACKAGE_STAGE_DIR) $(PACKAGE_ARCHIVE) $(PACKAGE_ARCHIVE).sha256
	@mkdir -p $(PACKAGE_STAGE_DIR)
	@echo "$(BLUE)[INFO]$(NC) Staging $(PACKAGE_NAME) ..."
# --prefix overrides CMAKE_INSTALL_PREFIX at install time (CMake >= 3.15), so the staged tree is
# self-contained: configure_package_config_file() and install(EXPORT) only ever emit paths relative
# to the installed config file, which is what lets a consumer extract the archive anywhere.
	cmake --install $(BUILD_DIR) --prefix $(PACKAGE_STAGE_DIR)
	cp LICENSE README.md RELEASE.md $(PACKAGE_STAGE_DIR)/
# Provenance for a BINARY artifact: protobuf gives no cross-version guarantee between generated
# code and runtime, so a consumer has to be able to tell what this archive was compiled against.
	@{ \
		echo "name:        $(LIBRARY_NAME)"; \
		echo "version:     $(ONDEWO_T2S_VERSION)"; \
		echo "platform:    $(PACKAGE_PLATFORM)"; \
		echo "protoc:      `protoc --version 2>/dev/null || echo unknown`"; \
		echo "libprotobuf: `pkg-config --modversion protobuf 2>/dev/null || echo unknown`"; \
		echo "libgrpc++:   `pkg-config --modversion grpc++ 2>/dev/null || echo unknown`"; \
		echo "compiler:    `$(CXX) --version 2>/dev/null | head -1 || echo unknown`"; \
	} > $(PACKAGE_STAGE_DIR)/PACKAGE-INFO.txt
	tar -czf $(PACKAGE_ARCHIVE) -C $(PACKAGE_DIR) $(PACKAGE_NAME)
# sha256sum is GNU coreutils; macOS ships shasum instead. Both print "<hash>  <file>", so the
# checksum file is verified the same way on either side: `cd $(PACKAGE_DIR) && sha256sum -c ...`.
	@cd $(PACKAGE_DIR) && if command -v sha256sum >/dev/null 2>&1; then \
		sha256sum $(PACKAGE_NAME).tar.gz > $(PACKAGE_NAME).tar.gz.sha256; \
	else \
		shasum -a 256 $(PACKAGE_NAME).tar.gz > $(PACKAGE_NAME).tar.gz.sha256; \
	fi
# The staged directory is removed again: the tarball is the artifact, and leaving an unpacked copy
# of it beside the one verify_package extracts would only invite consuming the wrong tree.
	@rm -rf $(PACKAGE_STAGE_DIR)
	@echo "$(GREEN)[SUCCESS]$(NC) `du -h $(PACKAGE_ARCHIVE) | awk '{print $$1}'` $(PACKAGE_ARCHIVE)"
	@cat $(PACKAGE_ARCHIVE).sha256

# THE test that makes the release archive worth publishing. It extracts the tarball into a
# throwaway tree and builds tests/package-consume against it - an unrelated CMake project that
# find_package()es the extracted archive, includes the umbrella header and links the exported
# ondewo:: target. Needs no credentials and uploads nothing, so CI runs it on every push.
verify_package: ## Extract the release archive and consume it with find_package() from an unrelated CMake project
	@test -f $(PACKAGE_ARCHIVE) || { \
		echo "$(RED)[ERROR]$(NC) $(PACKAGE_ARCHIVE) does not exist - run 'make build_package' first"; \
		exit 1; \
	}
	@rm -rf $(PACKAGE_TEST_DIR)
	@mkdir -p $(PACKAGE_TEST_DIR)
	@cd $(PACKAGE_DIR) && if command -v sha256sum >/dev/null 2>&1; then \
		sha256sum -c $(PACKAGE_NAME).tar.gz.sha256; \
	else \
		shasum -a 256 -c $(PACKAGE_NAME).tar.gz.sha256; \
	fi
	tar -xzf $(PACKAGE_ARCHIVE) -C $(PACKAGE_TEST_DIR)
# Every file a consumer is promised. Without this the consume build below could still succeed
# against a half-empty archive that happens to carry enough to link.
	@for f in PACKAGE-INFO.txt LICENSE \
			include/$(LIBRARY_NAME)/public-api.h \
			lib/lib$(LIBRARY_NAME).a \
			lib/cmake/$(LIBRARY_NAME)/$(LIBRARY_NAME)-config.cmake \
			lib/cmake/$(LIBRARY_NAME)/$(LIBRARY_NAME)-config-version.cmake \
			lib/cmake/$(LIBRARY_NAME)/$(LIBRARY_NAME)-targets.cmake; do \
		test -f "$(PACKAGE_TEST_DIR)/$(PACKAGE_NAME)/$$f" || { \
			echo "$(RED)[ERROR]$(NC) the release archive is missing $$f"; \
			exit 1; \
		}; \
	done
# The per-configuration half of the exported target set. Its name carries the build type
# ($(LIBRARY_NAME)-targets-release.cmake), so it is matched rather than named: without it
# find_package() still succeeds but the imported target has no archive to point at.
	@configs=`find $(PACKAGE_TEST_DIR)/$(PACKAGE_NAME)/lib/cmake/$(LIBRARY_NAME) -type f -name "$(LIBRARY_NAME)-targets-*.cmake" | grep -c . || true`; \
	if [ "$$configs" -lt 1 ]; then \
		echo "$(RED)[ERROR]$(NC) the release archive ships no $(LIBRARY_NAME)-targets-<config>.cmake"; \
		exit 1; \
	fi
	@packaged=`find $(PACKAGE_TEST_DIR)/$(PACKAGE_NAME)/include -type f -name "*.pb.h" | grep -c . || true`; \
	built=`find api -type f -name "*.pb.h" | grep -c . || true`; \
	if [ "$$packaged" != "$$built" ]; then \
		echo "$(RED)[ERROR]$(NC) the release archive ships $$packaged headers but api/ holds $$built"; \
		exit 1; \
	fi; \
	echo "$(BLUE)[INFO]$(NC) archive holds $$packaged generated headers, all package files present"
# CMAKE_PREFIX_PATH is the extracted archive and nothing else; ONDEWO_PACKAGE_PREFIX makes the
# consume project assert that find_package() really resolved there, so a stale system-wide or
# repository-root install tree cannot stand in for the archive under test.
	cmake -S $(PACKAGE_CONSUME_DIR) -B $(PACKAGE_TEST_DIR)/build \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_PREFIX_PATH="$(CURDIR)/$(PACKAGE_TEST_DIR)/$(PACKAGE_NAME)" \
		-DONDEWO_PACKAGE_PREFIX="$(CURDIR)/$(PACKAGE_TEST_DIR)/$(PACKAGE_NAME)" \
		-DONDEWO_LIBRARY_NAME=$(LIBRARY_NAME)
	cmake --build $(PACKAGE_TEST_DIR)/build --parallel $(BUILD_JOBS)
	./$(PACKAGE_TEST_DIR)/build/package_consume
	@rm -rf $(PACKAGE_TEST_DIR)
	@echo "$(GREEN)[SUCCESS]$(NC) $(PACKAGE_NAME).tar.gz is consumable with find_package($(LIBRARY_NAME)) after extraction"

# Attaches the archive to the GitHub release for the current version. `gh` takes its credential
# from the environment (GH_TOKEN, set by the release workflow from a GitHub secret) or from the
# `gh auth login` that login_to_gh performed - no token is ever named on this command line.
# --clobber so a re-run after a fixed build replaces the asset instead of failing.
upload_package: ## Attach the release archive and its checksum to the GitHub release of the current version
	@test -f $(PACKAGE_ARCHIVE) || { \
		echo "$(RED)[ERROR]$(NC) $(PACKAGE_ARCHIVE) does not exist - run 'make build_package' first"; \
		exit 1; \
	}
	gh release upload --repo $(GH_REPO) --clobber "$(ONDEWO_T2S_VERSION)" \
		$(PACKAGE_ARCHIVE) $(PACKAGE_ARCHIVE).sha256

########################################################
#		GITHUB

push_to_gh: login_to_gh build_gh_release ## Logs into GitHub CLI and Releases
	@echo 'Released to Github'

########################################################
#		DEVOPS-ACCOUNTS

ondewo_release: spc clone_devops_accounts run_release_with_devops ## Release with credentials from devops-accounts repo
	@rm -rf ${DEVOPS_ACCOUNT_GIT}

clone_devops_accounts: ## Clones devops-accounts repo
	if [ -d $(DEVOPS_ACCOUNT_GIT) ]; then rm -Rf $(DEVOPS_ACCOUNT_GIT); fi
	git clone git@bitbucket.org:ondewo/${DEVOPS_ACCOUNT_GIT}.git

run_release_with_devops: ## Read credentials from the cloned devops-accounts repo and run the full release
	$(eval info:= $(shell cat ${DEVOPS_ACCOUNT_DIR}/account_github.env | grep GITHUB_GH))
	@make release $(info)

spc: ## Checks if the Release Branch and Tag already exist
	$(eval filtered_branches:= $(shell git branch --all | grep -E "(^|[ /])release/$(subst .,\.,${ONDEWO_T2S_VERSION})$$"))
	$(eval filtered_tags:= $(shell git tag --list | grep -Fx "${ONDEWO_T2S_VERSION}"))
	@if test "$(filtered_branches)" != ""; then echo "-- Test 1: Branch exists!!" && exit 1; else echo "-- Test 1: Branch is fine";fi
	@if test "$(filtered_tags)" != ""; then echo "-- Test 2: Tag exists!!" && exit 1; else echo "-- Test 2: Tag is fine";fi
