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
 * @file daemons/faults/include/instr/faults/daemon.h
 * @brief faults daemon facade: core + command table + spoke
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <memory>

#include "instr/common/command_table.h"
#include "instr/common/config_sink.h"
#include "instr/common/periodic.h"
#include "instr/faults/domain.h"
#include "instr/platform/platform.h"
#include "instr/transport/transport.h"
#include "faults.grpc.pb.h"

namespace instr
{
/// @brief Faults daemon: passive kernel-log fault detection (§7.5, R6).
namespace faults
{
using namespace domain;
using namespace instr::transport;

/// @brief Configuration delivered by istate (R4: faults never opens /pst itself).
struct FaultsConfig
{
    bool enabled = true; ///< Master switch.
    bool backfill_boot_log = true; ///< Feed the boot ring once at start.
    double poll_interval_s = 1.0; ///< Kernel ring / journal follow interval.
    double max_events_per_s = 0; ///< Global emission ceiling; 0 = rule-level limits only.
    bool stream_to_hq = true; ///< Forward faults through istate.
};

/// @brief Faults domain core: rule loading, processor, config and record routing.
class FaultsCore
{
  public:
    /// @brief Construct an empty core (rules arrive via config or load_rules).
    FaultsCore() : processor_(std::vector<FaultRule>{})
    {
    }

    /// @brief Current configuration.
    /// @return Current configuration.
    const FaultsConfig& config() const
    {
        return cfg_;
    }
    /// @brief Apply an istate-delivered config document (§7.5, §9; rejects invalid values).
    /// @param doc Parsed, schema-validated document.
    /// @return ok(), or an error describing why the values were refused.
    Error apply_config(const ConfigDocument& doc)
    {
        FaultsConfig c;
        c.enabled = doc.get_bool("enabled", true);
        c.backfill_boot_log = doc.get_bool("backfill_boot_log", true);
        c.poll_interval_s = doc.get_double("poll_interval_s", 1.0);
        c.max_events_per_s = doc.get_double("max_events_per_s", 0);
        c.stream_to_hq = doc.get_bool("stream_to_hq", true);
        if (c.poll_interval_s <= 0) return Error::make(Errc::InvalidArgument, "poll_interval_s must be > 0", "faults config");
        cfg_ = c;
        if (processor_) (*processor_).set_global_rate_limit(cfg_.max_events_per_s);
        if (on_config_) on_config_(cfg_);
        return Error::none();
    }
    /// @brief Install a callback fired when a new config is accepted.
    /// @param f Config-accepted listener.
    void set_config_listener(std::function<void(const FaultsConfig&)> f)
    {
        on_config_ = std::move(f);
    }

    /// @brief Replace the rule set (compiled processor rebuilt on success).
    /// @param yaml_text Rules source text.
    /// @return ok(), or the rules parse error.
    Error load_rules(const std::string& yaml_text)
    {
        auto parsed = domain::load_rules(yaml_text);
        if (!parsed.ok()) return parsed.error;
        rules_text_ = yaml_text;
        processor_ = FaultProcessor(*parsed);
        return Error::none();
    }
    /// @brief Raw text of the loaded rules (surfaced by ctrl "rules").
    /// @return Raw text of the loaded rules (surfaced by ctrl "rules").
    const std::string& rules_text() const
    {
        return rules_text_;
    }

    /// @brief Feed kernel lines through the processor and route records to the sink.
    /// @param lines Kernel lines to scan.
    /// @param mono_ns Current monotonic time.
    /// @param backfill True for records recovered from the boot ring.
    /// @return The fault records produced by this batch.
    std::vector<FaultRecord> feed(const std::vector<std::string>& lines, uint64_t mono_ns, bool backfill)
    {
        auto out = (*processor_).feed(lines, mono_ns, backfill);
        for (const auto& r : out)
        {
            if (sink_) sink_(r);
        }
        return out;
    }
    /// @brief Install the fault-record sink (spoke emission).
    /// @param f Fault-record sink (spoke emission).
    void set_sink(std::function<void(const FaultRecord&)> f)
    {
        sink_ = std::move(f);
    }
    /// @brief Lifetime processor statistics.
    /// @return Lifetime processor statistics.
    const FaultProcessor::Stats& stats() const
    {
        return (*processor_).stats();
    }
    /// @brief The active rule set (empty until rules are loaded).
    /// @return The active rule set (empty until rules are loaded).
    const std::vector<FaultRule>& rules() const
    {
        return processor_ ? (*processor_).engine_rules() : empty_rules_;
    }

  private:
    std::string rules_text_; ///< Raw loaded rules text.
    std::optional<FaultProcessor> processor_; ///< Compiled processor (none until rules load).
    std::vector<FaultRule> empty_rules_; ///< Returned by rules() before rules load.
    std::function<void(const FaultRecord&)> sink_; ///< Fault-record sink.
    FaultsConfig cfg_; ///< Current configuration.
    std::function<void(const FaultsConfig&)> on_config_; ///< Config-accepted listener.
};

/// @brief Build the faults ctrl command table (rules, stats, feed).
/// @param core Domain core the table reads from.
/// @return The command table.
CommandTable build_faults_table(FaultsCore* core);

/// @brief Spoke: transport to istate plus the schema-validated config sink.
class FaultsSpoke
{
  public:
    /// @brief Wire the channel, config sink and handlers together.
    /// @param core Domain core.
    /// @param log Logger.
    /// @param table Command table.
    /// @param hub_target istate gRPC target.
    /// @param build_version Build version to advertise.
    FaultsSpoke(FaultsCore* core, Logger* log, const CommandTable* table, const std::string& hub_target, const std::string& build_version);
    /// @brief Start the channel.
    void start();
    /// @brief Stop the channel.
    void stop();
    /// @brief True while registered with istate.
    bool registered() const
    {
        return channel_->registered();
    }
    /// @brief Queue a fault record to istate.
    /// @param r Fault record to queue.
    void send_fault(const FaultRecord& r);
    /// @brief Queue a processor-statistics report to istate.
    void send_stats();
    /// @brief Install the schema every config delivery must satisfy (§9; "" = accepted).
    /// @param schema Schema every config delivery must satisfy.
    void set_schema(JValue schema);
    /// @brief Revision of the last config applied.
    uint64_t config_revision() const
    {
        return channel_->config_revision();
    }

  private:
    FaultsCore* core_; ///< Domain core (not owned).
    Logger* log_; ///< Logger (not owned).
    const CommandTable* table_; ///< Command table (not owned).
    std::unique_ptr<ConfigSink> config_sink_; ///< Validates and applies config deliveries.
    std::unique_ptr<SpokeChannel<faults::FaultHub, faults::FaultSpokeFrame, faults::FaultHubFrame>> channel_; ///< gRPC channel to istate.
};

/// @brief Kernel-log follower (§7.5: "real-time monitoring of the kernel ring buffer and journal").
///
/// Backfills the boot window once, then polls for new records on the
/// configured interval. Purely passive (R6): it observes and reports, and
/// never configures hardware or raises an alarm.
class FaultFollower
{
  public:
    /// @brief Construct the follower over a core, platform, clock and logger.
    /// @param core Domain core.
    /// @param plat Kernel-log source.
    /// @param ts Clock.
    /// @param log Logger.
    FaultFollower(FaultsCore* core, platform::IPlatform* plat, TimeSource* ts, Logger* log);
    /// @brief Read the ring from the start and feed it as backfill. Call once.
    /// @return Lines fed as backfill.
    size_t backfill();
    /// @brief One follow pass: feeds only records not seen before.
    /// @return Records collected this pass.
    size_t poll();
    /// @brief Start the periodic worker.
    void start();
    /// @brief Stop the periodic worker.
    void stop();
    /// @brief Override the follow interval.
    /// @param s New follow interval in seconds.
    void set_interval_s(double s)
    {
        worker_.set_interval_s(s);
    }
    /// @brief Passes completed since start.
    uint64_t polls() const
    {
        return worker_.ticks();
    }

  private:
    FaultsCore* core_; ///< Domain core (not owned).
    platform::IPlatform* plat_; ///< Kernel-log source (not owned).
    TimeSource* ts_; ///< Clock (not owned).
    Logger* log_; ///< Logger (not owned).
    PeriodicWorker worker_; ///< Polling scheduler.
};

} // namespace faults
} // namespace instr