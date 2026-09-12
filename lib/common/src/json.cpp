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
 * @file lib/common/src/json.cpp
 * @brief Implementation of the JSON value model, parser, and serialiser (REQ §, §).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/common/json.h"

#include <yaml-cpp/yaml.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <regex>

namespace instr
{

// ---------------- serialiser ----------------

/// @brief Append @p s to @p out as a quoted, escaped JSON string.
/// @param s String to escape.
/// @param out Buffer to append to.
static void dump_string(const std::string& s, std::string* out)
{
    out->push_back('"');
    for (char c : s)
    {
        switch (c)
        {
        case '"': *out += "\\\""; break;
        case '\\': *out += "\\\\"; break;
        case '\n': *out += "\\n"; break;
        case '\r': *out += "\\r"; break;
        case '\t': *out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                *out += buf;
            }
            else { out->push_back(c); }
        }
    }
    out->push_back('"');
}

/// @brief Recursively serialise @p v into @p out with the given indent.
/// @param v Value to serialise.
/// @param indent Spaces per level.
/// @param depth Current nesting depth.
/// @param out Buffer to append to.
static void dump_value(const JValue& v, int indent, int depth, std::string* out)
{
    const std::string pad(depth * indent, ' ');
    const std::string pad_in((depth + 1) * indent, ' ');
    switch (v.type())
    {
    case JValue::Type::Null: *out += "null"; break;
    case JValue::Type::Bool: *out += v.as_bool() ? "true" : "false"; break;
    case JValue::Type::Number: {
        double d = v.as_number();
        if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 9.0e15) { *out += std::to_string(static_cast<long long>(d)); }
        else
        {
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%.17g", d);
            *out += buf;
        }
        break;
    }
    case JValue::Type::String: dump_string(v.as_string(), out); break;
    case JValue::Type::Array: {
        *out += "[";
        bool first = true;
        for (const auto& item : v.items())
        {
            if (!first) *out += ",";
            first = false;
            if (indent > 0) *out += "\n" + pad_in;
            dump_value(item, indent, depth + 1, out);
        }
        if (indent > 0 && !v.items().empty()) *out += "\n" + pad;
        *out += "]";
        break;
    }
    case JValue::Type::Object: {
        *out += "{";
        bool first = true;
        for (const auto& kv : v.members())
        {
            if (!first) *out += ",";
            first = false;
            if (indent > 0) *out += "\n" + pad_in;
            dump_string(kv.first, out);
            *out += ":";
            if (indent > 0) *out += " ";
            dump_value(kv.second, indent, depth + 1, out);
        }
        if (indent > 0 && !v.members().empty()) *out += "\n" + pad;
        *out += "}";
        break;
    }
    }
}

/// @brief Serialise this value to JSON text.
/// @param indent Spaces per level; 0 produces compact output.
/// @return JSON text.
std::string JValue::dump(int indent) const
{
    std::string out;
    dump_value(*this, indent, 0, &out);
    return out;
}

// ---------------- parser ----------------

namespace
{

struct Parser
{
    const std::string& s;
    size_t i = 0;
    std::string err;

    /// @brief Bind the parser to @p text.
    /// @param text JSON source text.
    explicit Parser(const std::string& text) : s(text)
    {
    }

    /// @brief Fail the parse with @p msg plus the byte offset.
    /// @param msg Description of the syntax error.
    /// @return Fail the parse with @p msg plus the byte offset.
    [[noreturn]] void fail(const std::string& msg)
    {
        throw std::runtime_error(msg + " at byte " + std::to_string(i));
    }
    /// @brief Skip whitespace to the next token.
    void skip_ws()
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    /// @brief Current character without consuming it.
    /// @return The current character, or 0 at end of input.
    char peek()
    {
        skip_ws();
        if (i >= s.size()) fail("unexpected end of input");
        return s[i];
    }
    /// @brief Consume @p c when it is the current character.
    /// @return True when consumed.
    /// @param c Character to consume.
    /// @return True when consumed.
    bool consume(char c)
    {
        skip_ws();
        if (i < s.size() && s[i] == c)
        {
            ++i;
            return true;
        }
        return false;
    }
    /// @brief Consume @p c or fail with a diagnosable error.
    /// @param c Character to consume.
    void expect(char c)
    {
        if (!consume(c)) fail(std::string("expected '") + c + "'");
    }

    /// @brief Parse any JSON value (dispatches on the current character).
    JValue parse_value()
    {
        char c = peek();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') return JValue(parse_string());
        if (c == 't')
        {
            lit("true");
            return JValue(true);
        }
        if (c == 'f')
        {
            lit("false");
            return JValue(false);
        }
        if (c == 'n')
        {
            lit("null");
            return JValue();
        }
        return parse_number();
    }
    /// @brief Consume the expected literal text or fail.
    /// @param l Expected literal text.
    void lit(const char* l)
    {
        for (const char* p = l; *p; ++p)
        {
            if (i >= s.size() || s[i] != *p) fail(std::string("expected '") + l + "'");
            ++i;
        }
    }
    /// @brief Parse a JSON string literal with escapes.
    /// @return The decoded string.
    std::string parse_string()
    {
        expect('"');
        std::string out;
        while (true)
        {
            if (i >= s.size()) fail("unterminated string");
            char c = s[i++];
            if (c == '"') break;
            if (c == '\\')
            {
                if (i >= s.size()) fail("bad escape");
                char e = s[i++];
                switch (e)
                {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (i + 4 > s.size()) fail("bad \\u escape");
                    unsigned v = 0;
                    for (int k = 0; k < 4; ++k)
                    {
                        char h = s[i++];
                        v <<= 4;
                        if (h >= '0' && h <= '9')
                            v |= unsigned(h - '0');
                        else if (h >= 'a' && h <= 'f')
                            v |= unsigned(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F')
                            v |= unsigned(h - 'A' + 10);
                        else
                            fail("bad hex digit");
                    }
                    // UTF-8 encode (BMP only; surrogates rejected)
                    if (v >= 0xD800 && v <= 0xDFFF) fail("surrogates unsupported");
                    if (v < 0x80)
                        out += char(v);
                    else if (v < 0x800)
                    {
                        out += char(0xC0 | (v >> 6));
                        out += char(0x80 | (v & 0x3F));
                    }
                    else
                    {
                        out += char(0xE0 | (v >> 12));
                        out += char(0x80 | ((v >> 6) & 0x3F));
                        out += char(0x80 | (v & 0x3F));
                    }
                    break;
                }
                default: fail("bad escape");
                }
            }
            else { out += c; }
        }
        return out;
    }
    /// @brief Parse a JSON number.
    /// @return The number as a JValue.
    JValue parse_number()
    {
        size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '-' || s[i] == '+')) ++i;
        if (start == i) fail("expected number");
        try
        {
            return JValue(std::stod(s.substr(start, i - start)));
        }
        catch (...)
        {
            fail("bad number");
        }
    }
    /// @brief Parse a JSON array.
    /// @return The array as a JValue.
    JValue parse_array()
    {
        expect('[');
        JValue arr = JValue::array();
        if (consume(']')) return arr;
        while (true)
        {
            arr.push(parse_value());
            if (consume(']')) break;
            expect(',');
        }
        return arr;
    }
    /// @brief Parse a JSON object.
    /// @return The object as a JValue.
    JValue parse_object()
    {
        expect('{');
        JValue obj = JValue::object();
        if (consume('}')) return obj;
        while (true)
        {
            std::string key = parse_string();
            expect(':');
            obj.set(key, parse_value());
            if (consume('}')) break;
            expect(',');
        }
        return obj;
    }
};

} // namespace

/// @brief Strict JSON parser.
/// @param text JSON source text.
/// @return Parsed value, or an Error carrying the byte offset of the failure.
Result<JValue> json_parse(const std::string& text)
{
    try
    {
        Parser p(text);
        JValue v = p.parse_value();
        p.skip_ws();
        if (p.i != text.size()) p.fail("trailing content");
        return Result<JValue>::good(std::move(v));
    }
    catch (const std::exception& e)
    {
        return Result<JValue>::fail(Errc::InvalidArgument, std::string("JSON parse error: ") + e.what(), "json_parse");
    }
}

// ---------------- YAML → JSON ----------------

/// @brief Convert one YAML scalar node to the equivalent JValue.
/// @param n YAML node to convert.
/// @return The equivalent JValue.
static JValue yaml_scalar_to_json(const YAML::Node& n)
{
    if (n.IsNull()) return JValue();
    switch (n.Type())
    {
    case YAML::NodeType::Scalar: {
        const std::string& raw = n.Scalar();
        // Try quoted/plain conversions the way YAML would tag them.
        if (n.Tag() == "!!str" || n.Tag() == "!") return JValue(raw);
        if (raw == "true") return JValue(true);
        if (raw == "false") return JValue(false);
        if (raw == "null" || raw == "~" || raw.empty()) return JValue();
        try
        {
            size_t pos = 0;
            if (!raw.empty() && raw.find_first_of(".eE") == std::string::npos)
            {
                long long ll = std::stoll(raw, &pos);
                if (pos == raw.size()) return JValue(static_cast<double>(ll));
            }
            double d = std::stod(raw, &pos);
            if (pos == raw.size()) return JValue(d);
        }
        catch (...)
        {
        }
        return JValue(raw);
    }
    case YAML::NodeType::Sequence: {
        JValue arr = JValue::array();
        for (const auto& item : n) arr.push(yaml_scalar_to_json(item));
        return arr;
    }
    case YAML::NodeType::Map: {
        JValue obj = JValue::object();
        for (const auto& kv : n) obj.set(kv.first.as<std::string>(), yaml_scalar_to_json(kv.second));
        return obj;
    }
    default: return JValue();
    }
}

/// @brief YAML → JSON projection (scalars, maps, sequences).
///
/// Used to validate YAML configuration against JSON Schemas.
///
/// @param yaml_text YAML source text.
/// @return Equivalent JSON value, or an Error describing the YAML failure.
Result<JValue> json_from_yaml_text(const std::string& yaml_text)
{
    try
    {
        YAML::Node root = YAML::Load(yaml_text);
        return Result<JValue>::good(yaml_scalar_to_json(root));
    }
    catch (const std::exception& e)
    {
        return Result<JValue>::fail(Errc::InvalidArgument, std::string("YAML parse error: ") + e.what(), "json_from_yaml_text");
    }
}

// ---------------- JSON Schema subset validator ----------------

namespace
{

/// @brief Decide whether a value matches a schema "type" keyword value.
/// @param t Type keyword value.
/// @param v Value to test.
/// @return True when the value matches.
bool type_matches(const std::string& t, const JValue& v)
{
    if (t == "object") return v.is_object();
    if (t == "array") return v.is_array();
    if (t == "string") return v.is_string();
    if (t == "number") return v.is_number();
    if (t == "integer") return v.is_number() && v.as_number() == std::floor(v.as_number());
    if (t == "boolean") return v.is_bool();
    if (t == "null") return v.is_null();
    return true;
}

/// @brief Recursive validator worker; declared before use, defined below.
void check(const JValue& schema, const JValue& v, const std::string& path, std::vector<JsonValidator::Issue>* issues);

/// @brief Append one validation issue for @p path to @p issues.
/// @param issues Issue list to append to.
/// @param path Location of the violation.
/// @param msg Description.
void add_issue(std::vector<JsonValidator::Issue>* issues, const std::string& path, const std::string& msg)
{
    issues->push_back(JsonValidator::Issue{path, msg});
}

/// @brief Check scalar keywords (enum, minLength/maxLength, minimum/maximum, pattern).
/// @param schema Schema keywords.
/// @param v Value to check.
/// @param path Current location.
/// @param issues Issue list.
void check_scalar_keywords(const JValue& schema, const JValue& v, const std::string& path, std::vector<JsonValidator::Issue>* issues)
{
    if (const JValue* e = schema.find("enum"))
    {
        bool found = false;
        for (const auto& item : e->items())
            if (item.dump() == v.dump()) found = true;
        if (!found) add_issue(issues, path, "value not in enum: " + v.dump());
    }
    if (v.is_string())
    {
        const std::string& s = v.as_string();
        if (const JValue* m = schema.find("minLength"))
        {
            if (s.size() < static_cast<size_t>(m->as_number())) add_issue(issues, path, "string too short");
        }
        if (const JValue* m = schema.find("maxLength"))
        {
            if (s.size() > static_cast<size_t>(m->as_number())) add_issue(issues, path, "string too long");
        }
        if (const JValue* p = schema.find("pattern"))
        {
            try
            {
                std::regex re(p->as_string());
                if (!std::regex_search(s, re)) add_issue(issues, path, "string does not match pattern");
            }
            catch (const std::regex_error&)
            {
                add_issue(issues, path, "invalid pattern in schema");
            }
        }
    }
    if (v.is_number())
    {
        double d = v.as_number();
        if (const JValue* m = schema.find("minimum"))
        {
            if (d < m->as_number()) add_issue(issues, path, "number below minimum");
        }
        if (const JValue* m = schema.find("maximum"))
        {
            if (d > m->as_number()) add_issue(issues, path, "number above maximum");
        }
    }
}

/// @brief Recursively validate @p v against @p schema, recording pointer-style issues.
/// @param schema Schema document.
/// @param v Value to validate.
/// @param path Current pointer path.
/// @param issues Issue list.
void check(const JValue& schema, const JValue& v, const std::string& path, std::vector<JsonValidator::Issue>* issues)
{
    // type: string or list
    if (const JValue* t = schema.find("type"))
    {
        if (t->is_string())
        {
            if (!type_matches(t->as_string(), v)) add_issue(issues, path, "expected type " + t->as_string() + ", got instance of another type");
        }
        else if (t->is_array())
        {
            bool ok = false;
            for (const auto& tt : t->items())
                if (type_matches(tt.as_string(), v)) ok = true;
            if (!ok) add_issue(issues, path, "type not in allowed list");
        }
    }
    if (v.is_null() && !schema.has("enum"))
    {
        // null only valid if "type" includes null (already checked) — nothing else applies.
    }
    check_scalar_keywords(schema, v, path, issues);

    if (v.is_object())
    {
        if (const JValue* req = schema.find("required"))
        {
            for (const auto& r : req->items())
            {
                if (!v.has(r.as_string())) add_issue(issues, path.empty() ? "/" + r.as_string() : path + "/" + r.as_string(), "required property missing: " + r.as_string());
            }
        }
        const JValue* props = schema.find("properties");
        const JValue* addl = schema.find("additionalProperties");
        // additionalProperties is either a boolean (allow / forbid) or a schema
        // every unlisted property must satisfy — the latter is how open maps such
        // as monitord's per-resource-class intervals are constrained.
        const bool addl_is_schema = addl && addl->is_object();
        bool allow_addl = true;
        if (addl && addl->is_bool()) allow_addl = addl->as_bool();
        for (const auto& kv : v.members())
        {
            const JValue* ps = props ? props->find(kv.first) : nullptr;
            if (ps) { check(*ps, kv.second, path + "/" + kv.first, issues); }
            else if (addl_is_schema) { check(*addl, kv.second, path + "/" + kv.first, issues); }
            else if (!allow_addl) { add_issue(issues, path + "/" + kv.first, "additional property not allowed: " + kv.first); }
        }
    }
    if (v.is_array())
    {
        if (const JValue* mi = schema.find("minItems"))
        {
            if (v.items().size() < static_cast<size_t>(mi->as_number())) add_issue(issues, path, "too few items");
        }
        if (const JValue* ma = schema.find("maxItems"))
        {
            if (v.items().size() > static_cast<size_t>(ma->as_number())) add_issue(issues, path, "too many items");
        }
        if (const JValue* items = schema.find("items"))
        {
            size_t idx = 0;
            for (const auto& item : v.items())
            {
                check(*items, item, path + "/" + std::to_string(idx), issues);
                ++idx;
            }
        }
    }
}

} // namespace

/// @brief Validate @p instance against @p schema.
///
/// Supported keywords: type, properties, required, additionalProperties
/// (boolean or schema), enum, items, minItems, maxItems, minLength,
/// maxLength, minimum, maximum, pattern, description (ignored).
///
/// @param schema Schema document.
/// @param instance Document to validate.
/// @param issues Receives one Issue per violation; may be nullptr to skip.
/// @return True when the instance conforms; false with @p issues filled otherwise.
bool JsonValidator::validate(const JValue& schema, const JValue& instance, std::vector<Issue>* issues)
{
    std::vector<Issue> local;
    if (!issues) issues = &local;
    issues->clear();
    check(schema, instance, "", issues);
    return issues->empty();
}

} // namespace instr