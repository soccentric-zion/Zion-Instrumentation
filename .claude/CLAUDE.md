# Arches-Instrumentation

On-device management and observability for the Jetson: the **istate** hub plus nine spokes
(networkd, monitord, hardware, faults, alarms, logmanager, telemetry, ota, backup). Ships into
Arches-OS as its own Yocto layer (`yocto/`, layer name `instrumentation`). `REQ.md` is the
specification and its R-numbers are cited throughout this file — read it before changing
architecture.

**This repo overlaps `Arches-Configuration` one-to-one** — network, firewall, tunnel, update,
logs, monitoring, alerts, backup and device I/O exist in both, and where they touch the kernel
they touch the same objects. Which one owns device system state is an open human decision
(`Task.md` B-01). Do not resolve it in a commit.

## How you work here

* Commands: `.claude/commands/` — validate.
* Build: **host-native is the default and what the gate runs.** The aarch64 container in
  `docker/` is the release and CI target; on an x86_64 host it is qemu-emulated and slow. See
  README "Build". Both must pass before a release; only host-native gates an edit.
* Test: 321 tests — unit, contract, integration, fault-injection, secret-leak. No hardware
  needed; the simulated platform covers the board.
* Write: all target variation lives in `lib/platform` and in `descriptors/` (R9). There is no
  `#ifdef` for a board anywhere in daemon code, and adding one is a design failure, not a
  shortcut.

## How you validate

* Gate: `./.claude/validate.sh quick` (seconds) or `full` (adds tests, coverage, ASan/UBSan,
  TSan, clang-tidy). Run it before you claim done.
* Exit codes: `0` pass, `1`-`76` fail, `77` a check never ran. **77 is not a pass.**
* `full` needs `gcovr`, `clang-tidy` and `setarch` on the host. Without them those checks report
  `77`, which is honest, not green. `loop/tools/preflight.sh` catches this in seconds.
* The gate and everything under `.claude/lib`, `hooks`, `commands`, `settings.json` is
  human-owned. Run it, never edit it. Regenerate with `tools/validate/generate.sh`.

## Do

* Keep every spoke's only conversation with istate. New cross-daemon needs go **through** the
  hub, adding a hub operation rather than a socket between two spokes.
* Put persistent state under `/pst` and write it only through istate, so every key keeps its
  origin (`default`/`app`/`local`/`hq`).
* Scope every nftables change to this repo's own table. `render_firewall_ruleset` /
  `render_nat_ruleset` in `lib/platform/src/native.cpp` show the declare-delete-rebuild idiom.
* Give a sink or a shared accessor its own lock. `MemorySink` and `Sampler` own theirs because
  the callers cannot be relied on to hold the right one.
* Reach the boot partition through `platform::boot_dir()`, never a literal path.
* Run the sanitizers with `setarch $(uname -m) -R` and `TSAN_OPTIONS=suppressions=test/tsan.supp`.
  Both are required, and `.claude/validate.sh` and the `Jenkinsfile` already do it.

## Don't

* **Don't let a spoke talk to another spoke, or to anything outside the box.** R1: istate is the
  hub and the only external face. `REQ.md` calls violating this "the single worst thing you can
  do to this codebase", because it is what turns nine daemons into an N×N mesh across nine
  platforms.
* **Don't write `/pst` from anywhere but istate.** Two writers means the origin-precedence rule
  (an `app` write never overwrites an operator's `local` edit) is unenforceable, which is the
  same failure B-01 exists to resolve at the fleet level.
* **Don't put an application-facing surface on a `_ctrl` socket.** Ctrl sockets are 0600
  root-only (§3.2). The application surface is `/run/instrumentation/application.sock`, 0660
  `instr-istate:instr-apps` — group membership is the entire access decision.
* **Don't synthesise an alarm from observation.** R5: only an application declares or clears one,
  and that includes the alarms daemon itself.
* **Don't touch hardware outside the Hardware daemon.** R7, no exceptions.
* **Don't regenerate or fork `catalog/alarms.yaml`'s ID layout.** The 32-bit layout
  (`[31:28]` domain, `[27:20]` module, `[19:16]` type, `[15:0]` index) is locked, the C++ header
  is generated from it, and Arches-HQ decodes against it. Severity is **not** in the code — it is
  its own field, because an alarm's severity changes over its life. HQ got this wrong once and
  decoded four equal bytes for months (`Task.md` C-03).
* **Don't add a global `flush ruleset`.** It drops every table in the kernel including
  Arches-Configuration's `webconfig` table. On a device whose firewall protects the management
  port that is a security failure, not a cosmetic one.
* **Don't "fix" `.clang-format`'s `AlignTrailingComments: Leave` to `false`.** They are not
  synonyms: `false` means "do not align", which makes clang-format collapse this tree's
  hand-aligned `///<` padding and turns the format gate red across 26 files. The 300-column
  limit is likewise deliberate — it means "do not re-wrap", not "write 300-column lines".
* **Don't assume `INSTR_BOARD:<x>` overrides can name a descriptor set.** They key on MACHINE,
  and Arches-OS's machines are `jetson-orin-nano-devkit`, `p3768-0000-p3767-0000` and so on. An
  unmapped MACHINE now stops the build rather than silently shipping x86-64 descriptors.
* **Don't add a broad pattern to `test/tsan.supp`** to make a failure go away. Every entry there
  names a third-party library or namespace; a race in `instr::` code is a finding. Two real ones
  were found that way.
