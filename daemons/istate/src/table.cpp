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
 * @file daemons/istate/src/table.cpp
 * @brief istate command table (REQ §3.2): full coverage of the hub's operations dispatched to the shared IstateCore.
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/istate/table.h"

#include <future>

namespace instr
{
/// @brief istate: /pst owner, spoke orchestrator and HQ northbound bridge (R4, §7.1).
namespace istate
{

/// @brief Build the istate ctrl command table.
/// @param core istate core the table reads from.
/// @return The command table.
CommandTable build_istate_table(IstateCore* core)
{
    CommandTable t;
    t.add({"status", "hub status: provisioning, spokes, spool, HQ session", {}, [core](const std::string&) {
               JValue o = JValue::object();
               o.set("provisioning", JValue(core->provisioner().state_name(core->provisioner().state())));
               o.set("hostname", JValue(core->provisioner().identity().hostname));
               o.set("spool_records", JValue(double(core->northbound_spool().size())));
               o.set("hq_state", JValue(HqSession::state_name(core->hq_session().state())));
               o.set("degraded", JValue(core->orchestrator().degraded()));
               o.set("timeline_events", JValue(double(core->timeline().size())));
               return CommandResult::good(o.dump());
           }});
    t.add({"spokes", "list spokes with state and liveness", {}, [core](const std::string&) {
               auto j = json_parse(core->orchestrator().status_json());
               return j.ok() ? CommandResult::good((*j).dump()) : CommandResult::err(Errc::Internal, "status render failed");
           }});
    t.add({"config-get", "effective config for a daemon (drift report)", {{"daemon", true, "daemon name"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               return CommandResult::good(core->config_service().drift_report_json((*a).get_string("daemon")));
           }});
    t.add({"config-set", "set config values (origin rules apply; runtime + persistent)", {{"daemon", true, "daemon name"}, {"yaml", true, "config YAML"}, {"origin", false, "local|hq|app"}, {"force", false, "overwrite local keys"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               std::string daemon = (*a).get_string("daemon");
               auto o = origin_from_name((*a).get_string("origin", "local"));
               if (o.ok())
               {
                   // only caller-settable origins are local/app; hq requires signature
                   if (*o == Origin::Hq) return CommandResult::err(Errc::SignatureInvalid, "HQ config must arrive via apply_signed");
               }
               else { return CommandResult::from_error(o.error); }
               auto r = core->config_service().set(daemon, (*a).get_string("yaml"), *o, (*a).get_bool("force"));
               if (!r.ok) return CommandResult::err(Errc::InvalidArgument, r.error);
               core->audit().append("istate-ctrl", "local-console", "config_set", "{\"daemon\":\"" + daemon + "\"}", core->clock());
               core->audit().persist();
               JValue out = JValue::object();
               JValue arr = JValue::array();
               for (const auto& p : r.rejected) arr.push(JValue(p));
               out.set("rejected", arr);
               return CommandResult::good(out.dump());
           }});
    t.add({"timeline", "ordered event timeline", {{"since_seq", false, "sequence"}}, [core](const std::string& args) {
               auto a = args_object(args);
               uint64_t since = a.ok() ? uint64_t((*a).get_number("since_seq", 0)) : 0;
               JValue arr = JValue::array();
               for (const auto& ev : core->timeline().since(since))
               {
                   JValue o = JValue::object();
                   o.set("seq", JValue(double(ev.seq)));
                   o.set("kind", JValue(ev.kind));
                   o.set("actor", JValue(ev.actor));
                   o.set("detail", JValue(ev.detail_json));
                   arr.push(o);
               }
               return CommandResult::good(arr.dump());
           }});
    t.add({"budgets", "per-consumer storage budget status", {}, [core](const std::string&) { return CommandResult::good(core->budgets_json()); }});
    t.add({"spool-stats", "northbound spool statistics", {}, [core](const std::string&) {
               const auto& s = core->northbound_spool().stats();
               JValue o = JValue::object();
               o.set("count", JValue(double(core->northbound_spool().size())));
               o.set("dropped_telemetry", JValue(double(s.dropped_telemetry)));
               o.set("dropped_logref", JValue(double(s.dropped_logref)));
               o.set("dropped_event", JValue(double(s.dropped_event)));
               o.set("refused_protected", JValue(double(s.refused_protected)));
               return CommandResult::good(o.dump());
           }});
    t.add({"drain", "drain the northbound spool toward HQ now", {}, [core](const std::string&) {
               auto rep = core->drain_northbound();
               JValue o = JValue::object();
               o.set("sent", JValue(double(rep.sent)));
               o.set("paused", JValue(rep.paused));
               return CommandResult::good(o.dump());
           }});
    t.add({"audit", "audit trail (who changed what, when, from where)", {}, [core](const std::string&) {
               JValue arr = JValue::array();
               for (const auto& e : core->audit().entries())
               {
                   JValue o = JValue::object();
                   o.set("seq", JValue(double(e.seq)));
                   o.set("wall_unix_s", JValue(double(e.wall_unix_s)));
                   o.set("actor", JValue(e.actor));
                   o.set("source", JValue(e.source));
                   o.set("action", JValue(e.action));
                   o.set("detail", JValue(e.detail_json));
                   arr.push(o);
               }
               return CommandResult::good(arr.dump());
           }});
    t.add({"reset", "system reset (wipes system/ + state/, reseeds defaults)", {{"confirm", false, "must be 'SYSTEM-RESET'"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               if ((*a).get_string("confirm") != "SYSTEM-RESET") return CommandResult::err(Errc::InvalidArgument, "confirm=SYSTEM-RESET required");
               auto r = core->system_reset("local-console");
               return r.ok() ? CommandResult::good(std::string("{\"reset\":true}")) : CommandResult::from_error(r.error);
           }});
    t.add({"factory-reset", "factory reset (manufacturing only, explicitly gated)", {{"confirm", true, "must be 'FACTORY-RESET'"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               auto r = core->factory_reset("local-console", (*a).get_string("confirm"));
               return r.ok() ? CommandResult::good(std::string("{\"reset\":true}")) : CommandResult::from_error(r.error);
           }});
    t.add({"time-policy", "current time policy and lock state", {}, [core](const std::string&) {
               JValue o = JValue::object();
               o.set("source", JValue(core->time_policy().current_source));
               o.set("locked", JValue(core->time_policy().locked));
               o.set("offset_ms", JValue(core->time_policy().offset_ms));
               JValue prio = JValue::array();
               for (const auto& s : core->time_policy().source_priority) prio.push(JValue(s));
               o.set("source_priority", prio);
               return CommandResult::good(o.dump());
           }});
    t.add({"system-info", "device identity and system info", {}, [core](const std::string&) { return CommandResult::good(core->system_info_json()); }});
    t.add({"relay", "pass a command to a spoke (full coverage passthrough)", {{"daemon", true, "target spoke"}, {"op", true, "op name"}, {"args", false, "JSON args"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               common::Command cmd;
               cmd.set_op((*a).get_string("op"));
               cmd.set_json_args((*a).get_string("args", "{}"));
               cmd.set_issued_by("istate-ctrl");
               auto fut = core->relay_command((*a).get_string("daemon"), cmd);
               if (fut.wait_for(std::chrono::seconds(5)) != std::future_status::ready) return CommandResult::err(Errc::Unavailable, "spoke not responsive");
               auto j = json_parse(fut.get());
               return j.ok() ? CommandResult::good((*j).dump()) : CommandResult::err(Errc::Corrupt, "bad ack");
           }});
    return t;
}

} // namespace istate
} // namespace instr
