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
 * @file daemons/telemetry/src/domain.cpp
 * @brief telemetry domain implementation (REQ §7.8): metric registry, aggregation, batch shaping, bounded retention with drop policy, and per-device health scoring.
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/telemetry/domain.h"

#include <algorithm>
#include <cmath>

namespace instr
{
/// @brief Telemetry daemon: aggregation, health score and northbound batches (§7.8).
namespace telemetry
{
/// @brief Pure domain logic for this daemon: no transport, no sockets, fully unit-testable.
namespace domain
{

/// @brief Register a metric definition; duplicates and invalid defs are loud.
/// @param name Metric name.
/// @param unit Unit string.
/// @param type gauge or counter.
/// @param app Registering application.
/// @return The registration outcome.
MetricRegistry::Reg MetricRegistry::register_metric(const std::string& name, const std::string& unit, const std::string& type, const std::string& app)
{
    if (name.empty() || (type != "gauge" && type != "counter")) return Reg::Invalid;
    if (metrics_.count(name))
    {
        // Conflicting re-registration is an alarm-visible event, not a silent
        // overwrite (tlm_registration_conflict).
        return Reg::Duplicate;
    }
    MetricDef d;
    d.name = name;
    d.unit = unit;
    d.type = type;
    d.app = app;
    metrics_[name] = d;
    return Reg::Ok;
}

/// @brief Look up a metric definition.
/// @param name Metric name to look up.
/// @return Pointer to the definition, or nullptr.
const MetricDef* MetricRegistry::find(const std::string& name) const
{
    auto it = metrics_.find(name);
    return it == metrics_.end() ? nullptr : &it->second;
}

/// @brief Ingest one raw sample into the open window.
/// @param metric Metric name.
/// @param value Sample value.
/// @param mono_ns Sample time.
/// @param tags Dimension tags.
void Aggregator::add(const std::string& metric, double value, uint64_t mono_ns, const std::map<std::string, std::string>& tags)
{
    auto& q = current_[metric];
    if (q.empty()) window_start_ns_ = mono_ns;
    q.push_back(Sample{value, mono_ns, tags});
    if (window_start_ns_ == 0) window_start_ns_ = mono_ns;
}

/// @brief Close the window and aggregate per metric×function.
/// @param window_end_mono_ns Window close time.
/// @return Records for each metric×function.
std::vector<AggWindow> Aggregator::close(uint64_t window_end_mono_ns)
{
    std::vector<AggWindow> out;
    for (auto& kv : current_)
    {
        const auto& samples = kv.second;
        if (samples.empty()) continue;
        double mn = samples.front().v, mx = samples.front().v, sum = 0;
        for (const auto& s : samples)
        {
            mn = std::min(mn, s.v);
            mx = std::max(mx, s.v);
            sum += s.v;
        }
        double mean = sum / double(samples.size());
        double last = samples.back().v;
        for (const auto& fn : functions_)
        {
            AggWindow w;
            w.metric = kv.first;
            w.window_start_mono_ns = window_start_ns_;
            w.window_end_mono_ns = window_end_mono_ns;
            w.tags = samples.front().tags;
            w.count = uint32_t(samples.size());
            if (fn == "min")
            {
                w.fn = "min";
                w.value = mn;
            }
            else if (fn == "max")
            {
                w.fn = "max";
                w.value = mx;
            }
            else if (fn == "mean")
            {
                w.fn = "mean";
                w.value = mean;
            }
            else if (fn == "count")
            {
                w.fn = "count";
                w.value = double(samples.size());
            }
            else if (fn == "last")
            {
                w.fn = "last";
                w.value = last;
            }
            out.push_back(w);
        }
        kv.second.clear();
    }
    window_start_ns_ = 0;
    return out;
}

/// @brief Enqueue a batch, dropping oldest telemetry when full.
/// @param b Batch to enqueue.
/// @param cls Droppable or protected.
void RetentionBuffer::push(const MetricBatch& b, Class cls)
{
    buf_.push_back(Item{b, cls});
    // Drop policy: oldest-first, never protected classes (§7.8/§11). Protected
    // batches may exceed the bound temporarily — they are small and acked by
    // HQ quickly; telemetry is what the bound exists to shed.
    while (buf_.size() > max_records_)
    {
        bool dropped_one = false;
        for (auto it = buf_.begin(); it != buf_.end(); ++it)
        {
            if (it->cls == Class::Telemetry)
            {
                buf_.erase(it);
                ++dropped_;
                dropped_one = true;
                break;
            }
        }
        if (!dropped_one) break; // only protected remain — never drop them
    }
}

/// @brief Remove and return all retained batches.
/// @return All retained batches, oldest first.
std::vector<MetricBatch> RetentionBuffer::drain()
{
    std::vector<MetricBatch> out;
    for (auto& i : buf_) out.push_back(i.batch);
    buf_.clear();
    return out;
}

/// @brief Compute the composite health score with per-factor contributions.
/// @param in Health inputs from the other daemons.
/// @return The composite score with per-factor contributions.
HealthScoreValue compute_health_score(const HealthInputs& in)
{
    // Starts at 100; each factor subtracts its penalty, floored at 0.
    HealthScoreValue h;
    double cpu_pen = std::max(0.0, (in.cpu_util_pct - 80.0)) * 1.5; // saturating above 80%
    double mem_pen = std::max(0.0, (in.mem_used_pct - 85.0)) * 2.0;
    double thermal_pen = std::max(0.0, (in.max_thermal_c - 75.0)) * 3.0;
    double wear_pen = in.storage_wear_pct >= 0 ? std::max(0.0, (in.storage_wear_pct - 70.0)) * 2.0 : 0;
    double storage_pen = std::max(0.0, (90.0 - in.storage_free_pct)) * 1.0;
    double hw_pen = double(in.hardware_fails) * 15.0;
    double fault_pen = std::min(40.0, in.fault_rate_per_hour * 4.0);
    double alarm_pen = double(in.alarms_critical) * 25.0 + double(in.alarms_major) * 10.0 + double(in.alarms_minor) * 3.0;
    double total = cpu_pen + mem_pen + thermal_pen + wear_pen + storage_pen + hw_pen + fault_pen + alarm_pen;
    h.score = std::max(0.0, 100.0 - std::min(100.0, total));
    h.factors["cpu"] = cpu_pen;
    h.factors["memory"] = mem_pen;
    h.factors["thermal"] = thermal_pen;
    h.factors["storage_wear"] = wear_pen;
    h.factors["storage_free"] = storage_pen;
    h.factors["hardware"] = hw_pen;
    h.factors["faults"] = fault_pen;
    h.factors["alarms"] = alarm_pen;
    return h;
}

/// @brief Ingest a sample; emit a batch when full or the interval elapsed.
/// @param metric Metric name.
/// @param value Sample value.
/// @param unit Unit string.
/// @param mono_ns Sample time.
/// @param tags Dimension tags.
/// @return The emitted batch, or std::nullopt while accumulating.
std::optional<MetricBatch> BatchShaper::ingest(const std::string& metric, double value, const std::string& unit, uint64_t mono_ns, const std::map<std::string, std::string>& tags)
{
    agg_.add(metric, value, mono_ns, tags);
    (void)unit;
    ++total_pending_;
    uint64_t window_end = window_start_ns_ + uint64_t(flush_interval_s_ * 1e9);
    bool full = total_pending_ >= max_records_;
    bool due = last_flush_ns_ != 0 && mono_ns >= window_end;
    if (!full && !due) return std::nullopt;
    auto records = agg_.close(mono_ns);
    MetricBatch b;
    for (const auto& w : records)
    {
        MetricRecord r;
        r.metric = w.metric;
        r.value = w.value;
        r.unit = w.unit;
        r.agg_fn = w.fn;
        r.window_start_mono_ns = w.window_start_mono_ns;
        r.window_end_mono_ns = w.window_end_mono_ns;
        r.tags = w.tags;
        r.count = w.count;
        b.records.push_back(r);
    }
    b.health = pending_health_;
    b.repro = repro_;
    b.mono_ns = mono_ns;
    last_flush_ns_ = mono_ns;
    total_pending_ = 0;
    return b;
}

/// @brief Force a flush of the open aggregation window.
/// @param now_mono_ns Current monotonic time.
/// @return The flushed batch, or std::nullopt when nothing pending.
std::optional<MetricBatch> BatchShaper::flush(uint64_t now_mono_ns)
{
    if (total_pending_ == 0) return std::nullopt;
    auto records = agg_.close(now_mono_ns);
    if (records.empty()) return std::nullopt;
    MetricBatch b;
    for (const auto& w : records)
    {
        MetricRecord r;
        r.metric = w.metric;
        r.value = w.value;
        r.agg_fn = w.fn;
        r.window_start_mono_ns = w.window_start_mono_ns;
        r.window_end_mono_ns = w.window_end_mono_ns;
        r.tags = w.tags;
        r.count = w.count;
        b.records.push_back(r);
    }
    b.health = pending_health_;
    b.repro = repro_;
    b.mono_ns = now_mono_ns;
    total_pending_ = 0;
    last_flush_ns_ = now_mono_ns;
    return b;
}

} // namespace domain
} // namespace telemetry
} // namespace instr
namespace instr
{
/// @brief Telemetry daemon: aggregation, health score and northbound batches (§7.8).
namespace telemetry
{
/// @brief Pure domain logic for this daemon: no transport, no sockets, fully unit-testable.
namespace domain
{

/// @brief Shrink the retention bound (drops immediately, never lazily).
/// @param n New record bound (shrinking drops immediately).
void RetentionBuffer::set_max_records(size_t n)
{
    max_records_ = n;
    // Apply the new bound now: telemetry first, protected classes never.
    while (buf_.size() > max_records_)
    {
        bool dropped_one = false;
        for (auto it = buf_.begin(); it != buf_.end(); ++it)
        {
            if (it->cls == Class::Telemetry)
            {
                buf_.erase(it);
                ++dropped_;
                dropped_one = true;
                break;
            }
        }
        if (!dropped_one) break; // only protected entries remain: keep them all
    }
}

} // namespace domain
} // namespace telemetry
} // namespace instr
