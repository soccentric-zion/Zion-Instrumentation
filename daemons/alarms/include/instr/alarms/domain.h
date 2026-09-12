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
 * @file daemons/alarms/include/instr/alarms/domain.h
 * @brief classic telecom-style alarm manager (REQ §7.6, R5)
 *
 * Only applications set and clear alarms. State machine: raise → active →
 * acknowledged → cleared, plus severity change on an already-active alarm.
 * X.733 fields; time-based escalation; snapshot-on-fault; catalog merge
 * preserving operator edits. Active table persists across restart.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "common.pb.h"
#include "instr/common/alarm_codec.h"
#include "instr/common/error.h"
#include "instr/common/filesystem.h"
#include "instr/common/timestamps.h"

namespace instr
{
/// @brief Alarms daemon: X.733 alarm state machine, escalation and catalog (§7.6).
namespace alarms
{
/// @brief Pure domain logic for this daemon: no transport, no sockets, fully unit-testable.
namespace domain
{

/// @brief One alarm as tracked by the alarms daemon (X.733-flavoured record).
struct AlarmRecord
{
    uint32_t alarm_id = 0; ///< Packed 32-bit alarm identity.
    std::string symbolic_name; ///< Catalog name.
    common::Severity severity = common::SEV_INFO; ///< Current severity.
    std::string owner; ///< Who raised it (daemon or application).
    std::string additional_text; ///< Free-text detail.
    std::string specific_problem; ///< Specific problem code/text.
    std::string probable_cause; ///< Probable cause code/text.
    Stamp first_seen; ///< First occurrence.
    Stamp last_seen; ///< Most recent occurrence.
    uint32_t count = 0; ///< Occurrence count.
    bool acknowledged = false; ///< Operator acknowledgement state.
    std::vector<uint32_t> correlated; ///< Correlated alarm ids.
};

/// @brief X.733-flavoured event emitted to istate directly — never through telemetry batching (escalation must not be delayed).
struct AlarmStateEvent
{
    std::string action; ///< raise / clear / ack / severity_change / snapshot_request.
    AlarmRecord record; ///< The alarm in its new state.
    bool needs_snapshot = false; ///< True when a diagnostic capture is requested.
};

/// @brief Time-based escalation rule for an alarm (or the default when id is 0).
struct EscalationRule
{
    ///< Applies to an alarm id or (when alarm_id == 0) as the default policy.
    uint32_t alarm_id = 0; ///< Alarm id the rule applies to; 0 = default policy.
    uint64_t promote_after_s = 3600; ///< Seconds unacknowledged before promotion.
    common::Severity promote_to = common::SEV_CRITICAL; ///< Severity promoted to.
};

/// @brief One catalog entry (alarm definition) with operator-edit tracking.
struct CatalogEntry
{
    uint32_t id = 0; ///< Packed alarm identity.
    std::string name; ///< Symbolic name.
    common::Severity severity = common::SEV_INFO; ///< Default severity.
    std::string probable_cause; ///< Probable cause text.
    std::string specific_problem; ///< Specific problem text.
    std::string description; ///< Human description.
    std::string escalation; ///< Escalation policy name.
    std::string auto_clear; ///< Auto-clear policy name.
    bool hq_reportable = true; ///< False to keep the alarm local-only.
    // operator edit marker: severity overridden by a local operator is
    // preserved when the catalog is regenerated (§7.6).
    bool operator_edited = false; ///< Severity overridden locally (preserved on regeneration).
};

/// @brief Alarm state machine, escalation, persistence and catalog merge for the daemon.
class AlarmManager
{
  public:
    /// @brief Bind the manager to storage and a clock.
    /// @param fs Storage backend (active.db, catalog files).
    /// @param active_db_path Path of the active-alarm database.
    /// @param ts Clock for stamps.
    AlarmManager(IFileSystem& fs, const std::string& active_db_path, TimeSource& ts) : fs_(fs), db_path_(active_db_path), ts_(ts)
    {
    }

    /// @brief Raise an alarm: new → active; existing → count++, last_seen update.
    ///
    /// A severity change on an already-active alarm emits severity_change.
    ///
    /// @param alarm_id Packed alarm identity.
    /// @param owner Raising component.
    /// @param additional_text Free-text detail.
    /// @param correlated Correlated alarm ids.
    /// @return The emitted event, or an error when the id is not decodable.
    Result<AlarmStateEvent> raise(uint32_t alarm_id, const std::string& owner, const std::string& additional_text, const std::vector<uint32_t>& correlated = {});
    /// @brief Acknowledge an active alarm.
    /// @param alarm_id Alarm to acknowledge.
    /// @return The emitted event, or NotFound when inactive.
    Result<AlarmStateEvent> acknowledge(uint32_t alarm_id);
    /// @brief Clear an active alarm.
    /// @param alarm_id Alarm to clear.
    /// @param owner Clearing component (must own the alarm).
    /// @return The emitted event, or an error.
    Result<AlarmStateEvent> clear(uint32_t alarm_id, const std::string& owner);

    /// @brief Currently active alarms.
    /// @return Currently active alarms.
    const std::vector<AlarmRecord>& active() const
    {
        return active_;
    }
    /// @brief Look up an active alarm.
    /// @return Pointer to the record, or NotFound when inactive.
    /// @param alarm_id Alarm to look up.
    Result<const AlarmRecord*> find(uint32_t alarm_id) const;

    /// @brief Replace the escalation rules.
    /// @param rules Escalation rules to install.
    void set_escalation(const std::vector<EscalationRule>& rules)
    {
        escalation_ = rules;
    }
    /// @brief Current escalation rules.
    /// @return Current escalation rules.
    const std::vector<EscalationRule>& escalation() const
    {
        return escalation_;
    }
    /// @brief Sweep active, unacknowledged alarms past their promotion threshold.
    /// @param now_mono_ns Current monotonic time.
    /// @return Events emitted by the sweep.
    std::vector<AlarmStateEvent> escalation_sweep(uint64_t now_mono_ns);

    /// @brief Persist active alarms to state/alarms/active.db (survives restart).
    /// @return ok(), or the persistence error.
    Error save();
    /// @brief Load the active-alarm database from disk.
    /// @return ok(), or the load error.
    Error load();

    /// @brief Snapshot-on-fault: request a bounded diagnostic capture when a CRITICAL alarm fires.
    ///
    /// The evidence must exist at the moment it matters.
    /// @param f Diagnostic-capture request sink.
    void set_snapshot_sink(std::function<void(uint32_t alarm_id)> f)
    {
        snapshot_sink_ = std::move(f);
    }
    /// @brief Configuration may turn the capture off on resource-constrained boards.
    /// @param on True to capture on CRITICAL alarms.
    void set_snapshot_on_critical(bool on)
    {
        snapshot_on_critical_ = on;
    }
    /// @brief Whether snapshot-on-critical is enabled.
    bool snapshot_on_critical() const
    {
        return snapshot_on_critical_;
    }
    /// @brief Install the event sink (spoke emission).
    /// @param f Event sink (spoke emission).
    void set_event_sink(std::function<void(const AlarmStateEvent&)> f)
    {
        event_sink_ = std::move(f);
    }

    /// @brief Merge a freshly generated catalog (software update regeneration, §7.6).
    ///
    /// Adds new entries; preserves operator-edited severities and operator
    /// fields for existing ids.
    ///
    /// @param incoming Catalog entries from the release.
    /// @return Counts of what was added, updated and preserved.
    struct MergeStats
    {
        size_t added = 0; ///< New entries appended.
        size_t updated = 0; ///< Existing entries refreshed.
        size_t preserved_operator_edits = 0; ///< Operator edits kept through the merge.
    };
    /// @brief Perform the catalog merge.
    /// @param incoming Catalog entries from the release.
    /// @return Counts of what was added, updated and preserved.
    MergeStats merge_catalog(const std::vector<CatalogEntry>& incoming);
    /// @brief Current catalog.
    /// @return Current catalog.
    const std::vector<CatalogEntry>& catalog() const
    {
        return catalog_;
    }
    /// @brief Mutable catalog access (codegen regeneration path).
    /// @return Mutable catalog access (codegen regeneration path).
    std::vector<CatalogEntry>& mutable_catalog()
    {
        return catalog_;
    }
    /// @brief Save the catalog to @p path.
    /// @param path Catalog file to write.
    /// @return ok(), or the write error.
    Error save_catalog(const std::string& path);
    /// @brief Load the catalog from @p path.
    /// @param path Catalog file to read.
    /// @return ok(), or the read error.
    Error load_catalog(const std::string& path);

    /// @brief Render the active table as JSON (ctrl "list" response).
    /// @return The active table as JSON.
    std::string active_table_json() const;

  private:
    /// @brief Serialise the active table for persistence.
    /// @return The active table serialised for persistence.
    std::string active_json() const;
    IFileSystem& fs_; ///< Storage backend.
    std::string db_path_; ///< Active-alarm database path.
    TimeSource& ts_; ///< Clock for stamps.
    std::vector<AlarmRecord> active_; ///< Active alarm table.
    std::vector<EscalationRule> escalation_; ///< Escalation rules.
    std::vector<CatalogEntry> catalog_; ///< Loaded catalog.
    std::function<void(uint32_t)> snapshot_sink_; ///< Diagnostic-capture request sink.
    bool snapshot_on_critical_ = true; ///< Configurable capture switch.
    std::function<void(const AlarmStateEvent&)> event_sink_; ///< Event sink (spoke emission).
};

/// @brief Catalog YAML parsing (shared shape with the codegen catalog).
/// @param yaml_text Catalog source text.
/// @return Parsed entries, or a diagnosable parse error.
Result<std::vector<CatalogEntry>> load_catalog_entries(const std::string& yaml_text);

} // namespace domain
} // namespace alarms
} // namespace instr