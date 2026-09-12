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
 * @file daemons/alarms/src/domain.cpp
 * @brief alarms domain implementation (REQ §7.6): alarm state machine, severity change on active alarms, time-based escalation, persistence, snapshot-on-fault, and catalog merge that preserves operator edits.
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/alarms/domain.h"

#include <yaml-cpp/yaml.h>

#include "instr/common/json.h"

#include <algorithm>
#include <sstream>

namespace instr
{
/// @brief Alarms daemon: X.733 alarm state machine, escalation and catalog (§7.6).
namespace alarms
{
/// @brief Pure domain logic for this daemon: no transport, no sockets, fully unit-testable.
namespace domain
{

/// @brief Map a catalog severity string to the proto enum (INFO on unknown).
/// @param s Severity string from the catalog.
/// @return The proto severity enum.
static common::Severity sev_from(const std::string& s)
{
    if (s == "CRITICAL") return common::SEV_CRITICAL;
    if (s == "MAJOR") return common::SEV_MAJOR;
    if (s == "MINOR") return common::SEV_MINOR;
    if (s == "WARNING") return common::SEV_WARNING;
    return common::SEV_INFO;
}

/// @brief Canonical lowercase severity name.
/// @param s Severity to name.
/// @return Canonical lowercase name.
static const char* sev_name(common::Severity s)
{
    switch (s)
    {
    case common::SEV_CRITICAL: return "CRITICAL";
    case common::SEV_MAJOR: return "MAJOR";
    case common::SEV_MINOR: return "MINOR";
    case common::SEV_WARNING: return "WARNING";
    default: return "INFO";
    }
}

/// @brief Parse catalog YAML into entries.
/// @param yaml_text Catalog YAML source.
/// @return Parsed entries, or a diagnosable parse error.
Result<std::vector<CatalogEntry>> load_catalog_entries(const std::string& yaml_text)
{
    try
    {
        YAML::Node root = YAML::Load(yaml_text);
        std::vector<CatalogEntry> out;
        for (const auto& e : root["entries"])
        {
            CatalogEntry c;
            auto idr = [&]() -> Result<uint32_t> {
                std::string t = e["id"].as<std::string>("");
                std::string compact;
                for (char ch : t)
                    if (ch != '_') compact += ch;
                if (compact.rfind("0x", 0) == 0) compact = compact.substr(2);
                try
                {
                    return Result<uint32_t>::good(uint32_t(std::stoul(compact, nullptr, 16)));
                }
                catch (...)
                {
                    return Result<uint32_t>::fail(Errc::InvalidArgument, "bad id", t);
                }
            }();
            if (!idr.ok()) return Result<std::vector<CatalogEntry>>::fail(idr.error);
            c.id = *idr;
            c.name = e["name"].as<std::string>("");
            c.severity = sev_from(e["severity"].as<std::string>("INFO"));
            c.probable_cause = e["probable_cause"].as<std::string>("");
            c.specific_problem = e["specific_problem"].as<std::string>("");
            c.description = e["description"].as<std::string>("");
            c.escalation = e["escalation"].as<std::string>("none");
            c.auto_clear = e["auto_clear"].as<std::string>("manual");
            c.hq_reportable = e["hq_reportable"].as<bool>(true);
            out.push_back(c);
        }
        return Result<std::vector<CatalogEntry>>::good(std::move(out));
    }
    catch (const std::exception& e)
    {
        return Result<std::vector<CatalogEntry>>::fail(Errc::InvalidArgument, std::string("catalog parse: ") + e.what(), "alarms.yaml");
    }
}

/// @brief Raise an alarm: insert or refresh, emitting the state event.
/// @param alarm_id Packed alarm identity.
/// @param owner Raising component.
/// @param additional_text Free-text detail.
/// @param correlated Correlated alarm ids.
/// @return The emitted event, or an error.
Result<AlarmStateEvent> AlarmManager::raise(uint32_t alarm_id, const std::string& owner, const std::string& additional_text, const std::vector<uint32_t>& correlated)
{
    // Catalog lookup supplies X.733 fields so applications declare ids only.
    std::string sym, probable, specific;
    common::Severity sev = common::SEV_MAJOR;
    bool found_cat = false;
    for (const auto& c : catalog_)
        if (c.id == alarm_id)
        {
            sym = c.name;
            probable = c.probable_cause;
            specific = c.specific_problem;
            sev = c.severity;
            found_cat = true;
            break;
        }
    if (!found_cat) return Result<AlarmStateEvent>::fail(Errc::NotFound, "alarm id not in catalog: " + alarm_id_string(alarm_id) + " — alarms are declared, never invented", "raise");

    auto* existing = [&]() -> AlarmRecord* {
        for (auto& r : active_)
            if (r.alarm_id == alarm_id) return &r;
        return nullptr;
    }();

    AlarmStateEvent ev;
    if (existing)
    {
        existing->count++;
        existing->last_seen = make_stamp(ts_); // seq assigned once in make_stamp
        existing->owner = owner.empty() ? existing->owner : owner;
        if (!additional_text.empty()) existing->additional_text = additional_text;
        if (!correlated.empty()) existing->correlated = correlated;
        if (sev != existing->severity)
        {
            existing->severity = sev; // severity change on an already-active alarm
            ev.action = "severity_change";
        }
        else { ev.action = "raise"; }
        ev.record = *existing;
        if (event_sink_) event_sink_(ev);
        return Result<AlarmStateEvent>::good(ev);
    }

    AlarmRecord r;
    r.alarm_id = alarm_id;
    r.symbolic_name = sym;
    r.severity = sev;
    r.owner = owner;
    r.additional_text = additional_text;
    r.specific_problem = specific;
    r.probable_cause = probable;
    r.correlated = correlated;
    r.first_seen = make_stamp(ts_);
    r.last_seen = r.first_seen;
    r.count = 1;
    r.acknowledged = false;
    active_.push_back(r);

    ev.action = "raise";
    ev.record = r;
    // Snapshot-on-fault: CRITICAL fires → trigger bounded diagnostic capture.
    if (sev == common::SEV_CRITICAL && snapshot_on_critical_)
    {
        ev.needs_snapshot = true;
        if (snapshot_sink_) snapshot_sink_(alarm_id);
        // The snapshot request is itself an event to istate.
        AlarmStateEvent snap;
        snap.action = "snapshot_request";
        snap.record = r;
        if (event_sink_) event_sink_(snap);
    }
    if (event_sink_) event_sink_(ev);
    return Result<AlarmStateEvent>::good(ev);
}

/// @brief Acknowledge an active alarm.
/// @param alarm_id Alarm to acknowledge.
/// @return The emitted event, or NotFound when inactive.
Result<AlarmStateEvent> AlarmManager::acknowledge(uint32_t alarm_id)
{
    for (auto& r : active_)
    {
        if (r.alarm_id == alarm_id)
        {
            r.acknowledged = true;
            AlarmStateEvent ev;
            ev.action = "ack";
            ev.record = r;
            if (event_sink_) event_sink_(ev);
            return Result<AlarmStateEvent>::good(ev);
        }
    }
    return Result<AlarmStateEvent>::fail(Errc::NotFound, "alarm not active: " + alarm_id_string(alarm_id), "acknowledge");
}

/// @brief Clear an active alarm.
/// @param alarm_id Alarm to clear.
/// @param owner Clearing component (must own the alarm).
/// @return The emitted event, or an error.
Result<AlarmStateEvent> AlarmManager::clear(uint32_t alarm_id, const std::string& owner)
{
    // Only the owner may clear (applications declare alarms; R5).
    for (auto it = active_.begin(); it != active_.end(); ++it)
    {
        if (it->alarm_id == alarm_id)
        {
            if (!owner.empty() && it->owner != owner && !it->owner.empty()) return Result<AlarmStateEvent>::fail(Errc::InvalidArgument, "clear refused: owner mismatch (declared by " + it->owner + ")", "clear");
            AlarmStateEvent ev;
            ev.action = "clear";
            ev.record = *it;
            active_.erase(it);
            if (event_sink_) event_sink_(ev);
            return Result<AlarmStateEvent>::good(ev);
        }
    }
    return Result<AlarmStateEvent>::fail(Errc::NotFound, "alarm not active: " + alarm_id_string(alarm_id), "clear");
}

/// @brief Look up an active alarm.
/// @param alarm_id Alarm to look up.
/// @return Pointer to the record, or NotFound.
Result<const AlarmRecord*> AlarmManager::find(uint32_t alarm_id) const
{
    for (const auto& r : active_)
        if (r.alarm_id == alarm_id) return Result<const AlarmRecord*>::good(&r);
    return Result<const AlarmRecord*>::fail(Errc::NotFound, "alarm not active", "find");
}

/// @brief Promote stale unacknowledged alarms per the escalation rules.
/// @param now_mono_ns Current monotonic time.
/// @return Events emitted by the sweep.
std::vector<AlarmStateEvent> AlarmManager::escalation_sweep(uint64_t now_mono_ns)
{
    std::vector<AlarmStateEvent> out;
    for (auto& r : active_)
    {
        if (r.acknowledged) continue;
        const EscalationRule* rule = nullptr;
        for (const auto& e : escalation_)
            if (e.alarm_id == r.alarm_id) rule = &e;
        if (!rule)
            for (const auto& e : escalation_)
                if (e.alarm_id == 0) rule = &e; // default policy
        if (!rule) continue;
        uint64_t age_ns = now_mono_ns - r.first_seen.mono_ns;
        bool promoted = false;
        if (rule->promote_after_s > 0 && age_ns >= rule->promote_after_s * 1'000'000'000ull && r.severity < rule->promote_to)
        {
            r.severity = rule->promote_to;
            promoted = true;
        }
        if (promoted)
        {
            AlarmStateEvent ev;
            ev.action = "severity_change";
            ev.record = r;
            out.push_back(ev);
            if (event_sink_) event_sink_(ev);
        }
    }
    return out;
}

/// @brief Serialise the active table for persistence.
/// @return The active table serialised for persistence.
std::string AlarmManager::active_json() const
{
    std::ostringstream o;
    o << "[";
    bool first = true;
    for (const auto& r : active_)
    {
        if (!first) o << ",";
        first = false;
        o << "{\"alarm_id\":" << r.alarm_id << ",\"name\":\"" << r.symbolic_name << "\",\"severity\":\"" << sev_name(r.severity) << "\",\"owner\":\"" << r.owner << "\",\"text\":\"" << r.additional_text << "\",\"count\":" << r.count << ",\"probable_cause\":\"" << r.probable_cause
          << "\",\"specific_problem\":\"" << r.specific_problem << "\",\"acknowledged\":" << (r.acknowledged ? "true" : "false") << ",\"first_mono_ns\":" << r.first_seen.mono_ns << ",\"last_mono_ns\":" << r.last_seen.mono_ns << ",\"first_seq\":" << r.first_seen.seq
          << ",\"last_seq\":" << r.last_seen.seq << "}";
    }
    o << "]";
    return o.str();
}

/// @brief Persist the active table atomically.
Error AlarmManager::save()
{
    return fs_.write(db_path_, active_json());
}

/// @brief Load the active table (tolerates a missing file: nothing active).
Error AlarmManager::load()
{
    auto txt = fs_.read(db_path_);
    if (!txt.ok()) return Error::none(); // no persisted table → fresh start
    active_.clear();
    auto j = json_parse(*txt);
    if (!j.ok()) return j.error;
    for (const auto& item : (*j).items())
    {
        AlarmRecord r;
        if (const JValue* v = item.find("alarm_id")) r.alarm_id = uint32_t(v->as_number());
        if (const JValue* v = item.find("name")) r.symbolic_name = v->as_string();
        std::string sev = item.find("severity") ? item.find("severity")->as_string() : "INFO";
        r.severity = sev_from(sev);
        if (const JValue* v = item.find("owner")) r.owner = v->as_string();
        if (const JValue* v = item.find("text")) r.additional_text = v->as_string();
        if (const JValue* v = item.find("probable_cause")) r.probable_cause = v->as_string();
        if (const JValue* v = item.find("specific_problem")) r.specific_problem = v->as_string();
        if (const JValue* v = item.find("count")) r.count = uint32_t(v->as_number());
        if (const JValue* v = item.find("acknowledged")) r.acknowledged = v->as_bool();
        r.first_seen.mono_ns = item.find("first_mono_ns") ? uint64_t(item.find("first_mono_ns")->as_number()) : 0;
        r.last_seen.mono_ns = item.find("last_mono_ns") ? uint64_t(item.find("last_mono_ns")->as_number()) : 0;
        r.first_seen.seq = item.find("first_seq") ? uint64_t(item.find("first_seq")->as_number()) : 0;
        r.last_seen.seq = item.find("last_seq") ? uint64_t(item.find("last_seq")->as_number()) : 0;
        active_.push_back(r);
    }
    return Error::none();
}

/// @brief Merge a regenerated catalog, preserving operator edits.
/// @param incoming Catalog entries from the release.
/// @return Counts of what was added, updated and preserved.
AlarmManager::MergeStats AlarmManager::merge_catalog(const std::vector<CatalogEntry>& incoming)
{
    MergeStats st;
    for (const auto& in : incoming)
    {
        bool found = false;
        for (auto& existing : catalog_)
        {
            if (existing.id == in.id)
            {
                found = true;
                if (existing.operator_edited)
                {
                    ++st.preserved_operator_edits; // operator edits survive regeneration
                }
                else
                {
                    existing.severity = in.severity;
                    existing.probable_cause = in.probable_cause;
                    existing.specific_problem = in.specific_problem;
                    existing.description = in.description;
                    existing.escalation = in.escalation;
                    existing.auto_clear = in.auto_clear;
                    existing.hq_reportable = in.hq_reportable;
                    ++st.updated;
                }
                break;
            }
        }
        if (!found)
        {
            catalog_.push_back(in);
            ++st.added;
        }
    }
    return st;
}

/// @brief Persist the catalog atomically.
/// @param path Catalog file to write.
Error AlarmManager::save_catalog(const std::string& path)
{
    std::ostringstream o;
    o << "# persisted merged alarm catalog (operator edits preserved)\n";
    o << "version: 1\nentries:\n";
    for (const auto& c : catalog_)
    {
        o << "  - id: 0x" << std::hex << c.id << std::dec << "\n"
          << "    name: " << c.name << "\n"
          << "    severity: " << sev_name(c.severity) << "\n"
          << "    probable_cause: " << c.probable_cause << "\n"
          << "    specific_problem: " << c.specific_problem << "\n"
          << "    description: " << c.description << "\n"
          << "    escalation: " << c.escalation << "\n"
          << "    auto_clear: " << c.auto_clear << "\n"
          << "    hq_reportable: " << (c.hq_reportable ? "true" : "false") << "\n"
          << "    operator_edited: " << (c.operator_edited ? "true" : "false") << "\n";
    }
    return fs_.write(path, o.str());
}

/// @brief Load the catalog from disk (missing file is not an error).
/// @param path Catalog file to read.
Error AlarmManager::load_catalog(const std::string& path)
{
    auto txt = fs_.read(path);
    if (!txt.ok()) return Error::none();
    // Parse the persisted catalog (superset of shipped schema: extra
    // operator_edited key).
    try
    {
        YAML::Node root = YAML::Load(*txt);
        std::vector<CatalogEntry> out;
        for (const auto& e : root["entries"])
        {
            CatalogEntry c;
            std::string t = e["id"].as<std::string>("");
            std::string compact;
            for (char ch : t)
                if (ch != '_') compact += ch;
            if (compact.rfind("0x", 0) == 0) compact = compact.substr(2);
            c.id = uint32_t(std::stoul(compact, nullptr, 16));
            c.name = e["name"].as<std::string>("");
            c.severity = sev_from(e["severity"].as<std::string>("INFO"));
            c.probable_cause = e["probable_cause"].as<std::string>("");
            c.specific_problem = e["specific_problem"].as<std::string>("");
            c.description = e["description"].as<std::string>("");
            c.escalation = e["escalation"].as<std::string>("none");
            c.auto_clear = e["auto_clear"].as<std::string>("manual");
            c.hq_reportable = e["hq_reportable"].as<bool>(true);
            c.operator_edited = e["operator_edited"].as<bool>(false);
            out.push_back(c);
        }
        catalog_ = out;
        return Error::none();
    }
    catch (const std::exception& e)
    {
        return Error::make(Errc::Corrupt, std::string("catalog load: ") + e.what(), path);
    }
}

/// @brief Render the active table as JSON for ctrl responses.
/// @return The active table as JSON.
std::string AlarmManager::active_table_json() const
{
    return active_json();
}

} // namespace domain
} // namespace alarms
} // namespace instr