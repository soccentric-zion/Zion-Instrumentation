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
 * @file catalog/src/codegen.cpp
 * @brief Implementation of the alarm catalog code generator (REQ §8): YAML catalog parsing, entry validation, and rendering of the shared C++ alarm-identity header.
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/catalog/codegen.h"

#include "instr/common/alarm_codec.h"

#include <yaml-cpp/yaml.h>

#include <cctype>
#include <sstream>

namespace instr
{
/// @brief Alarm catalog parsing and C++ header code generation (§7.6, §8).
namespace catalog
{

/// @brief Canonical lowercase severity name.
/// @param s Severity to name.
/// @return Canonical lowercase name.
const char* severity_name(Severity s)
{
    switch (s)
    {
    case Severity::Info: return "INFO";
    case Severity::Warning: return "WARNING";
    case Severity::Minor: return "MINOR";
    case Severity::Major: return "MAJOR";
    case Severity::Critical: return "CRITICAL";
    }
    return "INFO";
}

/// @brief Parse a severity name (case-insensitive).
/// @param s Name to parse.
/// @param err Receives the failure when unknown.
/// @param n Name to parse.
Severity severity_from_name(const std::string& n, Error* err)
{
    std::string u;
    for (char c : n) u += char(std::toupper((unsigned char)c));
    if (u == "INFO") return Severity::Info;
    if (u == "WARNING") return Severity::Warning;
    if (u == "MINOR") return Severity::Minor;
    if (u == "MAJOR") return Severity::Major;
    if (u == "CRITICAL") return Severity::Critical;
    if (err) *err = Error::make(Errc::InvalidArgument, "unknown severity: " + n, "catalog");
    return Severity::Info;
}

/// @brief Encode id text "0x1_04_1_0007" → uint32 (validated field widths).
/// @param text Canonical id text.
/// @return The packed id, or an error for malformed fields.
Result<uint32_t> parse_alarm_id_text(const std::string& text)
{
    // Format: 0x<domain>_<module>_<type>_<index>
    if (text.rfind("0x", 0) != 0) return Result<uint32_t>::fail(Errc::InvalidArgument, "alarm id must start with 0x", text);
    std::vector<std::string> parts;
    std::string body = text.substr(2);
    std::stringstream ss(body);
    std::string item;
    while (std::getline(ss, item, '_')) parts.push_back(item);
    if (parts.size() != 4) return Result<uint32_t>::fail(Errc::InvalidArgument, "alarm id must have 4 fields: domain_module_type_index", text);
    if (parts[0].size() != 1 || parts[1].size() != 2 || parts[2].size() != 1 || parts[3].size() != 4) return Result<uint32_t>::fail(Errc::InvalidArgument, "field widths must be 1_2_1_4 hex digits", text);
    static const uint32_t shifts[4] = {28, 20, 16, 0};
    uint32_t v = 0;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        uint32_t field = 0;
        for (char c : parts[i])
        {
            int d;
            if (c >= '0' && c <= '9')
                d = c - '0';
            else if (c >= 'a' && c <= 'f')
                d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                d = c - 'A' + 10;
            else
                return Result<uint32_t>::fail(Errc::InvalidArgument, "bad hex digit in alarm id", text);
            field = (field << 4) | uint32_t(d);
        }
        v |= field << shifts[i];
    }
    return Result<uint32_t>::good(v);
}

/// @brief Parse + validate a catalog; duplicate IDs or names are loud errors.
/// @param yaml_text Catalog source text.
/// @return Parsed catalog, or a diagnosable error.
Result<AlarmCatalog> parse_catalog(const std::string& yaml_text)
{
    try
    {
        YAML::Node root = YAML::Load(yaml_text);
        AlarmCatalog cat;
        if (root["version"]) cat.version = root["version"].as<uint32_t>(1);
        const YAML::Node& entries = root["entries"];
        if (!entries || !entries.IsSequence()) return Result<AlarmCatalog>::fail(Errc::InvalidArgument, "catalog must contain an 'entries' list", "alarms.yaml");
        std::map<uint32_t, std::string> ids;
        std::map<std::string, bool> names;
        for (const auto& e : entries)
        {
            AlarmEntry a;
            Error err;
            auto idr = parse_alarm_id_text(e["id"].as<std::string>(""));
            a.id = idr.ok() ? *idr : 0;
            if (a.id == 0) return Result<AlarmCatalog>::fail(Errc::InvalidArgument, "invalid alarm id", e["id"].as<std::string>("?"));
            a.name = e["name"].as<std::string>("");
            if (a.name.empty()) return Result<AlarmCatalog>::fail(Errc::InvalidArgument, "entry missing name", "catalog");
            a.severity = severity_from_name(e["severity"].as<std::string>("INFO"), &err);
            if (!err.ok()) return Result<AlarmCatalog>::fail(err);
            a.probable_cause = e["probable_cause"].as<std::string>("");
            a.specific_problem = e["specific_problem"].as<std::string>("");
            a.description = e["description"].as<std::string>("");
            a.escalation = e["escalation"].as<std::string>("none");
            a.auto_clear = e["auto_clear"].as<std::string>("manual");
            a.hq_reportable = e["hq_reportable"].as<bool>(true);
            if (ids.count(a.id)) return Result<AlarmCatalog>::fail(Errc::InvalidArgument, "duplicate alarm id", a.name);
            if (names.count(a.name)) return Result<AlarmCatalog>::fail(Errc::InvalidArgument, "duplicate alarm name", a.name);
            ids[a.id] = a.name;
            names[a.name] = true;
            cat.entries.push_back(a);
        }
        return Result<AlarmCatalog>::good(std::move(cat));
    }
    catch (const std::exception& e)
    {
        return Result<AlarmCatalog>::fail(Errc::InvalidArgument, std::string("catalog parse: ") + e.what(), "alarms.yaml");
    }
}

/// @brief Convert a symbolic name to INSTR_ALARM_<UPPER_SNAKE> form.
/// @param name Symbolic alarm name.
/// @return The UPPER_SNAKE constant suffix.
static std::string to_upper_snake(const std::string& name)
{
    std::string out;
    for (char c : name)
    {
        if (c == '-' || c == '.')
            out += '_';
        else if (std::isalpha((unsigned char)c))
            out += char(std::toupper((unsigned char)c));
        else if (std::isdigit((unsigned char)c))
            out += c;
        else
            out += '_';
    }
    return out;
}

/// @brief Escape and quote a string as a C literal.
/// @param s Text to escape.
/// @return The quoted C literal.
static std::string c_str(const std::string& s)
{
    std::string out = "\"";
    for (char c : s)
    {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n')
        {
            out += "\\n";
            continue;
        }
        out += c;
    }
    return out + "\"";
}

/// @brief Render the C++ header: INSTR_ALARM_* constants + entry table.
/// @param cat Parsed catalog.
/// @param version Catalog version stamped into the output.
/// @return Generated header text, or an error.
Result<std::string> render_header(const AlarmCatalog& cat, const std::string& version)
{
    std::ostringstream o;
    o << "// Generated from catalog/alarms.yaml by alarmgen — DO NOT EDIT.\n"
      << "// Catalog version " << cat.version << ", suite version " << version << "\n"
      << "// Every application uses the same IDs (REQ §8); an ID is decodable\n"
      << "// without the catalog: 0x1_04_1_0007 = hardware / hardware / equipment / 7.\n"
      << "#ifndef INSTR_ALARMS_GEN_H\n#define INSTR_ALARMS_GEN_H\n\n"
      << "#include <cstdint>\n\n"
      << "namespace instr {\n\n"
      << "enum class AlarmSeverity : uint8_t { INFO, WARNING, MINOR, MAJOR, CRITICAL };\n\n";
    for (const auto& e : cat.entries)
    {
        o << "// " << e.description << "\n"
          << "constexpr uint32_t INSTR_ALARM_" << to_upper_snake(e.name) << " = 0x" << std::hex << e.id << std::dec << "u;  // " << alarm_id_string(e.id) << "\n";
    }
    o << "\nstruct AlarmCatalogEntry {\n"
      << "  uint32_t id;\n"
      << "  const char* name;\n"
      << "  AlarmSeverity severity;\n"
      << "  const char* probable_cause;\n"
      << "  const char* specific_problem;\n"
      << "  const char* description;\n"
      << "  const char* escalation;\n"
      << "  const char* auto_clear;\n"
      << "  bool hq_reportable;\n"
      << "};\n\n"
      << "inline constexpr AlarmCatalogEntry INSTR_ALARM_CATALOG[] = {\n";
    for (const auto& e : cat.entries)
    {
        o << "    {" << std::hex << "0x" << e.id << std::dec << "u, " << c_str(e.name) << ", AlarmSeverity::" << severity_name(e.severity) << ", " << c_str(e.probable_cause) << ", " << c_str(e.specific_problem) << ", " << c_str(e.description) << ", " << c_str(e.escalation) << ", "
          << c_str(e.auto_clear) << ", " << (e.hq_reportable ? "true" : "false") << "},\n";
    }
    o << "};\n\n"
      << "inline constexpr size_t INSTR_ALARM_CATALOG_SIZE = sizeof(INSTR_ALARM_CATALOG) / "
         "sizeof(INSTR_ALARM_CATALOG[0]);\n\n"
      << "}  // namespace instr\n\n#endif  // INSTR_ALARMS_GEN_H\n";
    return Result<std::string>::good(o.str());
}

} // namespace catalog
} // namespace instr