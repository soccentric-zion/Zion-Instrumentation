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
 * @file daemons/ota/src/daemon.cpp
 * @brief ota daemon facade: core state, shared command table construction, and spoke wiring (REQ §7.9).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/ota/daemon.h"

namespace instr
{
/// @brief OTA daemon: signed update pipeline with rollback (§7.9, §11).
namespace ota
{

/// @brief Render the update history as a JSON array.
/// @param h History entries to render.
/// @return The history as a JSON array.
static JValue history_json_value(const std::vector<HistoryEntry>& h)
{
    JValue arr = JValue::array();
    for (const auto& e : h)
    {
        JValue o = JValue::object();
        o.set("from", JValue(e.from_version));
        o.set("to", JValue(e.to_version));
        o.set("result", JValue(e.result));
        o.set("finished_unix_s", JValue(double(e.finished_unix_s)));
        arr.push(o);
    }
    return arr;
}

/// @brief Render engine state as JSON.
/// @return The engine state as JSON.
std::string OtaCore::status_json() const
{
    const UpdateState& st = engine_.state();
    JValue o = JValue::object();
    o.set("state", JValue(st.state));
    o.set("from_version", JValue(st.from_version));
    o.set("to_version", JValue(st.to_version));
    o.set("progress", JValue(st.progress));
    o.set("staged_slot", JValue(st.staged_slot));
    o.set("error", JValue(st.error));
    o.set("history", history_json_value(engine_.history()));
    return o.dump();
}

/// @brief Build the ota ctrl command table.
/// @param core Domain core the table reads from.
/// @return The command table.
CommandTable build_ota_table(OtaCore* core)
{
    CommandTable t;
    t.add({"status", "current slot, version, update state and history", {}, [core](const std::string&) { return CommandResult::good(core->status_json()); }});
    t.add({"receive-chunk", "artifact intake (resume-aware; lands on disk before applying)", {{"data_b64", true, "base64 chunk"}, {"offset", true, "byte offset"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               auto data = crypto::base64_decode((*a).get_string("data_b64"));
               if (!data.ok()) return CommandResult::from_error(data.error);
               uint64_t offset = uint64_t((*a).get_number("offset", 0));
               auto r = core->engine().receive_chunk(*data, offset);
               if (!r.ok()) return CommandResult::from_error(r.error);
               JValue o = JValue::object();
               o.set("received_bytes", JValue(double(*r)));
               return CommandResult::good(o.dump());
           }});
    t.add({"stage", "verify + stage the artifact to the inactive slot", {{"manifest", true, "artifact manifest JSON"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               auto m = core->engine().parse_manifest((*a).get_string("manifest"));
               if (!m.ok()) return CommandResult::from_error(m.error);
               std::string secret = core->secret();
               Error e = core->engine().stage(*m, secret, core->running_version());
               return e.ok() ? CommandResult::good(std::string("{\"staged\":true}")) : CommandResult::from_error(e);
           }});
    t.add({"apply", "apply the staged update (set boot slot)", {}, [core](const std::string&) {
               Error e = core->engine().apply();
               return e.ok() ? CommandResult::good(std::string("{\"applied\":true}")) : CommandResult::from_error(e);
           }});
    t.add({"confirm", "explicit post-boot confirmation (unconfirmed rolls back)", {}, [core](const std::string&) {
               Error e = core->engine().confirm();
               return e.ok() ? CommandResult::good(std::string("{\"confirmed\":true}")) : CommandResult::from_error(e);
           }});
    t.add({"rollback", "manual rollback to the previous slot", {{"reason", false, "why"}}, [core](const std::string& args) {
               auto a = args_object(args);
               Error e = core->engine().rollback(a.ok() ? (*a).get_string("reason") : "manual");
               return e.ok() ? CommandResult::good(std::string("{\"rolled_back\":true}")) : CommandResult::from_error(e);
           }});
    t.add({"history", "update history", {}, [core](const std::string&) { return CommandResult::good(history_json_value(core->engine().history()).dump()); }});
    t.add({"window-set", "set maintenance windows (runtime; config via istate)", {{"json", true, "[{\"day\":\"*\",\"start\":\"02:00\",\"end\":\"04:00\"}]"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               auto j = json_parse((*a).get_string("json"));
               if (!j.ok() || !(*j).is_array()) return CommandResult::err(Errc::InvalidArgument, "json must be an array");
               std::vector<OtaConfig::Window> wins;
               for (const auto& item : (*j).items())
               {
                   OtaConfig::Window w;
                   w.day = item.find("day") ? item.find("day")->as_string() : "*";
                   w.start = item.find("start") ? item.find("start")->as_string() : "00:00";
                   w.end = item.find("end") ? item.find("end")->as_string() : "23:59";
                   wins.push_back(w);
               }
               core->engine().set_windows(wins);
               return CommandResult::good(std::string("{\"set\":true}"));
           }});
    t.add({"window-check", "is now within a maintenance window?", {}, [core](const std::string&) {
               int64_t wall = core->wall_now_s();
               JValue o = JValue::object();
               o.set("allowed", JValue(core->engine().in_maintenance_window(wall)));
               return CommandResult::good(o.dump());
           }});
    t.add({"canary", "canary report for staged rollouts", {{"health_score", false, "0..100"}, {"fault_rate_24h", false, "faults/day"}}, [core](const std::string& args) {
               auto a = args_object(args);
               double hs = a.ok() ? (*a).get_number("health_score", 100) : 100;
               double fr = a.ok() ? (*a).get_number("fault_rate_24h", 0) : 0;
               return CommandResult::good(core->engine().canary_json(hs, fr));
           }});
    t.add({"tick", "run the confirmation watchdog tick", {}, [core](const std::string&) {
               auto ev = core->engine().tick(core->now_mono());
               JValue o = JValue::object();
               o.set("fired", JValue(ev.has_value()));
               if (ev) o.set("stage", JValue(ev->stage));
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
OtaSpoke::OtaSpoke(OtaCore* core, Logger* log, const CommandTable* table, const std::string& hub_target, const std::string& build_version) : core_(core), log_(log), table_(table)
{
    using ChannelT = SpokeChannel<ota::OtaHub, ota::OtaSpokeFrame, ota::OtaHubFrame>;
    ChannelT::Options o;
    o.target = hub_target;
    o.daemon = "ota";
    o.build_version = build_version;
    o.capabilities = {"stage-apply", "confirm-rollback", "resume", "maintenance-windows", "canary"};
    channel_ = std::make_unique<ChannelT>(std::move(o), *log);
    channel_->set_hub_handler([this](const ota::OtaHubFrame& hf) {
        if (hf.has_command())
        {
            common::Ack ack = dispatch_to_ack(table_, hf.command());
            channel_->send_ack(ack);
        }
    });
    core_->set_spoke_sink([this](const OtaProgressEvent& ev) { send_event(ev); });
    // Until a schema is installed, deliveries still apply — just unvalidated.
    // A packaging slip must not leave a daemon running unconfigured (R10).
    set_schema(JValue::object());
}

/// @brief Install the schema every config delivery must satisfy.
/// @param schema Schema every config delivery must satisfy.
void OtaSpoke::set_schema(JValue schema)
{
    config_sink_ = std::make_unique<ConfigSink>("ota", std::move(schema), [this](const ConfigDocument& doc) { return core_->apply_config(doc); });
    channel_->set_config_handler([this](const common::ConfigDelivery& delivery) { return config_sink_->deliver(delivery.yaml(), delivery.revision()); });
}

/// @brief Start the transport channel.
void OtaSpoke::start()
{
    channel_->start();
}
/// @brief Stop the transport channel.
void OtaSpoke::stop()
{
    channel_->stop();
}

/// @brief Queue a progress event to istate.
/// @param ev Progress event to queue.
void OtaSpoke::send_event(const OtaProgressEvent& ev)
{
    channel_->enqueue([&ev](ota::OtaSpokeFrame& f) {
        auto* e = f.mutable_event();
        e->set_stage(ev.stage);
        e->set_progress(ev.progress);
        e->set_message(ev.message);
        e->set_from_version(ev.from_version);
        e->set_to_version(ev.to_version);
    });
}

/// @brief Queue a state report to istate.
void OtaSpoke::send_status()
{
    channel_->send_status_json(core_->status_json());
}

} // namespace ota
} // namespace instr