# Arches Instrumentation — top-level developer entry point (REQ §14).
#
# Host-native is the default and only build you need day to day: a C++17
# toolchain + CMake + protoc/grpc on the host, no Docker required. Binaries
# land in build-<PRESET>/ (build-host-debug/ by default) via the
# `host-debug` CMake preset — the same preset .claude/validate.sh exercises.
#
# The aarch64 container build in docker/ (Ubuntu 22.04 arm64, JetPack 6
# userland) still exists for producing device-matching release artifacts —
# see `make aarch64-build` and the other `aarch64-*` targets below — but it
# is not part of the everyday workflow and nothing here runs it by default.

PRESET       ?= host-debug
BUILD_DIR    := build-$(PRESET)

DOCKER_DIR   := docker
DOCKER_IMAGE ?= arches/instrumentation-build
DOCKER_TAG   ?= aarch64

.DEFAULT_GOAL := build
.PHONY: help build test check-format format tidy clean \
        aarch64-build aarch64-image aarch64-image-force aarch64-test \
        aarch64-check-format aarch64-format aarch64-tidy \
        aarch64-ccache-stats aarch64-shell aarch64-clean

help:
	@echo 'Arches Instrumentation — make targets (host-native by default, PRESET=$(PRESET)):'
	@echo '  make build         default: host-native build -> $(BUILD_DIR)/'
	@echo '  make test          run the test suite (ctest) natively'
	@echo '  make check-format  clang-format gate natively'
	@echo '  make format        apply .clang-format natively'
	@echo '  make tidy          clang-tidy natively (run-clang-tidy target)'
	@echo '  make clean         remove $(BUILD_DIR)/'
	@echo
	@echo 'Variables: PRESET (default host-debug; see CMakePresets.json for others)'
	@echo
	@echo 'Cross/release build (aarch64, needs Docker) — not part of the default'
	@echo 'workflow, see README "Build":'
	@echo '  make aarch64-build          full aarch64 build -> build-aarch64/'
	@echo '  make aarch64-image          ensure the arm64 image exists'
	@echo '  make aarch64-image-force    rebuild the arm64 image'
	@echo '  make aarch64-test           test suite inside the arm64 container'
	@echo '  make aarch64-check-format   clang-format gate inside the container'
	@echo '  make aarch64-format         apply .clang-format inside the container'
	@echo '  make aarch64-tidy           clang-tidy inside the container'
	@echo '  make aarch64-ccache-stats   compiler-cache statistics'
	@echo '  make aarch64-shell          interactive shell in the arm64 container'
	@echo '  make aarch64-clean          remove build-aarch64/'

build:
	cmake --preset $(PRESET)
	cmake --build --preset $(PRESET) -j"$$(nproc)"

test: build
	ctest --preset $(PRESET) --output-on-failure

check-format:
	cmake --preset $(PRESET)
	cmake --build --preset $(PRESET) --target check-format

format:
	cmake --preset $(PRESET)
	cmake --build --preset $(PRESET) --target format

tidy:
	cmake --preset $(PRESET)
	cmake --build --preset $(PRESET) --target run-clang-tidy

clean:
	rm -rf $(BUILD_DIR)

aarch64-build:
	$(MAKE) -C $(DOCKER_DIR) build IMAGE=$(DOCKER_IMAGE) TAG=$(DOCKER_TAG)

aarch64-image:
	$(MAKE) -C $(DOCKER_DIR) image IMAGE=$(DOCKER_IMAGE) TAG=$(DOCKER_TAG)

aarch64-image-force:
	$(MAKE) -C $(DOCKER_DIR) image-force IMAGE=$(DOCKER_IMAGE) TAG=$(DOCKER_TAG)

aarch64-test:
	$(MAKE) -C $(DOCKER_DIR) test IMAGE=$(DOCKER_IMAGE) TAG=$(DOCKER_TAG)

aarch64-check-format:
	$(MAKE) -C $(DOCKER_DIR) check-format IMAGE=$(DOCKER_IMAGE) TAG=$(DOCKER_TAG)

aarch64-format:
	$(MAKE) -C $(DOCKER_DIR) format IMAGE=$(DOCKER_IMAGE) TAG=$(DOCKER_TAG)

aarch64-tidy:
	$(MAKE) -C $(DOCKER_DIR) tidy IMAGE=$(DOCKER_IMAGE) TAG=$(DOCKER_TAG)

aarch64-ccache-stats:
	$(MAKE) -C $(DOCKER_DIR) ccache-stats IMAGE=$(DOCKER_IMAGE) TAG=$(DOCKER_TAG)

aarch64-shell:
	$(MAKE) -C $(DOCKER_DIR) shell IMAGE=$(DOCKER_IMAGE) TAG=$(DOCKER_TAG)

aarch64-clean:
	$(MAKE) -C $(DOCKER_DIR) clean
