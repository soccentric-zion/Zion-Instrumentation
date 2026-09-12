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
 * @file daemons/monitord/src/transport.cpp
 * @brief monitord core facade implementation: sampler, high-resolution window, and schedule state wired into the shared command table (REQ §3.2, §7.3).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/monitord/transport.h"

#include "instr/monitord/table.h"

namespace instr
{
/// @brief monitord: passive resource sampling and wear projection (§7.3).
namespace monitord
{

/// @brief Apply a resource profile to the sampler intervals.
/// @param p Resource profile to apply.
void MonitorCore::configure(const platform::ResourceProfile& p)
{
    sampler_.set_default_interval_s(p.sample_interval_default_s);
    for (const auto& kv : p.sample_intervals_per_class_s) sampler_.set_interval_for(kv.first, kv.second);
    window_ = RollingWindow(p.highres_window_s, p.highres_interval_s);
}

/// @brief One sampling pass: sample, retain locally and emit.
/// @return Samples produced by this pass.
std::vector<MonitorSample> MonitorCore::sample_once()
{
    auto samples = sampler_.tick();
    uint64_t now = ts_.mono_ns();
    {
        std::lock_guard<std::mutex> lk(mu_);
        // High-res rolling window: fine-grained classes retained locally (§7.3).
        for (const auto& s : samples)
        {
            if (s.resource_class == "thermal" || s.resource_class == "load") window_.push(now, s);
        }
        if (emit_)
        {
            auto cb = emit_;
            for (const auto& s : samples) cb(s);
        }
    }
    return samples;
}

/// @brief Wear projections for every storage device.
/// @param age_days Device age so far in days.
/// @return Wear projections for every storage device.
std::vector<WearProjector::Projection> MonitorCore::wear_projections(uint64_t age_days) const
{
    std::vector<WearProjector::Projection> out;
    for (const auto& d : plat_.storage_devices()) out.push_back(WearProjector::project(d, age_days));
    return out;
}

/// @brief Render the current status as JSON.
/// @return The current status as JSON.
std::string MonitorCore::status_json() const
{
    JValue v = JValue::object();
    v.set("interval_default_s", JValue(sampler_.interval_for("default")));
    JValue iv = JValue::object();
    for (const char* cls : {"cpu", "memory", "storage", "thermal", "load", "system"}) iv.set(cls, JValue(sampler_.interval_for(cls)));
    v.set("intervals_per_class_s", iv);
    v.set("samples_produced", JValue(double(sampler_.stats().produced)));
    v.set("window_entries", JValue(double(window_.size())));
    return v.dump();
}

// ---------------- command table ----------------

/// @brief Apply an istate-delivered config document.
/// @param doc Parsed, schema-validated document.
/// @return ok(), or an error describing why the values were refused.
Error MonitorCore::apply_config(const ConfigDocument& doc)
{
    Config c;
    c.enabled = doc.get_bool("enabled", true);
    c.interval = doc.get_double("interval", 5.0);
    c.thermal_warn_c = doc.get_double("thermal_warn_c", 75.0);
    c.thermal_critical_c = doc.get_double("thermal_critical_c", 85.0);
    c.storage_warn_pct = doc.get_double("storage_warn_pct", 90.0);
    c.wear_projection_threshold_days = uint32_t(doc.get_int("wear_projection_threshold_days", 180));
    c.highres_window_s = doc.get_double("highres_window_s", 60.0);
    c.highres_interval_s = doc.get_double("highres_interval_s", 1.0);
    if (c.interval <= 0) return Error::make(Errc::InvalidArgument, "interval must be > 0", "monitord config");
    if (c.thermal_critical_c <= c.thermal_warn_c) return Error::make(Errc::InvalidArgument, "thermal_critical_c must exceed thermal_warn_c", "monitord config");
    for (const char* cls : {"cpu", "memory", "storage", "thermal", "load", "system"})
    {
        const std::string key = std::string("intervals_per_class.") + cls;
        if (doc.has(key))
        {
            double v = doc.get_double(key, c.interval);
            if (v <= 0) return Error::make(Errc::InvalidArgument, std::string("interval for class ") + cls + " must be > 0", "monitord config");
            c.intervals_per_class[cls] = v;
        }
    }

    std::lock_guard<std::mutex> lk(mu_);
    cfg_ = c;
    sampler_.set_default_interval_s(c.interval);
    for (const auto& kv : c.intervals_per_class) sampler_.set_interval_for(kv.first, kv.second);
    window_ = RollingWindow(c.highres_window_s, c.highres_interval_s);
    if (on_config_) on_config_(cfg_);
    return Error::none();
}

/// @brief Build the monitord ctrl command table.
/// @param core Domain core the table reads from.
/// @return The command table.
CommandTable build_monitor_table(MonitorCore* core)
{
    CommandTable t;
    t.add({"status", "show sampler status and intervals", {}, [core](const std::string&) {
               if (!core) return CommandResult::err(Errc::Unavailable, "tool dispatches remotely");
               return CommandResult::good(core->status_json());
           }});
    t.add({"sample-once", "take one sampling pass and print the samples", {}, [core](const std::string&) {
               if (!core) return CommandResult::err(Errc::Unavailable, "tool dispatches remotely");
               auto samples = core->sample_once();
               JValue arr = JValue::array();
               for (const auto& s : samples)
               {
                   JValue o = JValue::object();
                   o.set("metric", JValue(s.metric));
                   o.set("value", JValue(s.value));
                   o.set("unit", JValue(s.unit));
                   o.set("class", JValue(s.resource_class));
                   arr.push(o);
               }
               return CommandResult::good(arr.dump());
           }});
    t.add({"set-interval", "set the sampling interval for a resource class", {{"class", true, "resource class: cpu|memory|storage|thermal|load|system"}, {"seconds", true, "sampling interval in seconds"}}, [core](const std::string& args_json) {
               if (!core) return CommandResult::err(Errc::Unavailable, "tool dispatches remotely");
               auto a = args_object(args_json);
               if (!a.ok()) return CommandResult::from_error(a.error);
               std::string cls = (*a).get_string("class");
               double secs = (*a).get_number("seconds");
               if (cls.empty() || secs <= 0) return CommandResult::err(Errc::InvalidArgument, "class and positive seconds required");
               core->sampler().set_interval_for(cls, secs);
               return CommandResult::good(std::string("{\"set\":true}"));
           }});
    t.add({"wear", "storage wear projections for all devices", {}, [core](const std::string&) {
               if (!core) return CommandResult::err(Errc::Unavailable, "tool dispatches remotely");
               auto p = core->wear_projections(365);
               JValue arr = JValue::array();
               for (const auto& w : p)
               {
                   JValue o = JValue::object();
                   o.set("device", JValue(w.device));
                   o.set("wear_pct", JValue(w.wear_pct));
                   o.set("remaining_days", JValue(double(w.remaining_days)));
                   o.set("model", JValue(w.model));
                   arr.push(o);
               }
               return CommandResult::good(arr.dump());
           }});
    return t;
}

} // namespace monitord
} // namespace instr