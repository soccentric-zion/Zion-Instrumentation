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
 * @file lib/platform/src/descriptors.cpp
 * @brief Implementation of declarative descriptor loading: devices.yaml, resource profiles, and the capability manifest validated at load (REQ §.1).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/platform/descriptors.h"

#include <yaml-cpp/yaml.h>

namespace instr
{
/// @brief Platform abstraction: board and hardware access behind IPlatform (REQ §5).
namespace platform
{

// Convert YAML subtree to JValue via the shared projection (declared in
// json.h as text-based; here we build from yaml-cpp nodes directly using the
// same scalar rules as json_from_yaml_text).
/// @brief Recursively project a YAML node tree to JSON.
static JValue yaml_to_json(const YAML::Node& n);

/// @brief Convert one YAML scalar node to the equivalent JValue.
/// @param n YAML scalar node to convert.
/// @return The equivalent JValue.
static JValue yaml_scalar(const YAML::Node& n)
{
    if (n.IsNull()) return JValue();
    if (n.IsScalar())
    {
        const std::string& raw = n.Scalar();
        if (n.Tag() == "!!str") return JValue(raw);
        if (raw == "true") return JValue(true);
        if (raw == "false") return JValue(false);
        if (raw == "null" || raw == "~" || raw.empty()) return JValue();
        try
        {
            size_t pos;
            double d = std::stod(raw, &pos);
            if (pos == raw.size()) return JValue(d);
        }
        catch (...)
        {
        }
        return JValue(raw);
    }
    if (n.IsSequence())
    {
        JValue arr = JValue::array();
        for (const auto& item : n) arr.push(yaml_to_json(item));
        return arr;
    }
    JValue obj = JValue::object();
    for (const auto& kv : n) obj.set(kv.first.as<std::string>(), yaml_to_json(kv.second));
    return obj;
}

/// @brief Recursively project a YAML node tree to JSON.
/// @param n YAML node tree to project.
/// @return The equivalent JValue.
static JValue yaml_to_json(const YAML::Node& n)
{
    return yaml_scalar(n);
}

/// @brief Validate descriptor YAML against its schema with source-context errors.
/// @param schema JSON Schema to validate against.
/// @param yaml_text Descriptor YAML source.
/// @param source Origin of the text, used in errors.
/// @param out Receives the parse failure text when invalid.
/// @return True when the text conforms.
static Error validate(const JValue& schema, const std::string& yaml_text, const std::string& source, JValue* out)
{
    auto j = json_from_yaml_text(yaml_text);
    if (!j.ok()) return j.error;
    std::vector<JsonValidator::Issue> issues;
    if (!JsonValidator::validate(schema, *j, &issues))
    {
        std::string msg = "descriptor rejected (" + source + "):";
        for (const auto& is : issues) msg += " [" + is.path + "] " + is.message + ";";
        return Error::make(Errc::InvalidArgument, msg, source);
    }
    *out = *j;
    return Error::none();
}

/// @brief Load a device descriptor YAML, validated against @p schema.
/// @param yaml_text Descriptor YAML source.
/// @param schema JSON Schema to validate against.
/// @param source Origin of the text, used in errors.
/// @return The parsed descriptor, or an error.
Result<DeviceDescriptor> load_device_descriptor(const std::string& yaml_text, const JValue& schema, const std::string& source)
{
    JValue j;
    Error e = validate(schema, yaml_text, source, &j);
    if (!e.ok()) return Result<DeviceDescriptor>::fail(e);
    DeviceDescriptor d;
    d.board = j.find("board") ? (*j.find("board")).as_string() : "";
    const JValue* devs = j.find("devices");
    if (devs)
    {
        for (const auto& dv : devs->items())
        {
            DeviceSpec s;
            if (const JValue* v = dv.find("name")) s.name = v->as_string();
            if (const JValue* v = dv.find("kind")) s.kind = v->as_string();
            if (const JValue* v = dv.find("controls"))
                for (const auto& c : v->items()) s.controls.push_back(c.as_string());
            if (const JValue* v = dv.find("checks"))
            {
                for (const auto& c : v->items())
                {
                    DeviceCheck ch;
                    if (const JValue* n = c.find("name")) ch.name = n->as_string();
                    if (const JValue* o = c.find("op")) ch.op = o->as_string();
                    if (const JValue* p = c.find("params"))
                        for (const auto& pk : p->members()) ch.params[pk.first] = pk.second.is_string() ? pk.second.as_string() : pk.second.dump();
                    s.checks.push_back(ch);
                }
            }
            if (const JValue* p = dv.find("params"))
                for (const auto& pk : p->members()) s.params[pk.first] = pk.second.is_string() ? pk.second.as_string() : pk.second.dump();
            d.devices.push_back(s);
        }
    }
    return Result<DeviceDescriptor>::good(std::move(d));
}

/// @brief Load a resource profile YAML, validated against @p schema.
/// @param yaml_text Profile YAML source.
/// @param schema JSON Schema to validate against.
/// @param source Origin of the text, used in errors.
/// @return The parsed profile, or an error.
Result<ResourceProfile> load_resource_profile(const std::string& yaml_text, const JValue& schema, const std::string& source)
{
    JValue j;
    Error e = validate(schema, yaml_text, source, &j);
    if (!e.ok()) return Result<ResourceProfile>::fail(e);
    ResourceProfile p;
    if (const JValue* v = j.find("platform_class")) p.platform_class = v->as_string();
    if (const JValue* v = j.find("sample_interval_default_s")) p.sample_interval_default_s = v->as_number();
    if (const JValue* v = j.find("sample_intervals_per_class_s"))
        for (const auto& kv : v->members()) p.sample_intervals_per_class_s[kv.first] = kv.second.as_number();
    if (const JValue* v = j.find("highres_window_s")) p.highres_window_s = v->as_number();
    if (const JValue* v = j.find("highres_interval_s")) p.highres_interval_s = v->as_number();
    if (const JValue* v = j.find("telemetry_interval_s")) p.telemetry_interval_s = v->as_number();
    if (const JValue* v = j.find("telemetry_batch_max_records")) p.telemetry_batch_max_records = uint64_t(v->as_number());
    auto get_u64 = [&](const char* key, uint64_t dflt) {
        const JValue* v = j.find(key);
        return v ? uint64_t(v->as_number(double(dflt))) : dflt;
    };
    p.spool_max_bytes = get_u64("spool_max_bytes", p.spool_max_bytes);
    p.logs_max_bytes = get_u64("logs_max_bytes", p.logs_max_bytes);
    p.ota_staging_bytes = get_u64("ota_staging_bytes", p.ota_staging_bytes);
    p.backup_max_bytes = get_u64("backup_max_bytes", p.backup_max_bytes);
    p.timeline_max_events = get_u64("timeline_max_events", p.timeline_max_events);
    return Result<ResourceProfile>::good(std::move(p));
}

/// @brief Load a capability manifest YAML, validated against @p schema.
/// @param yaml_text Manifest YAML source.
/// @param schema JSON Schema to validate against.
/// @param source Origin of the text, used in errors.
/// @return The parsed manifest, or an error.
Result<CapabilityManifest> load_capability_manifest(const std::string& yaml_text, const JValue& schema, const std::string& source)
{
    JValue j;
    Error e = validate(schema, yaml_text, source, &j);
    if (!e.ok()) return Result<CapabilityManifest>::fail(e);
    CapabilityManifest m;
    if (const JValue* v = j.find("board")) m.board = v->as_string();
    if (const JValue* v = j.find("capabilities"))
        for (const auto& c : v->items()) m.capabilities.push_back(c.as_string());
    return Result<CapabilityManifest>::good(std::move(m));
}

} // namespace platform
} // namespace instr