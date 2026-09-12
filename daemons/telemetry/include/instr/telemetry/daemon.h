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
 * @file daemons/telemetry/include/instr/telemetry/daemon.h
 * @brief telemetry daemon facade: core (relays from monitord/hardware/faults via
 *
 * istate), command table, spoke.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <memory>

#include "instr/common/command_table.h"
#include "instr/common/config_sink.h"
#include "instr/common/periodic.h"
#include "instr/telemetry/domain.h"
#include "instr/transport/transport.h"
#include "telemetry.grpc.pb.h"

namespace instr
{
/// @brief Telemetry daemon: aggregation, health score and northbound batches (§7.8).
namespace telemetry
{
using namespace domain;
using namespace instr::transport;

/// @brief Telemetry domain core: registry, shaper, retention buffer and health.
class TelemetryCore
{
  public:
    /// @brief Bind the core to a clock with default shaping and retention.
    TelemetryCore(TimeSource& ts) : ts_(ts), shaper_(200, 30.0), buffer_(50)
    {
    }

    /// @brief Configuration delivered by istate (§7.8, §9).
    struct Config
    {
        bool enabled = true;                  ///< Master switch.
        double interval = 30.0;               ///< Batch/flush interval.
        size_t batch_max_records = 200;       ///< Record bound per batch.
        double aggregation_window_s = 30.0;   ///< Aggregation window length.
        size_t retention_batches = 50;        ///< Unsent-batch retention bound.
        std::vector<std::string> health_inputs{"monitord", "hardware", "faults", "alarms"};   ///< Health-score sources.
    };
    /// @brief Current configuration.
    /// @return Current configuration.
    const Config& config() const
    {
        return cfg_;
    }
    /// @brief Apply an istate-delivered config document (retunes the shaper/buffer live).
    /// @param doc Parsed, schema-validated document.
    /// @return ok(), or an error describing why the values were refused.
    Error apply_config(const ConfigDocument& doc)
    {
        Config c;
        c.enabled = doc.get_bool("enabled", true);
        c.interval = doc.get_double("interval", 30.0);
        c.batch_max_records = size_t(doc.get_int("batch_max_records", 200));
        c.aggregation_window_s = doc.get_double("aggregation_window_s", c.interval);
        c.retention_batches = size_t(doc.get_int("retention_batches", 50));
        auto inputs = doc.get_list("health_inputs");
        if (!inputs.empty()) c.health_inputs = inputs;
        if (c.interval <= 0) return Error::make(Errc::InvalidArgument, "interval must be > 0", "telemetry config");
        if (c.batch_max_records == 0) return Error::make(Errc::InvalidArgument, "batch_max_records must be >= 1", "telemetry config");
        cfg_ = c;
        shaper_.set_max_records(c.batch_max_records);
        shaper_.set_flush_interval_s(c.interval);
        buffer_.set_max_records(c.retention_batches);
        if (on_config_) on_config_(cfg_);
        return Error::none();
    }
    /// @brief Install a callback fired when a new config is accepted.
    /// @param f Config-accepted listener.
    void set_config_listener(std::function<void(const Config&)> f)
    {
        on_config_ = std::move(f);
    }

    /// @brief Registry access.
    /// @return Registry access.
    MetricRegistry& registry()
    {
        return registry_;
    }
    /// @brief Shaper access.
    /// @return Shaper access.
    BatchShaper& shaper()
    {
        return shaper_;
    }
    /// @brief Retention buffer access.
    /// @return Retention buffer access.
    RetentionBuffer& buffer()
    {
        return buffer_;
    }
    /// @brief Clock access.
    /// @return Clock access.
    TimeSource& clock()
    {
        return ts_;
    }

    /// @brief Application metric registration API. Duplicate → alarm-visible event.
    /// @param name Metric name.
    /// @param unit Unit string.
    /// @param type gauge or counter.
    /// @param app Registering application.
    /// @return The registration outcome (Duplicate is alarm-visible).
    MetricRegistry::Reg register_metric(const std::string& name, const std::string& unit, const std::string& type, const std::string& app);
    /// @brief Ingest relayed samples (monitord via istate) into the shaper.
    /// @param metric Metric name.
    /// @param value Sample value.
    /// @param unit Unit string.
    /// @param tags Dimension tags.
    void ingest_relayed(const std::string& metric, double value, const std::string& unit, const std::map<std::string, std::string>& tags);
    /// @brief Recompute the health score from inputs of the other daemons.
    /// @param in Health inputs from the other daemons.
    void update_health(const HealthInputs& in)
    {
        auto h = compute_health_score(in);
        shaper().set_health(h);
        last_health_ = h;
    }
    /// @brief Last computed health score.
    /// @return The last computed health score.
    const HealthScoreValue& last_health() const
    {
        return last_health_;
    }
    /// @brief Attach reproducibility metadata to emitted batches.
    /// @param m Metadata to attach to emitted batches.
    void set_repro(const ReproMetadata& m)
    {
        shaper().set_repro(m);
    }
    /// @brief Install the batch sink (spoke emission).
    /// @param f Batch sink (spoke emission).
    void set_sink(std::function<void(const MetricBatch&)> f)
    {
        sink_ = std::move(f);
    }

    uint64_t alarm_registrations_conflict = 0;

  private:
    TimeSource& ts_;                    ///< Clock (not owned).
    MetricRegistry registry_;           ///< Application-registered metrics.
    BatchShaper shaper_;                ///< Batch shaping and aggregation.
    RetentionBuffer buffer_;            ///< Unsent-batch retention.
    HealthScoreValue last_health_;      ///< Last computed health.
    std::function<void(const MetricBatch&)> sink_;   ///< Batch sink.
    Config cfg_;                        ///< Current configuration.
    std::function<void(const Config&)> on_config_;   ///< Config-accepted listener.
};

/// @brief Build the telemetry ctrl command table (register, metrics, batches, status).
/// @param core Domain core the table reads from.
/// @return The command table.
CommandTable build_telemetry_table(TelemetryCore* core);

/// @brief Spoke: transport to istate plus the schema-validated config sink.
class TelemetrySpoke
{
  public:
    /// @brief Wire the channel, config sink and handlers together.
    /// @param core Domain core.
    /// @param log Logger.
    /// @param table Command table.
    /// @param hub_target istate gRPC target.
    /// @param build_version Build version to advertise.
    TelemetrySpoke(TelemetryCore* core, Logger* log, const CommandTable* table, const std::string& hub_target, const std::string& build_version);
    /// @brief Start the channel.
    void start();
    /// @brief Stop the channel.
    void stop();
    /// @brief True while registered with istate.
    bool registered() const
    {
        return channel_->registered();
    }
    /// @brief Install the schema every config delivery must satisfy (R4, §9).
    /// @param schema Schema every config delivery must satisfy.
    void set_schema(JValue schema);
    /// @brief Revision of the last config applied.
    uint64_t config_revision() const
    {
        return channel_->config_revision();
    }
    /// @brief Queue a telemetry batch to istate.
    /// @param b Batch to queue.
    void send_batch(const MetricBatch& b);

  private:
    TelemetryCore* core_;                     ///< Domain core (not owned).
    Logger* log_;                             ///< Logger (not owned).
    const CommandTable* table_;               ///< Command table (not owned).
    std::unique_ptr<ConfigSink> config_sink_;      ///< Validates and applies config deliveries.
    std::unique_ptr<SpokeChannel<telemetry::TelemetryHub, telemetry::TelemetrySpokeFrame, telemetry::TelemetryHubFrame>> channel_;   ///< gRPC channel to istate.
};

} // namespace telemetry
} // namespace instr