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
 * @file lib/common/src/timestamps.cpp
 * @brief Implementation of system time sources and wall-clock reconciliation so a mid-run GNSS/NTP lock can never reorder history (REQ §).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/common/timestamps.h"

#include <ctime>

namespace instr
{

/// @brief Monotonic clock reading in nanoseconds (resets per boot).
uint64_t SystemTimeSource::mono_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
}
/// @brief Wall-clock reading in nanoseconds since the epoch; may be wrong before a lock.
int64_t SystemTimeSource::wall_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000ll + ts.tv_nsec;
}
/// @brief Boot identifier distinguishing monotonic timelines across reboots.
/// @return The boot id string.
std::string SystemTimeSource::boot_id()
{
    FILE* f = std::fopen("/proc/sys/kernel/random/boot_id", "r");
    if (!f) return "unknown-boot";
    char buf[64] = {};
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    std::string s(buf, n);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}
/// @brief Next process-wide arrival-order sequence number (strictly increasing).
uint64_t SystemTimeSource::next_seq()
{
    static uint64_t counter = 0;
    return ++counter;
}

/// @brief Build a Stamp from any TimeSource (seq assigned by the source).
/// @param ts Source to read from.
/// @return Fully populated Stamp with wall_valid set.
Stamp make_stamp(TimeSource& ts)
{
    Stamp s;
    s.mono_ns = ts.mono_ns();
    s.boot_id = ts.boot_id();
    s.seq = ts.next_seq();
    s.wall_ns = ts.wall_ns();
    s.wall_valid = true;
    return s;
}

/// @brief Record a monotonic-only observation awaiting wall-clock stamping.
///
/// Used for spooled data produced while the clock was unavailable.
///
/// @param mono_ns Monotonic reading of the observation.
void TimeReconciler::record_pending(uint64_t mono_ns)
{
    pending_.push_back(Pending{ts_.next_seq(), mono_ns});
}

/// @brief On lock: re-stamp all pending records.
///
/// History is never reordered: stamps materialise in seq order with
/// non-decreasing wall time.
///
/// @param offset_ns Wall-clock correction reported by the time source.
/// @param source Identifier of the lock source (e.g. "gnss", "ntp").
/// @return Number of pending records re-stamped.
size_t TimeReconciler::reconcile(int64_t offset_ns, const std::string& source)
{
    // Establish the mapping monotonic → wall with the correction applied.
    offset_ns_ = offset_ns;
    source_ = source;
    base_mono_ns_ = ts_.mono_ns();
    base_wall_ns_ = ts_.wall_ns() + offset_ns;
    locked_ = true;

    size_t n = 0;
    int64_t prev_wall = 0;
    for (auto& p : pending_)
    {
        Stamp s;
        s.mono_ns = p.mono_ns;
        s.boot_id = ts_.boot_id();
        s.seq = p.seq;
        s.wall_valid = true;
        s.wall_ns = base_wall_ns_ + static_cast<int64_t>(p.mono_ns - base_mono_ns_);
        // Never reorder history: a re-stamp must not move an earlier record's
        // wall time past a later record's.
        if (prev_wall != 0 && s.wall_ns < prev_wall) s.wall_ns = prev_wall;
        prev_wall = s.wall_ns;
        stamped_.push_back(s);
        ++n;
    }
    pending_.clear();
    return n;
}

/// @brief Remove and return the stamps produced by reconcile(), ordered by seq.
/// @return The stamps drained from the reconciler.
std::vector<Stamp> TimeReconciler::take_stamped()
{
    std::vector<Stamp> out;
    out.swap(stamped_);
    return out;
}

/// @brief Stamp an observation made *now*.
/// @return Stamp with wall_valid set only if a lock has been acquired.
Stamp TimeReconciler::stamp_now()
{
    Stamp s;
    s.mono_ns = ts_.mono_ns();
    s.boot_id = ts_.boot_id();
    s.seq = ts_.next_seq();
    s.wall_valid = locked_;
    s.wall_ns = locked_ ? base_wall_ns_ + static_cast<int64_t>(s.mono_ns - base_mono_ns_) : ts_.wall_ns();
    return s;
}

/// @brief Wall time for a historical monotonic sample under the reconciler's locked mapping.
///
/// For re-stamping historical monotonic samples:
/// wall(mono) = base_wall + (mono - base_mono).
///
/// @param rec Reconciler owning the locked timeline.
/// @param mono_ns Monotonic reading to project.
/// @return Projected wall time; 0 when no lock has been acquired.
int64_t project_wall_ns(const TimeReconciler& rec, uint64_t mono_ns)
{
    // Re-stamping spooled data: wall(mono) = base_wall + (mono - base_mono)
    // using the reconciler's locked mapping. Never reorders: callers keep the
    // original arrival sequence numbers.
    return rec.wall_for_mono(mono_ns);
}

} // namespace instr