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
 * @file daemons/alarms/src/daemon.cpp
 * @brief alarms daemon facade: core state, shared command table construction, and spoke wiring over the gRPC-over-UDS transport (REQ §7.6).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/alarms/daemon.h"

namespace instr
{
/// @brief Alarms daemon: X.733 alarm state machine, escalation and catalog (§7.6).
namespace alarms
{

/// @brief Map a severity name to the proto enum (table handlers).
/// @param s Severity name to map.
/// @return The proto severity enum.
static common::Severity sev_from_name(const std::string& s)
{
    if (s == "CRITICAL") return common::SEV_CRITICAL;
    if (s == "MAJOR") return common::SEV_MAJOR;
    if (s == "MINOR") return common::SEV_MINOR;
    if (s == "WARNING") return common::SEV_WARNING;
    return common::SEV_INFO;
}

/// @brief Build the alarms ctrl command table.
/// @param core Domain core the table reads from.
/// @return The command table.
CommandTable build_alarms_table(AlarmsCore* core)
{
    CommandTable t;
    t.add({"list", "list the active alarm table", {}, [core](const std::string&) { return CommandResult::good(core->manager().active_table_json()); }});
    // raise/clear are application operations (R5) — exposed for application
    // integrators and test harnesses; istate relays HQ commands likewise.
    t.add({"raise", "declare an alarm (applications only)", {{"id", true, "alarm id (0x1_04_1_0007 or decimal)"}, {"owner", true, "declaring application"}, {"text", false, "additional text"}, {"correlated", false, "comma-separated correlated ids"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               uint32_t id = uint32_t((*a).get_number("id", 0));
               std::string owner = (*a).get_string("owner");
               std::string text = (*a).get_string("text");
               std::vector<uint32_t> correlated;
               std::string corr = (*a).get_string("correlated");
               size_t pos = 0;
               while (pos < corr.size())
               {
                   size_t comma = corr.find(',', pos);
                   if (comma == std::string::npos) comma = corr.size();
                   if (comma > pos) correlated.push_back(uint32_t(std::stoul(corr.substr(pos, comma - pos), nullptr, 0)));
                   pos = comma + 1;
               }
               auto r = core->manager().raise(id, owner, text, correlated);
               if (!r.ok()) return CommandResult::from_error(r.error);
               JValue o = JValue::object();
               o.set("action", JValue(r->action));
               o.set("needs_snapshot", JValue(r->needs_snapshot));
               return CommandResult::good(o.dump());
           }});
    t.add({"ack", "acknowledge an active alarm", {{"id", true, "alarm id"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               auto r = core->manager().acknowledge(uint32_t((*a).get_number("id", 0)));
               return r.ok() ? CommandResult::good(std::string("{\"acknowledged\":true}")) : CommandResult::from_error(r.error);
           }});
    t.add({"clear", "clear an active alarm (owner or operator)", {{"id", true, "alarm id"}, {"owner", false, "owner for attribution"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               auto r = core->manager().clear(uint32_t((*a).get_number("id", 0)), (*a).get_string("owner"));
               return r.ok() ? CommandResult::good(std::string("{\"cleared\":true}")) : CommandResult::from_error(r.error);
           }});
    t.add({"escalation-policy-get", "show the escalation policy", {}, [core](const std::string&) {
               JValue arr = JValue::array();
               for (const auto& e : core->manager().escalation())
               {
                   JValue o = JValue::object();
                   o.set("alarm_id", JValue(double(e.alarm_id)));
                   o.set("promote_after_s", JValue(double(e.promote_after_s)));
                   o.set("promote_to", JValue(e.promote_to == common::SEV_CRITICAL ? "CRITICAL" : e.promote_to == common::SEV_MAJOR ? "MAJOR" : e.promote_to == common::SEV_MINOR ? "MINOR" : e.promote_to == common::SEV_WARNING ? "WARNING" : "INFO"));
                   arr.push(o);
               }
               return CommandResult::good(arr.dump());
           }});
    t.add({"escalation-policy-set", "set escalation rules (runtime; config via istate)", {{"json", true, "[{\"alarm_id\":0,\"promote_after_s\":3600,\"promote_to\":\"CRITICAL\"}]"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               auto j = json_parse((*a).get_string("json"));
               if (!j.ok() || !(*j).is_array()) return CommandResult::err(Errc::InvalidArgument, "json must be an array");
               std::vector<EscalationRule> rules;
               for (const auto& item : (*j).items())
               {
                   EscalationRule r;
                   r.alarm_id = uint32_t(item.find("alarm_id") ? item.find("alarm_id")->as_number() : 0);
                   r.promote_after_s = uint64_t(item.find("promote_after_s") ? item.find("promote_after_s")->as_number() : 3600);
                   std::string to = item.find("promote_to") ? item.find("promote_to")->as_string() : "CRITICAL";
                   r.promote_to = sev_from_name(to);
                   rules.push_back(r);
               }
               core->manager().set_escalation(rules);
               return CommandResult::good(std::string("{\"set\":true}"));
           }});
    t.add({"escalation-sweep", "run one escalation sweep now", {}, [core](const std::string&) {
               auto evs = core->manager().escalation_sweep(core->now_mono());
               JValue o = JValue::object();
               o.set("promoted", JValue(double(evs.size())));
               return CommandResult::good(o.dump());
           }});
    t.add({"catalog-merge", "merge new catalog entries (update regeneration)", {{"yaml", true, "catalog entries YAML"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               auto entries = load_catalog_entries((*a).get_string("yaml"));
               if (!entries.ok()) return CommandResult::from_error(entries.error);
               auto st = core->manager().merge_catalog(*entries);
               JValue o = JValue::object();
               o.set("added", JValue(double(st.added)));
               o.set("updated", JValue(double(st.updated)));
               o.set("preserved_operator_edits", JValue(double(st.preserved_operator_edits)));
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
AlarmsSpoke::AlarmsSpoke(AlarmsCore* core, Logger* log, const CommandTable* table, const std::string& hub_target, const std::string& build_version) : core_(core), log_(log), table_(table)
{
    using ChannelT = SpokeChannel<alarms::AlarmHub, alarms::AlarmSpokeFrame, alarms::AlarmHubFrame>;
    ChannelT::Options o;
    o.target = hub_target;
    o.daemon = "alarms";
    o.build_version = build_version;
    o.capabilities = {"state-machine", "escalation", "persistence", "snapshot-on-fault"};
    channel_ = std::make_unique<ChannelT>(std::move(o), *log);
    channel_->set_hub_handler([this](const alarms::AlarmHubFrame& hf) {
        if (hf.has_command())
        {
            common::Ack ack = dispatch_to_ack(table_, hf.command());
            channel_->send_ack(ack);
        }
    });
    // Alarm state changes go to istate directly as events (§7.6).
    core_->set_spoke_sink([this](const AlarmStateEvent& ev) { send_event(ev); });
    // Until a schema is installed, deliveries still apply — just unvalidated.
    // A packaging slip must not leave a daemon running unconfigured (R10).
    set_schema(JValue::object());
}

/// @brief Install the schema every config delivery must satisfy.
/// @param schema Schema every config delivery must satisfy.
void AlarmsSpoke::set_schema(JValue schema)
{
    config_sink_ = std::make_unique<ConfigSink>("alarms", std::move(schema), [this](const ConfigDocument& doc) { return core_->apply_config(doc); });
    channel_->set_config_handler([this](const common::ConfigDelivery& delivery) { return config_sink_->deliver(delivery.yaml(), delivery.revision()); });
}

/// @brief Start the transport channel.
void AlarmsSpoke::start()
{
    channel_->start();
}
/// @brief Stop the transport channel.
void AlarmsSpoke::stop()
{
    channel_->stop();
}

/// @brief Queue an alarm state event to istate.
/// @param ev Alarm state event to queue.
void AlarmsSpoke::send_event(const AlarmStateEvent& ev)
{
    channel_->enqueue([&ev](alarms::AlarmSpokeFrame& f) {
        auto* e = f.mutable_event();
        e->set_action(ev.action);
        e->set_alarm_id(ev.record.alarm_id);
        e->set_severity(ev.record.severity);
        e->set_owner(ev.record.owner);
        e->set_additional_text(ev.record.additional_text);
        e->set_specific_problem(ev.record.specific_problem);
        e->set_probable_cause(ev.record.probable_cause);
        e->set_count(ev.record.count);
        e->set_acknowledged(ev.record.acknowledged);
        e->set_needs_snapshot(ev.needs_snapshot);
        instr::transport::set_ts(e->mutable_first_seen(), ev.record.first_seen);
        instr::transport::set_ts(e->mutable_last_seen(), ev.record.last_seen);
        for (uint32_t c : ev.record.correlated) e->add_correlated(c);
    });
}

} // namespace alarms
} // namespace instr