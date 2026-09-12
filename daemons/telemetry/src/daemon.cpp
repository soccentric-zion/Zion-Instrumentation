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
 * @file daemons/telemetry/src/daemon.cpp
 * @brief telemetry daemon facade: core relaying from monitord/hardware/faults via istate, shared command table construction, and spoke wiring (REQ §7.8).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/telemetry/daemon.h"

namespace instr
{
/// @brief Telemetry daemon: aggregation, health score and northbound batches (§7.8).
namespace telemetry
{

/// @brief Application metric registration entry point.
/// @param name Metric name.
/// @param unit Unit string.
/// @param type gauge or counter.
/// @param app Registering application.
/// @return The registration outcome.
MetricRegistry::Reg TelemetryCore::register_metric(const std::string& name, const std::string& unit, const std::string& type, const std::string& app)
{
    auto r = registry_.register_metric(name, unit, type, app);
    if (r == MetricRegistry::Reg::Duplicate) ++alarm_registrations_conflict;
    return r;
}

/// @brief Ingest a relayed sample into the shaper.
/// @param metric Metric name.
/// @param value Sample value.
/// @param unit Unit string.
/// @param tags Dimension tags.
void TelemetryCore::ingest_relayed(const std::string& metric, double value, const std::string& unit, const std::map<std::string, std::string>& tags)
{
    auto maybe = shaper_.ingest(metric, value, unit, ts_.mono_ns(), tags);
    if (maybe)
    {
        buffer_.push(*maybe, RetentionBuffer::Class::Telemetry);
        if (sink_) sink_(*maybe);
    }
}

/// @brief Render a metric record as JSON.
/// @param r Metric record to render.
/// @return The record as JSON.
static JValue record_json(const MetricRecord& r)
{
    JValue o = JValue::object();
    o.set("metric", JValue(r.metric));
    o.set("value", JValue(r.value));
    o.set("agg_fn", JValue(r.agg_fn));
    o.set("count", JValue(double(r.count)));
    o.set("window_start", JValue(double(r.window_start_mono_ns)));
    o.set("window_end", JValue(double(r.window_end_mono_ns)));
    return o;
}

/// @brief Build the telemetry ctrl command table.
/// @param core Domain core the table reads from.
/// @return The command table.
CommandTable build_telemetry_table(TelemetryCore* core)
{
    CommandTable t;
    t.add({"list-metrics", "list registered metrics", {}, [core](const std::string&) {
               JValue arr = JValue::array();
               for (const auto& kv : core->registry().metrics())
               {
                   JValue o = JValue::object();
                   o.set("name", JValue(kv.second.name));
                   o.set("unit", JValue(kv.second.unit));
                   o.set("type", JValue(kv.second.type));
                   o.set("app", JValue(kv.second.app));
                   arr.push(o);
               }
               return CommandResult::good(arr.dump());
           }});
    t.add({"register-metric", "application metric registration", {{"name", true, "metric name"}, {"unit", true, "unit"}, {"type", true, "gauge|counter"}, {"app", true, "registering application"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               auto r = core->register_metric((*a).get_string("name"), (*a).get_string("unit"), (*a).get_string("type"), (*a).get_string("app"));
               if (r == MetricRegistry::Reg::Duplicate) return CommandResult::err(Errc::Busy, "metric already registered (alarm raised)");
               if (r == MetricRegistry::Reg::Invalid) return CommandResult::err(Errc::InvalidArgument, "name empty or bad type");
               return CommandResult::good(std::string("{\"registered\":true}"));
           }});
    t.add({"ingest", "ingest one sample (relayed from monitord via istate)", {{"metric", true, "metric name"}, {"value", true, "numeric value"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               core->ingest_relayed((*a).get_string("metric"), (*a).get_number("value"), (*a).get_string("unit"), {});
               return CommandResult::good(std::string("{\"ingested\":true}"));
           }});
    t.add({"flush", "close the current window and emit a batch", {}, [core](const std::string&) {
               auto b = core->shaper().flush(core->clock().mono_ns());
               if (!b) return CommandResult::good(std::string("{\"emitted\":false}"));
               JValue arr = JValue::array();
               for (const auto& r : b->records) arr.push(record_json(r));
               JValue o = JValue::object();
               o.set("emitted", JValue(true));
               o.set("records", arr);
               o.set("health_score", JValue(b->health.score));
               return CommandResult::good(o.dump());
           }});
    t.add({"buffer-stats", "retention buffer statistics", {}, [core](const std::string&) {
               JValue o = JValue::object();
               o.set("buffered", JValue(double(core->buffer().size())));
               o.set("dropped", JValue(double(core->buffer().dropped())));
               return CommandResult::good(o.dump());
           }});
    t.add({"health-score", "per-device health score with contributing factors", {}, [core](const std::string&) {
               JValue o = JValue::object();
               o.set("score", JValue(core->last_health().score));
               JValue factors = JValue::object();
               for (const auto& kv : core->last_health().factors) factors.set(kv.first, JValue(kv.second));
               o.set("factors", factors);
               return CommandResult::good(o.dump());
           }});
    t.add({"drain", "drain buffered unsent batches (returns count)", {}, [core](const std::string&) {
               auto drained = core->buffer().drain();
               JValue o = JValue::object();
               o.set("batches", JValue(double(drained.size())));
               return CommandResult::good(o.dump());
           }});
    return t;
}

/// @brief Wire the channel, config sink and handlers together.
/// @param core Domain core.
/// @param log Logger.
/// @param table Command table.
/// @param hub_target istate gRPC target.
/// @param build_version Build version to advertise.
TelemetrySpoke::TelemetrySpoke(TelemetryCore* core, Logger* log, const CommandTable* table, const std::string& hub_target, const std::string& build_version) : core_(core), log_(log), table_(table)
{
    using ChannelT = SpokeChannel<telemetry::TelemetryHub, telemetry::TelemetrySpokeFrame, telemetry::TelemetryHubFrame>;
    ChannelT::Options o;
    o.target = hub_target;
    o.daemon = "telemetry";
    o.build_version = build_version;
    o.capabilities = {"aggregation", "batches", "health-score", "app-registration"};
    channel_ = std::make_unique<ChannelT>(std::move(o), *log);
    // Hub frames: commands dispatch via the table; relays (monitord samples,
    // hardware results, fault rates) feed the aggregator (§7.8).
    channel_->set_hub_handler([this](const telemetry::TelemetryHubFrame& hf) {
        if (hf.has_command())
        {
            common::Ack ack = dispatch_to_ack(table_, hf.command());
            channel_->send_ack(ack);
        }
        if (hf.has_relay())
        {
            auto payload = json_parse(hf.relay().payload_json());
            if (payload.ok() && hf.relay().type() == "sample")
            {
                std::map<std::string, std::string> tags;
                if (const JValue* t = (*payload).find("tags"))
                    for (const auto& kv : t->members()) tags[kv.first] = kv.second.as_string();
                core_->ingest_relayed((*payload).get_string("metric"), (*payload).get_number("value"), (*payload).get_string("unit"), tags);
            }
        }
    });
    // Until a schema is installed, deliveries still apply — just unvalidated.
    // A packaging slip must not leave a daemon running unconfigured (R10).
    set_schema(JValue::object());
}

/// @brief Install the schema every config delivery must satisfy.
/// @param schema Schema every config delivery must satisfy.
void TelemetrySpoke::set_schema(JValue schema)
{
    config_sink_ = std::make_unique<ConfigSink>("telemetry", std::move(schema), [this](const ConfigDocument& doc) { return core_->apply_config(doc); });
    channel_->set_config_handler([this](const common::ConfigDelivery& delivery) { return config_sink_->deliver(delivery.yaml(), delivery.revision()); });
}

/// @brief Start the transport channel.
void TelemetrySpoke::start()
{
    channel_->start();
}
/// @brief Stop the transport channel.
void TelemetrySpoke::stop()
{
    channel_->stop();
}

/// @brief Queue a telemetry batch to istate.
/// @param b Batch to queue.
void TelemetrySpoke::send_batch(const MetricBatch& b)
{
    channel_->enqueue([&b](telemetry::TelemetrySpokeFrame& f) {
        auto* batch = f.mutable_batch();
        batch->mutable_ts()->set_mono_ns(b.mono_ns);
        for (const auto& r : b.records)
        {
            auto* rec = batch->add_records();
            rec->set_metric(r.metric);
            rec->set_value(r.value);
            rec->set_unit(r.unit);
            rec->set_agg_fn(r.agg_fn);
            rec->set_window_start_mono_ns(r.window_start_mono_ns);
            rec->set_window_end_mono_ns(r.window_end_mono_ns);
            rec->set_count(r.count);
            for (const auto& kv : r.tags) (*rec->mutable_tags())[kv.first] = kv.second;
        }
        batch->mutable_health()->set_score(b.health.score);
        for (const auto& kv : b.health.factors) (*batch->mutable_health()->mutable_factors())[kv.first] = kv.second;
        batch->set_firmware_version(b.repro.firmware_version);
        batch->set_config_schema_version(b.repro.config_schema_version);
        batch->set_hardware_revision(b.repro.hardware_revision);
        batch->set_device_serial(b.repro.device_serial);
    });
}

} // namespace telemetry
} // namespace instr