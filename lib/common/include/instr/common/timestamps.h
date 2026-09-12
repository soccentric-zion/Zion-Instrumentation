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
 * @file lib/common/include/instr/common/timestamps.h
 * @brief timestamps and time reconciliation (REQ §4)
 *
 * Every record carries CLOCK_MONOTONIC plus boot id alongside wall clock.
 * A GNSS or NTP lock mid-run must never reorder history: wall times are
 * re-stamped and clamped to be non-decreasing in arrival order.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "instr/common/error.h"

namespace instr
{

/// @brief One timestamped observation: monotonic time, boot id, wall time and arrival order.
struct Stamp
{
    uint64_t mono_ns = 0; ///< CLOCK_MONOTONIC reading in nanoseconds.
    std::string boot_id; ///< Boot the monotonic reading belongs to (monotonic resets per boot).
    int64_t wall_ns = 0; ///< Wall-clock time in nanoseconds since the epoch.
    uint64_t seq = 0; ///< Process-wide arrival order; history is never reordered below this.
    bool wall_valid = false; ///< True when wall_ns is trustworthy (a clock lock existed).
};

/// @brief Injected clock so domain logic has no OS dependency.
class TimeSource
{
  public:
    /// @brief Default destructor.
    /// @return Default destructor.
    virtual ~TimeSource() = default;
    /// @brief Monotonic clock reading in nanoseconds (resets per boot).
    /// @return Nanoseconds on the monotonic timeline.
    virtual uint64_t mono_ns() = 0;
    /// @brief Wall-clock reading in nanoseconds since the epoch; may be wrong before a lock.
    /// @return Nanoseconds since the epoch.
    virtual int64_t wall_ns() = 0;
    /// @brief Boot identifier distinguishing monotonic timelines across reboots.
    /// @return The boot id string.
    virtual std::string boot_id() = 0;
    /// @brief Next process-wide arrival-order sequence number (strictly increasing).
    /// @return The next sequence number.
    virtual uint64_t next_seq() = 0;
};

/// @brief Real clock: CLOCK_MONOTONIC + realtime + /proc/sys/kernel/random/boot_id.
class SystemTimeSource : public TimeSource
{
  public:
    /// @brief Monotonic clock reading in nanoseconds (resets per boot).
    /// @return Nanoseconds on the monotonic timeline.
    uint64_t mono_ns() override;
    /// @brief Wall-clock reading in nanoseconds since the epoch; may be wrong before a lock.
    /// @return Nanoseconds since the epoch.
    int64_t wall_ns() override;
    /// @brief Boot identifier distinguishing monotonic timelines across reboots.
    /// @return The boot id string.
    std::string boot_id() override;
    /// @brief Next process-wide arrival-order sequence number (strictly increasing).
    /// @return The next sequence number.
    uint64_t next_seq() override;
};

/// @brief Manual clock for tests; seq auto-increments.
class ManualTimeSource : public TimeSource
{
  public:
    uint64_t mono = 1'000'000'000ull; ///< Fixed monotonic reading (1 s).
    int64_t wall = 1'700'000'000'000'000'000ll; ///< Fixed wall reading.
    std::string bid = "boot-1"; ///< Fixed boot id.
    uint64_t seq_counter = 0; ///< Running sequence counter.
    /// @brief Monotonic clock reading in nanoseconds (resets per boot).
    uint64_t mono_ns() override
    {
        return mono;
    }
    /// @brief Wall-clock reading in nanoseconds since the epoch; may be wrong before a lock.
    int64_t wall_ns() override
    {
        return wall;
    }
    /// @brief Boot identifier distinguishing monotonic timelines across reboots.
    /// @return The boot id string.
    std::string boot_id() override
    {
        return bid;
    }
    /// @brief Next process-wide arrival-order sequence number (strictly increasing).
    uint64_t next_seq() override
    {
        return ++seq_counter;
    }
};

/// @brief Reconciliation API invoked when a time source acquires lock.
class TimeReconciler
{
  public:
    /// @brief Bind the reconciler to a time source.
    /// @param ts Source used for every stamp produced here.
    explicit TimeReconciler(TimeSource& ts) : ts_(ts)
    {
    }

    /// @brief Record a monotonic-only observation awaiting wall-clock stamping.
    ///
    /// Used for spooled data produced while the clock was unavailable.
    ///
    /// @param mono_ns Monotonic reading of the observation.
    void record_pending(uint64_t mono_ns);

    /// @brief A pending observation awaiting reconciliation.
    struct Pending
    {
        uint64_t seq; ///< Arrival-order sequence number.
        uint64_t mono_ns; ///< Monotonic reading taken before the lock.
    };
    /// @brief Queue of pending observations in arrival order.
    /// @return Queue of pending observations in arrival order.
    const std::deque<Pending>& pending() const
    {
        return pending_;
    }
    /// @brief Number of observations still awaiting reconciliation.
    size_t pending_count() const
    {
        return pending_.size();
    }

    /// @brief On lock: re-stamp all pending records.
    ///
    /// History is never reordered: stamps materialise in seq order with
    /// non-decreasing wall time.
    ///
    /// @param offset_ns Wall-clock correction reported by the time source.
    /// @param source Identifier of the lock source (e.g. "gnss", "ntp").
    /// @return Number of pending records re-stamped.
    size_t reconcile(int64_t offset_ns, const std::string& source);

    /// @brief Remove and return the stamps produced by reconcile(), ordered by seq.
    /// @return The stamps drained from the reconciler.
    std::vector<Stamp> take_stamped();
    /// @brief View of stamps produced by reconcile() so far, ordered by seq.
    /// @return View of stamps produced by reconcile() so far, ordered by seq.
    const std::vector<Stamp>& stamped() const
    {
        return stamped_;
    }

    /// @brief Stamp an observation made *now*.
    /// @return Stamp with wall_valid set only if a lock has been acquired.
    Stamp stamp_now();

    /// @brief Wall time for a historical monotonic sample under the locked mapping.
    /// @param mono_ns Monotonic reading to project.
    /// @return Projected wall time; 0 when no lock has been acquired yet.
    int64_t wall_for_mono(uint64_t mono_ns) const
    {
        if (!locked_) return 0;
        return base_wall_ns_ + static_cast<int64_t>(mono_ns - base_mono_ns_);
    }

    /// @brief True when a lock has been acquired and wall projection is valid.
    bool wall_valid() const
    {
        return locked_;
    }
    /// @brief Wall-clock correction last applied by reconcile().
    int64_t offset_ns() const
    {
        return offset_ns_;
    }
    /// @brief Identifier of the source that produced the last lock.
    /// @return Identifier of the source that produced the last lock.
    const std::string& source() const
    {
        return source_;
    }

  private:
    TimeSource& ts_; ///< Clock the reconciler reads from.
    std::deque<Pending> pending_; ///< Observations awaiting a wall stamp, in seq order.
    std::vector<Stamp> stamped_; ///< Stamps produced by reconcile(), in seq order.
    bool locked_ = false; ///< True after the first reconcile() call.
    int64_t offset_ns_ = 0; ///< Last applied wall correction.
    std::string source_; ///< Last lock source name.
    int64_t base_wall_ns_ = 0; ///< Wall anchor for the locked mapping.
    uint64_t base_mono_ns_ = 0; ///< Monotonic anchor for the locked mapping.
};

/// @brief Build a Stamp from any TimeSource (seq assigned by the source).
/// @param ts Source to read from.
/// @return Fully populated Stamp with wall_valid set.
Stamp make_stamp(TimeSource& ts);

/// @brief Wall time for a historical monotonic sample under the reconciler's locked mapping.
///
/// For re-stamping historical monotonic samples:
/// wall(mono) = base_wall + (mono - base_mono).
///
/// @param rec Reconciler owning the locked timeline.
/// @param mono_ns Monotonic reading to project.
/// @return Projected wall time; 0 when no lock has been acquired.
int64_t project_wall_ns(const TimeReconciler& rec, uint64_t mono_ns);

} // namespace instr