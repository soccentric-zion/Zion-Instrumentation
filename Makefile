# Arches Instrumentation — top-level developer entry point (REQ §14).
#
# Host-native only: a C++17 toolchain + CMake + protoc/grpc on the host is
# everything this repo needs to build and test. Binaries land in
# build-<PRESET>/ (build-host-debug/ by default) via the `host-debug` CMake
# preset — the same preset .claude/validate.sh exercises.

PRESET       ?= host-debug
BUILD_DIR    := build-$(PRESET)

.DEFAULT_GOAL := build
.PHONY: help build test check-format format tidy clean

help:
	@echo 'Arches Instrumentation — make targets (host-native, PRESET=$(PRESET)):'
	@echo '  make build         default: host-native build -> $(BUILD_DIR)/'
	@echo '  make test          run the test suite (ctest) natively'
	@echo '  make check-format  clang-format gate natively'
	@echo '  make format        apply .clang-format natively'
	@echo '  make tidy          clang-tidy natively (run-clang-tidy target)'
	@echo '  make clean         remove $(BUILD_DIR)/'
	@echo
	@echo 'Variables: PRESET (default host-debug; see CMakePresets.json for others)'

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
