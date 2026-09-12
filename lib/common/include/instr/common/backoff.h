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
 * @file lib/common/include/instr/common/backoff.h
 * @brief jittered exponential backoff (REQ §3.4, §11)
 *
 * Flood control: a site of hundreds of devices powering up together must not
 * stampede HQ. HQ backpressure is honoured by callers via reset().
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <functional>
#include <random>

namespace instr
{

/// @brief Jittered exponential backoff for retry loops (flood control, REQ §3.4, §11).
///
/// A site of hundreds of devices powering up together must not stampede HQ;
/// HQ backpressure is honoured by callers via reset().
class JitteredBackoff
{
  public:
    /// @brief Construct a backoff sequence.
    /// @param base_s Delay of the first attempt, seconds.
    /// @param max_s Upper bound on the delay, seconds.
    /// @param jitter_frac Fraction of ± randomisation applied to each delay.
    JitteredBackoff(double base_s, double max_s, double jitter_frac = 0.2) : base_(base_s), max_(max_s), jitter_(jitter_frac)
    {
    }

    /// @brief Next delay in seconds; grows exponentially, jittered ±jitter_frac.
    /// @return Delay in seconds, clamped to max_s before jitter is applied.
    double next()
    {
        if (attempt_ == 0)
        {
            attempt_ = 1;
            return base_ * jitter_scale();
        }
        attempt_ <<= 1;
        double d = base_ * static_cast<double>(attempt_);
        if (d > max_) d = max_;
        return d * jitter_scale();
    }

    /// @brief Restart the sequence: the next next() returns the base delay again.
    void reset()
    {
        attempt_ = 0;
    }
    /// @brief Number of delays handed out since the last reset().
    uint64_t attempts() const
    {
        return attempt_;
    }

    /// @brief Replace the random source (tests inject a deterministic generator).
    /// @param rng01 Callable returning uniform samples in [0,1); empty restores the internal mt19937.
    void set_rng(std::function<double()> rng01)
    {
        rng01_ = std::move(rng01);
    }

  private:
    /// @brief Multiplicative jitter factor in [1-jitter, 1+jitter].
    double jitter_scale()
    {
        double u = rng01_ ? double(rng01_()) : uniform_(rng_);
        return 1.0 + jitter_ * (2.0 * u - 1.0); // 1 ± jitter
    }
    double base_, max_, jitter_; ///< Configured base delay, cap and jitter fraction (seconds).
    uint64_t attempt_ = 0; ///< Doubling exponent; also the attempts-since-reset counter.
    std::mt19937 rng_{std::random_device{}()}; ///< Internal random engine.
    std::uniform_real_distribution<double> uniform_{0.0, 1.0}; ///< Uniform [0,1) sampler over rng_.
    std::function<double()> rng01_; ///< Optional injected sampler (tests).
};

} // namespace instr