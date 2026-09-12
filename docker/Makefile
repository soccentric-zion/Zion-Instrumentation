# arm64 (aarch64) build container for Arches Instrumentation.
#
# The image is Ubuntu 22.04 arm64 — the Ubuntu generation JetPack 6 (L4T
# r36.x) runs on the Jetson fleet — so binaries produced inside link against
# the on-device glibc and run without a cross toolchain (see Dockerfile).
#
# On an x86_64 workstation the arm64 container runs under qemu-user; the
# `binfmt` target installs the kernel handlers (idempotent, needs
# --privileged once). On an aarch64 host (a Jetson) everything is native and
# binfmt is skipped automatically.
#
# Speed model (x86_64 hosts, where compiles run emulated):
#   - `image` is a guard, not a hard prerequisite: once the image exists,
#     `make build` goes straight to compiling and never invokes docker
#     buildx. Rebuild the image explicitly with `make image-force` after
#     editing the Dockerfile.
#   - Compiles run under ccache backed by a persistent host directory
#     (CCACHE_DIR): a cache hit skips the emulated compile entirely, and the
#     cache survives `make clean`.
#   - Emulated compiler processes stall on qemu, so jobs are oversubscribed
#     (2x nproc) to keep the cores busy.

IMAGE      ?= arches/instrumentation-build
TAG        ?= aarch64
PLATFORM   ?= linux/arm64
BUILD_DIR  ?= build-aarch64
BUILD_TYPE ?= RelWithDebInfo

ROOT        := $(abspath ..)
FULL_IMAGE  := $(IMAGE):$(TAG)
HOST_ARCH   := $(shell uname -m)
NPROC       := $(shell nproc)

ifeq ($(HOST_ARCH),x86_64)
JOBS ?= $(shell echo $$(( $(NPROC) * 2 )))
else
JOBS ?= $(NPROC)
endif

# Persistent across container runs and `make clean`; override on the
# command line (or via the environment) to relocate it.
CCACHE_DIR ?= $(HOME)/.cache/arches-instrumentation-ccache

# Run as the invoking user so build outputs in the mounted repo are not
# root-owned; HOME under /tmp keeps tools that want a writable home happy.
RUN := docker run --rm --platform $(PLATFORM) \
       --user $(shell id -u):$(shell id -g) -e HOME=/tmp \
       -v $(CCACHE_DIR):/ccache -e CCACHE_DIR=/ccache \
       -v $(ROOT):/src -w /src $(FULL_IMAGE)

# One shared configure line: every target that needs a build dir configures
# with identical flags, so reconfigure never triggers and ccache launchers
# are set exactly once. Configure is idempotent and cached when
# $(BUILD_DIR) already exists.
CMAKE_CONFIGURE := cmake -S /src -B /src/$(BUILD_DIR) -G Ninja \
        -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DBUILD_TESTING=ON \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache

.DEFAULT_GOAL := help
.PHONY: help image image-force binfmt build test check-format format tidy \
        ccache-stats verify shell clean

help:
	@echo 'arm64 build container — targets:'
	@echo '  make image        ensure the arm64 image exists (skips if present)'
	@echo '  make image-force  rebuild the image (run after editing the Dockerfile)'
	@echo '  make build        cmake+Ninja build of the whole project -> /src/$(BUILD_DIR)'
	@echo '  make test         ctest inside the container (needs build first)'
	@echo '  make check-format  clang-format gate inside the container'
	@echo '  make format        apply .clang-format inside the container'
	@echo '  make tidy          clang-tidy inside the container (slow under qemu)'
	@echo '  make ccache-stats  compiler-cache hit/miss statistics'
	@echo '  make verify        show that the produced binaries are aarch64'
	@echo '  make shell         interactive shell in the container'
	@echo '  make binfmt        install qemu arm64 handlers (x86_64 hosts, once)'
	@echo '  make clean         remove $(BUILD_DIR) (ccache is kept)'
	@echo
	@echo 'Variables: IMAGE TAG PLATFORM BUILD_DIR BUILD_TYPE JOBS CCACHE_DIR'
	@echo "JOBS default: $(JOBS) on this host (nproc=$(NPROC)$$( [ '$(HOST_ARCH)' = x86_64 ] && echo ', oversubscribed 2x for qemu' || echo ''))"

# qemu handlers first on x86_64: the arm64 build stages run under emulation
# and fail with "exec format error" without them. No-op on aarch64 hosts.
binfmt:
	docker run --privileged --rm tonistiigi/binfmt --install arm64

# Guard, not a step: only builds when the image is missing. This is what
# keeps the docker part out of every code build — once the image exists,
# `make build` touches nothing docker-related.
image:
	@if docker image inspect $(FULL_IMAGE) >/dev/null 2>&1; then \
	  echo "$(FULL_IMAGE) already present — skipping (use 'make image-force' to rebuild)"; \
	else \
	  $(MAKE) image-force; \
	fi

# Explicit rebuild path. On x86_64 the arm64 layers build under qemu, so
# expect the first one to be slow; afterwards buildx layer cache makes
# repeats fast. Needed only after editing docker/Dockerfile.
ifeq ($(HOST_ARCH),x86_64)
image-force: binfmt
endif
image-force:
	docker buildx build --platform $(PLATFORM) --load -f $(ROOT)/docker/Dockerfile -t $(FULL_IMAGE) $(ROOT)

build: image
	@mkdir -p $(CCACHE_DIR)
	$(RUN) bash -ec '\
	  $(CMAKE_CONFIGURE) \
	  && cmake --build /src/$(BUILD_DIR) -j $(JOBS)'

test: image
	@mkdir -p $(CCACHE_DIR)
	$(RUN) bash -ec '\
	  $(CMAKE_CONFIGURE) \
	  && ctest --test-dir /src/$(BUILD_DIR) --output-on-failure'

# The §14 gates, in the same container and toolchain as the build. They only
# need a configured build dir, never a compiled one.
check-format: image
	@mkdir -p $(CCACHE_DIR)
	$(RUN) bash -ec '\
	  $(CMAKE_CONFIGURE) \
	  && cmake --build /src/$(BUILD_DIR) --target check-format'

format: image
	@mkdir -p $(CCACHE_DIR)
	$(RUN) bash -ec '\
	  $(CMAKE_CONFIGURE) \
	  && cmake --build /src/$(BUILD_DIR) --target format'

# clang-tidy over lib/, daemons/, tools/: every TU is re-analyzed, and under
# qemu on x86_64 hosts that is slow — expect this target to take a while.
tidy: image
	@mkdir -p $(CCACHE_DIR)
	$(RUN) bash -ec '\
	  $(CMAKE_CONFIGURE) \
	  && cmake --build /src/$(BUILD_DIR) --target run-clang-tidy'

ccache-stats:
	@mkdir -p $(CCACHE_DIR)
	$(RUN) bash -ec 'ccache -s 2>/dev/null | grep -E "Hits|Misses|Hit rate|Cache size" || true'

# Proof the output really targets arm64: every executable must say aarch64.
verify:
	@non_aarch64=$$(find $(ROOT)/$(BUILD_DIR) -type f -executable \
	     \( -name 'instr-*' -o -name '*_ctrl' -o -name 'unit_*' -o -name 'integration_*' \) \
	     | xargs file | grep -v 'ARM aarch64' || true); \
	if [ -n "$$non_aarch64" ]; then \
	  echo 'NOT aarch64:'; echo "$$non_aarch64"; exit 1; \
	else \
	  echo 'all daemons/tools/tests in $(BUILD_DIR)/ are aarch64 ELF'; \
	fi

shell: image
	$(RUN) bash

# Removes the build dir only. The compiler cache in $(CCACHE_DIR) survives,
# so the next `make build` reuses it instead of recompiling under qemu.
clean:
	rm -rf $(ROOT)/$(BUILD_DIR)