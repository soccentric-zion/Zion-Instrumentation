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
 * @file daemons/monitord/src/domain.cpp
 * @brief monitord domain implementation (REQ §7.3): class-based sampling intervals, observation-only resource records, and the high-resolution window.
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/monitord/domain.h"

namespace instr
{
/// @brief monitord: passive resource sampling and wear projection (§7.3).
namespace monitord
{
/// @brief Pure domain logic for this daemon: no transport, no sockets, fully unit-testable.
namespace domain
{

/// @brief Effective sampling interval for a resource class.
/// @param resource_class Resource class to query.
/// @return The effective interval in seconds.
double Sampler::interval_for(const std::string& resource_class) const
{
    // Called from the sampler thread every pass; the setters run on the
    // configuration thread. See intervals_mu_ in the header.
    std::lock_guard<std::mutex> lk(intervals_mu_);
    auto it = intervals_.find(resource_class);
    return it == intervals_.end() ? default_interval_ : it->second;
}

/// @brief Append a sample and bump the produced counter.
/// @param v Vector to append to.
/// @param s Sample to append.
static void push(std::vector<MonitorSample>* v, const MonitorSample& s)
{
    v->push_back(s);
}

/// @brief One sampling pass: rate deltas, gauges and wear indicators.
/// @return Samples produced this tick.
std::vector<MonitorSample> Sampler::tick()
{
    std::vector<MonitorSample> out;
    uint64_t now = ts_.mono_ns();

    // CPU: per-core + aggregate utilisation, frequency, governor.
    platform::CpuSnapshot cpu = plat_.cpu_snapshot();
    if (have_prev_cpu_ && cpu.busy_ticks.size() == prev_cpu_.busy_ticks.size())
    {
        double busy_total = 0, total_total = 0;
        for (size_t i = 0; i < cpu.busy_ticks.size(); ++i)
        {
            uint64_t busy = cpu.busy_ticks[i] - prev_cpu_.busy_ticks[i];
            uint64_t total = cpu.total_ticks[i] - prev_cpu_.total_ticks[i];
            double util = total > 0 ? double(busy) / double(total) * 100.0 : 0;
            busy_total += busy;
            total_total += total;
            MonitorSample s;
            s.metric = "cpu.util.core" + std::to_string(i);
            s.value = util;
            s.unit = "percent";
            s.resource_class = "cpu";
            s.mono_ns = now;
            push(&out, s);
        }
        MonitorSample agg;
        agg.metric = "cpu.util";
        agg.value = total_total > 0 ? busy_total / total_total * 100.0 : 0;
        agg.unit = "percent";
        agg.resource_class = "cpu";
        agg.mono_ns = now;
        push(&out, agg);
    }
    prev_cpu_ = cpu;
    have_prev_cpu_ = true;
    for (size_t i = 0; i < cpu.freq_mhz.size(); ++i)
    {
        MonitorSample s;
        s.metric = "cpu.freq_mhz.core" + std::to_string(i);
        s.value = cpu.freq_mhz[i];
        s.unit = "MHz";
        s.resource_class = "cpu";
        s.mono_ns = now;
        push(&out, s);
    }
    if (!cpu.governor.empty())
    {
        MonitorSample s;
        s.metric = "cpu.governor_state";
        s.value = cpu.governor == "performance" ? 1 : 0;
        s.unit = "enum";
        s.tags["governor"] = cpu.governor;
        s.resource_class = "cpu";
        s.mono_ns = now;
        push(&out, s);
    }

    // Memory and swap.
    platform::MemInfo mem = plat_.memory_info();
    if (mem.total_bytes > 0)
    {
        MonitorSample s;
        s.metric = "mem.used_pct";
        s.value = (1.0 - double(mem.available_bytes) / double(mem.total_bytes)) * 100.0;
        s.unit = "percent";
        s.resource_class = "memory";
        s.mono_ns = now;
        push(&out, s);
        if (mem.swap_total_bytes > 0)
        {
            MonitorSample sw;
            sw.metric = "mem.swap_used_pct";
            sw.value = (1.0 - double(mem.swap_free_bytes) / double(mem.swap_total_bytes)) * 100.0;
            sw.unit = "percent";
            sw.resource_class = "memory";
            sw.mono_ns = now;
            push(&out, sw);
        }
    }

    // Storage capacity / free space / wear indicators.
    for (const auto& d : plat_.storage_devices())
    {
        MonitorSample w;
        w.metric = "storage.wear_pct";
        w.value = d.wear_pct;
        w.unit = "percent";
        w.tags["device"] = d.name;
        w.tags["kind"] = d.kind;
        w.resource_class = "storage";
        w.mono_ns = now;
        if (d.wear_pct >= 0) push(&out, w);
        if (d.lifetime_days_estimate > 0)
        {
            MonitorSample life;
            life.metric = "storage.lifetime_days";
            life.value = double(d.lifetime_days_estimate);
            life.unit = "days";
            life.tags["device"] = d.name;
            life.resource_class = "storage";
            life.mono_ns = now;
            push(&out, life);
        }
    }

    // Thermal zones.
    for (const auto& z : plat_.thermal_zones())
    {
        MonitorSample s;
        s.metric = "thermal." + z.id;
        s.value = z.temp_c;
        s.unit = "celsius";
        s.tags["label"] = z.label;
        s.resource_class = "thermal";
        s.mono_ns = now;
        push(&out, s);
    }

    // Power sources (high-res window interest).
    for (const auto& p : plat_.power_sources())
    {
        MonitorSample s;
        s.metric = "power." + p.id;
        s.value = p.percent;
        s.unit = "percent";
        s.tags["kind"] = p.kind;
        s.tags["charging"] = p.charging ? "true" : "false";
        s.resource_class = "thermal"; // power window shares the thermal cadence
        s.mono_ns = now;
        push(&out, s);
    }

    // Load / processes / uptime / boot count.
    MonitorSample la;
    la.metric = "load.avg1m";
    la.value = plat_.load_average_1m();
    la.unit = "load";
    la.resource_class = "load";
    la.mono_ns = now;
    push(&out, la);

    MonitorSample pc;
    pc.metric = "sys.processes";
    pc.value = double(plat_.process_count());
    pc.unit = "count";
    pc.resource_class = "system";
    pc.mono_ns = now;
    push(&out, pc);

    MonitorSample up;
    up.metric = "sys.uptime_s";
    up.value = double(plat_.uptime_s());
    up.unit = "seconds";
    up.resource_class = "system";
    up.mono_ns = now;
    push(&out, up);

    MonitorSample bc;
    bc.metric = "sys.boot_count";
    bc.value = double(plat_.boot_count());
    bc.unit = "count";
    bc.resource_class = "system";
    bc.mono_ns = now;
    push(&out, bc);

    stats_.produced += out.size();
    return out;
}

/// @brief Append a sample and evict entries older than the window.
/// @param mono_ns Sample time.
/// @param s Sample to append.
void RollingWindow::push(uint64_t mono_ns, const MonitorSample& s)
{
    buf_.push_back(Entry{mono_ns, s});
    // Trim entries older than the window.
    while (!buf_.empty() && buf_.front().mono_ns + window_ns_ < mono_ns) buf_.pop_front();
}

/// @brief Samples within the window, oldest first.
/// @return Samples within the window, oldest first.
std::vector<MonitorSample> RollingWindow::window() const
{
    std::vector<MonitorSample> out;
    for (const auto& e : buf_) out.push_back(e.sample);
    return out;
}

/// @brief Project remaining life from wear indicators.
/// @param d Device with wear indicators.
/// @param age_days Device age so far in days.
/// @return The wear projection.
WearProjector::Projection WearProjector::project(const platform::StorageDevice& d, uint64_t age_days)
{
    Projection p;
    p.device = d.name;
    p.wear_pct = d.wear_pct;
    if (d.wear_pct < 0)
    {
        // Indicator unavailable: conservative 10-year floor so an alarm can be
        // raised before failure rather than never.
        p.remaining_days = 3650;
        p.model = "no-indicator-fallback";
        return p;
    }
    // Measured wear rate: wear% per day, projected linearly to 100%.
    double rate = age_days > 0 ? d.wear_pct / double(age_days) : 0;
    if (rate <= 0)
    {
        p.remaining_days = 3650;
        p.model = "zero-rate";
        return p;
    }
    double days = (100.0 - d.wear_pct) / rate;
    if (days < 0) days = 0;
    p.remaining_days = uint32_t(days);
    p.model = "linear-wear-rate";
    return p;
}

} // namespace domain
} // namespace monitord
} // namespace instr