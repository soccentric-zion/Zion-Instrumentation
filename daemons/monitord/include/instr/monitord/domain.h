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
 * @file daemons/monitord/include/instr/monitord/domain.h
 * @brief system resource monitoring domain (REQ §7.3)
 *
 * Observation only; raises no alarms. Pure domain logic: no sockets, no
 * direct hardware — platform access via IPlatform (testable, simulated).
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <mutex>
#include <map>
#include <string>
#include <vector>

#include "instr/common/timestamps.h"
#include "instr/platform/platform.h"

namespace instr
{
/// @brief monitord: passive resource sampling and wear projection (§7.3).
namespace monitord
{
/// @brief Pure domain logic for this daemon: no transport, no sockets, fully unit-testable.
namespace domain
{

/// @brief One measured metric sample produced by the sampler.
struct MonitorSample
{
    std::string metric;                          ///< Metric name, e.g. "cpu.util".
    double value = 0;                            ///< Measured value.
    std::string unit;                            ///< Unit string.
    std::map<std::string, std::string> tags;       ///< Dimension tags (device, core...).
    std::string resource_class;                  ///< cpu / memory / storage / thermal / load / system.
    uint64_t mono_ns = 0;                        ///< Sample time.
};

/// @brief Per-tick sampler: computes rate deltas (CPU%), gauges and wear indicators.
class Sampler
{
  public:
    /// @brief Bind the sampler to the platform and a clock.
    /// @param plat Platform readings.
    /// @param ts Clock.
    Sampler(platform::IPlatform& plat, TimeSource& ts) : plat_(plat), ts_(ts)
    {
    }

    /// @brief One sampling pass.
    ///
    /// First tick after construction yields no CPU util (deltas need two
    /// snapshots) but yields gauges.
    ///
    /// @return Samples produced this tick.
    std::vector<MonitorSample> tick();

    /// @brief Override the default sampling interval.
    ///
    /// Thread-safe: the sampler thread reads this through interval_for() on
    /// every pass while configuration arrives on another thread (see the note
    /// on intervals_mu_).
    ///
    /// @param s Default sampling interval in seconds.
    void set_default_interval_s(double s)
    {
        std::lock_guard<std::mutex> lk(intervals_mu_);
        default_interval_ = s;
    }
    /// @brief Override the interval for one resource class.
    ///
    /// Thread-safe; see set_default_interval_s.
    ///
    /// @param resource_class Resource class to retune.
    /// @param s Interval in seconds.
    void set_interval_for(const std::string& resource_class, double s)
    {
        std::lock_guard<std::mutex> lk(intervals_mu_);
        intervals_[resource_class] = s;
    }
    /// @brief Effective interval for a resource class.
    /// @param resource_class Resource class to query.
    /// @return The effective interval in seconds.
    double interval_for(const std::string& resource_class) const;

    /// @brief Lifetime counters.
    struct Stats
    {
        uint64_t produced = 0;   ///< Samples produced.
        uint64_t dropped = 0;    ///< Samples dropped (e.g. unsupported metrics).
    };
    /// @brief Current statistics.
    /// @return Current statistics.
    const Stats& stats() const
    {
        return stats_;
    }

  private:
    platform::IPlatform& plat_;       ///< Platform readings (not owned).
    TimeSource& ts_;                   ///< Clock (not owned).
    platform::CpuSnapshot prev_cpu_;  ///< Previous tick snapshot for rate deltas.
    bool have_prev_cpu_ = false;      ///< False until the second tick.
    // Sampling intervals are written by configuration and read by the sampler
    // thread, so they need their own lock.
    //
    // The writers are MonitorCore::configure(), apply_config() and the
    // `set-interval` ctrl command; the reader is MonitorSpoke::sample_loop(),
    // which calls interval_for() once per pass to compute its wait deadline.
    // Nothing synchronised the two: a operator retuning an interval mutated a
    // std::map while the sampler thread was traversing it, which is a torn
    // read at best and a crash at worst.
    //
    // The lock lives in Sampler rather than in the callers on purpose. Two of
    // the three writers already hold MonitorCore::mu_ and one does not, and
    // the reader holds nothing -- so an external lock only appears to protect
    // this. Owning the invariant here makes it hold however the class is
    // called. Found by the TSan stage (Task.md A-05).
    mutable std::mutex intervals_mu_;           ///< Guards default_interval_ and intervals_.
    double default_interval_ = 5.0;   ///< Default per-class interval.
    std::map<std::string, double> intervals_;   ///< Per-class interval overrides.
    Stats stats_;                     ///< Lifetime counters.
};

/// @brief High-resolution rolling window for thermal and power retained locally at finer granularity than telemetry ships (REQ §7.3).
class RollingWindow
{
  public:
    /// @brief Construct a window of @p window_s seconds sampled at @p interval_s.
    /// @param window_s Window length in seconds.
    /// @param interval_s Expected sampling interval in seconds.
    RollingWindow(double window_s, double interval_s) : window_ns_(uint64_t(window_s * 1e9)), interval_ns_(uint64_t(interval_s * 1e9))
    {
    }

    /// @brief Append a sample, evicting entries older than the window.
    /// @param mono_ns Sample time.
    /// @param s Sample to append.
    void push(uint64_t mono_ns, const MonitorSample& s);
    /// @brief Samples within the window, oldest first.
    /// @return Samples within the window, oldest first.
    std::vector<MonitorSample> window() const;
    /// @brief Entries currently retained.
    size_t size() const
    {
        return buf_.size();
    }
    /// @brief Discard all retained samples.
    void clear()
    {
        buf_.clear();
    }

  private:
    /// @brief One retained sample.
    struct Entry
    {
        uint64_t mono_ns;        ///< Sample time.
        MonitorSample sample;    ///< The sample.
    };
    std::deque<Entry> buf_;          ///< Ring, oldest first.
    uint64_t window_ns_;             ///< Window length in nanoseconds.
    uint64_t interval_ns_;           ///< Expected sampling interval in nanoseconds.
};

/// @brief Predictive storage wear: project remaining life from wear indicators (REQ §7.3).
///
/// Linear model over measured wear rate; falls back to a conservative 10-year
/// estimate when the indicator is unknown.
class WearProjector
{
  public:
    /// @brief One device's wear projection.
    struct Projection
    {
        std::string device;             ///< Device name.
        double wear_pct = 0;            ///< Measured wear so far.
        uint32_t remaining_days = 0;    ///< Projected remaining life.
        std::string model;              ///< Model used ("linear" / "conservative").
    };
    /// @brief Project remaining life for one device.
    /// @param d Device with wear indicators.
    /// @param age_days Age of the device so far.
    /// @return The wear projection.
    static Projection project(const platform::StorageDevice& d, uint64_t age_days);
};

} // namespace domain
} // namespace monitord
} // namespace instr