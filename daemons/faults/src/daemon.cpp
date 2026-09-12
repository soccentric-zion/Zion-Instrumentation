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
 * @file daemons/faults/src/daemon.cpp
 * @brief faults daemon facade: core state, shared command table construction, and spoke wiring (REQ §7.5).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/faults/daemon.h"

namespace instr
{
/// @brief Faults daemon: passive kernel-log fault detection (§7.5, R6).
namespace faults
{

/// @brief Render a fault record as JSON for control responses.
/// @param r Fault record to render.
/// @return The record as JSON.
static JValue record_json(const FaultRecord& r)
{
    JValue o = JValue::object();
    o.set("fault_id", JValue(double(r.fault_id)));
    o.set("id_text", JValue(alarm_id_string(r.fault_id)));
    o.set("rule", JValue(r.rule));
    o.set("severity", JValue(r.severity == common::SEV_CRITICAL ? "CRITICAL" : r.severity == common::SEV_MAJOR ? "MAJOR" : r.severity == common::SEV_MINOR ? "MINOR" : r.severity == common::SEV_WARNING ? "WARNING" : "INFO"));
    o.set("raw_line", JValue(r.raw_line));
    o.set("occurrences", JValue(double(r.occurrences)));
    o.set("device_key", JValue(r.device_key));
    o.set("x733_type", JValue(r.x733_type));
    o.set("backfill", JValue(r.backfill));
    return o;
}

/// @brief Build the faults ctrl command table.
/// @param core Domain core the table reads from.
/// @return The command table.
CommandTable build_faults_table(FaultsCore* core)
{
    CommandTable t;
    t.add({"stats", "scan statistics (lines, faults, duplicates, suppressed)", {}, [core](const std::string&) {
               const auto& s = core->stats();
               JValue o = JValue::object();
               o.set("lines_scanned", JValue(double(s.lines_scanned)));
               o.set("faults_raised", JValue(double(s.faults_raised)));
               o.set("duplicates_merged", JValue(double(s.duplicates_merged)));
               o.set("backfill_lines", JValue(double(s.backfill_lines)));
               return CommandResult::good(o.dump());
           }});
    t.add({"rules-list", "list loaded fault rules", {}, [core](const std::string&) {
               JValue arr = JValue::array();
               for (const auto& r : core->rules())
               {
                   JValue o = JValue::object();
                   o.set("name", JValue(r.name));
                   o.set("pattern", JValue(r.pattern));
                   o.set("id_text", JValue(alarm_id_string(r.fault_id)));
                   o.set("severity", JValue(r.severity == common::SEV_CRITICAL ? "CRITICAL" : r.severity == common::SEV_MAJOR ? "MAJOR" : r.severity == common::SEV_MINOR ? "MINOR" : r.severity == common::SEV_WARNING ? "WARNING" : "INFO"));
                   o.set("x733_type", JValue(r.x733_type));
                   arr.push(o);
               }
               return CommandResult::good(arr.dump());
           }});
    t.add({"rules-reload", "reload the rule set from text (rules are data)", {{"yaml", true, "full fault-rules.yaml document"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               Error e = core->load_rules((*a).get_string("yaml"));
               return e.ok() ? CommandResult::good(std::string("{\"reloaded\":true}")) : CommandResult::from_error(e);
           }});
    t.add({"test-line", "test a kernel line against the rule set", {{"line", true, "kernel log line"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               auto recs = core->feed({(*a).get_string("line")}, 1'000'000'000ull, false);
               JValue arr = JValue::array();
               for (const auto& r : recs) arr.push(record_json(r));
               return CommandResult::good(arr.dump());
           }});
    return t;
}

/// @brief Wire the channel, config sink and handlers together.
/// @param core Domain core.
/// @param log Logger.
/// @param table Command table.
/// @param hub_target istate gRPC target.
/// @param build_version Build version to advertise.
FaultsSpoke::FaultsSpoke(FaultsCore* core, Logger* log, const CommandTable* table, const std::string& hub_target, const std::string& build_version) : core_(core), log_(log), table_(table)
{
    using ChannelT = SpokeChannel<faults::FaultHub, faults::FaultSpokeFrame, faults::FaultHubFrame>;
    ChannelT::Options o;
    o.target = hub_target;
    o.daemon = "faults";
    o.build_version = build_version;
    o.capabilities = {"kernel-log", "backfill", "dedup", "rate-limit"};
    channel_ = std::make_unique<ChannelT>(std::move(o), *log);
    channel_->set_hub_handler([this](const faults::FaultHubFrame& hf) {
        if (hf.has_command())
        {
            common::Ack ack = dispatch_to_ack(table_, hf.command());
            channel_->send_ack(ack);
        }
    });
    core_->set_sink([this](const FaultRecord& r) { send_fault(r); });
    // Until a schema is installed the sink still applies deliveries, just
    // without schema validation — a packaging slip must not leave the daemon
    // unconfigured (R10).
    set_schema(JValue::object());
}

// ---- kernel-log follower (§7.5) ----

/// @brief Construct the follower and its periodic worker.
FaultFollower::FaultFollower(FaultsCore* core, platform::IPlatform* plat, TimeSource* ts, Logger* log) : core_(core), plat_(plat), ts_(ts), log_(log), worker_("faults-follow", core->config().poll_interval_s, [this] { (void)poll(); })
{
    // A delivered configuration retunes the follow interval in place.
    core_->set_config_listener([this](const FaultsConfig& c) { worker_.set_interval_s(c.enabled ? c.poll_interval_s : 0.0); });
}

/// @brief Read the boot ring from the start as backfill (once).
size_t FaultFollower::backfill()
{
    if (!core_->config().backfill_boot_log) return 0;
    auto lines = plat_->kernel_log_lines(/*from_start=*/true);
    if (!lines.ok())
    {
        log_->warning("faults", "kernel boot log unavailable: " + lines.error.str());
        return 0;
    }
    if (lines->empty()) return 0;
    auto recs = core_->feed(*lines, ts_->mono_ns(), /*backfill=*/true);
    log_->info("faults", "backfilled " + std::to_string(lines->size()) + " boot log lines, " + std::to_string(recs.size()) + " faults");
    return recs.size();
}

/// @brief One follow pass over the kernel ring.
size_t FaultFollower::poll()
{
    auto lines = plat_->kernel_log_lines(/*from_start=*/false);
    if (!lines.ok() || lines->empty()) return 0;
    return core_->feed(*lines, ts_->mono_ns(), /*backfill=*/false).size();
}

/// @brief Start the periodic worker.
void FaultFollower::start()
{
    worker_.start();
}
/// @brief Stop the periodic worker.
void FaultFollower::stop()
{
    worker_.stop();
}

/// @brief Install the schema every config delivery must satisfy.
/// @param schema Schema every config delivery must satisfy.
void FaultsSpoke::set_schema(JValue schema)
{
    // Configuration is delivered by istate and validated here before it can
    // change behaviour (§9). A rejection keeps the previous config (R10).
    config_sink_ = std::make_unique<ConfigSink>("faults", std::move(schema), [this](const ConfigDocument& doc) { return core_->apply_config(doc); });
    channel_->set_config_handler([this](const common::ConfigDelivery& d) { return config_sink_->deliver(d.yaml(), d.revision()); });
}

/// @brief Start the transport channel.
void FaultsSpoke::start()
{
    channel_->start();
}
/// @brief Stop the transport channel.
void FaultsSpoke::stop()
{
    channel_->stop();
}

/// @brief Queue a fault record to istate.
/// @param r Fault record to queue.
void FaultsSpoke::send_fault(const FaultRecord& r)
{
    channel_->enqueue([&r](faults::FaultSpokeFrame& f) {
        auto* ev = f.mutable_event();
        ev->set_fault_id(r.fault_id);
        ev->mutable_ts()->set_mono_ns(r.mono_ns);
        ev->set_rule(r.rule);
        ev->set_severity(r.severity);
        ev->set_raw_line(r.raw_line);
        ev->set_occurrences(r.occurrences);
        ev->set_device_key(r.device_key);
        ev->set_x733_type(r.x733_type);
        ev->set_probable_cause(r.probable_cause);
        ev->set_backfill(r.backfill);
    });
}

/// @brief Queue a processor-statistics report to istate.
void FaultsSpoke::send_stats()
{
    const auto& s = core_->stats();
    JValue o = JValue::object();
    o.set("lines_scanned", JValue(double(s.lines_scanned)));
    o.set("faults_raised", JValue(double(s.faults_raised)));
    o.set("duplicates_merged", JValue(double(s.duplicates_merged)));
    o.set("suppressed_events", JValue(double(s.suppressed_events)));
    o.set("backfill_lines", JValue(double(s.backfill_lines)));
    channel_->send_status_json(o.dump());
}

} // namespace faults
} // namespace instr