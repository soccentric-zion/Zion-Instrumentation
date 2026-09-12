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
 * @file lib/common/src/config.cpp
 * @brief Implementation of configuration loading: YAML parsing with JSON Schema validation, origin tracking, and merge semantics that preserve operator edits (REQ §, §).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/common/config.h"

#include "instr/common/config_sink.h"
#include "instr/common/filesystem.h"

#include <yaml-cpp/yaml.h>

#include <cmath>

namespace instr
{

/// @brief Stable lowercase name of an origin ("default", "app", "local", "hq").
/// @param o Origin to name.
/// @return Static string for the origin.
const char* origin_name(Origin o)
{
    switch (o)
    {
    case Origin::Default: return "default";
    case Origin::App: return "app";
    case Origin::Local: return "local";
    case Origin::Hq: return "hq";
    }
    return "default";
}

/// @brief Parse an origin name (case-insensitive).
/// @param n Name as produced by origin_name().
/// @return The parsed origin, or an InvalidArgument error when unknown.
Result<Origin> origin_from_name(const std::string& n)
{
    if (n == "default") return Result<Origin>::good(Origin::Default);
    if (n == "app") return Result<Origin>::good(Origin::App);
    if (n == "local") return Result<Origin>::good(Origin::Local);
    if (n == "hq") return Result<Origin>::good(Origin::Hq);
    return Result<Origin>::fail(Errc::InvalidArgument, "unknown origin: " + n, "origin_from_name");
}

/// @brief Load a JSON schema from text (for schemas/*.json at daemon startup).
/// @param text Schema JSON text.
/// @return Parsed schema, or an Error describing the parse failure.
Result<JValue> load_schema_text(const std::string& text)
{
    auto j = json_parse(text);
    if (!j.ok()) return j;
    if (!(*j).is_object()) return Result<JValue>::fail(Errc::InvalidArgument, "schema root must be an object", "load_schema_text");
    return j;
}

/// @brief Empty document (no schema validation) for programmatic construction.
ConfigDocument ConfigDocument::empty()
{
    return ConfigDocument();
}

/// @brief Parse YAML text, validate against the JSON Schema, flatten to dot-paths.
///
/// @p source_name appears in rejection errors for file-and-line diagnosability.
///
/// @param yaml_text YAML source text.
/// @param schema JSON Schema the document must satisfy.
/// @param source_name Human-readable origin of the text, used in errors.
/// @return Parsed document, or a diagnosable Error listing every violation.
Result<ConfigDocument> ConfigDocument::parse(const std::string& yaml_text, const JValue& schema, const std::string& source_name)
{
    auto j = json_from_yaml_text(yaml_text);
    if (!j.ok()) return Result<ConfigDocument>::fail(Error::make(j.error.code, j.error.message, source_name));
    std::vector<JsonValidator::Issue> issues;
    if (!JsonValidator::validate(schema, *j, &issues))
    {
        std::string msg = "configuration rejected (" + source_name + "):";
        for (const auto& is : issues) msg += " [" + is.path + "] " + is.message + ";";
        return Result<ConfigDocument>::fail(Errc::InvalidArgument, msg, source_name);
    }
    ConfigDocument doc;
    // Flatten: walk the JSON projection.
    // Lambda flattening object trees into dot-paths.
    struct Flatten
    {
        /// @brief Recursively flatten a JSON document into dot-path → scalar-text pairs.
        /// @param v JSON node to flatten.
        /// @param prefix Dot-path prefix for nested keys.
        /// @param out Receives the flattened pairs.
        static void walk(const JValue& v, const std::string& prefix, std::map<std::string, std::string>* out)
        {
            if (v.is_object())
            {
                for (const auto& kv : v.members())
                {
                    std::string p = prefix.empty() ? kv.first : prefix + "." + kv.first;
                    walk(kv.second, p, out);
                }
            }
            else if (v.is_array())
            {
                // Lists stay as YAML in the raw projection; store size marker plus
                // indexed items for structured access.
                out->operator[](prefix + ".__count") = std::to_string(v.items().size());
                for (size_t i = 0; i < v.items().size(); ++i)
                {
                    std::string p = prefix + "." + std::to_string(i);
                    walk(v.items()[i], p, out);
                }
            }
            else if (v.is_null()) { out->operator[](prefix) = ""; }
            else if (v.is_bool()) { out->operator[](prefix) = v.as_bool() ? "true" : "false"; }
            else if (v.is_number())
            {
                double d = v.as_number();
                if (d == std::floor(d) && std::fabs(d) < 9.0e15)
                    out->operator[](prefix) = std::to_string(static_cast<long long>(d));
                else
                    out->operator[](prefix) = std::to_string(d);
            }
            else { out->operator[](prefix) = v.as_string(); }
        }
    };
    Flatten::walk(*j, "", &doc.values_);
    for (const auto& kv : doc.values_) doc.origins_[kv.first] = Origin::Default;
    return Result<ConfigDocument>::good(std::move(doc));
}

/// @brief True when the dot-path exists in this document.
/// @param dot_path Flattened key, e.g. "network.ifaces.0.mode".
/// @return True when present.
bool ConfigDocument::has(const std::string& dot_path) const
{
    return values_.count(dot_path) > 0;
}

/// @brief Raw value as a YAML-ish scalar string.
/// @param dot_path Flattened key.
/// @return The scalar text, or "" when the key is absent.
std::string ConfigDocument::raw(const std::string& dot_path) const
{
    auto it = values_.find(dot_path);
    return it == values_.end() ? std::string() : it->second;
}

/// @brief String value at @p dot_path with fallback.
/// @param dot_path Flattened key.
/// @param dflt Value returned when absent or not a string.
/// @return The string value.
std::string ConfigDocument::get_string(const std::string& dot_path, const std::string& dflt) const
{
    std::string v = raw(dot_path);
    if (v.empty() && !has(dot_path)) return dflt;
    return v;
}

/// @brief Integer value at @p dot_path with fallback.
/// @param dot_path Flattened key.
/// @param dflt Value returned when the key is absent or not an integer.
/// @return The integer value.
int64_t ConfigDocument::get_int(const std::string& dot_path, int64_t dflt) const
{
    std::string v = raw(dot_path);
    if (v.empty() && !has(dot_path)) return dflt;
    try
    {
        return std::stoll(v);
    }
    catch (...)
    {
        return dflt;
    }
}

/// @brief Floating-point value at @p dot_path with fallback.
/// @param dot_path Flattened key.
/// @param dflt Value returned when the key is absent or not numeric.
/// @return The numeric value.
double ConfigDocument::get_double(const std::string& dot_path, double dflt) const
{
    std::string v = raw(dot_path);
    if (v.empty() && !has(dot_path)) return dflt;
    try
    {
        return std::stod(v);
    }
    catch (...)
    {
        return dflt;
    }
}

/// @brief Boolean value at @p dot_path with fallback.
/// @param dot_path Flattened key.
/// @param dflt Value returned when the key is absent or not a bool.
/// @return The boolean value.
bool ConfigDocument::get_bool(const std::string& dot_path, bool dflt) const
{
    std::string v = raw(dot_path);
    if (v.empty() && !has(dot_path)) return dflt;
    return v == "true" || v == "True" || v == "1" || v == "yes";
}

/// @brief List values (e.g. log sources); empty when absent.
/// @param dot_path Flattened key.
/// @return The list elements; empty when absent.
std::vector<std::string> ConfigDocument::get_list(const std::string& dot_path) const
{
    std::vector<std::string> out;
    std::string count_key = dot_path + ".__count";
    if (!has(count_key)) return out;
    int64_t n = get_int(count_key, 0);
    for (int64_t i = 0; i < n; ++i) out.push_back(raw(dot_path + "." + std::to_string(i)));
    return out;
}

/// @brief Set a scalar string value with its origin.
/// @param dot_path Flattened key, e.g. "network.ifaces.0.mode".
/// @param scalar Value as text.
/// @param origin Who set this value.
void ConfigDocument::set(const std::string& dot_path, const std::string& scalar, Origin origin)
{
    values_[dot_path] = scalar;
    origins_[dot_path] = origin;
    // Clear a stale list marker when a scalar replaces a list.
    values_.erase(dot_path + ".__count");
}

/// @brief Set a numeric value with its origin.
/// @param dot_path Flattened key.
/// @param v Value to store (rendered as text).
/// @param origin Who set this value.
void ConfigDocument::set_number(const std::string& dot_path, double v, Origin origin)
{
    if (v == std::floor(v) && std::fabs(v) < 9.0e15)
        set(dot_path, std::to_string(static_cast<long long>(v)), origin);
    else
        set(dot_path, std::to_string(v), origin);
}

/// @brief Set a boolean value with its origin.
/// @param dot_path Flattened key.
/// @param v Value to store (rendered as text).
/// @param origin Who set this value.
void ConfigDocument::set_bool(const std::string& dot_path, bool v, Origin origin)
{
    set(dot_path, v ? "true" : "false", origin);
}

/// @brief Origin of a key.
/// @param dot_path Flattened key.
/// @return The recorded origin; Origin::Default when the key is unset.
Origin ConfigDocument::origin_of(const std::string& dot_path) const
{
    auto it = origins_.find(dot_path);
    return it == origins_.end() ? Origin::Default : it->second;
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
std::vector<std::string> ConfigDocument::merge_from(const ConfigDocument& incoming, Origin in_origin, bool force)
{
    std::vector<std::string> rejected;
    for (const auto& kv : incoming.values_)
    {
        auto oit = origins_.find(kv.first);
        Origin existing = (oit == origins_.end()) ? Origin::Default : oit->second;
        // Origin rules: an HQ push may overwrite default/app/hq keys; local
        // operator edits are protected unless explicitly forced.
        bool allowed = true;
        if (in_origin == Origin::Hq && existing == Origin::Local && !force) allowed = false;
        if (in_origin == Origin::App && existing == Origin::Local && !force) allowed = false;
        if (!allowed)
        {
            rejected.push_back(kv.first);
            continue;
        }
        values_[kv.first] = kv.second;
        origins_[kv.first] = in_origin;
    }
    return rejected;
}

/// @brief Release merge: add keys that are missing entirely (new defaults).
///
/// Every existing value is kept.
///
/// @param defaults Release defaults to merge in.
/// @return The added dot-paths.
std::vector<std::string> ConfigDocument::merge_release(const ConfigDocument& defaults)
{
    std::vector<std::string> added;
    for (const auto& kv : defaults.values_)
    {
        if (!values_.count(kv.first))
        {
            values_[kv.first] = kv.second;
            origins_[kv.first] = Origin::Default;
            added.push_back(kv.first);
        }
    }
    return added;
}

/// @brief System reset: drop everything except origin=Default entries (REQ §6.1).
void ConfigDocument::reset_to_defaults()
{
    for (auto it = values_.begin(); it != values_.end();)
    {
        auto oit = origins_.find(it->first);
        if (oit != origins_.end() && oit->second != Origin::Default)
        {
            origins_.erase(oit);
            it = values_.erase(it);
        }
        else { ++it; }
    }
}

// ---- YAML serialisation ----

namespace
{

// Escapes a scalar minimally: quote when it would otherwise parse as another
// type or contain YAML specials.
/// @brief Decide whether a scalar needs YAML quoting.
/// @param s Scalar text to test.
bool needs_quotes(const std::string& s)
{
    // Quote only when the scalar would break YAML parsing: empty, leading or
    // trailing whitespace, block indicators, or inline specials. Typed scalars
    // (true/30) stay unquoted so they round-trip as the same value.
    if (s.empty()) return true;
    if (s.front() == ' ' || s.front() == '\t' || s.back() == ' ' || s.back() == '\t') return true;
    if (s.find(':') != std::string::npos || s.find(" #") != std::string::npos) return true;
    if (s.find_first_of("{}[]&*!|>'\"%@`\n") != std::string::npos) return true;
    if (s.rfind("-", 0) == 0 || s.rfind("? ", 0) == 0) return true;
    return false;
}

/// @brief Quote a scalar for YAML output when required.
/// @param s Scalar text to quote.
/// @param s Scalar text to quote.
/// @return The quoted (or verbatim) text.
std::string quote(const std::string& s)
{
    std::string out = "\"";
    for (char c : s)
    {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

struct YamlWriter
{
    /// @brief Recursively rebuild nested YAML from flattened dot-path values.
    /// @param values Flattened values.
    /// @param origins Origin sidecar.
    /// @param prefix Current dot-path.
    /// @param indent Current indent level.
    /// @param out YAML buffer.
    static void walk(const std::map<std::string, std::string>& values, const std::map<std::string, Origin>& origins, const std::string& prefix, int indent, std::string* out)
    {
        // Collect direct children and list indices.
        std::vector<std::string> keys;
        std::vector<int64_t> indices;
        std::string count_key = prefix + ".__count";
        bool is_list = values.count(count_key) > 0;
        for (const auto& kv : values)
        {
            const std::string& p = kv.first;
            if (!prefix.empty())
            {
                if (p.rfind(prefix + ".", 0) != 0) continue;
            }
            std::string rest = prefix.empty() ? p : p.substr(prefix.size() + 1);
            if (rest.empty()) continue;
            size_t dot = rest.find('.');
            std::string head = (dot == std::string::npos) ? rest : rest.substr(0, dot);
            if (head == "__count") continue;
            // Unique-ify.
            if (!head.empty() && std::isdigit(static_cast<unsigned char>(head[0])) && head.find_first_not_of("0123456789") == std::string::npos)
            {
                int64_t idx = std::stoll(head);
                bool seen = false;
                for (int64_t v : indices) seen |= (v == idx);
                if (!seen) indices.push_back(idx);
            }
            else
            {
                bool seen = false;
                for (const auto& k : keys) seen |= (k == head);
                if (!seen) keys.push_back(head);
            }
        }
        std::string pad(indent * 2, ' ');
        if (is_list)
        {
            int64_t n = 0;
            auto it = values.find(count_key);
            if (it != values.end()) n = std::stoll(it->second);
            for (int64_t i = 0; i < n; ++i)
            {
                std::string child = prefix.empty() ? std::to_string(i) : prefix + "." + std::to_string(i);
                emit_item(values, origins, child, indent, out);
            }
            return;
        }
        for (const auto& k : keys)
        {
            std::string child = prefix.empty() ? k : prefix + "." + k;
            bool has_children = subtree_exists(values, prefix, k);
            if (has_children)
            {
                *out += pad + k + ":\n";
                walk(values, origins, child, indent + 1, out);
            }
            else
            {
                auto vit = values.find(child);
                if (vit == values.end()) continue;
                std::string v = vit->second;
                if (v.empty()) { *out += pad + k + ": null\n"; }
                else if (needs_quotes(v)) { *out += pad + k + ": " + quote(v) + "\n"; }
                else { *out += pad + k + ": " + v + "\n"; }
            }
        }
    }

    /// @brief True when any value key lives under the dot-path prefix.
    /// @param values Flattened values.
    /// @param prefix Parent dot-path.
    /// @param head Child segment to test.
    /// @return True when a subtree exists.
    static bool subtree_exists(const std::map<std::string, std::string>& values, const std::string& prefix, const std::string& head)
    {
        std::string child = prefix.empty() ? head : prefix + "." + head;
        // A child is a subtree if any key starts with "child." and is not just
        // the child itself, or child is a list with a __count marker.
        if (values.count(child + ".__count") > 0) return true;
        for (const auto& kv : values)
        {
            if (kv.first.rfind(child + ".", 0) == 0) return true;
        }
        return false;
    }

    /// @brief Emit one YAML mapping item at the current indentation.
    /// @param values Flattened values.
    /// @param origins Origin sidecar.
    /// @param item Dot-path of the item.
    /// @param indent Current indent level.
    /// @param out YAML buffer.
    static void emit_item(const std::map<std::string, std::string>& values, const std::map<std::string, Origin>& origins, const std::string& item, int indent, std::string* out)
    {
        std::string pad(indent * 2, ' ');
        // Item is a list element: could be scalar or subtree.
        if (values.count(item) > 0 && !subtree_exists(values, parent_of(item), last_of(item)))
        {
            std::string v = values.at(item);
            if (v.empty())
                *out += pad + "- null\n";
            else if (needs_quotes(v))
                *out += pad + "- " + quote(v) + "\n";
            else
                *out += pad + "- " + v + "\n";
            return;
        }
        // Subtree item: "- key: value" style with subsequent lines deeper.
        std::string body;
        walk(values, origins, item, 0, &body);
        bool first = true;
        size_t pos = 0;
        while (pos < body.size())
        {
            size_t nl = body.find('\n', pos);
            if (nl == std::string::npos) nl = body.size();
            std::string line = body.substr(pos, nl - pos);
            if (first)
            {
                *out += pad + "- " + (line.empty() ? "" : line.substr(0)) + "\n";
                first = false;
            }
            else { *out += pad + "  " + line + "\n"; }
            pos = nl + 1;
        }
    }

    /// @brief Parent dot-path of a flattened key ("" at the root).
    /// @param p Flattened key.
    /// @return The parent path, or "" at the root.
    static std::string parent_of(const std::string& p)
    {
        size_t pos = p.find_last_of('.');
        return pos == std::string::npos ? std::string() : p.substr(0, pos);
    }
    /// @brief Last path segment of a flattened key.
    /// @param p Flattened key.
    /// @return The last segment.
    static std::string last_of(const std::string& p)
    {
        size_t pos = p.find_last_of('.');
        return pos == std::string::npos ? p : p.substr(pos + 1);
    }
};

} // namespace

/// @brief Serialise back to YAML (nested maps; scalar strings preserved verbatim).
/// @return YAML text for the values.
std::string ConfigDocument::to_yaml() const
{
    std::string out;
    YamlWriter::walk(values_, origins_, "", 0, &out);
    return out;
}

/// @brief Serialise the origins sidecar (dot-path → origin name) as YAML.
/// @return YAML text for the origins.
std::string ConfigDocument::origins_to_yaml() const
{
    std::string out;
    for (const auto& kv : origins_) out += kv.first + ": " + origin_name(kv.second) + "\n";
    return out;
}

/// @brief Load an origins sidecar produced by origins_to_yaml().
/// @param text YAML text of the sidecar.
void ConfigDocument::load_origins_yaml(const std::string& text)
{
    try
    {
        YAML::Node root = YAML::Load(text);
        for (const auto& kv : root)
        {
            auto o = origin_from_name(kv.second.as<std::string>("default"));
            if (o.ok()) origins_[kv.first.as<std::string>()] = *o;
        }
    }
    catch (const std::exception&)
    {
        // Origins sidecar unreadable: keys stay at their parsed default origin.
    }
}

} // namespace instr
// ---- spoke-side schema loading (config_sink.h) ----

namespace instr
{

/// @brief Loads a daemon's JSON Schema from the image (installed into ${datadir}/instrumentation/schemas).
///
/// A missing schema is not fatal: the document is then accepted unvalidated
/// and the caller warns, because a packaging slip must not stop the device
/// booting (R10).
///
/// @param fs Filesystem abstraction for the load.
/// @param sysroot Root to resolve the schema directory under.
/// @param daemon Daemon name whose schema is wanted.
/// @return Parsed schema, or an Error (typically NotFound) when unavailable.
Result<JValue> load_daemon_schema(IFileSystem& fs, const std::string& sysroot, const std::string& daemon)
{
    const std::string path = sysroot + "/usr/share/instrumentation/schemas/" + daemon + ".schema.json";
    auto text = fs.read(path);
    if (!text.ok()) return Result<JValue>::fail(Errc::NotFound, "schema not installed", path);
    auto schema = load_schema_text(*text);
    if (!schema.ok()) return schema;
    return schema;
}

} // namespace instr
