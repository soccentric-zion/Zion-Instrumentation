# arm64 build environment for Arches Instrumentation (REQ §14: CMake with
# presets, cross-compilation for all nine targets — this image covers the
# aarch64 targets, the Jetson fleet, natively).
#
# Base is Ubuntu 22.04 arm64: the same Ubuntu generation as JetPack 6
# (L4T r36.x) on the Jetson targets, so binaries built inside link against
# the glibc the device actually runs. Building in an arm64 container instead
# of cross-compiling means the device toolchain is exercised exactly as the
# device sees it — no sysroot drift.
#
# On an x86_64 workstation the image builds and runs under qemu-user via
# binfmt (see docker/Makefile `binfmt`); on an aarch64 host it is native.
FROM --platform=linux/arm64 ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Toolchain and every dependency the CMakeLists finds: protobuf + gRPC
# (Ubuntu's gRPC has no CMake config, the pkg-config fallback in the
# top-level CMakeLists handles that), yaml-cpp, GoogleTest, Ninja for fast
# incremental builds, python3 (run-clang-tidy needs it), file (verifying the
# produced binaries really are aarch64).
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl gnupg git python3 file \
      build-essential ninja-build pkg-config \
      protobuf-compiler protobuf-compiler-grpc libprotobuf-dev \
      libgrpc++-dev libgrpc-dev libssl-dev \
      libyaml-cpp-dev libgtest-dev \
    && rm -rf /var/lib/apt/lists/*

# Kitware APT repo: CMake >= 3.25 so CMakePresets v6 parses inside the image
# (jammy's stock cmake is 3.22 and rejects the preset version).
RUN curl -fsSL https://apt.kitware.com/keys/kitware-archive-latest.asc \
      | gpg --dearmor -o /usr/share/keyrings/kitware-archive-keyring.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ jammy main" \
      > /etc/apt/sources.list.d/kitware.list \
    && apt-get update && apt-get install -y --no-install-recommends cmake \
    && rm -rf /var/lib/apt/lists/*

# LLVM 18: clang-format-18 / clang-tidy-18 / run-clang-tidy-18 for the §14
# gates. The CMake find_program calls look for exactly these names.
RUN curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
      | gpg --dearmor -o /usr/share/keyrings/llvm-archive-keyring.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/llvm-archive-keyring.gpg] http://apt.llvm.org/jammy/ llvm-toolchain-jammy-18 main" \
      > /etc/apt/sources.list.d/llvm18.list \
    && apt-get update && apt-get install -y --no-install-recommends \
      clang-18 clang-format-18 clang-tidy-18 \
    && rm -rf /var/lib/apt/lists/*

# ccache: compile cache persisted on the host (docker/Makefile mounts it at
# /ccache, so it survives container runs and `make clean`). Under qemu on
# x86_64 hosts a cache hit skips the emulated compile entirely — which is
# where all the rebuild time goes. Kept in its own final layer so the
# layers above stay cached when only this one changes.
RUN apt-get update && apt-get install -y --no-install-recommends ccache \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
CMD ["/bin/bash"]