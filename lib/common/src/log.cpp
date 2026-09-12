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
 * @file lib/common/src/log.cpp
 * @brief Implementation of structured, levelled logging with per-source runtime levels, rate limiting, and secret redaction (REQ §, §10).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/common/log.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace instr
{

/// @brief Canonical uppercase name of a level ("INFO", "ERROR", ...).
/// @param l Level to name.
/// @return Static string for the level; "INFO" when out of range.
const char* log_level_name(LogLevel l)
{
    switch (l)
    {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warning: return "WARNING";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Critical: return "CRITICAL";
    }
    return "?";
}

/// @brief Parse a level name (case-insensitive, e.g. "INFO" -> Info).
/// @param n Name as produced by log_level_name().
/// @return The parsed level; Info when the name is unknown.
LogLevel log_level_from_name(const std::string& n)
{
    std::string u;
    for (char c : n) u += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (u == "TRACE") return LogLevel::Trace;
    if (u == "DEBUG") return LogLevel::Debug;
    if (u == "WARNING" || u == "WARN") return LogLevel::Warning;
    if (u == "ERROR") return LogLevel::Error;
    if (u == "CRITICAL" || u == "FATAL") return LogLevel::Critical;
    return LogLevel::Info;
}

// ---------------- sinks ----------------

/// @brief Consume one accepted log record.
/// @param r Record to write; the sink must not retain a reference past the call.
void StderrSink::write(const LogRecord& r)
{
    char tbuf[32];
    time_t secs = static_cast<time_t>(r.wall_ns / 1'000'000'000ll);
    struct tm tm;
    gmtime_r(&secs, &tm);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    std::fprintf(stderr, "%s %s %s: %s", tbuf, log_level_name(r.level), r.source.c_str(), r.message.c_str());
    for (const auto& kv : r.fields) std::fprintf(stderr, " %s=%s", kv.first.c_str(), kv.second.c_str());
    std::fprintf(stderr, "\n");
}

/// @brief Consume one accepted log record.
/// @param r Record to write; the sink must not retain a reference past the call.
void MemorySink::write(const LogRecord& r)
{
    // Called from every thread that logs; see mu_ in the header.
    std::lock_guard<std::mutex> lk(mu_);
    buf_.push_back(r);
    while (buf_.size() > cap_) buf_.pop_front();
}

// ---------------- redaction ----------------

/// @brief The credential-like key names that are always redacted.
/// @return Static list, e.g. contains password, token, psk and friends.
const std::vector<std::string>& Redactor::secret_key_names()
{
    static const std::vector<std::string> names = {
        "password", "passwd", "secret", "token", "credential", "private_key", "privatekey", "api_key", "apikey", "wg-private", "psk", "session_key",
    };
    return names;
}

/// @brief Lowercase helper for case-insensitive name matching.
/// @param s Text to lowercase.
/// @return The lowercased text.
static std::string lower(const std::string& s)
{
    std::string o;
    for (char c : s) o += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

/// @brief Decide whether a field key refers to credential material.
/// @param key Field key to test.
/// @return True when the key must be redacted.
bool Redactor::is_secret_key(const std::string& key)
{
    const std::string k = lower(key);
    for (const auto& n : secret_key_names())
        if (k.find(n) != std::string::npos) return true;
    return false;
}

/// @brief Redact a field value under a credential-like key.
/// @param key Field key (drives redaction shape, e.g. partial reveal).
/// @param value Raw value that must not reach a sink verbatim.
/// @return Safe replacement text for the value.
std::string Redactor::redact_value(const std::string& key, const std::string& value)
{
    if (is_secret_key(key)) return "[REDACTED]";
    return redact_text(value);
}

namespace
{

// Characters that may appear inside a key token ("wg-private.key", "api_key").
/// @brief Decide whether a character may appear inside a log field key.
/// @param c Character to test.
/// @return True when valid in a key.
inline bool is_key_char(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.';
}
// A value ends at the first structural delimiter.
/// @brief Decide whether a character terminates a log value token.
/// @param c Character to test.
/// @return True when it terminates a value.
inline bool ends_value(char c)
{
    return std::isspace(static_cast<unsigned char>(c)) || c == ',' || c == '"' || c == '\'' || c == '}' || c == ']' || c == ')' || c == ';';
}

} // namespace

/// @brief Mask raw credential material appearing inside free-form text.
/// @param text Message body to scrub.
/// @return Text with any recognised credential substrings masked.
std::string Redactor::redact_text(const std::string& text)
{
    const std::string lowered = lower(text);
    std::string out;
    size_t i = 0;
    while (i < text.size())
    {
        // Find a secret name at this position. The name may be only part of a
        // longer key token — "wg-private.key" and "api_key_v2" both count — so
        // after matching we run to the end of the key token before looking for
        // the separator.
        size_t name_len = 0;
        for (const auto& n : secret_key_names())
        {
            if (lowered.compare(i, n.size(), n) == 0)
            {
                name_len = n.size();
                break;
            }
        }
        if (name_len == 0)
        {
            out += text[i];
            ++i;
            continue;
        }
        // A match must start at a key boundary, else "reset_token" style
        // substrings inside unrelated words would trigger.
        if (i > 0 && is_key_char(text[i - 1]))
        {
            out += text[i];
            ++i;
            continue;
        }
        size_t j = i + name_len;
        while (j < text.size() && is_key_char(text[j])) ++j; // rest of the key
        size_t k = j;
        if (k < text.size() && (text[k] == '"' || text[k] == '\'')) ++k; // closing quote
        while (k < text.size() && std::isspace(static_cast<unsigned char>(text[k]))) ++k;
        if (k >= text.size() || (text[k] != '=' && text[k] != ':'))
        {
            // Not a key=value pair; emit the key verbatim and carry on.
            out += text.substr(i, j - i);
            i = j;
            continue;
        }
        ++k; // the separator
        while (k < text.size() && std::isspace(static_cast<unsigned char>(text[k]))) ++k;
        if (k < text.size() && (text[k] == '"' || text[k] == '\'')) ++k; // opening quote
        size_t end = k;
        while (end < text.size() && !ends_value(text[end])) ++end;
        out += text.substr(i, k - i) + "[REDACTED]";
        i = end;
    }
    return out;
}

// ---------------- logger ----------------

/// @brief Monotonic seconds used for rate-limit bookkeeping.
double Logger::now_s()
{
    return static_cast<double>(ts_->mono_ns()) / 1e9;
}

/// @brief Rate-limit one source: max N messages/s, surplus dropped and summarised.
/// @param source Source name to limit.
/// @param max_per_s Allowed message rate; surplus is suppressed.
void Logger::set_rate_limit(const std::string& source, double max_per_s)
{
    rate_limits_[source] = RateBucket{max_per_s, now_s(), max_per_s};
}

/// @brief Emit a record: level-filtered, rate-limited, secret-redacted, then written to the sink.
/// @param lvl Severity of the record.
/// @param source Emitting component name.
/// @param msg Event text (credential material is masked before writing).
/// @param fields Optional structured key=value pairs (secret keys redacted).
void Logger::log(LogLevel lvl, const std::string& source, const std::string& msg, const std::map<std::string, std::string>& fields)
{
    // Effective level: per-source override, else global.
    auto it = source_levels_.find(source);
    LogLevel eff = (it != source_levels_.end()) ? it->second : level_;
    if (lvl < eff) return;

    // Rate limiting per source (token bucket refill).
    auto rit = rate_limits_.find(source);
    if (rit != rate_limits_.end())
    {
        double t = now_s();
        RateBucket& b = rit->second;
        b.tokens += (t - b.last_s) * b.rate;
        if (b.tokens > b.rate) b.tokens = b.rate; // burst = 1 s worth
        b.last_s = t;
        if (b.tokens < 1.0)
        {
            ++dropped_;
            ++suppressed_[source];
            return;
        }
        b.tokens -= 1.0;
        auto sit = suppressed_.find(source);
        if (sit != suppressed_.end() && sit->second > 0)
        {
            LogRecord summary;
            summary.level = LogLevel::Warning;
            summary.source = source;
            summary.message = "suppressed " + std::to_string(sit->second) + " events";
            summary.mono_ns = ts_->mono_ns();
            summary.wall_ns = ts_->wall_ns();
            summary.wall_valid = true;
            sink_->write(summary);
            suppressed_.erase(source);
        }
    }

    LogRecord r;
    r.level = lvl;
    r.source = source;
    r.message = Redactor::redact_text(msg);
    r.mono_ns = ts_->mono_ns();
    r.wall_ns = ts_->wall_ns();
    r.wall_valid = true;
    for (const auto& kv : fields) r.fields[kv.first] = Redactor::redact_value(kv.first, kv.second);
    sink_->write(r);
}

} // namespace instr