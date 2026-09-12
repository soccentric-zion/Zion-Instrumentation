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
 * @file lib/common/include/instr/common/rate_limiter.h
 * @brief rate limiting primitives (REQ §4)
 *
 * Token bucket with a "suppressed N events" summary, used by Faults,
 * LogManager and Telemetry so a stuck driver emitting 10k lines/s cannot
 * drown the system.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace instr
{

/// @brief Token-bucket rate limiter so a stuck driver emitting 10k lines/s cannot drown the system.
///
/// Used by Faults, LogManager and Telemetry; keeps a "suppressed N events"
/// counter for summaries.
class TokenBucket
{
  public:
    /// @brief Construct a bucket, filled to @p burst.
    /// @param rate_per_s Refill rate in tokens per second.
    /// @param burst Maximum token count (bucket capacity).
    /// @param now_s Injected clock (seconds, monotonic); empty uses the steady clock.
    TokenBucket(double rate_per_s, double burst, std::function<double()> now_s = nullptr) : rate_(rate_per_s), burst_(burst), now_(std::move(now_s))
    {
        if (!now_) now_ = [] { return steady_now_s(); };
        tokens_ = burst_;
        last_ = now_();
    }

    /// @brief Try to consume one token.
    /// @return True when allowed; false when rate-limited (suppression counters advance).
    bool try_acquire()
    {
        refill();
        if (tokens_ >= 1.0)
        {
            tokens_ -= 1.0;
            return true;
        }
        ++suppressed_;
        ++streak_;
        return false;
    }

    /// @brief Total acquisitions refused since construction.
    uint64_t suppressed_total() const
    {
        return suppressed_;
    }
    /// @brief Length of the current run of consecutive refusals.
    uint64_t current_streak() const
    {
        return streak_;
    }
    /// @brief Configured refill rate in tokens per second.
    double rate() const
    {
        return rate_;
    }
    /// @brief Configured bucket capacity.
    double burst() const
    {
        return burst_;
    }
    /// @brief Current token count (for tests and introspection).
    double tokens() const
    {
        return tokens_;
    }
    /// @brief Refill the bucket to capacity and clear the refusal streak.
    void reset()
    {
        tokens_ = burst_;
        streak_ = 0;
    }

    /// @brief Monotonic clock in seconds, used when no injected clock is given.
    /// @return Seconds on the steady-clock timeline.
    static double steady_now_s()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

  private:
    /// @brief Add tokens for elapsed time, capped at the bucket capacity.
    void refill()
    {
        double t = now_();
        tokens_ += (t - last_) * rate_;
        if (tokens_ > burst_) tokens_ = burst_;
        last_ = t;
    }
    double rate_, burst_, tokens_, last_; ///< Configured rate/capacity, current tokens, last refill time.
    std::function<double()> now_; ///< Clock in seconds (injected or steady).
    uint64_t suppressed_ = 0, streak_ = 0; ///< Lifetime refusals and current refusal streak.
};

/// @brief Emits "suppressed N events" summaries for a TokenBucket's refusal streaks.
///
/// When a suppressed streak ends and N reached the threshold, exactly one
/// summary is produced for the batch.
class SuppressionSummarizer
{
  public:
    /// @brief Construct a summarizer.
    /// @param threshold Minimum streak length that earns a summary.
    explicit SuppressionSummarizer(uint64_t threshold = 1) : threshold_(threshold)
    {
    }

    /// @brief Feed the outcome of one try_acquire() call.
    ///
    /// Returns a summary string exactly once per streak when the streak met
    /// the threshold.
    ///
    /// @param acquired Result of the caller's try_acquire().
    /// @param bucket Bucket the attempt was made against (streak source).
    /// @return Summary line once per qualifying streak, otherwise std::nullopt.
    std::optional<std::string> on_attempt(bool acquired, TokenBucket& bucket)
    {
        if (acquired)
        {
            if (streak_ >= threshold_)
            {
                std::string s = "suppressed " + std::to_string(streak_) + " events";
                streak_ = 0;
                return s;
            }
            streak_ = 0;
            return std::nullopt;
        }
        streak_ = bucket.current_streak();
        return std::nullopt;
    }

    /// @brief Current streak length as tracked by this summarizer (for tests).
    uint64_t streak() const
    {
        return streak_;
    }

  private:
    uint64_t threshold_; ///< Minimum streak length that earns a summary.
    uint64_t streak_ = 0; ///< Streak length seen so far.
};

} // namespace instr