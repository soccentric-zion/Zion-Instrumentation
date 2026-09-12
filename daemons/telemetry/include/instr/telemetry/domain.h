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
 * @file daemons/telemetry/include/instr/telemetry/domain.h
 * @brief aggregation from monitord, Hardware, Faults and applications;
 *
 * batch shaping; bounded retention with drop policy; per-device health score
 * with contributing factors; reproducibility metadata (REQ §7.8).
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <deque>
#include <optional>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "instr/common/error.h"
#include "instr/common/timestamps.h"

namespace instr
{
/// @brief Telemetry daemon: aggregation, health score and northbound batches (§7.8).
namespace telemetry
{
/// @brief Pure domain logic for this daemon: no transport, no sockets, fully unit-testable.
namespace domain
{

/// @brief A metric definition registered by an application (§7.8).
struct MetricDef
{
    std::string name;     ///< Metric name.
    std::string unit;     ///< Unit string.
    std::string type;     ///< gauge | counter.
    std::string app;      ///< Registering application.
};

/// @brief One aggregation-window record.
struct AggWindow
{
    std::string metric;                 ///< Metric name.
    std::string fn;                     ///< min / max / mean / count / last.
    double value = 0;                   ///< Aggregated value.
    uint32_t count = 0;                 ///< Samples aggregated.
    uint64_t window_start_mono_ns = 0;  ///< Window start.
    uint64_t window_end_mono_ns = 0;    ///< Window end.
    std::string unit;                   ///< Unit string.
    std::map<std::string, std::string> tags;   ///< Dimension tags.
};

/// @brief Device health score with its contributing factors.
struct HealthScoreValue
{
    double score = 100;                    ///< 0..100 composite score.
    std::map<std::string, double> factors;  ///< Per-factor contributions.
};

/// @brief Reproducibility metadata shipped with every batch (§9).
struct ReproMetadata
{
    std::string firmware_version;        ///< Build version.
    std::string config_schema_version;   ///< Config schema version.
    std::string hardware_revision;       ///< Board revision.
    std::string device_serial;           ///< Device identity.
};

/// @brief One aggregated metric record in a batch.
struct MetricRecord
{
    std::string metric;                 ///< Metric name.
    double value = 0;                   ///< Aggregated value.
    std::string unit;                   ///< Unit string.
    std::string agg_fn;                 ///< Aggregation function.
    uint64_t window_start_mono_ns = 0;  ///< Window start.
    uint64_t window_end_mono_ns = 0;    ///< Window end.
    std::map<std::string, std::string> tags;   ///< Dimension tags.
    uint32_t count = 0;                 ///< Samples aggregated.
};

/// @brief One northbound telemetry batch.
struct MetricBatch
{
    std::vector<MetricRecord> records;   ///< Aggregated records.
    HealthScoreValue health;             ///< Health score at send time.
    ReproMetadata repro;                 ///< Reproducibility metadata.
    uint64_t mono_ns = 0;                ///< Batch time.
};

/// @brief Application metric registration API: duplicates are loud (alarm-visible).
class MetricRegistry
{
  public:
    /// @brief Registration outcome.
    enum class Reg
    {
        Ok,          ///< Registered.
        Duplicate,   ///< Same name already registered (by another app).
        Invalid      ///< Malformed name/unit/type.
    };
    /// @brief Register a metric definition.
    /// @return The outcome (Duplicate and Invalid are loud, alarm-visible).
    /// @param name Metric name.
    /// @param unit Unit string.
    /// @param type gauge or counter.
    /// @param app Registering application.
    Reg register_metric(const std::string& name, const std::string& unit, const std::string& type, const std::string& app);
    /// @brief All registered metrics.
    /// @return All registered metrics.
    const std::map<std::string, MetricDef>& metrics() const
    {
        return metrics_;
    }
    /// @brief Look up a metric definition.
    /// @return Pointer to the def, or nullptr when unregistered.
    /// @param name Metric name to look up.
    const MetricDef* find(const std::string& name) const;

  private:
    std::map<std::string, MetricDef> metrics_;    ///< Registered metrics by name.
};

/// @brief Aggregation windows: functions min, max, mean, count, last.
class Aggregator
{
  public:
    /// @brief Ingest one raw sample into the open window.
    /// @param window_end_mono_ns Window close time.
    /// @param metric Metric name.
    /// @param value Sample value.
    /// @param mono_ns Sample time.
    /// @param tags Dimension tags.
    /// @return Records for each metric×function.
    void add(const std::string& metric, double value, uint64_t mono_ns, const std::map<std::string, std::string>& tags = {});
    /// @brief Close the current window and produce records for each metric×function.
    /// @param fns Aggregation functions to apply at close.
    /// @param window_end_mono_ns Window close time.
    /// @return Records for each metric×function.
    std::vector<AggWindow> close(uint64_t window_end_mono_ns);
    /// @brief Replace the aggregation function set.
    /// @param fns Aggregation functions to apply at close.
    void set_functions(const std::vector<std::string>& fns)
    {
        functions_ = fns;
    }

  private:
    /// @brief One raw sample awaiting aggregation.
    struct Sample
    {
        double v;                            ///< Value.
        uint64_t ns;                         ///< Timestamp.
        std::map<std::string, std::string> tags;   ///< Dimension tags.
    };
    std::map<std::string, std::deque<Sample>> current_;    ///< Open-window samples per metric.
    std::vector<std::string> functions_ = {"min", "max", "mean", "count", "last"};   ///< Functions applied at close.
    uint64_t window_start_ns_ = 0;    ///< Open-window start.
};

/// @brief Bounded local retention of unsent data; drop policy oldest-first, never faults or alarms.
class RetentionBuffer
{
  public:
    /// @brief Construct a buffer with a record bound.
    /// @param max_records Record bound.
    RetentionBuffer(size_t max_records) : max_records_(max_records)
    {
    }
    /// @brief Retention class of a batch.
    enum class Class
    {
        Telemetry,     ///< Droppable.
        Protected      ///< Never dropped.
    };
    /// @brief Bounded local retention of unsent data; oldest telemetry dropped first (§7.8, §11).
    ///
    /// @return The current record bound.
    /// Shrinking the bound drops immediately, never lazily.
    /// @param n New record bound (shrinking drops immediately).
    void set_max_records(size_t n);
    /// @brief Current record bound.
    size_t max_records() const
    {
        return max_records_;
    }
    /// @brief Enqueue a batch.
    /// @return All retained batches, oldest first.
    /// @param b Batch to enqueue.
    /// @param cls Droppable or protected.
    void push(const MetricBatch& b, Class cls);
    /// @brief Dropped telemetry batches since the last query.
    uint64_t dropped() const
    {
        return dropped_;
    }
    /// @brief Remove and return all retained batches, oldest first.
    /// @return All retained batches, oldest first.
    std::vector<MetricBatch> drain();
    /// @brief Batches currently retained.
    size_t size() const
    {
        return buf_.size();
    }

  private:
    /// @brief One retained batch with its class.
    struct Item
    {
        MetricBatch batch;    ///< The batch.
        Class cls;            ///< Droppable or protected.
    };
    std::deque<Item> buf_;          ///< Retained batches, oldest first.
    size_t max_records_;           ///< Record bound.
    uint64_t dropped_ = 0;         ///< Dropped telemetry counter.
};

/// @brief Health-score inputs rolled up from monitord + Hardware + Faults + Alarms.
struct HealthInputs
{
    double cpu_util_pct = 0;           ///< CPU utilisation.
    double mem_used_pct = 0;           ///< Memory pressure.
    double max_thermal_c = 0;          ///< Hottest zone.
    double storage_wear_pct = -1;      ///< Worst wear (-1 unknown).
    double storage_free_pct = 100;     ///< Free storage.
    uint32_t hardware_fails = 0;       ///< Recent check failures.
    double fault_rate_per_hour = 0;    ///< Fault rate.
    uint32_t alarms_critical = 0;      ///< Active critical alarms.
    uint32_t alarms_major = 0;         ///< Active major alarms.
    uint32_t alarms_minor = 0;         ///< Active minor alarms.
};

/// @brief Compute the composite health score with contributing factors exposed.
/// @param in Health inputs from the other daemons.
/// @return The composite score with per-factor contributions.
HealthScoreValue compute_health_score(const HealthInputs& in);

/// @brief Batch shaping: bounded records per batch, flush interval.
class BatchShaper
{
  public:
    /// @brief Construct a shaper with record bound and flush interval.
    /// @param max_records Record bound per batch.
    /// @param flush_interval_s Flush cadence in seconds.
    BatchShaper(size_t max_records, double flush_interval_s) : max_records_(max_records), flush_interval_s_(flush_interval_s)
    {
    }
    /// @brief Ingest a raw metric sample.
    ///
    /// When the batch is full or the interval elapsed, emits a shaped batch of
    /// aggregation windows.
    ///
    /// @return The emitted batch, or std::nullopt while accumulating.
    /// @param metric Metric name.
    /// @param value Sample value.
    /// @param unit Unit string.
    /// @param mono_ns Sample time.
    /// @param tags Dimension tags.
    std::optional<MetricBatch> ingest(const std::string& metric, double value, const std::string& unit, uint64_t mono_ns, const std::map<std::string, std::string>& tags);
    /// @brief Force a flush of the open window (interval elapsed or shutdown).
    /// @param now_mono_ns Current monotonic time.
    /// @return The flushed batch, or std::nullopt when nothing pending.
    std::optional<MetricBatch> flush(uint64_t now_mono_ns);
    /// @brief Attach a health score to the next emitted batch.
    /// @param h Health score for the next batch.
    void set_health(const HealthScoreValue& h)
    {
        pending_health_ = h;
    }
    /// @brief Retune the record bound when configuration arrives (§7.8, §9).
    /// @param n New record bound.
    void set_max_records(size_t n)
    {
        max_records_ = n;
    }
    /// @brief Retune the flush interval.
    /// @param s New flush interval in seconds.
    void set_flush_interval_s(double s)
    {
        flush_interval_s_ = s;
    }
    /// @brief Current flush interval.
    double flush_interval_s() const
    {
        return flush_interval_s_;
    }
    /// @brief Current record bound.
    size_t max_records() const
    {
        return max_records_;
    }
    /// @brief Attach reproducibility metadata to emitted batches.
    /// @param m Metadata to attach to emitted batches.
    void set_repro(const ReproMetadata& m)
    {
        repro_ = m;
    }
    /// @brief Samples awaiting aggregation.
    size_t pending_samples() const
    {
        return total_pending_;
    }

  private:
    Aggregator agg_;                   ///< Window aggregation.
    size_t max_records_;               ///< Record bound per batch.
    double flush_interval_s_;          ///< Flush cadence.
    uint64_t last_flush_ns_ = 0;       ///< Last flush time.
    uint64_t window_start_ns_ = 0;     ///< Open-window start.
    size_t total_pending_ = 0;         ///< Samples awaiting aggregation.
    HealthScoreValue pending_health_;  ///< Health attached to the next batch.
    ReproMetadata repro_;              ///< Metadata attached to emitted batches.
};

} // namespace domain
} // namespace telemetry
} // namespace instr