# Arches Instrumentation — Interface Reference

Generated from proto/ and schemas/ — do not edit by hand.

## alarms.proto (package `instr.alarms`)

Messages: `AlarmEvent`, `ActiveAlarm`, `AlarmSpokeFrame`, `AlarmHubFrame`

### service `AlarmHub`

| RPC | request | response | streaming |
|-----|---------|----------|-----------|
| `Register` | `common.RegistrationRequest` | `common.RegistrationReply` | unary |
| `Channel` | `stream AlarmSpokeFrame` | `AlarmHubFrame` | client-stream |

## backup.proto (package `instr.backup`)

Messages: `BackupEvent`, `RestorePreviewEntry`, `BackupSpokeFrame`, `BackupHubFrame`

### service `BackupHub`

| RPC | request | response | streaming |
|-----|---------|----------|-----------|
| `Register` | `common.RegistrationRequest` | `common.RegistrationReply` | unary |
| `Channel` | `stream BackupSpokeFrame` | `BackupHubFrame` | client-stream |

## common.proto (package `instr.common`)

Messages: `TimeStamp`, `RegistrationRequest`, `RegistrationReply`, `Command`, `Ack`, `Backpressure`, `LivenessPing`, `LivenessPong`, `RelayedData`, `WgTunnel`, `EnrollRequest`, `EnrollReply`, `ConfigDelivery`, `ConfigRequest`, `ConfigAck`, `SignedConfig`

## faults.proto (package `instr.faults`)

Messages: `FaultEvent`, `FaultStats`, `FaultSpokeFrame`, `FaultHubFrame`

### service `FaultHub`

| RPC | request | response | streaming |
|-----|---------|----------|-----------|
| `Register` | `common.RegistrationRequest` | `common.RegistrationReply` | unary |
| `Channel` | `stream FaultSpokeFrame` | `FaultHubFrame` | client-stream |

## hardware.proto (package `instr.hardware`)

Messages: `HealthCheckResult`, `DeviceInfo`, `HardwareSpokeFrame`, `HardwareHubFrame`

### service `HardwareHub`

| RPC | request | response | streaming |
|-----|---------|----------|-----------|
| `Register` | `common.RegistrationRequest` | `common.RegistrationReply` | unary |
| `Channel` | `stream HardwareSpokeFrame` | `HardwareHubFrame` | client-stream |

## istate.proto (package `instr.istate`)

Messages: `SystemInfoRequest`, `SystemInfo`, `GetConfigRequest`, `ConfigSnapshot`, `SetConfigRequest`, `SetConfigReply`, `ListSpokesRequest`, `SpokeStatus`, `SpokeStatusList`, `SpokeControlRequest`, `ResetRequest`, `EventFilter`, `TimelineEvent`, `TailSpec`, `HealthScoreRequest`, `HealthScoreReply`, `EnrollmentRequest`, `EnrollmentStatus`, `RotateKeyRequest`, `RelayRequest`, `BudgetStatus`, `BudgetsRequest`, `BudgetsReply`, `RaiseAlarmRequest`, `ClearAlarmRequest`, `AlarmAck`, `RegisterMetricRequest`, `MetricSample`, `ReportMetricsRequest`, `ReportMetricsReply`, `SetAppConfigRequest`, `EventsFrame`, `FaultsFrame`, `DeviceFrame`, `HqFrame`

### service `ConfigurationApi`

| RPC | request | response | streaming |
|-----|---------|----------|-----------|
| `GetSystemInfo` | `SystemInfoRequest` | `SystemInfo` | unary |
| `GetConfig` | `GetConfigRequest` | `ConfigSnapshot` | unary |
| `SetConfig` | `SetConfigRequest` | `SetConfigReply` | unary |
| `ListSpokes` | `ListSpokesRequest` | `SpokeStatusList` | unary |
| `SpokeControl` | `SpokeControlRequest` | `common.Ack` | unary |
| `Reset` | `ResetRequest` | `common.Ack` | unary |
| `StreamEvents` | `EventFilter` | `TimelineEvent` | server-stream |
| `StreamLogs` | `TailSpec` | `logmanager.TailLine` | server-stream |
| `GetHealthScore` | `HealthScoreRequest` | `HealthScoreReply` | unary |
| `GetEnrollmentStatus` | `EnrollmentRequest` | `EnrollmentStatus` | unary |
| `RotateKey` | `RotateKeyRequest` | `common.Ack` | unary |
| `RelayCommand` | `RelayRequest` | `common.Ack` | unary |
| `Budgets` | `BudgetsRequest` | `BudgetsReply` | unary |

### service `ApplicationApi`

| RPC | request | response | streaming |
|-----|---------|----------|-----------|
| `GetSystemInfo` | `SystemInfoRequest` | `SystemInfo` | unary |
| `GetConfig` | `GetConfigRequest` | `ConfigSnapshot` | unary |
| `SetAppConfig` | `SetAppConfigRequest` | `SetConfigReply` | unary |
| `RaiseAlarm` | `RaiseAlarmRequest` | `AlarmAck` | unary |
| `ClearAlarm` | `ClearAlarmRequest` | `AlarmAck` | unary |
| `RegisterMetric` | `RegisterMetricRequest` | `common.Ack` | unary |
| `ReportMetrics` | `ReportMetricsRequest` | `ReportMetricsReply` | unary |

### service `HqPeer`

| RPC | request | response | streaming |
|-----|---------|----------|-----------|
| `Session` | `stream DeviceFrame` | `HqFrame` | client-stream |

## logmanager.proto (package `instr.logmanager`)

Messages: `TailLine`, `BundleHandle`, `CrashReport`, `LogSpokeFrame`, `LogHubFrame`

### service `LogHub`

| RPC | request | response | streaming |
|-----|---------|----------|-----------|
| `Register` | `common.RegistrationRequest` | `common.RegistrationReply` | unary |
| `Channel` | `stream LogSpokeFrame` | `LogHubFrame` | client-stream |

## monitord.proto (package `instr.monitord`)

Messages: `Sample`, `WearProjection`, `MonitorStatus`, `MonitorSpokeFrame`, `MonitorHubFrame`

### service `MonitorHub`

| RPC | request | response | streaming |
|-----|---------|----------|-----------|
| `Register` | `common.RegistrationRequest` | `common.RegistrationReply` | unary |
| `Channel` | `stream MonitorSpokeFrame` | `MonitorHubFrame` | client-stream |

## networkd.proto (package `instr.networkd`)

Messages: `InterfaceInfo`, `ConnectivityState`, `TunnelState`, `TimeState`, `RemoteAccessSession`, `NetworkEvent`, `NetworkSpokeFrame`, `NetworkHubFrame`

### service `NetworkHub`

| RPC | request | response | streaming |
|-----|---------|----------|-----------|
| `Register` | `common.RegistrationRequest` | `common.RegistrationReply` | unary |
| `Channel` | `stream NetworkSpokeFrame` | `NetworkHubFrame` | client-stream |

## ota.proto (package `instr.ota`)

Messages: `OtaEvent`, `OtaStatus`, `OtaSpokeFrame`, `OtaHubFrame`

### service `OtaHub`

| RPC | request | response | streaming |
|-----|---------|----------|-----------|
| `Register` | `common.RegistrationRequest` | `common.RegistrationReply` | unary |
| `Channel` | `stream OtaSpokeFrame` | `OtaHubFrame` | client-stream |

## telemetry.proto (package `instr.telemetry`)

Messages: `HealthScore`, `TelemetryRecord`, `TelemetryBatch`, `TelemetrySpokeFrame`, `TelemetryHubFrame`

### service `TelemetryHub`

| RPC | request | response | streaming |
|-----|---------|----------|-----------|
| `Register` | `common.RegistrationRequest` | `common.RegistrationReply` | unary |
| `Channel` | `stream TelemetrySpokeFrame` | `TelemetryHubFrame` | client-stream |

## YAML Schemas

- `alarm-catalog.schema.json` — Master alarm catalog (REQ §8)
- `alarms.schema.json` — alarms configuration (REQ \u00a77.6)
- `backup.schema.json` — backup configuration (REQ \u00a77.10)
- `capability.schema.json` — Capability manifest (REQ §5.1): honest reporting of what exists
- `devices.schema.json` — Board device descriptor (REQ §5.1)
- `fault-rules.schema.json` — Kernel-log anomaly signature rule set (REQ §7.5)
- `faults.schema.json` — faults configuration (REQ \u00a77.5)
- `hardware.schema.json` — hardware configuration (REQ \u00a77.4)
- `identity.schema.json` — Factory identity (REQ §6: /pst/factory/identity/identity.yaml)
- `istate-time-policy.schema.json` — Time policy (REQ §7.1: istate owns policy, networkd applies)
- `istate.schema.json` — istate configuration (REQ \u00a77.1)
- `logmanager.schema.json` — logmanager configuration (REQ \u00a77.7)
- `monitord.schema.json` — monitord configuration (REQ \u00a77.3)
- `networkd.schema.json` — networkd configuration (REQ \u00a77.2)
- `ota.schema.json` — ota configuration (REQ \u00a77.9)
- `resource-profile.schema.json` — Resource profile: sampling, retention, budgets by platform class (REQ §5.1)
- `telemetry.schema.json` — telemetry configuration (REQ \u00a77.8)

## Ctrl sockets

Each daemon: gRPC over `/run/instrumentation/<daemon>.sock` (0660, instr:<daemon>);
raw-UDS ctrl over `/run/instrumentation/<daemon>-ctrl.sock` (0600, root-only);
one command table drives both surfaces (REQ §3.2).
