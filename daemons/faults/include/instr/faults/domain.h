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
 * @file daemons/faults/include/instr/faults/domain.h
 * @brief purely passive kernel-log observation (REQ §7.5, R6). Never
 *
 * configures hardware, never raises an alarm. Rule-based matching, dedup,
 * hard rate limiting with "suppressed N events" summaries.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "common.pb.h"
#include "instr/common/alarm_codec.h"
#include "instr/common/error.h"
#include "instr/common/rate_limiter.h"
#include "instr/common/timestamps.h"

namespace instr
{
/// @brief Faults daemon: passive kernel-log fault detection (§7.5, R6).
namespace faults
{
/// @brief Pure domain logic for this daemon: no transport, no sockets, fully unit-testable.
namespace domain
{

/// @brief One fault-detection rule from fault-rules.yaml (rules are data).
struct FaultRule
{
    std::string name; ///< Rule name.
    std::string pattern; ///< Regex source text.
    std::regex re; ///< Compiled pattern.
    uint32_t fault_id = 0; ///< Packed fault identity.
    common::Severity severity = common::SEV_MAJOR; ///< Severity to assign.
    std::string x733_type; ///< X.733 event type.
    std::string probable_cause; ///< Probable cause text.
    std::string dedup_key; ///< Template, e.g. "bus:$device".
    uint32_t dedup_window_s = 60; ///< Dedup merge window.
    double rate_limit_per_s = 5; ///< Per-rule emission cap.
};

/// @brief One detected fault as reported upward.
struct FaultRecord
{
    uint32_t fault_id = 0; ///< Packed fault identity.
    uint64_t mono_ns = 0; ///< Detection time.
    std::string rule; ///< Rule that matched.
    common::Severity severity = common::SEV_MAJOR; ///< Severity.
    std::string raw_line; ///< Triggering kernel line.
    uint32_t occurrences = 1; ///< Merged occurrence count.
    std::string device_key; ///< Rendered dedup key.
    std::string x733_type; ///< X.733 event type.
    std::string probable_cause; ///< Probable cause text.
    bool backfill = false; ///< True when recovered from the boot ring.
};

/// @brief Loads and validates a fault-rules.yaml document.
/// @param yaml_text Rules source text.
/// @return Parsed rules, or a diagnosable parse error.
Result<std::vector<FaultRule>> load_rules(const std::string& yaml_text);

/// @brief Rule matching with $device extraction heuristics for dedup keys.
class RuleEngine
{
  public:
    /// @brief Construct an engine over compiled rules.
    explicit RuleEngine(std::vector<FaultRule> rules) : rules_(std::move(rules))
    {
    }
    /// @brief Loaded rules.
    /// @return The compiled rule set.
    const std::vector<FaultRule>& rules() const
    {
        return rules_;
    }
    /// @brief First matching rule for a kernel line.
    /// @return The rule, or std::nullopt when nothing matches.
    /// @param line Kernel line to match.
    std::optional<const FaultRule*> match(const std::string& line) const;
    /// @brief Render a dedup template, replacing $device with an extracted token.
    /// @param tmpl Dedup template from the rule.
    /// @param line Kernel line providing the tokens.
    /// @return The rendered dedup key.
    static std::string render_dedup_key(const std::string& tmpl, const std::string& line);

  private:
    std::vector<FaultRule> rules_; ///< Compiled rule set.
};

/// @brief Deduplication: identical keys within the window merge occurrence counts.
class Deduplicator
{
  public:
    /// @brief Observe a dedup key occurrence.
    /// @param key Rendered dedup key.
    /// @param mono_ns Current monotonic time.
    /// @param window_s Merge window.
    /// @return (occurrence count, true when this is the first occurrence within the window).
    std::pair<uint32_t, bool> observe(const std::string& key, uint64_t mono_ns, uint32_t window_s);
    /// @brief Keys currently tracked (test introspection).
    size_t active_keys() const
    {
        return map_.size();
    }

  private:
    /// @brief Per-key window state.
    struct Entry
    {
        uint64_t first_ns = 0; ///< First occurrence in the window.
        uint64_t last_ns = 0; ///< Most recent occurrence.
        uint32_t count = 0; ///< Occurrences merged.
    };
    std::map<std::string, Entry> map_; ///< Active dedup windows.
};

/// @brief Parse a fault id in the catalog's "0x5_05_1_0001" form.
///
/// §8: fault ids use the same 32-bit scheme as alarms, under the fault domain.
///
/// @param text Canonical id text.
/// @return The packed id, or an InvalidArgument error.
Result<uint32_t> parse_fault_id_text(const std::string& text);

/// @brief The full passive processor: scan lines → faults with dedup + suppression.
class FaultProcessor
{
  public:
    /// @brief A suppressed streak must reach this before a summary is emitted.
    static constexpr uint64_t kSuppressionSummaryThreshold = 10;

    /// @brief Construct a processor over compiled rules.
    FaultProcessor(std::vector<FaultRule> rules) : engine_(std::move(rules))
    {
    }

    /// @brief Lifetime counters.
    struct Stats
    {
        uint64_t lines_scanned = 0; ///< Kernel lines fed.
        uint64_t faults_raised = 0; ///< Records emitted.
        uint64_t duplicates_merged = 0; ///< Occurrences folded into existing faults.
        uint64_t suppressed_events = 0; ///< Events dropped by rate limiting.
        uint64_t backfill_lines = 0; ///< Lines recovered from the boot ring.
    };

    /// @brief Feed kernel-log lines.
    /// @param lines Lines to scan.
    /// @param mono_ns Current monotonic time.
    /// @param backfill True for records recovered from the boot ring (read from the start on launch, §7.5).
    /// @return The fault records produced by this batch.
    std::vector<FaultRecord> feed(const std::vector<std::string>& lines, uint64_t mono_ns, bool backfill);
    /// @brief Lifetime statistics.
    /// @return Lifetime statistics.
    const Stats& stats() const
    {
        return stats_;
    }
    /// @brief The rule engine's rule set.
    /// @return The rule engine's rule set.
    const std::vector<FaultRule>& engine_rules() const
    {
        return engine_.rules();
    }

    /// @brief Daemon-wide ceiling on emitted faults, on top of the per-rule limits.
    ///
    /// §7.5: a stuck driver emitting 10k lines a second must not drown the
    /// system, even when it trips several rules at once. 0 disables it.
    /// @param per_s Global ceiling in events per second (0 disables).
    void set_global_rate_limit(double per_s)
    {
        global_rate_per_s_ = per_s;
        global_tokens_ = per_s;
        global_last_mono_ = 0;
    }
    /// @brief Configured global ceiling (0 = disabled).
    double global_rate_limit() const
    {
        return global_rate_per_s_;
    }
    /// @brief Events dropped by the global ceiling.
    uint64_t globally_suppressed() const
    {
        return global_suppressed_;
    }

  private:
    /// @brief False when the global ceiling is exhausted for this instant.
    /// @param mono_ns Current monotonic time.
    /// @return True when the ceiling allows an emission.
    bool global_allow(uint64_t mono_ns);

    RuleEngine engine_; ///< Rule matching.
    Deduplicator dedup_; ///< Window-based merging.
    std::map<std::string, double> bucket_tokens_; ///< Per-rule token buckets.
    std::map<std::string, uint64_t> bucket_last_mono_; ///< Per-rule last-refill time.
    std::map<std::string, uint64_t> suppressed_streak_; ///< Per-rule suppression streaks.
    Stats stats_; ///< Lifetime counters.
    double global_rate_per_s_ = 0; ///< Global ceiling (0 = disabled).
    double global_tokens_ = 0; ///< Global token count.
    uint64_t global_last_mono_ = 0; ///< Global last-refill time.
    uint64_t global_suppressed_ = 0; ///< Global suppression counter.
};

} // namespace domain
} // namespace faults
} // namespace instr