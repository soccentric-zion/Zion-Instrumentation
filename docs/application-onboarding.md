# Onboarding an application

An *application* is any process on the device that is not one of the ten
instrumentation daemons — `arches-hal`, `arches-client`, and anything else that
runs alongside them. R5 says an alarm is declared by an application and by
nothing else, so an application that has not done the steps below cannot
participate in the alarm story at all.

Every one of these steps was missed for all three applications that already
exist. That is the reason this page exists: the steps are individually obvious
and collectively easy to forget, and nothing fails loudly when you skip them —
the socket connect just quietly returns unavailable.

## Checklist

**1. Join the `instr-apps` group.**

```ini
# your-app.service, [Service]
SupplementaryGroups=instr-apps
```

`/run/instrumentation/application.sock` is mode 0660 owned
`instr-istate:instr-apps`. `system/sysusers.d/instrumentation.conf` puts it
plainly: *"Membership is the whole access decision: no group, no app surface."*
Without this the connect fails and nothing else on this page matters.

**2. Add the log sink to `ReadWritePaths`, prefixed with `-`.**

```ini
ReadWritePaths=/var/lib/your-app -/var/log/instrumentation
```

The application log sink is `/var/log/instrumentation` (2770 setgid, group
`instr-apps` — the same group from step 1, so one membership covers both).

The `-` prefix is load-bearing. Under `ProtectSystem=full` or `strict`, systemd
refuses to start a unit whose `ReadWritePaths` entry does not exist, and that
path is absent on an image built without instrumentation. Prefixed, it is
skipped instead of bricking your application.

**3. Order after istate, but do not require it.**

```ini
# [Unit]
After=instr-istate.service
```

`After=` and **not** `Requires=`. An application must keep running when
instrumentation is down. That is the failure-behaviour contract Instrumentation
itself commits to, and inverting it means a bug in a spoke can take out the
product.

**4. Link a client.**

```cmake
find_package(instrumentation REQUIRED)
instrumentation_generate_client(your_app PROTOS istate.proto)
target_link_libraries(your_app PRIVATE instrumentation::client_deps)
```

Imports are resolved transitively, so naming `istate.proto` is enough.
`ApplicationApi` is the only surface you need: `GetSystemInfo`, `GetConfig`,
`SetAppConfig`, `RaiseAlarm`, `ClearAlarm`, `RegisterMetric`, `ReportMetrics`.

`docs/example-client/` is a complete, compilable version of everything below.

**5. Register your metrics at startup, with unit and type.**

```cpp
req.set_name("runs_total");
req.set_unit("runs");     // carried through to HQ
req.set_type("counter");  // gauge | counter
req.set_app(kOwner);
```

Unit and type are metadata HQ cannot reconstruct later. A metric registered
without them arrives as a bare number.

**6. Use one stable `owner`, everywhere.**

`owner` is required on every alarm call (R5). It is how the timeline attributes
a condition to the application that declared it. Pick one name per application,
put it in a constant, and never vary it — an alarm raised as `arches-client` and
cleared as `arches_client` does not clear.

**7. Handle `needs_snapshot`.**

A CRITICAL alarm can come back with `needs_snapshot` set, asking you to capture
bounded diagnostics. Ignoring it is not an error, but acting on it is most of
the value of having raised the alarm.

**8. Never make instrumentation a hard dependency.**

Connect with a deadline, treat failure as normal, and carry on. Repeating step 3
in code: an image without instrumentation is a supported configuration.

## What you get for it

- Alarms you declare appear in the local UI, in the health score, and — once
  the device↔HQ link is settled (`Task.md` C-01) — in HQ.
- Your metrics are batched and shipped by the telemetry spoke, with the units
  you registered.
- Your `app.log` is collected into log bundles alongside the daemons'.
- Your config writes land in `/pst` with origin `app`, which means an operator's
  `local` edit is never silently overwritten by your defaults.

## Alarm IDs

Do not invent an alarm id at the call site. The 32-bit layout is locked
(`[31:28]` domain, `[27:20]` module, `[19:16]` type, `[15:0]` index), the C++
header is generated from `catalog/alarms.yaml`, and HQ decodes against that same
catalog. Adding an alarm means adding a catalog entry.

Module ranges for the IV&V applications (HAL, Client, Server) are allocated in
the `application` domain — see `catalog/alarms.yaml`. Allocation is one-way:
the layout is locked, so an id that has shipped cannot be reused for something
else.
