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
 * @file daemons/istate/src/northbound.cpp
 * @brief istate northbound implementation (REQ §7.1, §3.4, §11): spool drain with backoff and backpressure, drop reporting, event timeline, and live log tail fanout.
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/istate/northbound.h"

#include "instr/istate/pst.h"

namespace instr
{
/// @brief istate: /pst owner, spoke orchestrator and HQ northbound bridge (R4, §7.1).
namespace istate
{

// ---------------- EventTimeline ----------------

/// @brief Append a stamped event, evicting past the bound.
/// @param kind Event kind.
/// @param actor Who produced it.
/// @param detail_json Event detail.
/// @param ts Clock for stamping.
void EventTimeline::append(const std::string& kind, const std::string& actor, const std::string& detail_json, TimeSource& ts)
{
    TimelineRecord ev;
    ev.seq = next_seq_++;
    ev.ts.mono_ns = ts.mono_ns();
    ev.ts.boot_id = ts.boot_id();
    ev.ts.wall_ns = ts.wall_ns();
    ev.ts.seq = ts.next_seq();
    ev.ts.wall_valid = true;
    ev.kind = kind;
    ev.actor = actor;
    ev.detail_json = detail_json;
    events_.push_back(ev);
    while (events_.size() > max_events_) events_.pop_front();
}

/// @brief Events after a sequence number.
/// @param s Line callback.
/// @param seq Sequence to query after.
/// @return A subscription id.
std::vector<TimelineRecord> EventTimeline::since(uint64_t seq) const
{
    std::vector<TimelineRecord> out;
    for (const auto& e : events_)
        if (e.seq > seq) out.push_back(e);
    return out;
}

// ---------------- TailFanout ----------------

/// @brief Subscribe to the live tail.
/// @param s Line callback.
/// @return A subscription id.
int TailFanout::subscribe(Subscriber s)
{
    std::lock_guard<std::mutex> lk(mu_);
    int id = next_id_++;
    Sub sub;
    sub.id = id;
    sub.fn = std::move(s);
    subs_.push_back(std::move(sub));
    return id;
}

/// @brief Cancel a subscription.
/// @param id Subscription to cancel.
void TailFanout::unsubscribe(int id)
{
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = subs_.begin(); it != subs_.end(); ++it)
        if (it->id == id)
        {
            subs_.erase(it);
            break;
        }
}

/// @brief Publish a line to all subscribers.
/// @param json_line Line to publish.
void TailFanout::publish(const std::string& json_line)
{
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& sub : subs_)
    {
        sub.pending.push_back(json_line);
        // Bounded per subscriber: drop oldest lines when the slow consumer lags.
        while (sub.pending.size() > 256) sub.pending.pop_front();
        if (sub.fn) sub.fn(json_line);
    }
}

// ---------------- NorthboundSpool ----------------

/// @brief Bind the spool to storage, config and budgets.
/// @param fs Storage backend.
/// @param cfg Spool configuration.
/// @param budgets Budget enforcement (may be null).
NorthboundSpool::NorthboundSpool(IFileSystem& fs, const SpoolConfig& cfg, BudgetManager* budgets) : fs_(fs), spool_(fs, cfg), budgets_(budgets)
{
}

/// @brief Open the backing store.
/// @return ok() when the store opened.
Error NorthboundSpool::open()
{
    return spool_.open();
}

/// @brief Enqueue a record (protected classes never drop).
/// @param prio Eviction class.
/// @param payload Record bytes.
/// @param mono_ns Monotonic production time.
/// @return True when accepted.
Result<bool> NorthboundSpool::enqueue(SpoolPriority prio, const std::string& payload, uint64_t mono_ns)
{
    // Budget enforcement before the write: alarm before the limit (§6).
    if (budgets_)
    {
        auto r = budgets_->reserve("spool", payload.size());
        if (!r.ok())
        {
            // Budget exceeded → the spool drop policy still applies for telemetry;
            // protected records get refused loudly by the DiskSpool itself.
            return Result<bool>::fail(r.error);
        }
    }
    int dropped = 0;
    Error e = spool_.append(prio, payload, mono_ns, &dropped);
    if (!e.ok()) return Result<bool>::fail(e);
    if (budgets_) budgets_->record_usage("spool", spool_.bytes());
    return Result<bool>::good(true);
}

/// @brief Drain a batch, honouring backpressure.
/// @param send Per-record send callback (false pauses).
/// @param max_batch Maximum records this drain.
/// @return The drain report.
NorthboundSpool::DrainReport NorthboundSpool::drain(const std::function<bool(const DiskSpool::Entry&)>& send, size_t max_batch)
{
    DrainReport rep;
    auto records = spool_.peek(max_batch);
    for (const auto& r : records)
    {
        // HQ backpressure honoured: a false return pauses the drain (§11).
        if (!send(r))
        {
            rep.paused = true;
            break;
        }
        ++rep.sent;
    }
    // Only the entries HQ acked are removed; a paused drain retains the rest.
    if (rep.sent > 0) spool_.pop_front(rep.sent);
    // Drop counters are cumulative spool statistics, not per-drain figures.
    const auto& st = spool_.stats();
    rep.dropped_telemetry = st.dropped_telemetry;
    rep.dropped_logref = st.dropped_logref;
    rep.dropped_event = st.dropped_event;
    rep.refused_protected = st.refused_protected;
    if (budgets_) budgets_->record_usage("spool", spool_.bytes());
    return rep;
}

} // namespace istate
} // namespace instr