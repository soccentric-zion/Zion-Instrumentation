// ============================================================================
//  Arches Instrumentation
//
//  Copyright (C) 2026 Soccentric. All rights reserved.
//  Author: Sandesh Ghimire (sandesh@soccentric)
//
//  This file is part of a proprietary product of Soccentric.
//
//  NO WARRANTY:
//  This software is provided "AS IS", without warranty of any kind, either
//  express or implied, including but not limited to the warranties of
//  merchantability, fitness for a particular purpose, and non-infringement.
//  In no event shall Soccentric be liable for any claim, damages, or other
//  liability arising from the use of this software.
//
//  This software may not be used, copied, modified, distributed, or
//  disclosed, in whole or in part, without the express prior written
//  consent of Soccentric.
// ============================================================================
/**
 * @file daemons/istate/include/instr/istate/core.h
 * @brief the hub core (REQ §7.1). Owns system state, provisioning,
 *
 * persistent configuration, orchestration, and every external interface
 * (R2: the only process with an external interface).
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <memory>
#include <mutex>

#include "instr/common/log.h"
#include "instr/istate/config_service.h"
#include "instr/istate/northbound.h"
#include "instr/istate/orchestrator.h"
#include <future>

#include "instr/transport/transport.h"
#include "instr/istate/provisioning.h"
#include "instr/istate/pst.h"
#include "instr/platform/platform.h"

namespace instr
{
/// @brief istate: /pst owner, spoke orchestrator and HQ northbound bridge (R4, §7.1).
namespace istate
{

/// @brief istate runtime options.
struct IstateOptions
{
    std::string pst_root;         ///< /pst (or test root).
    std::string run_dir;          ///< /run/instrumentation.
    std::string build_version;    ///< Build version.
    double liveness_hang_timeout_s = 30;       ///< Liveness hang threshold.
    uint64_t spool_max_bytes = 64ull * 1024 * 1024;   ///< Northbound spool budget.
    double budget_spool = 64ull * 1024 * 1024;    ///< Spool consumer budget.
    double budget_logs = 128ull * 1024 * 1024;    ///< Logs consumer budget.
    double budget_ota = 512ull * 1024 * 1024;     ///< OTA staging budget.
    double budget_backup = 256ull * 1024 * 1024;  ///< Backup budget.
};

/// @brief istate core: owns /pst, orchestration, northbound transport and policy.
class IstateCore
{
  public:
    /// @brief Construct the core over storage, platform, clock, logger and options.
    /// @param opts Runtime options.
    /// @param fs Storage backend.
    /// @param plat Platform.
    /// @param ts Clock.
    /// @param log Logger.
    IstateCore(IstateOptions opts, IFileSystem& fs, platform::IPlatform& plat, TimeSource& ts, Logger& log);

    /// @brief Mount /pst, validate, seed defaults on virgin; provisioning; start the spokes in order.
    ///
    /// Returns false only when the platform is unusable.
    /// @param default_yaml Image default YAML per daemon.
    /// @return True when the platform is usable; false only when unusable.
    bool boot(const std::map<std::string, std::string>& default_yaml);
    /// @brief Graceful shutdown: stop spokes, persist state, write the clean marker.
    void shutdown();

    /// @brief Subsystems (wired into the hub server).
    /// @{
    /// @return The /pst store.
    PstStore& pst()
    {
        return pst_;
    }
    /// @brief Budget manager access.
    /// @return The budget manager.
    BudgetManager& budgets()
    {
        return budgets_;
    }
    /// @brief Provisioner access.
    /// @return The provisioner.
    Provisioner& provisioner()
    {
        return provisioner_;
    }
    /// @brief Orchestrator access.
    /// @return The orchestrator.
    Orchestrator& orchestrator()
    {
        return orchestrator_;
    }
    /// @brief Event timeline access.
    /// @return The event timeline.
    EventTimeline& timeline()
    {
        return timeline_;
    }
    /// @brief Live-tail fanout access.
    /// @return The tail fanout.
    TailFanout& tail_fanout()
    {
        return tail_fanout_;
    }
    /// @brief Northbound spool access.
    /// @return The northbound spool.
    NorthboundSpool& northbound_spool()
    {
        return northbound_spool_;
    }
    /// @brief HQ session state machine access.
    /// @return The HQ session.
    HqSession& hq_session()
    {
        return hq_session_;
    }
    /// @brief Config service access.
    /// @return The config service.
    ConfigService& config_service()
    {
        return config_service_;
    }
    /// @brief Audit log access.
    /// @return The audit log.
    AuditLog& audit()
    {
        return audit_;
    }
    /// @brief Logger access.
    /// @return Logger access.
    Logger& log()
    {
        return log_;
    }
    /// @}
    /// @brief Clock access.
    /// @return Clock access.
    TimeSource& clock()
    {
        return ts_;
    }
    /// @brief Platform access.
    /// @return Platform access.
    platform::IPlatform& platform()
    {
        return plat_;
    }
    /// @brief Runtime options.
    /// @return Runtime options.
    const IstateOptions& options() const
    {
        return opts_;
    }

    /// @brief Time policy (istate owns policy; networkd applies, §R8).
    /// @{
    struct TimePolicyState
    {
        std::vector<std::string> source_priority = {"gnss", "ptp", "ntp"};   ///< Locked preference order.
        std::string current_source = "unsynced";   ///< Source currently in use.
        double offset_ms = 0;         ///< Reported offset.
        double lock_quality = 0;      ///< 0..1 confidence.
        bool locked = false;          ///< Any source disciplined.
    };
    /// @brief Time-policy state access.
    /// @return Time-policy state access.
    TimePolicyState& time_policy()
    {
        return time_policy_;
    }
    /// @brief Called when a time source acquires lock: re-stamp spooled data (§4).
    /// @param source Lock source name.
    /// @param offset_ns Wall-clock correction in nanoseconds.
    /// @return Number of pending records re-stamped.
    size_t on_time_lock(const std::string& source, int64_t offset_ns);
    /// @brief Wall-clock reconciler access.
    /// @return Wall-clock reconciler access.
    TimeReconciler& reconciler()
    {
        return reconciler_;
    }

    /// @brief Northbound ingest from spokes.
    ///
    /// Each spoke's typed frames arrive as GenericSpokeMsg; istate routes them:
    /// telemetry → northbound spool (SPOOL_TELEMETRY), faults → protected,
    /// alarms → protected + timeline, samples → relay to telemetry, logs → tail
    /// fanout + spool refs, ota/backup events → timeline.
    /// @param msg Spoke frame routed by type.
    void on_spoke_msg(const transport::GenericSpokeMsg& msg);

    /// @brief Spoke command relay (Configuration → spoke, §3.3): correlated acks.
    struct PendingRelay
    {
        std::promise<std::string> promise;   ///< Fulfilled by the spoke's ack.
    };
    /// @brief Relay a command to a spoke; resolves when its ack arrives.
    /// @param daemon Spoke to relay to.
    /// @param cmd Command to run.
    /// @return Resolves with the ack JSON when the spoke answers.
    std::future<std::string> relay_command(const std::string& daemon, const common::Command& cmd);
    /// @brief Complete a pending relay with a spoke's ack.
    /// @param daemon Spoke that acked.
    /// @param ack Ack frame.
    void on_spoke_ack(const std::string& daemon, const common::Ack& ack);

    /// @brief Install the HQ send hook (implemented in hubs.cpp against the HqPeer stub).
    /// @param f HQ send hook (type, json) → bool.
    void set_hq_send(std::function<bool(const std::string& type, const std::string& json)> f)
    {
        hq_send_ = std::move(f);
    }
    /// @brief Wiring: forward hub messages to a spoke's session (per-daemon sessions register themselves with the core).
    using RelayFn = std::function<bool(const std::string& daemon, const transport::GenericHubMsg&)>;
    /// @brief Install the hub-message relay into spoke sessions.
    /// @param f Relay callback (daemon, message) → bool.
    void set_relay_fn(RelayFn f)
    {
        relay_to_spoke_ = std::move(f);
    }
    /// @brief Image default YAML provider (used for seeding + system reset).
    /// @param f Image defaults provider.
    void set_default_provider(std::function<std::map<std::string, std::string>()> f)
    {
        default_provider_ = std::move(f);
    }
    /// @brief Enrollment secret (factory-provisioned) for signature verification.
    /// @param s Enrollment secret.
    void set_enrollment_secret(std::string s)
    {
        enrollment_secret_ = std::move(s);
    }
    /// @brief Enrollment secret accessor.
    /// @return The enrollment secret.
    const std::string& enrollment_secret() const
    {
        return enrollment_secret_;
    }
    /// @brief Apply or release HQ backpressure.
    /// @param pause True to pause; false to resume draining.
    void set_hq_backpressure(bool pause);
    /// @brief True while HQ asked us to pause.
    bool hq_backpressured() const
    {
        return hq_backpressured_;
    }
    /// @brief Drain the northbound spool toward HQ (called when connected).
    /// @param max_batch Maximum records this drain.
    /// @return The drain report.
    NorthboundSpool::DrainReport drain_northbound(size_t max_batch = 50);

    /// @brief A spoke completed (or failed) the registration handshake (§3.1).
    ///
    /// The orchestrator records build version, proto version and capabilities,
    /// and a mismatch raises an alarm — refusing quietly would leave a
    /// partially updated fleet looking healthy.
    /// @param req Registration request.
    /// @param accepted Whether the handshake was accepted.
    void on_spoke_registration(const common::RegistrationRequest& req, bool accepted);

    /// @brief Alarms istate declares (R5, §6, §11).
    ///
    /// R5: alarms are declared, never inferred — only an application sets one.
    /// istate is the application that owns storage budgets, /pst integrity, the
    /// registration handshake and the clock, so these are its to declare. It
    /// does so by asking the Alarms daemon through the same relay everything
    /// else uses (R1); it never writes the alarm table itself.
    /// @{
    /// @param alarm_id Packed alarm identity.
    /// @param text Additional text.
    void raise_alarm(uint32_t alarm_id, const std::string& text);
    /// @brief Clear an istate-declared alarm through the Alarms relay.
    /// @param alarm_id Packed alarm identity.
    void clear_alarm(uint32_t alarm_id);
    /// @}

    /// @brief Capability manifest (§5.1), loaded from the board's descriptor.
    ///
    /// istate must report what this board actually has: a hardcoded list claims
    /// GNSS and CAN on every target and turns "honest reporting" into "fails
    /// the call later".
    /// @param caps Board capability manifest.
    void set_capabilities(std::vector<std::string> caps)
    {
        capabilities_ = std::move(caps);
    }
    /// @brief Reported capabilities.
    /// @return Reported capabilities.
    const std::vector<std::string>& capabilities() const
    {
        return capabilities_;
    }

    /// @brief Configuration delivery to spokes (R4, §7, §9).
    ///
    /// istate is the sole reader of /pst: a spoke asks on registration and is
    /// pushed a new delivery whenever its config changes.
    /// @{
    /// @brief Push a daemon's config; false when it has no open session (it will ask again on reconnect).
    /// @param daemon Daemon to push to.
    /// @return False when the spoke has no open session.
    bool deliver_config(const std::string& daemon);
    /// @brief Push the current config to every spoke with an open session.
    void deliver_config_all();
    /// @brief Revision of the config last delivered to a daemon (0 = never).
    /// @param daemon Daemon to query.
    /// @return The last delivered revision (0 = never).
    uint64_t config_revision(const std::string& daemon) const;
    /// @brief Last delivery outcome reported by a spoke (§9: a rejected push is loud).
    struct ConfigAckState
    {
        uint64_t revision = 0;       ///< Revision the ack refers to.
        bool applied = false;        ///< Whether the spoke accepted it.
        std::string reason;          ///< Rejection reason when not applied.
    };
    /// @brief Ack state for a daemon.
    /// @param daemon Daemon to query.
    /// @return The last ack state for the daemon.
    ConfigAckState config_ack_state(const std::string& daemon) const;
    /// @}

    /// @brief System info for the Configuration API.
    /// @return The system info as JSON.
    std::string system_info_json() const;
    /// @brief Budget usage report.
    /// @return The budget usage report as JSON.
    std::string budgets_json() const;

    /// @brief System reset (§6.1, §3.3 Configuration API).
    /// @param actor Who requested the reset (audit).
    /// @return True when performed, or an error.
    Result<bool> system_reset(const std::string& actor);
    /// @brief Factory reset (manufacturing operation, explicitly gated).
    /// @param actor Who requested the reset (audit).
    /// @param confirmation Required confirmation string.
    /// @return True when performed, or an error.
    Result<bool> factory_reset(const std::string& actor, const std::string& confirmation);

    /// @brief Unclean-shutdown detection (§11).
    ///
    /// The clean-shutdown marker lives under /pst/state (persistent — /run is
    /// tmpfs and would look unclean every boot). boot() observes the previous
    /// run's marker, reports an unclean shutdown northbound, then clears it;
    /// shutdown() writes it again.
    void write_clean_shutdown_marker();
    /// @brief Marker file path.
    /// @return The marker file path.
    std::string clean_shutdown_marker_path() const;
    /// @brief True when the previous shutdown was clean.
    bool last_shutdown_clean() const
    {
        return observed_clean_shutdown_;
    }

    /// @brief Queue a timeline event into the northbound spool.
    /// @param payload_json Timeline event payload.
    void push_event_to_spool(const std::string& payload_json)
    {
        northbound_spool_.enqueue(SpoolPriority::Event, payload_json, ts_.mono_ns());
    }

  private:
    IstateOptions opts_;              ///< Runtime options.
    IFileSystem& fs_;                  ///< Storage backend.
    platform::IPlatform& plat_;        ///< Platform (not owned).
    TimeSource& ts_;                   ///< Clock (not owned).
    Logger& log_;                      ///< Logger (not owned).

    BudgetManager budgets_;            ///< Per-consumer budgets.
    PstStore pst_;                     ///< /pst store.
    Provisioner provisioner_;          ///< First-boot provisioning.
    Orchestrator orchestrator_;        ///< Spoke lifecycle.
    EventTimeline timeline_;           ///< Ordered event stream.
    TailFanout tail_fanout_;           ///< Live tail subscribers.
    NorthboundSpool northbound_spool_;    ///< Queue toward HQ.
    HqSession hq_session_;             ///< HQ session state machine.
    ConfigService config_service_;     ///< Config get/set/verify.
    AuditLog audit_;                   ///< Audit trail.
    TimeReconciler reconciler_;        ///< Wall-clock reconciliation.
    TimePolicyState time_policy_;      ///< Current time policy.

    mutable std::mutex config_mu_;     ///< Guards config_revisions_/config_acks_.
    std::map<std::string, uint64_t> config_revisions_;    ///< Delivered revision per daemon.
    std::map<std::string, ConfigAckState> config_acks_;   ///< Last ack per daemon.

    std::map<uint64_t, std::shared_ptr<PendingRelay>> pending_relays_;   ///< In-flight command relays.
    std::mutex relay_mu_;              ///< Guards pending_relays_.
    uint64_t next_relay_id_ = 1;       ///< Relay id generator.
    std::atomic<bool> hq_backpressured_{false};   ///< HQ backpressure flag.
    std::function<bool(const std::string&, const std::string&)> hq_send_;   ///< HQ send hook.
    std::string enrollment_secret_;    ///< Factory-provisioned secret.
    std::vector<std::string> capabilities_;       ///< Board capability manifest.
    ///< Conditions found during boot, before the Alarms daemon has attached.
    std::vector<std::pair<uint32_t, std::string>> pending_alarms_;
    bool observed_clean_shutdown_ = false;   ///< Result of the marker check at boot.
    RelayFn relay_to_spoke_;           ///< Hub-message relay into spoke sessions.
    std::function<std::map<std::string, std::string>()> default_provider_;   ///< Image defaults provider.
};

} // namespace istate
} // namespace instr