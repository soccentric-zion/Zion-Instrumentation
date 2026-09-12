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
 * @file lib/common/include/instr/common/config.h
 * @brief configuration with origin tracking and merge
 *
 * semantics (REQ §4, §9). Every key carries an origin: default / app / local
 * / hq. Resets and HQ pushes consult origin to decide what they may
 * overwrite; merge semantics preserve operator edits when a release adds
 * keys. All formats validated against JSON Schemas at load.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <map>
#include <string>
#include <vector>

#include "instr/common/error.h"
#include "instr/common/json.h"

namespace instr
{

/// @brief Who last set a configuration value; drives overwrite and reset rules.
enum class Origin
{
    Default = 0, ///< Shipped with the release.
    App = 1, ///< Set by an application via the provisioning API.
    Local = 2, ///< Set by the on-device operator; protected from overwrite.
    Hq = 3 ///< Pushed by HQ via istate.
};

/// @brief Stable lowercase name of an origin ("default", "app", "local", "hq").
/// @param o Origin to name.
/// @return Static string for the origin.
const char* origin_name(Origin o);

/// @brief Parse an origin name (case-insensitive).
/// @param n Name as produced by origin_name().
/// @return The parsed origin, or an InvalidArgument error when unknown.
Result<Origin> origin_from_name(const std::string&);

/// @brief A daemon configuration: flattened dot-path keys with per-key origin tracking.
class ConfigDocument
{
  public:
    /// @brief Construct an empty document (no keys).
    ConfigDocument() = default;

    /// @brief Parse YAML text, validate against the JSON Schema, flatten to dot-paths.
    ///
    /// @p source_name appears in rejection errors for file-and-line diagnosability.
    ///
    /// @param yaml_text YAML source text.
    /// @param schema JSON Schema the document must satisfy.
    /// @param source_name Human-readable origin of the text, used in errors.
    /// @return Parsed document, or a diagnosable Error listing every violation.
    static Result<ConfigDocument> parse(const std::string& yaml_text, const JValue& schema, const std::string& source_name);

    /// @brief Empty document (no schema validation) for programmatic construction.
    /// @return An empty document.
    static ConfigDocument empty();

    /// @brief True when the dot-path exists in this document.
    /// @param dot_path Flattened key, e.g. "network.ifaces.0.mode".
    /// @return True when present.
    bool has(const std::string& dot_path) const;
    /// @brief String value at @p dot_path with fallback.
    std::string get_string(const std::string& dot_path, const std::string& dflt = "") const;
    /// @brief Integer value at @p dot_path with fallback.
    /// @param dot_path Flattened key.
    /// @param dflt Value returned when the key is absent or not an integer.
    /// @return The integer value.
    int64_t get_int(const std::string& dot_path, int64_t dflt = 0) const;
    /// @brief Floating-point value at @p dot_path with fallback.
    /// @param dot_path Flattened key.
    /// @param dflt Value returned when the key is absent or not numeric.
    /// @return The numeric value.
    double get_double(const std::string& dot_path, double dflt = 0) const;
    /// @brief Boolean value at @p dot_path with fallback.
    /// @param dot_path Flattened key.
    /// @param dflt Value returned when the key is absent or not a bool.
    /// @return The boolean value.
    bool get_bool(const std::string& dot_path, bool dflt = false) const;
    /// @brief List values (e.g. log sources); empty when absent.
    /// @param dot_path Flattened key.
    /// @return The list elements; empty when absent.
    std::vector<std::string> get_list(const std::string& dot_path) const;

    /// @brief Raw value as a YAML-ish scalar string.
    /// @param dot_path Flattened key.
    /// @return The scalar text, or "" when the key is absent.
    std::string raw(const std::string& dot_path) const;

    /// @brief Set a scalar string value with its origin.
    /// @param dot_path Flattened key, e.g. "network.ifaces.0.mode".
    /// @param scalar Value as text.
    /// @param origin Who set this value.
    void set(const std::string& dot_path, const std::string& scalar, Origin origin);
    /// @brief Set a numeric value with its origin.
    /// @param dot_path Flattened key.
    /// @param v Value to store (rendered as text).
    /// @param origin Who set this value.
    void set_number(const std::string& dot_path, double v, Origin origin);
    /// @brief Set a boolean value with its origin.
    /// @param dot_path Flattened key.
    /// @param v Value to store (rendered as text).
    /// @param origin Who set this value.
    void set_bool(const std::string& dot_path, bool v, Origin origin);

    /// @brief Origin of a key.
    /// @param dot_path Flattened key.
    /// @return The recorded origin; Origin::Default when the key is unset.
    Origin origin_of(const std::string& dot_path) const;
    /// @brief View of all flattened key → scalar-text values.
    /// @return View of all flattened key → scalar-text values.
    const std::map<std::string, std::string>& values() const
    {
        return values_;
    }
    /// @brief View of all key → origin assignments.
    /// @return View of all key → origin assignments.
    const std::map<std::string, Origin>& origins() const
    {
        return origins_;
    }

    /// @brief Merge @p incoming (origin @p in_origin) into this document.
    ///
    /// HQ pushes overwrite default/app/hq keys; local (operator) keys are
    /// protected unless @p force. Merge semantics preserve operator edits
    /// when a release adds keys.
    ///
    /// @param incoming Document to merge.
    /// @param in_origin Origin claimed for @p incoming.
    /// @param force When true, overwrite even protected local keys.
    /// @return The rejected dot-paths (protected keys not overwritten).
    std::vector<std::string> merge_from(const ConfigDocument& incoming, Origin in_origin, bool force);

    /// @brief Release merge: add keys that are missing entirely (new defaults).
    ///
    /// Every existing value is kept.
    ///
    /// @param defaults Release defaults to merge in.
    /// @return The added dot-paths.
    std::vector<std::string> merge_release(const ConfigDocument& defaults);

    /// @brief System reset: drop everything except origin=Default entries (REQ §6.1).
    void reset_to_defaults();

    /// @brief Serialise back to YAML (nested maps; scalar strings preserved verbatim).
    /// @return YAML text for the values.
    std::string to_yaml() const;
    /// @brief Serialise the origins sidecar (dot-path → origin name) as YAML.
    /// @return YAML text for the origins.
    std::string origins_to_yaml() const;
    /// @brief Load an origins sidecar produced by origins_to_yaml().
    /// @param text YAML text of the sidecar.
    void load_origins_yaml(const std::string& text);

  private:
    std::map<std::string, std::string> values_; ///< Flattened key → scalar text.
    std::map<std::string, Origin> origins_; ///< Flattened key → who set it.
};

/// @brief Load a JSON schema from text (for schemas/*.json at daemon startup).
/// @param text Schema JSON text.
/// @return Parsed schema, or an Error describing the parse failure.
Result<JValue> load_schema_text(const std::string& text);

} // namespace instr