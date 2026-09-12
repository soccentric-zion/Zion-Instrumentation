# Arches Instrumentation

Complete C++17 on-device software management and observability suite for a
fleet of embedded Linux devices across nine hardware targets (NVIDIA Jetson,
AMD Xilinx Zynq, NXP i.MX, TI Sitara, Intel/AMD x86, Raspberry Pi and three
further targets), plus its Yocto integration and test suite.

**Ten daemons, one external face.** `istate` is the hub: system state,
provisioning, persistent configuration, orchestration, and the only process
with an external interface (local gRPC-over-UDS for Configuration, gRPC over
a WireGuard tunnel to HQ). The nine spokes — `networkd`, `monitord`,
`hardware`, `faults`, `alarms`, `logmanager`, `telemetry`, `ota`, `backup` —
talk to istate and to nothing else (R1). `Configuration` (the local web UI)
is architecturally identical to a spoke: a gRPC-over-UDS client of istate.

```
instrumentation/
  proto/        one .proto per daemon + common.proto (R3: one wire format)
  lib/
    common/     config+origins, JSON+schema validation, logging, timestamps,
                rate limiter, disk spool, atomic writes, crypto (SHA-256,
                HMAC, X25519, ChaCha20, HKDF), command tables, tar, ctrl
                framing, daemon skeleton
    platform/   abstraction layer + complete simulated implementation +
                native sysfs/procfs implementation + declarative descriptors
    ctrl/       raw-UDS control server/client + shared _ctrl tool main
    transport/  gRPC-over-UDS plumbing shared by all ten daemons
  daemons/      istate + nine spokes (domain logic separate from transport)
  tools/        one _ctrl per daemon with full operation coverage
  schemas/      JSON Schema for every YAML format (validated at load)
  catalog/      master alarm catalog + code generator; default fault rules
  descriptors/  per-board device descriptors, resource profiles, capabilities
  system/       systemd units, sysusers.d, tmpfiles.d, udev, default YAML set
  test/         unit, contract, integration harness, fault injection,
                secret-leak
  yocto/        bitbake recipe (installs; carries no system files of its own)
  docker/       arm64 (Jetson/aarch64) build container + its Makefile
  docs/         generated interface reference (docgen.py, cannot drift)
```

## Topology

Everything below the `device` line runs on-box; HQ lives off-device behind
the tunnel. istate is the hub and the only process with an external face
(R2); the nine spokes, Configuration and colocated applications are all
gRPC-over-UDS clients of istate and of nothing else (R1).

```
                                      ┌─────────────────────────────────────────────────────────────────────┐
                                      │                        HQ — cloud fleet plane                       │
                                      │fleet management · configuration · monitoring                        │
                                      │verifies enrollment vs manufacturing record                          │
                                      │returns tunnel configuration (WireGuard)                             │
                                      │signs config pushes · uploads OTA artifacts                          │
                                      │ingests telemetry · faults · alarms · log bundle refs                │
                                      │staged canary rollout · auto-halt on regression                      │
                                      │backpressure to devices · config-drift compare                       │
                                      │devices report effective config vs intent                            │
                                      └──────────────────────────────────┬──────────────────────────────────┘
        gRPC · HqPeer.Session — ONE bidirectional stream over the        │ ▲ north · DeviceFrame:
        WireGuard tunnel · networkd brings the tunnel up, istate         │ │  telemetry batches · timeline events
        owns the gRPC session · device identity = the WireGuard          │ │  fault events · log bundle refs
        keypair, generated on device at first boot · the private         │ │  canary state · liveness pong
        key never leaves the device                                      │ ▼ south · HqFrame:
                                                                         │ │  signed config pushes · OTA commands
                                                                         │ │  relayed commands · tunnel config
                                                                         │ │  backpressure
 ════════ device ════════════════════════════════════════════════════════════════════════════════════════════════════
                                                                         │
┌───────────────────────────────┐   ┌────────────────────────────────────┴────────────────────────────────────┐
│     colocated applications    │   │       istate · the hub — R1 hub-and-spoke · R2 sole external face       │
│ApplicationApi (gRPC/UDS) on   │   │peers: nine spokes + Configuration + colocated applications (R1)         │
│/run/.../application.sock      │──►│gRPC-over-UDS server — one socket per peer:                              │
│0660 · group instr-apps        │   │  /run/instrumentation/<daemon>.sock · 0660 · instr:<daemon>             │
│RaiseAlarm / ClearAlarm —      │   │  handshake: name · build ver · proto schema ver · capability list       │
│R5: only apps declare alarms;  │   │  schema mismatch → refuse the registration + raise an alarm             │
│CRITICAL → needs_snapshot      │   │  streams: samples · fault events · telemetry batches ·                  │
│Register / ReportMetrics (§7.8)│   │           OTA progress · log tail → fan-out to HQ AND Configuration     │
│GetConfig / SetAppConfig —     │   │gRPC client → HQ: HqPeer.Session over the WG tunnel (R3: one             │
│lands origin=app, never        │   │  wire format — gRPC/UDS on-box · gRPC/WireGuard to HQ)                  │
│overwrites operator 'local'    │   │northbound spool: disk-backed · bounded · drains on reconnect ·          │
│GetSystemInfo                  │   │  jittered backoff · honours HQ backpressure · drop policy:              │
│logs → app.log → logmanager    │   │  oldest telemetry first — NEVER alarms or faults                        │
└───────────────────────────────┘   │config service: per-key origin — default / app / local / hq              │
                                    │  HQ pushes signed + verified on device · resets consult origin          │
┌───────────────────────────────┐   │  → operator 'local' edits survive · config change needs NO OTA          │
│  Configuration — local web UI │   │orchestrator: liveness-checks spokes, restarts hung · spokes             │
│Rust · on-box · stateless      │   │  survive istate crash and re-attach · timeline/event fan-out ·          │
│NO device state of its own:    │   │  budgets · audit trail                                                  │
│every read/write is an istate  │──►│time: CLOCK_MONOTONIC + boot id on every record · re-stamp on            │
│call — never touches systemd,  │   │  time-source lock — history never reorders                              │
│network stack, update system   │   │startup: faults/monitord/hw → alarms/logmgr/telemetry →                  │
│or /pst directly (§3.3)        │   │  networkd → ota/backup → tunnel + HQ session LAST (R10)                 │
│pairing + enrollment live in   │   │/pst: SOLE writer (R4) · atomic temp→fsync→rename · redundant            │
│istate; Config renders them    │   │  copy · both corrupt → boot unprovisioned + report                      │
├───────────────────────────────┤   └─────┬───────────────────────────────────────────────────────────────────┘
│ConfigurationApi (gRPC/UDS):   │         │
│GetSystemInfo · GetConfig ·    │ ┐───────┘
│SetConfig (origin-tracked)     │ ├─────────────────────┬────────────────────────────────────────┬
│ListSpokes · SpokeControl      │ │                     │                                        │
│Reset · StreamEvents/Logs      │ │   ┌─────────────────┴──────────────────┐   ┌─────────────────┴──────────────────┐
│GetHealthScore · Budgets       │ │   │           networkd — R8            │   │              monitord              │
│GetEnrollmentStatus ·          │ │   │owns ALL network-adjacent (R8):     │   │periodic resource sampling:         │
│RotateKey · RelayCommand       │ │   │interfaces, wireless, routing, DNS, │   │cpu, memory, disk, thermal, power   │
└───────────────────────────────┘ │   │firewall, SSH, port forwarding      │   │sources                             │
                                  │   │WireGuard tunnel bring-up; killing  │   │storage wear: eMMC lifetime, SD     │
┌───────────────────────────────┐ │   │changes auto-revert if HQ is not    │   │health, SMART                       │
│  _ctrl tools — one per daemon │ │   │reachable within the timeout        │   │samples stream to istate at a       │
│istate_ctrl · alarm_ctrl ·     │ │   │time daemons: chrony, ptp4l,        │   │configured interval → batches       │
│ota_ctrl · backup_ctrl ·       │ │   │phc2sys, gpsd — istate sets policy  │   │northbound via istate               │
│network_ctrl · monitor_ctrl ·  │ │   └────────────────────────────────────┘   └────────────────────────────────────┘
│hardware_ctrl · fault_ctrl ·   │ │
│log_ctrl · telemetry_ctrl      │ ├─────────────────────┬────────────────────────────────────────┬
│raw-UDS (not gRPC) · 0600,     │ │                     │                                        │
│root-only, runtime-only —      │ │   ┌─────────────────┴──────────────────┐   ┌─────────────────┴──────────────────┐
│NEVER persists to /pst (§3.2)  │ │   │           hardware — R7            │   │            faults — R6             │
│one shared command table:      │ │   │ONLY hardware writer (R7): full     │   │kernel log observer, READ-ONLY:     │
│gRPC + ctrl both dispatch →    │ │   │control of board devices            │   │never configures hardware, never    │
│full coverage · --help ·       │ │   │GPIO, LED, USB, CAN, display        │   │raises alarms (R6)                  │
│--json · completion            │ │   │hardware watchdog · boot slot       │   │fault IDs share the 32-bit alarm    │
└───────────────────────────────┘ │   │control, rollback, golden boot      │   │scheme → HQ correlates kernel       │
                                  │   │thermal zones · power sources       │   │fault ↔ app-raised alarm            │
┌───────────────────────────────┐ │   │driven by declarative per-board     │   │rate-limited: token bucket,         │
│  notes — the non-negotiables  │ │   │descriptors — no #ifdefs (R9)       │   │'suppressed N events' summary       │
│R1 hub-and-spoke: nine spokes  │ │   └────────────────────────────────────┘   └────────────────────────────────────┘
│talk to istate, to nothing else│ │
│R2 istate = only external face;│ ├─────────────────────┬────────────────────────────────────────┬
│no other daemon knows HQ exists│ │                     │                                        │
│R3 gRPC everywhere: UDS on-box,│ │   ┌─────────────────┴──────────────────┐   ┌─────────────────┴──────────────────┐
│WireGuard to HQ · one IDL      │ │   │            alarms — R5             │   │             logmanager             │
│R4 istate alone writes /pst    │ │   │alarm state machine · declared by   │   │collects journal + app.log          │
│R5 alarms declared, never      │ │   │apps ONLY, never inferred (R5)      │   │(instr-apps group, setgid)          │
│inferred · R6 faults read-only │ │   │X.733 fields: probable cause,       │   │rotation · retention · level        │
│R7 hardware only writer · R8   │ │   │specific problem, correlated        │   │control · storage budgets           │
│networkd owns network-adjacent │ │   │notifications — HQ groups across    │   │bundles: tar on demand, fetched     │
│R9 no #ifdefs outside platform │ │   │the fleet                           │   │via handle                          │
│R10 degrade, don't block:      │ │   │escalation = config (HQ or local)   │   │live tail: filtered, rate-limited,  │
│tunnel, HQ, spoke or clock loss│ │   │· auto-clear policy                 │   │auto-stop on timeout                │
│never stops the device         │ │   └────────────────────────────────────┘   └────────────────────────────────────┘
└───────────────────────────────┘ │
                                  ├─────────────────────┬────────────────────────────────────────┬
                                  │                     │                                        │
                                  │   ┌─────────────────┴──────────────────┐   ┌─────────────────┴──────────────────┐
                                  │   │             telemetry              │   │                ota                 │
                                  │   │metric registry: gauge/counter,     │   │apply-artifact · confirm ·          │
                                  │   │app-registered (§7.8)               │   │rollback — unconfirmed update       │
                                  │   │health score                        │   │auto-rolls back                     │
                                  │   │collects per YAML config at a       │   │artifact streamed down via          │
                                  │   │configured interval → batches       │   │istate · lands on disk before       │
                                  │   │→ istate → spool → HQ + Config      │   │apply                               │
                                  │   │spool-full: drop oldest telemetry   │   │canary: fault rate, health,         │
                                  │   │first, report the drop              │   │version → HQ staged rollout         │
                                  │   └────────────────────────────────────┘   └────────────────────────────────────┘
                                  │
                                  ├─────────────────────┬────────────────────────────────────────┬
                                                        │                                        │
                                      ┌─────────────────┴──────────────────┐   ┌─────────────────┴──────────────────┐
                                      │               backup               │   │      /pst — persistent store       │
                                      │tar snapshot of /pst; restore =     │   │istate is the SOLE writer (R4);     │
                                      │untar under istate direction (R4)   │   │all others via istate's API         │
                                      │snapshot → HQ + Config via istate   │   │atomic: temp → fsync → rename,      │
                                      │excludes factory/, WG keys —        │   │checksum · redundant copy           │
                                      │device identity is NOT portable     │   │both copies corrupt → boot          │
                                      │encrypted: local key derived from   │   │unprovisioned + report alarm        │
                                      │device identity; to HQ encrypted    │   │config lives here → HQ can          │
                                      │to HQ's public key                  │   │reconfigure any daemon w/o OTA      │
                                      └────────────────────────────────────┘   └────────────────────────────────────┘


        nine spokes — gRPC-over-UDS to istate and to nothing else (R1) · no daemon-to-daemon links
        startup: istate → faults, monitord, hardware → alarms, logmanager, telemetry → networkd →
        ota, backup → tunnel + HQ session LAST — never a precondition for local operation (R10)
```

istate is the server on every spoke socket
(`/run/instrumentation/<daemon>.sock`, 0660, owned `instr:<daemon>`). A
spoke dials in on start and registers with its name, build version, proto
schema version and capability list; on a schema mismatch istate refuses the
registration and raises an alarm — it never silently degrades a mismatched
peer. Streaming where the data is a stream (monitord samples, fault events,
telemetry batches, OTA progress, log tail), unary otherwise.

| Spoke | Owns |
|---|---|
| `networkd` | interfaces, wireless, routing, DNS, firewall, SSH, the WireGuard tunnel, time daemons (R8) |
| `monitord` | resource sampling, storage wear |
| `hardware` | the only hardware writer — GPIO, LED, USB, CAN, display (R7) |
| `faults` | kernel log observation, read-only (R6) |
| `alarms` | alarm state, escalation, X.733 fields; declared by applications, never inferred (R5) |
| `logmanager` | journal and `app.log` collection, bundles, live tail |
| `telemetry` | metrics, health score, northbound batches |
| `ota` | update apply / confirm / rollback, canary |
| `backup` | encrypted snapshot/restore of /pst, device identity excluded |

### istate ↔ Configuration

Configuration — the local Rust web UI running on the same box — is a
gRPC-over-UDS client of istate, architecturally identical to a spoke (§3.3):

- It holds no device state. Every read and write is an istate call; it never
  touches systemd, the network stack, the update system, or /pst directly.
- Pairing and enrollment live in istate; Configuration renders the flow.
- Its surface is `ConfigurationApi` (`proto/istate.proto`): config read/write
  with origin tracking, spoke status and control, reset, event and log
  streaming, health score, enrollment status, key rotation, relayed commands
  and storage budgets.

### istate ↔ HQ

gRPC over the WireGuard tunnel: networkd brings the tunnel up, istate owns
the gRPC session over it (§3.4) — one bidirectional stream, `HqPeer.Session`:

- **Identity.** The WireGuard keypair is generated on the device at first
  boot; the public key is signed with the factory identity and enrolled with
  HQ. The private key never leaves the device.
- **Northbound** (`DeviceFrame`): telemetry batches, timeline events, fault
  events, log bundle references, canary state (fault rate, health score,
  version) and liveness pongs.
- **Southbound** (`HqFrame`): signed config pushes, OTA commands
  (apply-artifact / confirm / rollback), generic relayed commands, tunnel
  configuration and backpressure signals.
- **Spool.** Telemetry, faults and log references are spooled to disk first
  and drained when the tunnel comes up — nothing northbound depends on the
  tunnel being available at the moment it is produced. Spool full drops
  oldest telemetry first, never alarms or faults.
- **Flood control.** Jittered exponential backoff on reconnect and on drain,
  honouring HQ backpressure, so a site of hundreds of devices powering up
  together does not stampede HQ.
- **Config from HQ.** Pushes are signed and verified on device, then land in
  /pst with origin `hq`; origin decides what a push or reset may overwrite,
  so an operator's `local` edits survive (R4, §6). No OTA is needed to
  reconfigure any daemon.
- **Degrade, don't block (R10).** Tunnel or HQ loss leaves the device fully
  operational; a firewall change that kills the tunnel auto-reverts if HQ is
  unreachable within the configured timeout.

**Startup order:** istate → Faults, monitord, Hardware → Alarms, LogManager,
Telemetry → networkd → OTA, Backup → tunnel and HQ session. The HQ session
is last and is never a precondition for local operation (R10); observation
daemons start early so boot-window anomalies are captured.

## Build

There are two build targets and they have different jobs. Confusing them is
what made this section contradict itself for a while, so the split is stated
first:

| | Host-native (x86_64) | Container (aarch64) |
|---|---|---|
| Used for | **the gate** — every edit, every loop | release artifacts and CI |
| Speed | seconds | slow: qemu-emulated on an x86_64 host |
| Needs | a C++17 toolchain, CMake, protoc/grpc | Docker + binfmt |
| Proves | the code is correct | the code is correct *for the device* |

**Host-native is the default developer workflow and what `.claude/validate.sh`
runs.** It is fast enough to run on every change, which is the only property
that matters for a gate.

```bash
cmake --preset host-debug
cmake --build --preset host-debug -j
ctest --preset host-debug --output-on-failure
```

Presets: `host-debug`, `host-release`, `ninja-host-debug`,
`ninja-host-release`, `asan`, `ubsan`, `tsan`, `coverage`,
`jenkins` (CI), plus cross toolchains per target under `cmake/toolchains/`
(`cross-jetson-orin-nano`, `cross-zynq-ultrascale`, …).

**The container build is aarch64** — the Jetson fleet — in `docker/`
(Ubuntu 22.04 arm64, the JetPack 6 userland generation, so the binaries match
the device glibc; on an x86_64 host it runs under qemu via binfmt, installed
automatically). Use it to produce what ships, and before a release:

```bash
make               # aarch64 build for the fleet -> build-aarch64/
make test          # the test suite, inside the arm64 container
make check-format  # clang-format gate, inside the arm64 container
make tidy          # clang-tidy, inside the arm64 container (slow under qemu)
make shell         # interactive shell in the arm64 container
```

Both must pass before a release. Only the host-native one has to pass before
every commit.

Formatting is enforced in CI (`.clang-format`, Microsoft-based, effectively
unwrapped — see the rationale in that file):

```bash
make check-format                                        # verify (CI gate)
make format                                              # rewrite in place
cmake --build --preset host-debug --target check-format  # manual equivalent
```

## Deploy

The repo ships everything the image needs; the Yocto recipe only builds and
installs:

```bash
# in your Yocto layer's bblayers.conf:
BBLAYERS += "/path/to/instrumentation/yocto"
# in your image recipe:
IMAGE_INSTALL:append = " instrumentation"
```

Units, sysusers, tmpfiles, udev rules and the default YAML set are installed
from the repo (`system/`); the device boots into `instr-istate.service`,
which orchestrates the spokes (observation daemons first, tunnel + HQ
session last, never a precondition for local operation).

## Operate

Every daemon exposes a raw-UDS control socket `/run/instrumentation/<daemon>-ctrl.sock`
(0600, root-only) driven by one command table shared with its gRPC surface —
full coverage, help and shell completion for free:

```bash
istate_ctrl status | spokes | config-get --daemon monitord | timeline | budgets | audit
alarm_ctrl list | raise id=0x1_04_1_0007 owner=app
ota_ctrl status | apply-artifact | confirm | rollback | canary
backup_ctrl create | preview --name <archive> | restore --name <archive>
network_ctrl status | ssh-open operator=alice ttl_s=600 | tunnel-status
monitor_ctrl status | sample-once | wear
hardware_ctrl list-devices | check-all | gpio-set device=gpio-1 value=true
fault_ctrl rules-list | test-line line="Out of memory: Killed process 4321 (myapp)"
log_ctrl tail | bundle-create | bundle-fetch --handle <handle>
telemetry_ctrl health-score | flush | register-metric name=app.metric unit=ms type=gauge app=myapp
```

All `_ctrl` changes are runtime-only and never persist to `/pst`; only
istate writes persistent configuration, every key carrying its origin
(default / app / local / hq).

## Colocated applications

The `_ctrl` sockets above are 0600 root-only (§3.2), so they are not the
surface an application uses. Applications get their own face on istate —
architecturally identical to the Configuration peer of §3.3 — at
`/run/instrumentation/application.sock`, mode 0660, owned
`instr-istate:instr-apps`. Add your application's user to the `instr-apps`
group and it can call `ApplicationApi` (see `proto/istate.proto`):

| RPC | Why |
|---|---|
| `RaiseAlarm` / `ClearAlarm` | R5: an alarm is declared by an application and by nothing else. `owner` is required and is recorded as the alarm's owner. A CRITICAL alarm comes back with `needs_snapshot` set (§7.6). |
| `RegisterMetric` / `ReportMetrics` | §7.8 application metric registration and reporting. |
| `GetConfig` / `SetAppConfig` | §6.2: applications override values as they see fit, with origin recorded. Writes always land with origin `app` and never overwrite an operator's `local` edit. |
| `GetSystemInfo` | serial, hardware revision, firmware and schema versions for the app's own records. |

R1/R2/R4 are unaffected: an application talks to istate and to nothing else,
istate relays to the alarms and telemetry spokes, and every `/pst` write is
still istate's alone.

For logs, append to `/var/log/instrumentation/app.log` (group `instr-apps`,
setgid). LogManager collects it alongside the journal, subject to the same
rotation, retention, level control and storage budget as every other source.

## Failure behaviour (contract, §11)

Tunnel down → spool northbound, device fully operational, drain on
reconnect. Spool full → drop oldest telemetry first, never alarms/faults,
report the drop. Spoke hang → istate liveness check detects and restarts.
istate crash → spokes keep running, re-attach. /pst corrupt → redundant copy;
if both fail, boot unprovisioned and report. Unconfirmed update → automatic
rollback. Clock unavailable → monotonic operation, re-stamp on lock.
Firewall change kills tunnel → auto-revert within the timeout. Every one of
these is implemented and covered by the fault-injection suite.

## Tests

```bash
ctest --preset host-debug                 # unit + contract + integration + fault injection
./build-host-debug/test/integration_harness   # full topology on a build host
```

The integration harness runs the hub, all nine spokes and the simulated
platform — the complete topology on a laptop, no hardware. Coverage gate,
sanitizers and the secret-leak test are enforced in CI (Jenkinsfile).

`integration_application` covers the colocated-application path end to end:
a real gRPC client on a real socket against a live hub with the alarms and
telemetry spokes attached.