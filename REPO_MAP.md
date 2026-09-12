# Arches-Instrumentation

**Remote:** `git@github.com:soccentric-arches/Arches-Instrumentation.git`
**Parent project:** [Arches](../Arches.md) (target hardware: **NVIDIA Jetson** (Tegra family, L4T/JetPack OS) application processor with an **STM32** real-time co-processor.)
**Leaf type:** `<Project>-Instrumentation` — Arches-only; no other project in the fleet has one.
**Status:** REAL — internally complete (10 daemons, 11 protos, 321 tests), integration in progress.

The **on-device management and observability suite**: everything the device needs to
describe, configure and repair itself, and the only component that talks to the cloud.

## Shape: one hub, nine spokes

`REQ.md` R1 — every spoke talks to istate and to nothing else. There is no
daemon-to-daemon communication of any kind. This is the load-bearing rule of the
codebase; REQ.md calls violating it "the single worst thing you can do here",
because it is what turns nine daemons into an N×N mesh across nine platforms.

| Daemon | Role |
|---|---|
| **istate** | The hub. Owns `/pst`, config origins, spoke registration, the HQ session, and the application surface. The only external face. |
| networkd | Interfaces, routes, DNS, firewall, wireless, the WireGuard tunnel |
| monitord | CPU/memory/storage/thermal/load sampling, health score, rolling windows |
| hardware | The only hardware writer (R7): GPIO, LEDs, CAN, serial, USB, display |
| faults | Kernel-level fault detection and correlation |
| alarms | X.733 alarm lifecycle: raise, clear, ack, escalate, auto-clear |
| logmanager | Log collection, the application sink, bundle creation |
| telemetry | Metric registration and sample batching |
| ota | A/B updates via SWUpdate, with pending/confirm/rollback and a canary |
| backup | Named archives, preview, restore |

## Layout

| Path | What |
|---|---|
| `daemons/<name>/` | One directory per daemon: `domain` (pure logic, unit-tested), `daemon`/`spoke` (wiring), `main.cpp`. |
| `lib/common/` | Errors, JSON, YAML config, logging and redaction, spool, tar, crypto, signatures, timestamps, the alarm codec. |
| `lib/platform/` | **All target variation lives here** (R9). `native.cpp` for a real board, `simulated.cpp` for tests and CI. No `#ifdef` for a board exists anywhere in daemon code. |
| `lib/transport/` | gRPC-over-UDS plumbing shared by hub and spokes. |
| `lib/ctrl/` | The `*_ctrl` command-table surface. |
| `proto/` | 11 files, packages `instr.*`. **Not** shared with HAL/Client/Server — this repo owns its own wire contract end to end. |
| `tools/<name>_ctrl/` | Ten operator CLIs, one per daemon, over the 0600 root-only ctrl sockets. |
| `catalog/` | `alarms.yaml` (the locked alarm identity catalog) and `fault-rules.yaml`, code-generated into a C++ header at build time. |
| `descriptors/` | Per-board `devices` / `capabilities` / `resource-profile` YAML. Data, never code. |
| `schemas/` | JSON Schema for every config format; validated at load. |
| `system/` | systemd units, sysusers.d, tmpfiles.d, udev rules, and the default YAML set that seeds `/pst`. |
| `yocto/` | The `instrumentation` Yocto layer (`conf/layer.conf` plus the recipe). |
| `test/` | unit, contract, integration, fault-injection, secret-leak. Plus `tsan.supp`. |
| `docker/` | The aarch64 build container — the **release** target, not the gate. |

## Contracts that are locked

* **Alarm identity** — 32 bits: `[31:28]` domain, `[27:20]` module, `[19:16]` type, `[15:0]` index.
  Severity is *not* in the code; it is reported separately, because an alarm's severity changes
  over its life. `catalog/alarms.yaml` is the source of truth and Arches-HQ decodes against it.
* **`/pst` origins** — every key carries `default` / `app` / `local` / `hq`, and an `app` write
  never overwrites an operator's `local` edit. Only istate writes `/pst`.
* **Socket access** — `*_ctrl` sockets are 0600 root-only. The application surface is
  `/run/instrumentation/application.sock`, 0660 `instr-istate:instr-apps`; group membership is
  the entire access decision.

## Build and validate

Host-native is the gate and the default developer workflow; the aarch64 container is the
release and CI target (it is qemu-emulated on an x86_64 host). See README "Build".

```bash
./.claude/validate.sh quick   # format, configure, compile — seconds
./.claude/validate.sh full    # + tests, coverage, ASan/UBSan, TSan, clang-tidy
```

`full` needs `gcovr`, `clang-tidy` and `setarch` on the host; without them those checks report
`77`, which is "never ran", not a pass. The sanitizers require `setarch -R` and
`TSAN_OPTIONS=suppressions=test/tsan.supp` — both are wired into the gate and the `Jenkinsfile`.

## Relationships

* **Arches-OS** — ships this repo as the `instrumentation` Yocto layer. The layer is real, but
  `kas/common.yaml` does not include it yet (`Task.md` D-08), so it is in no image today.
* **Arches-HAL / Client / Server** — colocated *applications*, not peers. They reach istate
  through `ApplicationApi` by joining the `instr-apps` group. No proto is shared.
* **Arches-HQ** — the intended northbound peer. Both ends are implemented and mutually
  unintelligible: istate dials `instr.istate.HqPeer`, HQ serves `archeshq.agent.AgentLink`.
  Resolving that is `Task.md` C-01.
* **Arches-Configuration** — **overlaps this repo one-to-one** (network, firewall, tunnel,
  update, logs, monitoring, alerts, backup, device I/O) and touches the same kernel objects.
  Which one owns device system state is an open human decision, `Task.md` B-01. Do not settle
  it in a commit.

## Related

* [`.claude/CLAUDE.md`](.claude/CLAUDE.md) — how to work here, and the Don'ts.
* `REQ.md` — the specification; R-numbers are cited throughout the code.
* `README.md` — architecture, startup order, and the build story.
* `missing.md` — what was deliberately left undone, and why.
