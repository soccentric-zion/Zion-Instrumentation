# Gap report — Arches Instrumentation vs REQ.md

**Status: all findings closed.** Build clean (no warnings), 310/310 tests
passing (was 278), clean under ASan and UBSan, and clean under TSan in our
own code — the residual TSan reports are all inside the uninstrumented
system libgrpc/libabsl and none point at a file in this repo.

Audit date: 2026-09-05. Baseline: clean build, 278/278 tests passing.
Method: requirement-by-requirement sweep of REQ.md §§1–15 against the tree,
plus a search for code that is implemented but has no production caller —
which is where every finding below came from.

## G1 — LogManager never collects anything (§7.7)

`LogStore::append()` is called from **tests only**. The daemon configures
sources, rotates, prunes, tails and bundles a store that nothing ever
writes to. `SourceSpec::kind` is parsed and reported but never dispatched
on, and `IPlatform::journal_lines()` — implemented in both the native and
simulated platforms — has no caller anywhere in `daemons/`.

§7.7 opens with "Collects from the journal and from application log sinks".
That is the daemon's primary job and it is absent.

**Fix:** a `LogCollector` (same shape as the existing `FaultFollower`) that
polls each configured source — `journal` via `journal_lines()`, `file`/`app`
by tailing `path` from a retained offset — and appends into the store,
honouring the per-source runtime level. Wired into `main.cpp` and
reconfigured on config delivery.

## G2 — Colocated applications have no interface (R5, §7.6, §7.8, §6.2)

REQ is emphatic that applications are the *only* entities that may raise or
clear an alarm (R5, §7.6), that Telemetry offers an "application metric
registration API" (§7.8), and that after provisioning "applications may then
override values as they see fit, with origin recorded" (§6.2).

No such interface exists. Alarm raise/clear and metric registration are
reachable only over the per-daemon `_ctrl` sockets, which §3.2 fixes at mode
**0600, root only** — an unprivileged colocated application cannot open one.
`ConfigurationApi` is the only app-shaped gRPC surface and it has no alarm or
metric RPC. So R5 is, today, unsatisfiable by any application on the box.

**Fix:** an `ApplicationApi` gRPC service on istate at
`/run/instrumentation/application.sock`, mode 0660, owned
`instr-istate:instr-apps` — architecturally identical to the Configuration
peer of §3.3, so R1/R2/R4 are preserved (apps reach spokes only by istate
relaying). RPCs: `GetSystemInfo`, `RaiseAlarm`, `ClearAlarm`,
`RegisterMetric`, `ReportMetric`, `GetConfig`, `SetAppConfig` (origin forced
to `app`). Plus an `instr-apps` group in sysusers.d and an application log
sink in the logmanager defaults.

## G3 — CAN devices cannot be driven (§7.4)

`DeviceManager::control()` branches on `gpio`, `led`, `usb` and `display`.
There is no `can` branch, and no `devices-*.yaml` gives a CAN device a
`controls:` key — so a CAN device can only be loopback-checked, never
driven. `IPlatform::can_send()` / `can_recv()` are implemented on both
platforms and unreachable from any daemon.

§7.4 names CAN in the managed device set and requires Hardware to drive
devices, "not read-only".

**Fix:** a `can` control branch (`can_send`, `can_recv`) and `controls:` on
the CAN devices in the descriptors.

## G4 — Crash forensics never triggers (§7.7)

`ForensicsCapture::capture()` works and is covered by a unit test, but its
only caller is the manual `crash-report` ctrl op. Nothing detects an
abnormal daemon exit, and `IPlatform::coredump_dir()` has no caller — so the
capture cannot find a coredump on its own.

§7.7 requires capture "on abnormal daemon exit". A forensic path that only
fires when an operator asks for it has missed the event it exists for.

**Fix:** istate's orchestrator already tracks spoke state; have an abnormal
transition relay a crash-report to logmanager, which resolves the coredump
from `coredump_dir()`.

## G5 — CtrlServer accumulates a thread handle per connection (defect)

`CtrlServer::serve_accept()` prunes finished connection threads with
`if (!it->joinable())`, which is never true: a `std::thread` stays joinable
after its function returns, right up until it is joined or detached. The
erase branch is dead, so `conn_threads_` grows by one entry — and one
unjoined OS thread — for every ctrl connection ever accepted, for the
lifetime of the daemon. On a device polled periodically by a `_ctrl` tool
this is an unbounded leak in all ten daemons.

**Fix:** track completion explicitly and join finished threads on accept.


---

# Closure

| # | Gap | Fix | Tests added |
|---|-----|-----|-------------|
| G1 | LogManager never collects | `LogCollector` polling journal + file/app sinks, wired into `main.cpp`; app sink in the shipped defaults | 10 (`LogCollect.*`) |
| G2 | Applications have no interface | `ApplicationApi` on istate at `application.sock` (0660, `instr-istate:instr-apps`); `instr-apps` group | 15 (`Application.*`) |
| G3 | CAN cannot be driven | `can` branch in `DeviceManager::control()`, `can_receive()`, `can-send`/`can-recv` ctrl ops, `controls:` in 6 descriptors | 6 (`HardwareCan.*`) |
| G4 | Crash forensics never triggers | istate relays `crash-report` on an abnormal spoke transition; logmanager resolves the coredump via `coredump_dir()` and folds in the stored tail | covered via the ctrl surface |
| G5 | CtrlServer thread accumulation | done-flag reaping in `serve_accept()` | 1 (`FinishedConnectionsAreReaped…`) |

## G6 — data race on `listen_fd_` (found while verifying G5 under TSan)

`CtrlServer::listen_fd_` and `BulkServer::listen_fd_` were plain `int`s
written by `stop()` while the accept thread read them. TSan flagged both.
Beyond the tool's objection this matters on the fleet: half these targets are
aarch64, where the accept loop can miss the close and spin on a stale
descriptor. Both are now `std::atomic<int>`.

## Verification

The G5 regression test was checked against the original code, not just the
fixed code: with the old `joinable()` prune restored it reports 60 threads
tracked after 60 closed connections and fails; with the fix it reports 0.
A test that passes either way would not have been worth adding.

## Note for whoever picks this up next — CLOSED

*(Was: `.clang-format` describes "Google style with a 100-column limit" while
setting `BasedOnStyle: Microsoft` / `ColumnLimit: 300`.)*

Resolved. Microsoft/300 was the deliberate setting — the tree is written in
Allman braces throughout — so the header comment was the stale half and has
been rewritten to describe what the file actually does, including why 300 is
there (it means "do not re-wrap", not a target width).

The claim that this was merely cosmetic turned out to be wrong. `make
check-format` did **not** pass: it failed on 26 files nobody had touched. The
cause was `AlignTrailingComments: false`, which does not mean "leave trailing
comments alone" — it means "do not align them", so clang-format collapsed the
tree's hand-aligned `///<` padding down to one space. The config's own comment
promised the opposite. Changing it to `Kind: Leave` turned the gate green with
zero code reformatting.
