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
 * @file catalog/include/instr/catalog/codegen.h
 * @brief alarm catalog code generator domain (REQ §8)
 *
 * Pure logic: parse the YAML catalog → validated entries → render the
 * shared C++ header. No filesystem access; the main wraps it with IO.
 *
 * @details
 * This header declares the domain core of the alarm catalog code generator
 * (REQ §7.6, REQ §8). The generator is the single source of truth for the
 * alarm identifiers that every instrumentation daemon and control tool
 * compiles against:
 *
 * @par Pipeline
 * - @ref parse_alarm_id_text unpacks and width-validates the canonical packed
 *   alarm id text (`0x1_04_1_0007` style).
 * - @ref parse_catalog parses the YAML catalog text into a fully validated
 *   @ref AlarmCatalog. Duplicate ids, duplicate names, unknown severities, and
 *   malformed escalation/clearing values are loud, diagnosable errors (see
 *   ::instr::Error) — the generator never emits a partially-valid header.
 * - @ref render_header renders the validated catalog into the shared C++
 *   header: one `INSTR_ALARM_<UPPER_SNAKE>` constant per entry, plus a static
 *   entry table carrying description, probable cause, specific problem,
 *   escalation, and auto-clear policy.
 *
 * @par Design constraints
 * - **Pure and testable.** This translation unit performs no filesystem
 *   access, no I/O, and no global state. `alarmgen_main.cpp` is the thin IO
 *   shell that reads `catalog/alarms.yaml` from disk and writes the rendered
 *   header; everything in between lives here and runs under the unit tests.
 * - **Deterministic output.** The same catalog text always renders byte-for-byte
 *   identical header text, so the build is reproducible.
 * - **Loud on error.** Every failure path returns a ::instr::Result with a
 *   filled ::instr::Error naming the offending entry; there is no silent
 *   skip, no default-fill, and no warning-and-continue mode.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 * @{\n */

#pragma once
#include <string>
#include <vector>

#include "instr/common/error.h"
#include "instr/common/json.h"

namespace instr
{
/**
 * @namespace catalog
 * @brief Alarm catalog parsing and C++ header code generation (§7.6, §8).
 *
 * @details
 * Declares the data model (@ref AlarmCatalog, @ref AlarmEntry, @ref Severity)
 * and the three pure entry points (@ref parse_catalog, @ref render_header,
 * @ref parse_alarm_id_text) that turn the human-authored YAML catalog into the
 * machine-generated C++ header shared by every daemon and tool. See the file
 * description for the pipeline and design constraints.
 */
namespace catalog
{

/**
 * @brief Catalog severity levels.
 *
 * @details
 * Five-level severity scale, ordered from least to most severe. The scale is
 * the canonical ordering used by the alarm daemons for escalation and by the
 * control tools for filtering; members are listed in ascending severity.
 *
 * | Enumerator | Meaning |
 * |------------|---------|
 * | `Info`     | Informational condition; no operator action expected. |
 * | `Warning`  | Abnormal but not yet alarming condition; watch closely. |
 * | `Minor`    | Degraded condition; operator should investigate. |
 * | `Major`    | Significant impairment; operator action required. |
 * | `Critical` | Severe failure; immediate operator action required. |
 *
 * @note The ordering `Info < Warning < Minor < Major < Critical` is part of
 * the domain contract: promotion (escalation) moves an alarm strictly upward.
 */
enum class Severity
{
    Info,       ///< Informational condition; no operator action expected.
    Warning,    ///< Abnormal but not yet alarming condition; watch closely.
    Minor,      ///< Degraded condition; operator should investigate.
    Major,      ///< Significant impairment; operator action required.
    Critical    ///< Severe failure; immediate operator action required.
};

/**
 * @brief Canonical UPPER name of a severity.
 * @param s Severity to name.
 * @return Canonical name (`"INFO"`, `"WARNING"`, `"MINOR"`, `"MAJOR"`, or
 *         `"CRITICAL"`).
 *
 * @details
 * Returns a string literal owned by the program; the pointer is valid for the
 * lifetime of the process and needs no deletion. The returned form is the
 * one used to render `AlarmSeverity::*` enumerators in the generated header,
 * so it is always upper-case and NUL-terminated.
 */
const char* severity_name(Severity s);

/**
 * @brief Parse a severity name (case-insensitive).
 * @param s Name to parse (e.g. `"info"`, `"Warning"`, `"CRITICAL"`).
 * @param[out] err Receives the failure describing the unknown name; left
 *         untouched on success and ignored when `nullptr`.
 * @return The matching severity, or `Severity::Info` on error (always
 *         inspect `err` before trusting the result).
 *
 * @details
 * Comparison is case-insensitive: `"info"`, `"Info"`, and `"INFO"` all parse
 * to `Severity::Info`. A name that matches no severity is a hard error with a
 * message naming the offending text — never a silent default.
 */
Severity severity_from_name(const std::string&, Error* err);

/**
 * @brief One alarm definition in the catalog.
 *
 * @details
 * A fully validated unit of catalog data. Fields are populated by
 * @ref parse_catalog; code that consumes the generated header sees the
 * rendered form of these fields (@c INSTR_ALARM_<UPPER_SNAKE> constants and
 * the static entry table).
 *
 * | Member           | Type          | Default   | Meaning |
 * |------------------|---------------|-----------|---------|
 * | `id`             | `uint32_t`    | `0`       | Packed alarm identity, see @ref parse_alarm_id_text for the field layout. |
 * | `name`           | `std::string` | `""`      | Symbolic name, rendered as `INSTR_ALARM_<UPPER_SNAKE>`. Must be unique in the catalog. |
 * | `severity`       | `Severity`    | `Info`    | Initial severity at raise time. |
 * | `probable_cause` | `std::string` | `""`      | Human-readable probable-cause text (ANSI/CTA-206 style). |
 * | `specific_problem` | `std::string` | `""`    | Human-readable specific-problem text. |
 * | `description`    | `std::string` | `""`      | Short human description. |
 * | `escalation`     | `std::string` | `"none"`  | `"none"`, `"promote-after-15m"`, or `"promote-after-1h"`. |
 * | `auto_clear`     | `std::string` | `"manual"` | `"manual"` or `"auto"` (auto-clearing by the daemon). |
 * | `hq_reportable`  | `bool`        | `true`    | `false` keeps the alarm local-only (suppressed from HQ reporting). |
 */
struct AlarmEntry
{
    uint32_t id = 0;             ///< Packed alarm identity (`parse_alarm_id_text`).
    std::string name;            ///< Symbolic name → INSTR_ALARM_<UPPER_SNAKE>.
    Severity severity = Severity::Info;   ///< Initial severity at raise time.
    std::string probable_cause;  ///< Probable cause text.
    std::string specific_problem;   ///< Specific problem text.
    std::string description;     ///< Short human description.
    std::string escalation;      ///< none / promote-after-15m / promote-after-1h.
    std::string auto_clear;      ///< manual / auto.
    bool hq_reportable = true;   ///< false keeps the alarm local-only.
};

/**
 * @brief A parsed alarm catalog.
 *
 * @details
 * The in-memory result of @ref parse_catalog: a schema version plus an
 * ordered list of alarm definitions. Entries preserve catalog order; that
 * order is the order in which `INSTR_ALARM_*` constants and table rows are
 * rendered by @ref render_header.
 *
 * @see AlarmEntry
 */
struct AlarmCatalog
{
    uint32_t version = 1;              ///< Catalog schema version.
    std::vector<AlarmEntry> entries;    ///< Alarm definitions, in catalog order.
};

/**
 * @brief Parse and validate a catalog; duplicate IDs or names are loud errors.
 * @param yaml_text Full text of the alarm catalog (e.g. `catalog/alarms.yaml`).
 * @return On success, the validated @ref AlarmCatalog. On failure, an error
 *         identifying the offending construct (duplicate id/name, unknown
 *         severity, malformed id text, invalid escalation/auto-clear value).
 *
 * @details
 * Validation is total: either every entry is well-formed and unique (ids and
 * names each appear at most once) or the call fails with a diagnosable
 * ::instr::Error. There is no lenient mode; partial results are never
 * returned, so downstream code can assume that any successful catalog is
 * safe to render.
 *
 * @note This function is pure — no filesystem or network access.
 *
 * @see AlarmCatalog, render_header()
 */
Result<AlarmCatalog> parse_catalog(const std::string& yaml_text);

/**
 * @brief Render the generated C++ header: `INSTR_ALARM_*` constants + entry table.
 * @param cat Parsed and validated catalog, as returned by @ref parse_catalog.
 * @param version Catalog version stamped into the output header.
 * @return On success, the complete generated header text. On failure, an
 *         error describing which entry could not be rendered.
 *
 * @details
 * The generated header is the single shared source of alarm identities:
 * one `INSTR_ALARM_<UPPER_SNAKE>` `constexpr` per entry, plus a static entry
 * table carrying id, severity, description, probable cause, specific
 * problem, escalation policy, auto-clear policy, and HQ-reportability. All
 * daemons and tools compile against this header, which is what keeps 10+
 * consumers in lockstep with a single YAML edit.
 *
 * Output is deterministic: identical (cat, version) inputs produce
 * byte-identical text, keeping the build reproducible.
 *
 * @note This function is pure — no filesystem or network access.
 *
 * @see parse_catalog(), AlarmCatalog
 */
Result<std::string> render_header(const AlarmCatalog& cat, const std::string& version);

/**
 * @brief Decode canonical id text `"0x1_04_1_0007"` into a packed `uint32_t` (validated field widths).
 * @param text Canonical id text with underscore-grouped hex fields.
 * @return On success, the packed alarm id. On failure, an error naming the
 *         malformed field (wrong digit count, non-hex digit, unexpected
 *         grouping).
 *
 * @details
 * The canonical text groups the packed id fields with underscores; each
 * group has a fixed hex-digit width, and this function enforces those widths
 * before packing. It is the id-encoding counterpart of @ref severity_name:
 * parse_catalog routes every id field through it, so a typo in the catalog
 * is rejected here with a precise, machine-checkable reason.
 *
 * @note This function is pure — no filesystem or network access.
 *
 * @see parse_catalog()
 */
Result<uint32_t> parse_alarm_id_text(const std::string& text);

} // namespace catalog
/**
 * @} End of instrumentation alarm-catalog module group.
 */
} // namespace instr