# Arches Instrumentation — Yocto recipe (REQ §13).
# The recipe builds and installs; all system files (units, sysusers,
# tmpfiles, udev, defaults) ship inside the repo and are installed from it.
SUMMARY = "Arches Instrumentation on-device management and observability suite"
DESCRIPTION = "Hub-and-spoke instrumentation: istate hub, nine spokes, one external face"
HOMEPAGE = "https://arches.example"
# CLOSED, matching every source header in this repo ("a proprietary product of
# Soccentric... may not be used, copied, modified, distributed, or disclosed...
# without the express prior written consent") and matching the sibling
# meta-webconfig recipes, which also declare CLOSED.
#
# This previously said Apache-2.0 with the canonical Apache full-text checksum
# (md5 3b83ef96...). Three things were wrong with that at once: the 151 source
# files declare proprietary terms, the LICENSE file was an Apache *reference*
# rather than the full text so the checksum could never match, and an image
# whose license manifest claims Apache-2.0 over proprietary code is a
# compliance problem rather than a cosmetic one. The mismatch also means this
# recipe has never successfully built -- do_populate_lic would fail.
# See Task.md D-14.
LICENSE = "CLOSED"

SRC_URI = "git://github.com/soccentric-arches/Arches-Instrumentation.git;protocol=ssh;branch=develop"
# Pinned, not AUTOREV. Two builds of the same release tag must produce the same
# software, which is disqualifying to get wrong for an image with an OTA story:
# a SWUpdate artifact built from a floating branch is not reproducible, so a
# rollback cannot be reasoned about. Bump this deliberately when releasing.
#
# For bring-up, a floating branch is still available without editing the
# recipe: set ARCHES_DEV_BUILD = "1" in local.conf. That is opt-in on purpose
# -- a release build must never silently float.
# Held in its own variable: referencing SRCREV from inside SRCREV's own
# expansion is self-referential and bitbake would recurse on it.
INSTR_SRCREV ?= "fb0fe9cedb5729909a1ae15df2e8ed8189db4b7a"
SRCREV = "${@d.getVar('AUTOREV') if d.getVar('ARCHES_DEV_BUILD') == '1' else d.getVar('INSTR_SRCREV')}"
# One version for the whole suite (REQ §2): read it from the VERSION file
# rather than restating it here, where it would drift.
PV = "${@open(d.getVar('S') + '/VERSION').read().strip() if os.path.exists(d.getVar('S') + '/VERSION') else '0.0.0'}"
S = "${WORKDIR}/git"

inherit cmake systemd pkgconfig useradd

# One unprivileged system user per daemon (§10, §13). sysusers.d covers a
# running system; this covers the rootfs at build time so file ownership is
# right before anything starts.
USERADD_PACKAGES = "${PN}"
GROUPADD_PARAM:${PN} = "--system instr"
USERADD_PARAM:${PN} = "--system --no-create-home --shell /sbin/nologin -g instr instr-istate; \
    --system --no-create-home --shell /sbin/nologin -g instr instr-networkd; \
    --system --no-create-home --shell /sbin/nologin -g instr instr-monitord; \
    --system --no-create-home --shell /sbin/nologin -g instr instr-hardware; \
    --system --no-create-home --shell /sbin/nologin -g instr instr-faults; \
    --system --no-create-home --shell /sbin/nologin -g instr instr-alarms; \
    --system --no-create-home --shell /sbin/nologin -g instr instr-logmanager; \
    --system --no-create-home --shell /sbin/nologin -g instr instr-telemetry; \
    --system --no-create-home --shell /sbin/nologin -g instr instr-ota; \
    --system --no-create-home --shell /sbin/nologin -g instr instr-backup; \
    --system --no-create-home --shell /sbin/nologin -g instr instr-configuration"

DEPENDS = "grpc grpc-native protobuf protobuf-native yaml-cpp openssl"
# Runtime tools networkd and ota actually invoke (see the platform layer):
# iproute2 for interfaces and routes, nftables for firewall and NAT,
# wpa-supplicant/hostapd for wireless client and AP mode, wireguard-tools
# for the tunnel, the time daemons istate's policy drives, and SWUpdate as
# the update engine.
RDEPENDS:${PN} = "\
    iproute2 nftables \
    wireguard-tools \
    wpa-supplicant hostapd \
    chrony linuxptp gpsd \
    swupdate \
    openssh-sshd \
"

EXTRA_OECMAKE = " \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DCMAKE_SYSTEM_PROCESSOR=${TARGET_ARCH} \
"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/daemons/istate/istate        ${D}${bindir}/instr-istate
    install -m 0755 ${B}/daemons/networkd/networkd    ${D}${bindir}/instr-networkd
    install -m 0755 ${B}/daemons/monitord/monitord    ${D}${bindir}/instr-monitord
    install -m 0755 ${B}/daemons/hardware/hardware    ${D}${bindir}/instr-hardware
    install -m 0755 ${B}/daemons/faults/faults        ${D}${bindir}/instr-faults
    install -m 0755 ${B}/daemons/alarms/alarms        ${D}${bindir}/instr-alarms
    install -m 0755 ${B}/daemons/logmanager/logmanager ${D}${bindir}/instr-logmanager
    install -m 0755 ${B}/daemons/telemetry/telemetry  ${D}${bindir}/instr-telemetry
    install -m 0755 ${B}/daemons/ota/ota              ${D}${bindir}/instr-ota
    install -m 0755 ${B}/daemons/backup/backup        ${D}${bindir}/instr-backup
    install -m 0755 ${B}/tools/istate_ctrl/istate_ctrl      ${D}${bindir}/istate_ctrl
    install -m 0755 ${B}/tools/network_ctrl/network_ctrl    ${D}${bindir}/network_ctrl
    install -m 0755 ${B}/tools/monitor_ctrl/monitor_ctrl    ${D}${bindir}/monitor_ctrl
    install -m 0755 ${B}/tools/hardware_ctrl/hardware_ctrl  ${D}${bindir}/hardware_ctrl
    install -m 0755 ${B}/tools/fault_ctrl/fault_ctrl        ${D}${bindir}/fault_ctrl
    install -m 0755 ${B}/tools/alarm_ctrl/alarm_ctrl        ${D}${bindir}/alarm_ctrl
    install -m 0755 ${B}/tools/log_ctrl/log_ctrl            ${D}${bindir}/log_ctrl
    install -m 0755 ${B}/tools/telemetry_ctrl/telemetry_ctrl ${D}${bindir}/telemetry_ctrl
    install -m 0755 ${B}/tools/ota_ctrl/ota_ctrl            ${D}${bindir}/ota_ctrl
    install -m 0755 ${B}/tools/backup_ctrl/backup_ctrl      ${D}${bindir}/backup_ctrl

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${S}/system/systemd/*.service ${D}${systemd_unitdir}/system/
    install -d ${D}${sysconfdir}/sysusers.d
    install -m 0644 ${S}/system/sysusers.d/instrumentation.conf ${D}${sysconfdir}/sysusers.d/
    install -d ${D}${nonarch_libdir}/tmpfiles.d
    install -m 0644 ${S}/system/tmpfiles.d/instrumentation.conf ${D}${nonarch_libdir}/tmpfiles.d/
    install -d ${D}${sysconfdir}/udev/rules.d
    install -m 0644 ${S}/system/udev/70-instrumentation.rules ${D}${sysconfdir}/udev/rules.d/
    # Descriptors: install this MACHINE's set under the plain names the
    # daemons open. Shipping every board's file and expecting the daemon to
    # guess which one applies is how "devices.yaml not found" happens on a
    # real image — the daemons read devices.yaml, capabilities.yaml and
    # resource-profile.yaml, so that is what has to be there.
    install -d ${D}${sysconfdir}/instrumentation
    install -m 0644 ${S}/descriptors/devices-${INSTR_BOARD}.yaml \
        ${D}${sysconfdir}/instrumentation/devices.yaml
    install -m 0644 ${S}/descriptors/capabilities-${INSTR_BOARD}.yaml \
        ${D}${sysconfdir}/instrumentation/capabilities.yaml
    install -m 0644 ${S}/descriptors/resource-profile-${INSTR_BOARD}.yaml \
        ${D}${sysconfdir}/instrumentation/resource-profile.yaml
    # Fault rules: the board's own signature set where one exists, otherwise
    # the shared default (§5.1: rule sets are data).
    if [ -f ${S}/descriptors/fault-rules-${INSTR_BOARD}.yaml ]; then
        install -m 0644 ${S}/descriptors/fault-rules-${INSTR_BOARD}.yaml \
            ${D}${sysconfdir}/instrumentation/fault-rules.yaml
    else
        install -m 0644 ${S}/catalog/fault-rules.yaml \
            ${D}${sysconfdir}/instrumentation/fault-rules.yaml
    fi
    install -d ${D}${datadir}/instrumentation/catalog
    install -m 0644 ${S}/catalog/alarms.yaml ${D}${datadir}/instrumentation/catalog/
    # The default YAML set that seeds /pst on first boot and after a system
    # reset (§6.1, §9). Each daemon folder carries its config.yaml plus any
    # catalogs or rule sets that daemon owns in /pst.
    install -d ${D}${datadir}/instrumentation/defaults
    for d in ${S}/system/defaults/*/; do
        name=$(basename ${d})
        install -d ${D}${datadir}/instrumentation/defaults/${name}
        install -m 0644 ${d}/*.yaml ${D}${datadir}/instrumentation/defaults/${name}/
    done
    install -d ${D}${datadir}/instrumentation/schemas
    install -m 0644 ${S}/schemas/*.json ${D}${datadir}/instrumentation/schemas/

    # Application SDK (Task.md E-03): the .proto files an application compiles
    # a client from, plus the CMake package config that generates the stubs.
    #
    # Installed explicitly rather than by cmake_do_install: this recipe
    # overrides do_install() wholesale, so the upstream CMake install() rules
    # never run. They still exist and are what `cmake --install` uses for a
    # host build; this keeps the two in step.
    install -d ${D}${datadir}/instrumentation/proto
    install -m 0644 ${S}/proto/*.proto ${D}${datadir}/instrumentation/proto/
    install -d ${D}${libdir}/cmake/instrumentation
    install -m 0644 ${B}/instrumentationConfig.cmake \
        ${B}/instrumentationConfigVersion.cmake \
        ${D}${libdir}/cmake/instrumentation/
}

SYSTEMD_PACKAGES = "${PN}"
SYSTEMD_SERVICE:${PN} = "instr-istate.service instr-networkd.service instr-monitord.service instr-hardware.service instr-faults.service instr-alarms.service instr-logmanager.service instr-telemetry.service instr-ota.service instr-backup.service"
SYSTEMD_AUTO_ENABLE = "enable"

# Which board's descriptors go into this image.
#
# These overrides key on MACHINE, so they must spell MACHINE exactly. They
# previously read `jetson-orin-nano` / `jetson-agx-orin`, which are descriptor
# names, not machines -- no override ever matched, INSTR_BOARD kept its
# `x86-64` default, and every Jetson image shipped x86-64 devices.yaml,
# capabilities.yaml and resource-profile.yaml. That is exactly the failure the
# comment in do_install says this mechanism exists to prevent. See Task.md D-02.
#
# The MACHINE list is Arches-OS/kas/boards/*.yaml. The p-numbers are NVIDIA
# module/carrier part numbers: p3767 is the Orin Nano / Orin NX module and
# p3701 is the AGX Orin module, so each maps to that module's descriptor set.
#
# COMPATIBLE_MACHINE restricts parsing to exactly the MACHINEs listed below.
# Without it, wiring this layer into any kas build makes bitbake parse this
# recipe for every MACHINE in the layer set -- including Arches-OS's current
# boards (jetson-nano-devkit, jetson-tx1-devkit, jetson-tx2-devkit*,
# jetson-xavier-nx-devkit-tx2-nx), none of which have an INSTR_BOARD mapping
# below -- and the do_install-guard python __anonymous() further down would
# bb.fatal on every one of them. COMPATIBLE_MACHINE makes bitbake cleanly
# skip the recipe there instead, exactly like an unmapped MACHINE should be
# handled: refuse to guess, don't crash the whole layer set.
COMPATIBLE_MACHINE = "^(jetson-orin-nano-devkit|jetson-orin-nano-devkit-nvme|p3509-a02-p3767-0000|p3768-0000-p3767-0000|p3768-0000-p3767-0001|p3768-0000-p3767-0003|p3768-0000-p3767-0004|jetson-agx-orin-devkit|jetson-agx-orin-devkit-industrial|p3737-0000-p3701-0004|p3737-0000-p3701-0005|zynq-ultrascale|zynq-7000|imx8m-plus|sitara-am62|raspberrypi4-64|raspberrypi5|qemux86-64|genericx86-64)$"

INSTR_BOARD ?= ""

# Orin Nano class (p3767 module).
INSTR_BOARD:jetson-orin-nano-devkit = "jetson-orin-nano"
INSTR_BOARD:jetson-orin-nano-devkit-nvme = "jetson-orin-nano"
INSTR_BOARD:p3509-a02-p3767-0000 = "jetson-orin-nano"
INSTR_BOARD:p3768-0000-p3767-0000 = "jetson-orin-nano"
INSTR_BOARD:p3768-0000-p3767-0001 = "jetson-orin-nano"
INSTR_BOARD:p3768-0000-p3767-0003 = "jetson-orin-nano"
INSTR_BOARD:p3768-0000-p3767-0004 = "jetson-orin-nano"

# AGX Orin class (p3701 module).
INSTR_BOARD:jetson-agx-orin-devkit = "jetson-agx-orin"
INSTR_BOARD:jetson-agx-orin-devkit-industrial = "jetson-agx-orin"
INSTR_BOARD:p3737-0000-p3701-0004 = "jetson-agx-orin"
INSTR_BOARD:p3737-0000-p3701-0005 = "jetson-agx-orin"

# Non-Jetson targets the descriptor set covers, kept for the other fleet
# boards and for host/emulator builds. These are explicit rather than a
# catch-all default: an unmapped MACHINE must fail, not quietly pick one.
INSTR_BOARD:zynq-ultrascale = "zynq-ultrascale"
INSTR_BOARD:zynq-7000 = "zynq-7000"
INSTR_BOARD:imx8m-plus = "imx8m-plus"
INSTR_BOARD:sitara-am62 = "sitara-am62"
INSTR_BOARD:raspberrypi4-64 = "rpi-4b"
INSTR_BOARD:raspberrypi5 = "rpi-5"
INSTR_BOARD:qemux86-64 = "x86-64"
INSTR_BOARD:genericx86-64 = "x86-64"

# Stop the build on an unmapped MACHINE rather than installing another board's
# descriptors. A wrong descriptor set does not fail loudly at runtime -- the
# daemons come up and describe hardware the board does not have -- so this is
# the last point at which the mistake is cheap to catch.
python __anonymous() {
    board = d.getVar('INSTR_BOARD')
    if not board:
        bb.fatal("instrumentation: no INSTR_BOARD mapping for MACHINE '%s'. "
                 "Add an INSTR_BOARD:%s override to instrumentation.bb naming "
                 "the descriptor set this board should ship (see descriptors/)."
                 % (d.getVar('MACHINE'), d.getVar('MACHINE')))
}

FILES:${PN} += "${sysusersdir} ${nonarch_libdir}/tmpfiles.d ${datadir}/instrumentation"

# ---- application SDK (-dev) (Task.md E-03) ----
#
# A colocated application declares alarms and reports metrics over
# ApplicationApi, so it has to compile a gRPC client. Before this, do_install
# placed binaries, units, sysusers, tmpfiles, udev rules, descriptors,
# catalogs, defaults and schemas -- and no .proto files, no CMake package
# config, nothing to build a client against. An application could be given the
# right group (E-01) and still have nothing to compile, which is why no leaf
# links one today.
#
# The upstream CMake install rules place proto/*.proto under
# ${datadir}/instrumentation/proto and a package config under
# ${libdir}/cmake/instrumentation. cmake_do_install already runs them; this
# just routes them into ${PN}-dev.
#
# Note there is no stub *library* here, deliberately: generated protobuf/gRPC
# sources must be compiled by the consumer's own toolchain against its own
# protobuf runtime. The package ships the .proto files and a codegen helper
# instead. See cmake/instrumentationConfig.cmake.in and docs/example-client/.
FILES:${PN}-dev += " \
    ${datadir}/instrumentation/proto \
    ${libdir}/cmake/instrumentation \
"
RDEPENDS:${PN}-dev += "protobuf-dev grpc-dev"
# The protos are the interface an application builds against; they belong in
# -dev even though nothing else in this recipe produces a -dev package.
ALLOW_EMPTY:${PN}-dev = "0"
