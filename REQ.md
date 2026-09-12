LLM Build Prompt — Arches Instrumentation
You are building Arches Instrumentation, a complete C++17 on-device software management and observability suite for embedded Linux, plus its Yocto integration and test suite. Build the entire system. Do not stub, do not leave TODOs, do not defer functionality to "a later phase". Every daemon, every CLI, every config schema, every test.

0. Mission
Instrumentation is the production-path software layer for a fleet of embedded Linux devices spanning nine hardware targets (NVIDIA Jetson, AMD Xilinx Zynq, NXP i.MX, TI Sitara, Intel/AMD x86, Raspberry Pi, and three further targets). It owns system state, hardware control and health, kernel fault detection, alarms, logs, telemetry, networking, updates, and backup/restore. It is the device's single point of contact with the outside world.
It is consumed by two external peers:
Configuration — a local per-device web configuration UI, written in Rust, running on the same box.
HQ — a cloud fleet management, configuration, and monitoring service.
Instrumentation ships as a single monorepo that Yocto pulls and deploys into the device image.
Product context
Two independent product paths share only the OS layer. Instrumentation belongs to the production path and must not depend on, link against, or share protocol definitions with the IV&V path.
Path
Components
IV&V
OS / HAL / Client / Server / Dashboard
Production
OS / Instrumentation / Configuration / HQ


1. Non-negotiable architectural rules
These are locked. Do not redesign them.
R1 — Hub and spoke. Ten daemons: istate is the hub, the other nine are spokes. Every spoke talks to istate and to nothing else. There is no daemon-to-daemon communication of any kind. This rule exists to prevent an N×N service mesh across nine platforms; violating it is the single worst thing you can do to this codebase.
R2 — One external face. istate is the only process with an external interface. No other daemon opens a network socket, holds a cloud credential, or knows HQ exists.
R3 — One wire format for services. gRPC everywhere: over Unix domain sockets on-box, over a WireGuard tunnel to HQ. One IDL, one generator.
R4 — istate is the sole writer to /pst. Every other component reads and writes persistent configuration through istate's API. Backup/Restore is the only component that may touch /pst files directly, and only under istate's direction.
R5 — Alarms are declared, never inferred. Only an application sets or clears an alarm. No daemon — including Alarms itself — synthesises one from observation.
R6 — Faults is read-only. The Faults daemon observes the kernel log and reports. It never configures hardware and never raises an alarm.
R7 — Hardware is the only hardware writer. The Hardware daemon has full control of board devices. No other daemon touches hardware directly.
R8 — networkd owns everything network-adjacent. Interfaces, wireless, routing, DNS, firewall, SSH, port forwarding, the WireGuard tunnel, and the time daemons (chrony, ptp4l, phc2sys, gpsd). istate owns time policy; networkd applies it.
R9 — No platform conditionals outside the platform layer. No #ifdef for a target anywhere in daemon code. All variation lives in libplatform and in declarative descriptors. The same binaries ship on every target.
R10 — Degrade, don't block. Loss of the tunnel, of HQ, of any spoke, or of the clock must never stop the device.

2. Repository
Single monorepo. One CMake project. One version for the whole suite. Yocto pulls this repo and deploys it into the image, so the repo carries all system files — systemd units, sysusers, tmpfiles, default configs, socket permissions. Nothing system-related lives in the Yocto layer; the recipe only builds and installs.
instrumentation/
  CMakeLists.txt
  CMakePresets.json
  VERSION
  README.md
  LICENSE

  proto/                        all .proto definitions
    common.proto
    istate.proto
    networkd.proto
    monitord.proto
    hardware.proto
    faults.proto
    alarms.proto
    logmanager.proto
    telemetry.proto
    ota.proto
    backup.proto

  lib/
    common/                     shared core library — see §4
    platform/                   platform abstraction layer — see §5
    ctrl/                       raw-UDS control server + client framework

  daemons/
    istate/
    networkd/
    monitord/
    hardware/
    faults/
    alarms/
    logmanager/
    telemetry/
    ota/
    backup/

  tools/                        one _ctrl per daemon
    istate_ctrl/
    network_ctrl/
    monitor_ctrl/
    hardware_ctrl/
    fault_ctrl/
    alarm_ctrl/
    log_ctrl/
    telemetry_ctrl/
    ota_ctrl/
    backup_ctrl/

  schemas/                      JSON Schema for every YAML format
  catalog/
    alarms.yaml                 master alarm catalog
    fault-rules.yaml            default anomaly signatures
  descriptors/                  per-board device descriptors

  system/
    systemd/                    unit files, one per daemon
    sysusers.d/
    tmpfiles.d/
    udev/
    defaults/                   default YAML set that seeds /pst

  test/
    unit/                       per-component unit tests
    integration/                host-side full-topology harness
    fixtures/
    fault-injection/

  yocto/
    recipes-instrumentation/    bitbake recipe consuming this repo

  docs/                         generated interface reference

Each daemon directory follows the same shape: src/, include/, test/, CMakeLists.txt. Each contains a thin main.cpp and a domain implementation that is testable without sockets or hardware.

3. Interfaces
3.1 Spoke ↔ istate
gRPC over Unix domain socket.
istate is the server. Each spoke dials in on start, registers, and stays connected.
One .proto per daemon plus common.proto for shared types.
Socket path: /run/instrumentation/<daemon>.sock, created by istate, mode 0660, owned by instr:<daemon>.
Each daemon runs as its own unprivileged system user. Socket permissions enforce which daemon may call which service.
Streaming where the data is a stream: monitord samples, Faults events, Telemetry batches, OTA progress, log tail. Unary otherwise.
Registration handshake. On connect a spoke sends: daemon name, build version, proto schema version, and a capability list. istate verifies the schema version against its own. On mismatch it refuses the registration and raises an alarm — it never guesses or silently degrades a mismatched peer. This protects against partial updates and rollbacks.
3.2 _ctrl ↔ its own daemon
Raw Unix domain socket, not gRPC. Length-prefixed frames, simple text or protobuf payload.
Socket path: /run/instrumentation/<daemon>-ctrl.sock, mode 0600, root only.
Each _ctrl must be able to exercise every option and operation its daemon supports — full coverage, not a convenience subset.
_ctrl changes are runtime-only and never persist to /pst. They are lost on restart. Only istate writes persistent config.
Each _ctrl supports: --help, subcommand help, --json output, --socket override, and shell completion generation.
Implementation requirement. Each daemon defines its operations once, in a single command table (name, description, argument spec, handler). Both the gRPC service implementation and the ctrl server dispatch from that table. This makes _ctrl coverage structural rather than hand-maintained, and gives help and completion for free. Do not write the two surfaces separately.
3.3 istate ↔ Configuration
Configuration is a gRPC-over-UDS client of istate, architecturally identical to a spoke. It holds no device state. Every read and write is an istate call. It never touches systemd, the network stack, the update system, or /pst directly.
Pairing and enrollment live in istate; Configuration renders the flow.
3.4 istate ↔ HQ
gRPC over a WireGuard tunnel. networkd brings the tunnel up; istate owns the gRPC session over it.
Device identity is the WireGuard key, generated on device at first boot.
istate maintains a persistent on-disk spool. Telemetry, faults, and log references are spooled first and drained when the tunnel is up. Nothing northbound depends on the tunnel being available at the moment it is produced.
Flood control: jittered exponential backoff on reconnect and on spool drain, so a site of hundreds of devices powering up together does not stampede HQ. Honour backpressure signals from HQ.

4. lib/common — the shared core
Every daemon links this. A new daemon should be roughly 200 lines of domain logic plus its command table. Provide:
Config loading. YAML parse, JSON-Schema validation, typed access, clear rejection on invalid input with file and line. A typo must be a loud error, never a silent behavioural change.
Config origin tracking. Every key carries its origin: default, app, local, or hq. Resets and HQ pushes consult origin to decide what they may overwrite. Merge semantics preserve operator edits when a new release adds keys.
Logging. Structured, levelled, journal-aware, rate-limited, with per-source runtime level control.
Timestamps. Every record carries CLOCK_MONOTONIC plus a boot ID alongside wall clock. Provide a reconciliation API invoked when a time source acquires lock, so spooled data, alarms, and logs are re-stamped correctly. A GNSS or NTP lock mid-run must never reorder history.
Lifecycle. Standard daemon skeleton: signal handling, graceful shutdown, readiness notification, systemd integration, the registration handshake, reconnect with backoff.
gRPC and ctrl server boilerplate, both dispatching from the shared command table.
Error types. One result/error model across the suite.
Rate limiter. Token bucket with a "suppressed N events" summary, used by Faults, LogManager, and Telemetry.
Bounded queue / spool primitive. Disk-backed, size-capped, with an explicit drop policy.
Atomic file writes. write-temp → fsync → rename, with checksum and redundant copy.

5. lib/platform — the platform abstraction layer
The only place target variation exists. One interface, one implementation per target. Provide:
Storage and partition topology; wear indicators (eMMC life-time estimates, SD health, SMART).
Boot slot control, rollback, golden boot state.
Thermal zones and power sources.
Hardware watchdog.
Hardware identity sources.
Time source availability.
Network device enumeration.
GPIO, LED, USB, CAN, and display access primitives for the Hardware daemon.
Entropy availability check (needed for first-boot key generation).
A simulated implementation is required, complete enough to run the entire suite on a build host with no hardware. This is not optional — the integration harness depends on it.
5.1 Declarative descriptors
Board-specific content is data, never code:
Device descriptor — which devices exist on this board and which check and control operations apply to each.
Fault rule set — kernel-log anomaly signatures and their classification.
Resource profile — sampling intervals, retention, spool and log budgets, by platform resource class.
Capability manifest — which features exist on this platform, so istate reports honestly rather than failing calls.
Adding a board means adding descriptors, not editing daemons.

6. /pst — persistent state
A dedicated persistent partition. Survives OTA and rollback. /home and /pst are never overwritten by an update unless explicitly requested in the update command.
/pst/
  factory/                    read-only after manufacture; never backed up
    identity/     identity.yaml (serial, hw revision, board variant, mfg date), .checksum
    calibration/  calibration.yaml, .checksum
    enrollment/   enrollment.pem (factory-signed device identity), .checksum

  system/                     istate is the sole writer; daemons read only
    istate/       config.yaml, .checksum
    networkd/     config.yaml, wg-private.key (0600), wg-peer.yaml, .checksum
    monitord/     config.yaml, .checksum
    hardware/     config.yaml, devices.yaml, .checksum
    faults/       config.yaml, fault-rules.yaml, .checksum
    alarms/       config.yaml, catalog.yaml, .checksum
    logmanager/   config.yaml, .checksum
    telemetry/    config.yaml, .checksum
    ota/          config.yaml, .checksum
    backup/       config.yaml, .checksum
    .schema-version

  state/                      runtime persistence; not config, not backed up
    alarms/active.db
    spool/
    ota/

  backup/                     local archives

Rules:
factory/ is mounted read-only in normal operation, written only by the factory programming tool.
system/ is what backup captures and what a system reset wipes. One folder per daemon, each owning its config.yaml plus its catalogs and keys.
state/ is neither backed up nor restored, and may be wiped freely. Recommend a separate partition for state/ — spool and OTA staging are the high-write paths and must never be able to corrupt system/.
Every folder carries a .checksum. All writes atomic. Redundant copy per level.
.schema-version drives migration when a release adds keys.
Storage headroom is reserved so spool, logs, and OTA staging can never fill the partition and wedge the device. Hard per-consumer budgets, enforced, with an alarm raised before the limit.
6.1 Reset semantics
Operation
Wipes
Keeps
System reset
system/, state/
factory/, backup/
Factory reset
everything
— (manufacturing operation only, must be explicitly gated)

After a system reset, istate reseeds system/ from the default YAML set shipped in the image.
6.2 Provisioning
istate holds a provisioning state machine. If it detects a virgin system it generates all default configuration; applications may then override values as they see fit, with origin recorded.
First-boot sequence:
Mount and validate /pst; detect virgin state.
Read factory identity; derive hostname.
Seed system/ from image defaults.
Check entropy availability, then generate the WireGuard keypair. The private key never leaves the device.
Sign the public key with the factory identity; enroll with HQ.
HQ verifies against the manufacturing record and returns tunnel configuration.
Transition to operational.
Key rotation must be supported without a site visit: regenerate, re-enroll, no factory involvement.

7. The ten daemons
Every daemon: gRPC/UDS client of istate, own raw-UDS ctrl socket, own _ctrl with full coverage, own config.yaml in /pst/system/<daemon>/, own unprivileged user, own systemd unit, no platform conditionals, full unit tests.
7.1 istate — the hub
The main application. Owns system state, provisioning, persistent configuration, orchestration, and every external interface.
gRPC server for all nine spokes and for Configuration; gRPC client to HQ over the tunnel.
Sole writer to /pst; serves all persistent config reads and writes.
Manages factory and system levels; system reset and factory reset.
Provisioning state machine (§6.2); virgin detection and default generation.
Device identity, hostname, system info: revisions, build IDs, capability manifest.
Orchestration, not supervision. Declares readiness order and dependency edges, requests start and stop through systemd, tracks each spoke's operational state, exposes it northbound. systemd owns restart policy, resource limits, and sandboxing. If istate restarts, spokes keep running and re-attach.
Watchdog. Pets the hardware watchdog via libplatform. Liveness-checks each spoke so a hung (not crashed) daemon is detected and restarted — a process that is running but not responding must not look healthy.
Time policy: source priority (GNSS > PTP > NTP), servers, PTP domain and profile, GNSS constellations. Persists to /pst, passes to networkd, publishes current source, offset, and lock quality.
Passthrough to networkd (network, firewall, SSH, tunnel), to OTA, and to Backup/Restore.
Northbound spool: persistent, disk-backed, bounded, with drop policy — oldest telemetry first, never alarms or faults. Jittered backoff, HQ backpressure honoured.
Fans out live log tail to HQ and Configuration.
Event timeline: a single ordered per-device stream of boot, config change, alarm, fault, update, and restart events.
istate_ctrl.
Startup order: istate → Faults, monitord, Hardware → Alarms, LogManager, Telemetry → networkd → OTA, Backup → tunnel and HQ session. The HQ session is last and is never a precondition for local operation. Observation daemons start early so boot-window anomalies are captured.
7.2 networkd
Owns everything network-adjacent and everything that touches an interface.
Wired: DHCP, static, VLAN, bonding. Wireless: client mode, SSID and credential management, scanning, and AP mode for field commissioning where supported.
Interface enumeration, link state, routing, DNS.
Firewall rules, SSH, port forwarding, network security.
WireGuard tunnel bring-up and state.
Time daemons: owns chrony, ptp4l, phc2sys, and gpsd. Applies the policy istate passes down; reports lock state, offset, and source quality upward.
Connectivity checks and reachability state; interface priority and failover.
Credentials are stored in /pst via istate, never written locally.
Tunnel-loss guard: any firewall or network change that kills the tunnel auto-reverts if HQ is not reachable within a configured timeout. This prevents remote bricking and is mandatory.
Remote access: inbound SSH is off by default. It is opened per device, time-boxed with automatic expiry, attributed to a named operator, and logged as an alarm the customer can see. A local console path remains available when the device is wedged.
network_ctrl.
7.3 monitord
System resource monitoring. Observation only; raises no alarms.
CPU utilisation per-core and aggregate, frequency, governor state.
Memory and swap; storage capacity, free space, and wear indicators.
Thermal zones; load average; process and thread counts; uptime and boot count.
Predictive storage wear: project remaining eMMC/SD life from wear indicators and expose a projection so an alarm can be raised before failure.
High-resolution rolling window for thermal and power retained locally at finer granularity than telemetry ships — when something fails you need the minute before, not the hour average.
Configurable sampling interval with per-resource-class defaults; streams samples to istate.
monitor_ctrl.
7.4 Hardware
Full control of board hardware. The only daemon that touches hardware directly.
Manages the 1:1 board devices: display, GPIO, CAN, USB, board LEDs, and everything else in the device descriptor.
Drives devices: set a GPIO, blink an LED, reset a USB device, control the display — not read-only.
Quick health checks, roughly one per device: presence detection plus a cheap behavioural sanity check. Not a full qualification test — that is the IV&V path.
Results: pass / fail / degraded / not-present, with a per-check detail payload.
Periodic scheduled checks plus on-demand checks triggered locally or from HQ.
Board knowledge lives entirely in devices.yaml; the same binary ships on every platform.
hardware_ctrl.
7.5 Faults
Purely passive. Never configures hardware, never raises an alarm.
Real-time monitoring of the kernel ring buffer and journal.
On launch, reads the full ring buffer from the start and backfills — the kernel boot log is the most diagnostic window there is and Faults starts after userspace.
Rule-based matching against fault-rules.yaml: bus errors, ECC events, link resets, thermal throttling, storage errors, device timeouts, watchdog trips, OOM.
Classification, severity assignment, deduplication, hard rate limiting with "suppressed N events" summaries — a stuck driver emitting 10k lines a second must not drown the system.
Fault IDs use the same 32-bit scheme as alarms (§8) with the fault domain, so HQ can correlate a kernel fault to the alarm an application raised for it.
Collects locally; streams to HQ through istate when enabled by configuration.
Rule sets are data — a new signature ships as configuration, not a release.
fault_ctrl.
7.6 Alarms
Classic telecom-style alarm manager. Only applications set and clear alarms.
Severities: CRITICAL, MAJOR, MINOR, WARNING, INFO.
State machine: raise → active → acknowledged → cleared, plus severity change on an already-active alarm.
Active alarm table with owner, severity, first and last occurrence, occurrence count, acknowledgement state. Persists across restart in state/alarms/active.db.
Time-based escalation — severity promotion for alarms left uncleared. Escalation policy is configuration, distributed from HQ or set locally.
X.733 fields: probable cause, specific problem, additional text, correlated notifications — so HQ can group alarms across a fleet.
Alarm state changes go to istate directly as events, never through Telemetry batching — escalation semantics must not be delayed by an aggregation window.
Snapshot-on-fault: when a CRITICAL alarm fires, trigger a bounded diagnostic capture (logs, resource state, hardware check results, active alarms) held for upload. The evidence must exist at the moment it matters.
Regenerates or merges its catalog when the software is updated, preserving operator edits.
alarm_ctrl.
7.7 LogManager
YAML config lists which logs to collect and send.
Collects from the journal and from application log sinks.
Rotation by size and age, retention per source, compression, per-source runtime level control.
Live tail: bounded, on-demand, filtered by source/level/since, rate-limited, auto-stopping on timeout. Server-streams to istate, which fans out to HQ and Configuration.
Bundles: support archive for a time window, with integrity checksum. Bulk transfer by handle — the control API carries a handle and metadata; the payload never traverses istate's control path.
Crash forensics: bounded coredump capture plus last-N log lines on abnormal daemon exit, folded into the bundle.
Storage budget enforced per resource class.
log_ctrl.
7.8 Telemetry
YAML config defines what to collect from the system; the daemon collects it and at a configured interval sends it to HQ and Configuration via istate.
Aggregates from monitord, Hardware, Faults, and applications; application metric registration API.
Configurable sampling rate and aggregation windows; functions min, max, mean, count, last.
Batch shaping; bounded local retention of unsent data; drop policy oldest-first, never faults or alarms.
Per-device health score: rolls monitord + Hardware + Faults + Alarms into a single number with contributing factors exposed, so a fleet is sortable.
Reproducibility metadata on every record: firmware version, config schema version, hardware revision.
telemetry_ctrl.
7.9 OTA
A thin manager on top of SWUpdate (SWUpdate is the chosen engine; any earlier RAUC path is superseded).
HQ and Configuration upload and store artifacts; the artifact is streamed down to the device via istate and lands on disk before applying. True streaming update is a later addition — design the interface so it can be added without breaking callers.
istate points the OTA manager at the artifact and asks it to apply.
Signature verification before staging; integrity verification; download with resume.
Stage to the inactive slot, apply, set boot slot.
Explicit post-boot confirmation — an unconfirmed update rolls back automatically. Manual rollback and golden boot fallback.
Anti-rollback: refuse downgrade below a configured minimum version.
/home and /pst are preserved unless the update explicitly requests otherwise.
Config migration on first boot after update, driven by .schema-version, merging new keys while preserving operator edits by origin.
Progress as a stream; status reports current slot, version, update history.
Bandwidth limiting; maintenance windows — per-device schedules where updates and reboots are permitted.
Canary support: report enough state (fault rate, health score, version) for HQ to run a staged rollout and auto-halt on regression.
ota_ctrl.
7.10 Backup / Restore
YAML lists the paths. Tar the files into an archive; restore by untarring. Snapshot /pst, create the archive, send it to HQ and Configuration via istate.
Manifest inside the archive: hardware revision, schema version, timestamp, firmware version. Restore refuses an incompatible archive.
Excludes factory/ and the WireGuard keys. A backup is portable config; device identity is not portable — restoring it onto another board would clone the identity.
Encrypted: local archives with a key derived from device identity; HQ uploads encrypted to HQ's public key.
Consequence, which is the correct field behaviour: restore on the same board is complete; restore on a replacement board gives config and forces fresh enrollment.
Transactional restore — completes fully or leaves prior state intact. Restore preview shows what will change before committing.
Scheduled and on-demand; local and remote destinations.
All /pst writes under istate's direction.
backup_ctrl.

8. Alarm and fault identity
A YAML catalog defines every alarm, code-generated into a shared C++ header at build time so every application uses the same IDs.
32-bit ID layout (locked):
[31:28]  domain     hardware / software / network / application / fault
[27:20]  module     which daemon or subsystem (256)
[19:16]  type       X.733 category — equipment, communications,
                    processing error, quality of service, environmental
[15:0]   index      alarm within that module (65536)

An ID is decodable without the catalog. 0x1_04_1_0007 reads as hardware / Hardware daemon / equipment / alarm 7.
Per catalog entry: ID, symbolic name, default severity, probable cause, specific problem, description, escalation rule, auto-clear policy, HQ-reportable flag.
The same catalog is consumed by HQ, so alarms render without hardcoding. Faults use the same scheme under the fault domain, enabling correlation between a kernel-level fault and an application-raised alarm.

9. Configuration format
All configuration is YAML. Every format has a JSON Schema in schemas/, validated at load, rejected with a clear file-and-line error on failure.
Every key carries an origin: default, app, local, hq.
The image ships a default set in system/defaults/ which seeds /pst on first boot and after a system reset.
HQ may change any daemon's config without an OTA, since config lives in /pst.
Signed configuration: config pushes from HQ are signed and verified on device, so tunnel compromise alone does not let an attacker reconfigure a fleet.
Bulk config operations are staged with the same canary discipline as updates — a bad config push across a fleet is as damaging as a bad image.
Config drift: the device reports actual effective config so HQ can compare against intent.

10. Security requirements
One external face means one attack surface. Enforce it in code review and in tests.
Device identity cryptographic and factory-provisioned; tunnel key generated on device, private key never leaves it; rotation supported without a site visit.
Each daemon unprivileged and confined by systemd sandboxing. Capabilities granted only where genuinely required — OTA for boot state, Backup for /pst, Hardware for device access, networkd for network administration.
Socket permissions enforce the call graph.
Inbound remote access off by default, time-boxed, attributed, audited, alarm-visible.
Secret hygiene: credentials never in logs, never in unencrypted backups, never in telemetry. Enforce with a test, not a convention.
Audit trail: who changed what, when, from where — config, updates, remote access sessions.
Report measured boot state upward where the platform supports it.

11. Failure behaviour
Implement each of these explicitly and test each one.
Failure
Required behaviour
Tunnel down
Spool northbound; device fully operational; drain on reconnect
Spool full
Drop oldest telemetry first; never drop alarms or faults; report the drop
HQ unreachable
Jittered exponential backoff; identical to tunnel-down from the device's view
Spoke crashes
systemd restarts; istate reports degraded; spoke re-registers
Spoke hangs
istate liveness check detects it and requests restart
istate crashes
Spokes keep running and re-attach; no external interface until it returns
/pst corrupt
Fall back to redundant copy; if both fail, boot unprovisioned and report — never fail to boot
Update fails to confirm
Automatic rollback via golden boot
Clock unavailable at boot
Operate on monotonic time; re-stamp spooled data on time acquisition
Storage near full
Enforce budgets, alarm before the limit, never wedge the device
Firewall change kills tunnel
Auto-revert if HQ unreachable within the timeout
Proto version mismatch
Refuse registration, raise an alarm, do not guess
Unclean shutdown
Detect via clean-shutdown marker; report on next boot
Fleet-wide simultaneous boot
Jittered enrollment and spool drain; honour HQ backpressure


12. Testing — mandatory
All code ships with unit tests and full coverage. This is a hard requirement, and it constrains the design: domain logic must be a plain class with no socket, no filesystem, and no hardware dependency; transport is a thin shell around it. A design that cannot reach high coverage is a design defect, not a testing shortfall.
Required:
Unit tests — GoogleTest, every daemon's domain logic, every library component, every YAML schema, the alarm ID codec, the config merge and origin logic, the spool drop policy, the rate limiter, the time reconciliation.
Contract tests — against the .proto definitions, so a breaking schema change fails CI rather than a field device.
Integration harness — hub, all nine spokes, simulated platform layer, running on a build host with no hardware. The full topology must be testable on a laptop.
Fault injection — storage errors, link loss, thermal events, daemon hangs, corrupt /pst, full disk, clock jumps, proto mismatch. The failure paths are the ones that ship untested.
Simulated device — the full daemon set in a container, so HQ and Configuration can be developed without hardware.
Coverage gate in CI, enforced on Jenkins.
Secret-leak test — asserts credentials never appear in logs, backups, or telemetry.

13. System integration
The repo ships everything the image needs:
systemd units per daemon, with dependency ordering, restart policy, resource limits, and sandboxing directives (ProtectSystem, PrivateTmp, NoNewPrivileges, capability bounding sets, and so on).
sysusers.d — one system user per daemon.
tmpfiles.d — /run/instrumentation with correct ownership and modes.
udev rules where device access requires them.
Default YAML set for seeding /pst.
Yocto recipe in yocto/ that builds and installs this repo. The recipe installs; it does not carry system files of its own.

14. Build and quality standards
C++17, no exceptions across ABI boundaries, RAII throughout, no raw owning pointers.
CMake with presets; cross-compilation for all nine targets; static analysis (clang-tidy) and sanitizers (ASan, UBSan, TSan) in CI.
One version for the whole suite, from VERSION.
Generated documentation — the interface reference is generated from the .proto and YAML schemas so it cannot drift.
Consistent error handling; no silent failures anywhere. Every rejected input produces a diagnosable message.
Keep footprint in mind: these run on targets differing by more than an order of magnitude in memory. Resource profiles handle scale; behaviour stays identical.

15. Deliverables
Produce the complete repository:
All .proto definitions.
lib/common, lib/platform (with a simulated implementation), lib/ctrl.
All ten daemons, complete and functional.
All ten _ctrl tools with full operation coverage.
JSON Schemas for every YAML format.
Alarm catalog with the code generator, and the default fault rule set.
Device descriptors and resource profiles.
All system files — systemd, sysusers, tmpfiles, udev, defaults.
The complete test suite: unit, contract, integration, fault injection.
The Yocto recipe.
Generated interface documentation and a README covering build, deploy, and operate.
Build it all. No stubs, no placeholders, no deferred functionality.

