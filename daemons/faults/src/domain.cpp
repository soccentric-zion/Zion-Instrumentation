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
 * @file daemons/faults/src/domain.cpp
 * @brief faults domain implementation (REQ §7.5): rule-based kernel-log matching, dedup, and hard rate limiting with suppressed-event summaries.
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/faults/domain.h"

#include <yaml-cpp/yaml.h>

#include <cctype>

namespace instr
{
/// @brief Faults daemon: passive kernel-log fault detection (§7.5, R6).
namespace faults
{
/// @brief Pure domain logic for this daemon: no transport, no sockets, fully unit-testable.
namespace domain
{

// Fault ids arrive as "0x5_05_1_0001"-style text.
/// @brief Parse a canonical "0x5_05_1_0001"-form fault id.
/// @param text Canonical id text.
/// @return The packed id, or an InvalidArgument error.
Result<uint32_t> parse_fault_id_text(const std::string& text)
{
    if (text.rfind("0x", 0) != 0) return Result<uint32_t>::fail(Errc::InvalidArgument, "fault id must start with 0x", text);
    std::string compact;
    for (char c : text.substr(2))
        if (c != '_') compact += c;
    try
    {
        return Result<uint32_t>::good(uint32_t(std::stoul(compact, nullptr, 16)));
    }
    catch (...)
    {
        return Result<uint32_t>::fail(Errc::InvalidArgument, "bad fault id", text);
    }
}

/// @brief Parse and validate a fault-rules.yaml document.
/// @param yaml_text Rules source text.
/// @return Parsed rules, or a diagnosable parse error.
Result<std::vector<FaultRule>> load_rules(const std::string& yaml_text)
{
    try
    {
        YAML::Node root = YAML::Load(yaml_text);
        std::vector<FaultRule> out;
        const YAML::Node& rules = root["rules"];
        if (!rules || !rules.IsSequence()) return Result<std::vector<FaultRule>>::fail(Errc::InvalidArgument, "fault-rules.yaml must contain a 'rules' list", "fault-rules.yaml");
        std::map<std::string, bool> names;
        for (const auto& r : rules)
        {
            FaultRule fr;
            fr.name = r["name"].as<std::string>("");
            std::string pattern = r["pattern"].as<std::string>("");
            if (fr.name.empty() || pattern.empty()) return Result<std::vector<FaultRule>>::fail(Errc::InvalidArgument, "rule needs name and pattern", "rules[]");
            if (names.count(fr.name)) return Result<std::vector<FaultRule>>::fail(Errc::InvalidArgument, "duplicate rule name: " + fr.name, fr.name);
            names[fr.name] = true;
            try
            {
                // icase: kernel messages are inconsistent about case between
                // drivers ("CPU2: Package temperature" vs "cpu clock throttled"),
                // and a signature that misses because of capitalisation is a fault
                // class silently unmonitored on every device.
                fr.re = std::regex(pattern, std::regex::ECMAScript | std::regex::optimize | std::regex::icase);
            }
            catch (const std::regex_error& e)
            {
                return Result<std::vector<FaultRule>>::fail(Errc::InvalidArgument, "bad regex for rule " + fr.name + ": " + e.what(), fr.name);
            }
            std::string fid_raw = r["fault_id"].as<std::string>("0x0");
            std::string fid_compact;
            for (char c : fid_raw)
                if (c != '_') fid_compact += c;
            auto id = decode_alarm_id(uint32_t(std::stoul(fid_compact, nullptr, 16)));
            if (!id.ok() || id->domain != AlarmDomain::Fault) return Result<std::vector<FaultRule>>::fail(Errc::InvalidArgument, "fault_id must be a fault-domain 32-bit id", fr.name);
            fr.fault_id = uint32_t(std::stoul(fid_compact, nullptr, 16));
            std::string sev = r["severity"].as<std::string>("MAJOR");
            if (sev == "INFO")
                fr.severity = common::SEV_INFO;
            else if (sev == "WARNING")
                fr.severity = common::SEV_WARNING;
            else if (sev == "MINOR")
                fr.severity = common::SEV_MINOR;
            else if (sev == "MAJOR")
                fr.severity = common::SEV_MAJOR;
            else if (sev == "CRITICAL")
                fr.severity = common::SEV_CRITICAL;
            fr.x733_type = r["x733_type"].as<std::string>("equipment");
            fr.probable_cause = r["probable_cause"].as<std::string>("");
            fr.dedup_key = r["dedup_key"].as<std::string>(fr.name);
            fr.dedup_window_s = r["dedup_window_s"].as<uint32_t>(60);
            fr.rate_limit_per_s = r["rate_limit_per_s"].as<double>(5);
            out.push_back(fr);
        }
        return Result<std::vector<FaultRule>>::good(std::move(out));
    }
    catch (const std::exception& e)
    {
        return Result<std::vector<FaultRule>>::fail(Errc::InvalidArgument, std::string("rules parse: ") + e.what(), "fault-rules.yaml");
    }
}

/// @brief First rule matching a kernel line.
/// @param line Kernel line to match.
/// @return The first matching rule, or std::nullopt.
std::optional<const FaultRule*> RuleEngine::match(const std::string& line) const
{
    for (const auto& r : rules_)
    {
        try
        {
            if (std::regex_search(line, r.re)) return &r;
        }
        catch (const std::regex_error&)
        {
            continue;
        }
    }
    return std::nullopt;
}

/// @brief Render a dedup template, extracting $device tokens from the line.
/// @param tmpl Dedup template from the rule.
/// @param line Kernel line providing the tokens.
/// @return The rendered dedup key.
std::string RuleEngine::render_dedup_key(const std::string& tmpl, const std::string& line)
{
    if (tmpl.find("$device") == std::string::npos && tmpl.find("$zone") == std::string::npos && tmpl.find("$process") == std::string::npos) return tmpl;
    // Extract the first plausible device/zone/process token from the line.
    std::string token;
    for (const char* kw : {"sd", "mmcblk", "nvme", "nvme0n1", "eth", "end", "enp", "wlan", "thermal_zone", "can"})
    {
        size_t pos = line.find(kw);
        if (pos != std::string::npos)
        {
            size_t end = pos;
            while (end < line.size() && (std::isalnum((unsigned char)line[end]) || line[end] == '_' || line[end] == '.')) ++end;
            token = line.substr(pos, end - pos);
            break;
        }
    }
    if (token.empty())
    {
        // OOM lines carry a process name: "Killed process 123 (name)".
        size_t p = line.find("(");
        if (p != std::string::npos)
        {
            size_t e = line.find(")", p);
            if (e != std::string::npos) token = line.substr(p + 1, e - p - 1);
        }
    }
    std::string out = tmpl;
    for (const char* var : {"$device", "$zone", "$process"})
    {
        size_t pos = out.find(var);
        if (pos != std::string::npos) out.replace(pos, std::strlen(var), token);
    }
    return out;
}

/// @brief Observe one key occurrence; merges counts inside the window.
/// @param key Rendered dedup key.
/// @param mono_ns Current monotonic time.
/// @param window_s Merge window in seconds.
/// @return (Occurrence count, first-in-window flag).
std::pair<uint32_t, bool> Deduplicator::observe(const std::string& key, uint64_t mono_ns, uint32_t window_s)
{
    Entry& e = map_[key];
    uint64_t window_ns = uint64_t(window_s) * 1'000'000'000ull;
    if (e.count > 0 && mono_ns - e.last_ns <= window_ns)
    {
        ++e.count;
        e.last_ns = mono_ns;
        return {e.count, false};
    }
    e.first_ns = mono_ns;
    e.last_ns = mono_ns;
    e.count = 1;
    return {1, true};
}

/// @brief Token-bucket check of the daemon-wide emission ceiling.
/// @param mono_ns Current monotonic time.
/// @return True when the ceiling allows an emission.
bool FaultProcessor::global_allow(uint64_t mono_ns)
{
    if (global_rate_per_s_ <= 0) return true; // disabled
    const double burst = std::max(global_rate_per_s_, 1.0);
    if (global_last_mono_ == 0)
    {
        global_tokens_ = burst;
        global_last_mono_ = mono_ns;
    }
    const double dt_s = std::max(0.0, (double(mono_ns) - double(global_last_mono_)) / 1e9);
    global_tokens_ = std::min(burst, global_tokens_ + dt_s * global_rate_per_s_);
    global_last_mono_ = mono_ns;
    if (global_tokens_ < 1.0)
    {
        ++global_suppressed_;
        return false;
    }
    global_tokens_ -= 1.0;
    return true;
}

/// @brief Scan lines, dedup, rate-limit and emit fault records.
/// @param lines Kernel lines to scan.
/// @param mono_ns Current monotonic time.
/// @param backfill True for boot-ring backfill records.
/// @return The fault records produced by this batch.
std::vector<FaultRecord> FaultProcessor::feed(const std::vector<std::string>& lines, uint64_t mono_ns, bool backfill)
{
    std::vector<FaultRecord> out;
    for (const auto& line : lines)
    {
        ++stats_.lines_scanned;
        if (backfill) ++stats_.backfill_lines;
        auto m = engine_.match(line);
        if (!m) continue;
        const FaultRule* rule = *m;
        // Per-rule rate limiting (hard), driven by the feed's monotonic
        // timestamps so the limiter is deterministic and testable.
        // Bucket starts full (burst), refills at rate with the feed's clock.
        double burst = std::max(rule->rate_limit_per_s, 1.0);
        if (!bucket_last_mono_.count(rule->name)) bucket_tokens_[rule->name] = burst;
        double last = bucket_last_mono_.count(rule->name) ? double(bucket_last_mono_[rule->name]) : double(mono_ns);
        double dt_s = std::max(0.0, (double(mono_ns) - last) / 1e9);
        bucket_tokens_[rule->name] = std::min(burst, bucket_tokens_[rule->name] + dt_s * rule->rate_limit_per_s);
        bucket_last_mono_[rule->name] = mono_ns;
        bool acquired = bucket_tokens_[rule->name] >= 1.0;
        if (acquired) bucket_tokens_[rule->name] -= 1.0;
        if (!acquired)
        {
            ++suppressed_streak_[rule->name];
            continue;
        }
        // Streak ended: exactly one summary when the suppressed batch is large
        // enough to matter ("suppressed N events", REQ §7.5).
        if (suppressed_streak_[rule->name] >= FaultProcessor::kSuppressionSummaryThreshold)
        {
            FaultRecord s;
            s.fault_id = rule->fault_id;
            s.rule = rule->name;
            s.severity = rule->severity;
            s.x733_type = rule->x733_type;
            s.probable_cause = rule->probable_cause;
            s.raw_line = "suppressed " + std::to_string(suppressed_streak_[rule->name]) + " events";
            s.occurrences = 0;
            s.backfill = backfill;
            s.mono_ns = mono_ns;
            s.device_key = rule->name + ":suppressed";
            out.push_back(s);
            stats_.suppressed_events += suppressed_streak_[rule->name];
        }
        suppressed_streak_[rule->name] = 0;
        std::string key = RuleEngine::render_dedup_key(rule->dedup_key, line);
        auto occ = dedup_.observe(key, mono_ns, rule->dedup_window_s);
        if (!occ.second)
        {
            ++stats_.duplicates_merged;
            // Update the last emitted record for this key rather than emitting anew.
            bool merged = false;
            for (auto it = out.rbegin(); it != out.rend(); ++it)
            {
                if (it->rule == rule->name && it->device_key == key)
                {
                    it->occurrences = occ.first;
                    it->raw_line = line;
                    merged = true;
                    break;
                }
            }
            if (merged) continue;
        }
        // Daemon-wide ceiling, applied after per-rule limiting and dedup so the
        // cheap filters run first.
        if (!global_allow(mono_ns)) continue;
        FaultRecord fr;
        fr.fault_id = rule->fault_id;
        fr.mono_ns = mono_ns;
        fr.rule = rule->name;
        fr.severity = rule->severity;
        fr.raw_line = line;
        fr.occurrences = occ.first;
        fr.device_key = key;
        fr.x733_type = rule->x733_type;
        fr.probable_cause = rule->probable_cause;
        fr.backfill = backfill;
        out.push_back(fr);
        ++stats_.faults_raised;
    }
    return out;
}

// NOTE: suppression summaries are emitted on the next acquire after a
// streak; a flood that ends mid-batch is summarised by the next feed().

} // namespace domain
} // namespace faults
} // namespace instr