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
 * @file daemons/istate/include/instr/istate/northbound.h
 * @brief northbound: persistent spool, HQ session with jittered backoff
 *
 * and backpressure, event timeline, live log tail fanout (REQ §7.1, §3.4).
 * Nothing northbound depends on the tunnel being available at the moment it
 * is produced (§3.4). Drop policy: oldest telemetry first, never alarms or
 * faults; drops are reported (§11).
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "instr/common/backoff.h"
#include "instr/common/spool.h"
#include "instr/common/timestamps.h"

namespace instr
{
/// @brief istate: /pst owner, spoke orchestrator and HQ northbound bridge (R4, §7.1).
namespace istate
{

class BudgetManager; ///< forward (defined in pst.h)

/// @brief Event timeline: single ordered per-device stream (§7.1).
struct TimelineRecord
{
    uint64_t seq = 0;        ///< Monotonic event sequence.
    Stamp ts;                ///< Event timestamp.
    std::string kind;        ///< boot / config_change / alarm / fault / update / restart / audit.
    std::string actor;       ///< Who produced it.
    std::string detail_json; ///< Event detail.
};

/// @brief Bounded event timeline with sequence-based queries.
class EventTimeline
{
  public:
    /// @brief Construct a timeline with an event bound.
    /// @param max_events Event bound.
    EventTimeline(size_t max_events) : max_events_(max_events)
    {
    }
    /// @brief Append a stamped event (oldest evicted past the bound).
    /// @param kind Event kind.
    /// @param actor Who produced it.
    /// @param detail_json Event detail.
    /// @param ts Clock for stamping.
    void append(const std::string& kind, const std::string& actor, const std::string& detail_json, TimeSource& ts);
    /// @brief Retained events, oldest first.
    /// @return Retained events, oldest first.
    const std::deque<TimelineRecord>& events() const
    {
        return events_;
    }
    /// @brief Events with sequence greater than @p seq.
    /// @param seq Sequence to query after.
    /// @return Events with sequence greater than @p seq.
    std::vector<TimelineRecord> since(uint64_t seq) const;
    /// @brief Events currently retained.
    size_t size() const
    {
        return events_.size();
    }

  private:
    std::deque<TimelineRecord> events_;   ///< Retained events, oldest first.
    size_t max_events_;                   ///< Event bound.
    uint64_t next_seq_ = 1;               ///< Next sequence number.
};

/// @brief Tail fanout: live log tail to HQ and Configuration (§7.1).
class TailFanout
{
  public:
    /// @brief One subscriber's line callback.
    using Subscriber = std::function<void(const std::string& json_line)>;
    /// @brief Subscribe; returns a subscription id for unsubscribe().
    /// @param s Line callback.
    /// @return A subscription id for unsubscribe().
    int subscribe(Subscriber s);
    /// @brief Cancel a subscription.
    /// @param id Subscription to cancel.
    void unsubscribe(int id);
    /// @brief Publish one line to every subscriber (bounded per-subscriber queue).
    /// @param json_line Line to publish.
    void publish(const std::string& json_line);

  private:
    /// @brief One subscriber with its pending queue.
    struct Sub
    {
        int id;                          ///< Subscription id.
        Subscriber fn;                   ///< Line callback.
        std::deque<std::string> pending; ///< Bounded per subscriber.
    };
    std::vector<Sub> subs_;    ///< Active subscribers.
    int next_id_ = 1;          ///< Subscription id generator.
    std::mutex mu_;            ///< Guards subs_.
};

/// @brief Northbound spool: disk-backed queue to HQ with drop policy and budgeting.
class NorthboundSpool
{
  public:
    /// @brief Bind the spool to storage, configuration and budgeting.
    /// @param fs Storage backend.
    /// @param cfg Spool location and sizing.
    /// @param budgets Budget enforcement (may be null in tests).
    NorthboundSpool(IFileSystem& fs, const SpoolConfig& cfg, BudgetManager* budgets);

    /// @brief Open/create the backing store.
    /// @return ok(), or the open error.
    Error open();
    /// @brief Enqueue with the spool class; protected classes never drop.
    /// @param prio Eviction class.
    /// @param payload Record bytes.
    /// @param mono_ns Monotonic production time.
    /// @return True when accepted; false when refused.
    Result<bool> enqueue(SpoolPriority prio, const std::string& payload, uint64_t mono_ns);
    /// @brief Drain up to @p max_batch records through @p send.
    ///
    /// The callback gets each record; return false to pause (backpressure).
    /// Honours HQ backpressure and jittered drain pacing (§3.4, §11).
    struct DrainReport
    {
        size_t sent = 0;                    ///< Records sent this drain.
        size_t dropped_telemetry = 0;       ///< Telemetry evictions.
        size_t dropped_logref = 0;          ///< Log-ref evictions.
        size_t dropped_event = 0;           ///< Event evictions.
        size_t refused_protected = 0;       ///< Protected appends refused.
        bool paused = false;                ///< Drain paused by backpressure.
    };
    /// @brief Run one drain pass.
    /// @param send Per-record send callback (false pauses).
    /// @param max_batch Maximum records this drain.
    /// @return The drain report.
    DrainReport drain(const std::function<bool(const DiskSpool::Entry&)>& send, size_t max_batch);
    /// @brief Lifetime spool statistics.
    /// @return Lifetime spool statistics.
    const DiskSpool::Stats& stats() const
    {
        return spool_.stats();
    }
    /// @brief Records currently spooled.
    size_t size() const
    {
        return spool_.size();
    }

  private:
    IFileSystem& fs_;            ///< Storage backend.
    DiskSpool spool_;            ///< Backing spool.
    BudgetManager* budgets_;     ///< Budget enforcement (may be null in tests).
};

/// @brief HQ session state machine: connection states and reconnect backoff.
class HqSession
{
  public:
    /// @brief Session states.
    enum class State
    {
        Disconnected,     ///< No tunnel/session.
        Backoff,          ///< Waiting to reconnect.
        Connected,        ///< Session established.
        Draining,         ///< Spool draining to HQ.
        Backpressured     ///< HQ asked us to pause.
    };
    /// @brief Canonical name of a state.
    /// @param s State to name.
    /// @return Canonical lowercase name.
    static const char* state_name(State s)
    {
        switch (s)
        {
        case State::Disconnected: return "disconnected";
        case State::Backoff: return "backoff";
        case State::Connected: return "connected";
        case State::Draining: return "draining";
        case State::Backpressured: return "backpressured";
        }
        return "?";
    }

    /// @brief Construct a session with reconnect backoff parameters.
    /// @param backoff_base_s Reconnect backoff base.
    /// @param backoff_max_s Reconnect backoff cap.
    HqSession(double backoff_base_s = 1.0, double backoff_max_s = 60.0) : backoff_(backoff_base_s, backoff_max_s)
    {
    }

    /// @brief Current session state.
    State state() const
    {
        return state_;
    }
    /// @brief Next reconnect delay (jittered).
    double next_delay_s()
    {
        return backoff_.next();
    }
    /// @brief Transition on a successful connect.
    void on_connected()
    {
        state_ = State::Connected;
        backoff_.reset();
    }
    /// @brief Transition on a lost session.
    void on_disconnected()
    {
        state_ = State::Backoff;
    }
    /// @brief Transition on HQ backpressure (pause) or release (drain).
    /// @param pause True to pause (Backpressured); false to drain.
    void on_backpressure(bool pause)
    {
        state_ = pause ? State::Backpressured : State::Draining;
    }
    /// @brief Force the draining state.
    void set_draining()
    {
        state_ = State::Draining;
    }
    /// @brief Force the disconnected state.
    void set_disconnected()
    {
        state_ = State::Disconnected;
    }
    /// @brief Reconnect attempts since the last successful connect.
    uint64_t attempts() const
    {
        return backoff_.attempts();
    }
    /// @brief Deterministic jitter for tests; production uses the seeded RNG.
    /// @param rng01 Uniform [0,1) sampler for deterministic tests.
    void set_backoff_rng(std::function<double()> rng01)
    {
        backoff_.set_rng(std::move(rng01));
    }

  private:
    State state_ = State::Disconnected;   ///< Current state.
    JitteredBackoff backoff_;             ///< Reconnect backoff sequence.
};

} // namespace istate
} // namespace instr