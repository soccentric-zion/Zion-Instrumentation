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
 * @file lib/common/include/instr/common/periodic.h
 * @brief One stoppable interval worker, shared by every daemon.
 *
 * The daemons own scheduled work — the fault follower, the alarm escalation
 * sweep, hardware's periodic checks, OTA's rollback deadline, backup's
 * schedule, telemetry's send interval, LogManager's prune, networkd's
 * connectivity probe and SSH expiry. Each of those is a timer, and a timer
 * written nine times is a timer with nine different shutdown bugs.
 *
 * Stopping is prompt: the worker waits on a condition variable, so stop()
 * returns without waiting out the remaining interval. The interval may be
 * changed at runtime (configuration arrives from istate while running) and
 * takes effect on the next wait.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace instr
{

/// @brief Named periodic worker thread running a tick callback on an interval.
///
/// interval_s <= 0 leaves the worker idle: it stays startable but does no
/// work, which is how a configuration disables scheduled checks without
/// tearing the thread down.
class PeriodicWorker
{
  public:
    /// @brief Construct a named worker over @p tick with @p interval_s cadence.
    /// @param name Thread/worker name for diagnostics.
    /// @param interval_s Tick cadence in seconds; <= 0 leaves the worker idle.
    /// @param tick Work to run on each interval.
    PeriodicWorker(std::string name, double interval_s, std::function<void()> tick) : name_(std::move(name)), interval_s_(interval_s), tick_(std::move(tick))
    {
    }
    /// @brief Stop the worker and join its thread.
    /// @return Stop the worker and join its thread.
    ~PeriodicWorker()
    {
        stop();
    }

    /// @brief Non-copyable: the worker owns a thread.
    PeriodicWorker(const PeriodicWorker&) = delete;
    /// @brief Non-copyable: the worker owns a thread.
    PeriodicWorker& operator=(const PeriodicWorker&) = delete;

    /// @brief Start the worker thread (idempotent).
    void start()
    {
        if (running_.exchange(true)) return;
        thread_ = std::thread([this] { loop(); });
    }

    /// @brief Stop the worker and join its thread (idempotent).
    void stop()
    {
        if (!running_.exchange(false)) return;
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    /// @brief Runtime interval change (configuration delivered while running).
    ///
    /// Takes effect on the next wait; wakes the worker so a shortened interval
    /// is not held up by the previous long one.
    ///
    /// @param s New cadence in seconds; <= 0 disables the work (idle poll).
    void set_interval_s(double s)
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            interval_s_ = s;
        }
        cv_.notify_all();
    }
    /// @brief Current cadence in seconds.
    double interval_s() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return interval_s_;
    }

    /// @brief True while the worker thread should run.
    bool running() const
    {
        return running_.load();
    }
    /// @brief Ticks executed so far.
    uint64_t ticks() const
    {
        return ticks_.load();
    }
    /// @brief Worker name.
    /// @return The worker name.
    const std::string& name() const
    {
        return name_;
    }

    /// @brief Run one tick synchronously.
    ///
    /// Tests drive the work directly rather than sleeping on wall-clock time.
    void tick_once()
    {
        if (tick_) tick_();
        ++ticks_;
    }

  private:
    /// @brief Worker loop: wait for the interval (or stop), then tick.
    void loop()
    {
        while (running_.load())
        {
            double wait_s;
            {
                std::unique_lock<std::mutex> lk(mu_);
                wait_s = interval_s_;
                if (wait_s <= 0) wait_s = 0.5; // idle poll; re-checks for a new interval
                cv_.wait_for(lk, std::chrono::duration<double>(wait_s), [this] { return !running_.load(); });
            }
            if (!running_.load()) return;
            if (interval_s() <= 0) continue; // disabled by configuration
            tick_once();
        }
    }

    std::string name_;               ///< Worker name for diagnostics.
    mutable std::mutex mu_;          ///< Guards interval_s_.
    std::condition_variable cv_;     ///< Wakes the loop on stop/interval change.
    double interval_s_;              ///< Current cadence in seconds (guarded by mu_).
    std::function<void()> tick_;     ///< Work run on each interval.
    std::thread thread_;             ///< Worker thread.
    std::atomic<bool> running_{false};     ///< Lifecycle flag.
    std::atomic<uint64_t> ticks_{0};       ///< Ticks executed.
};

} // namespace instr
