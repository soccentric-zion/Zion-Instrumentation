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
 * @file lib/common/include/instr/common/json.h
 * @brief JSON value, parser, serialiser and a JSON Schema
 *
 * subset validator (REQ §4 config loading, §9). Every YAML format has a JSON
 * Schema in schemas/ and is validated at load, rejected with a clear
 * file-and-line error on failure.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "instr/common/error.h"

namespace instr
{

/// @brief A JSON value: null, bool, number, string, array or object.
///
/// Objects preserve insertion order so serialised config stays diff-friendly.
class JValue
{
  public:
    /// @brief The JSON type of this value.
    enum class Type
    {
        Null, ///< Absent or explicit null.
        Bool, ///< true/false.
        Number, ///< Double-precision number (integers stored exactly up to 2^53).
        String, ///< UTF-8 text.
        Array, ///< Ordered sequence of JValues.
        Object ///< Ordered map of key → JValue.
    };

/// @brief Construct a string value (overload that beats pointer→bool conversion).
/// @param s String payload.
/// @param ' '..
/// @param N N..
/// @param u U..
/// @param l L..
/// @param l L..
/// @param ' '..
/// @param ' '..
/// @param N N..
/// @param u U..
/// @param l L..
/// @param l L..
/// @param ' '..
/// @param ' '..
/// @param N N..
/// @param u U..
/// @param l L..
/// @param l L..
/// @param ' '..
/// @param ' '..
/// @param N N..
/// @param u U..
/// @param l L..
/// @param l L..
/// @param ' '..
    JValue() : type_(Type::Null)
    {
    }
    /// @brief Construct a boolean value.
    /// @param b Boolean payload.
    explicit JValue(bool b) : type_(Type::Bool), bool_(b)
    {
    }
    /// @brief Construct a number value.
    /// @param n Numeric payload.
    explicit JValue(double n) : type_(Type::Number), num_(n)
    {
    }
    /// @brief Construct a number value from an int.
    /// @param n Numeric payload.
    explicit JValue(int n) : type_(Type::Number), num_(n)
    {
    }
/// @brief Construct a string value (overload that beats pointer→bool conversion).
/// @param s String payload.
    explicit JValue(int64_t n) : type_(Type::Number), num_(static_cast<double>(n))
    {
    }
/// @brief Construct a string value (overload that beats pointer→bool conversion).
/// @param s String payload.
    explicit JValue(std::string s) : type_(Type::String), str_(std::move(s))
    {
    }
    /// @brief Construct a string value (overload that beats pointer→bool conversion).
    /// @param s String payload.
    explicit JValue(const char* s) : type_(Type::String), str_(s)
    {
    }

    /// @brief Construct an empty array.
    static JValue array()
    {
        JValue v;
        v.type_ = Type::Array;
        return v;
    }
    /// @brief Construct an empty object.
    static JValue object()
    {
        JValue v;
        v.type_ = Type::Object;
        return v;
    }

    /// @brief The type of this value.
    Type type() const
    {
        return type_;
    }
    /// @brief True when this is a null value.
    bool is_null() const
    {
        return type_ == Type::Null;
    }
    /// @brief True when this is a boolean.
    bool is_bool() const
    {
        return type_ == Type::Bool;
    }
    /// @brief True when this is a number.
    bool is_number() const
    {
        return type_ == Type::Number;
    }
    /// @brief True when this is a string.
    bool is_string() const
    {
        return type_ == Type::String;
    }
    /// @brief True when this is an array.
    bool is_array() const
    {
        return type_ == Type::Array;
    }
    /// @brief True when this is an object.
    bool is_object() const
    {
        return type_ == Type::Object;
    }

    /// @brief Boolean view with fallback.
    /// @param dflt Value returned when this is not a bool.
    bool as_bool(bool dflt = false) const
    {
        return is_bool() ? bool_ : dflt;
    }
    /// @brief Numeric view with fallback.
    /// @param dflt Value returned when this is not a number.
    double as_number(double dflt = 0) const
    {
        return is_number() ? num_ : dflt;
    }
    /// @brief String view; the empty string when this is not a string.
    /// @return The string value; the empty string when this is not a string.
    const std::string& as_string() const
    {
        static const std::string e;
        return is_string() ? str_ : e;
    }

    /// @brief Elements of this array, in order; empty when not an array.
    /// @return Elements of this array, in order; empty when not an array.
    const std::vector<JValue>& items() const
    {
        return arr_;
    }
    /// @brief Append a value to this array.
    /// @param v Value to append.
    void push(JValue v)
    {
        arr_.push_back(std::move(v));
    }
    /// @brief Element count: array items, object members, otherwise 0.
    size_t size() const
    {
        return type_ == Type::Array ? arr_.size() : (type_ == Type::Object ? obj_.size() : 0);
    }

    /// @brief Members of this object in insertion order; empty when not an object.
    /// @return Members of this object in insertion order; empty when not an object.
    const std::vector<std::pair<std::string, JValue>>& members() const
    {
        return obj_;
    }
    /// @brief Look up an object member (const overload).
    /// @param key Member key.
    /// @return Pointer to the value, or nullptr when absent or not an object.
    const JValue* find(const std::string& key) const
    {
        if (type_ != Type::Object) return nullptr;
        for (const auto& kv : obj_)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    /// @brief Look up an object member (mutable overload).
    /// @param key Member key.
    /// @return Pointer to the value, or nullptr when absent or not an object.
    JValue* find(const std::string& key)
    {
        if (type_ != Type::Object) return nullptr;
        for (auto& kv : obj_)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    /// @brief Set an object member, replacing an existing value for @p key.
    ///
    /// Converts a non-object value into an empty object first.
    ///
    /// @param key Member key.
    /// @param v Value to store.
    void set(const std::string& key, JValue v)
    {
        if (type_ != Type::Object)
        {
            type_ = Type::Object;
            obj_.clear();
            arr_.clear();
        }
        for (auto& kv : obj_)
            if (kv.first == key)
            {
                kv.second = std::move(v);
                return;
            }
        obj_.emplace_back(key, std::move(v));
    }
    /// @brief True when this object has a member with the given key.
    /// @param key Member key.
    /// @return True when present.
    bool has(const std::string& key) const
    {
        return find(key) != nullptr;
    }

    /// @brief Convenience accessors for command-table handlers.
    /// @{
    /// @brief String member with fallback; numbers are dumped to text.
    std::string get_string(const std::string& key, const std::string& dflt = "") const
    {
        const JValue* v = find(key);
        return (v && v->is_string()) ? v->as_string() : (v && v->is_number() ? v->dump() : dflt);
    }
    /// @brief Numeric member with fallback.
    /// @param key Member key.
    /// @param dflt Value returned when absent or not numeric.
    /// @return The numeric value.
    double get_number(const std::string& key, double dflt = 0) const
    {
        const JValue* v = find(key);
        return v && v->is_number() ? v->as_number() : dflt;
    }
    /// @brief Integer member with fallback (truncates the numeric view).
    /// @param key Member key.
    /// @param dflt Value returned when absent or not numeric.
    /// @return The integer value.
    int64_t get_int(const std::string& key, int64_t dflt = 0) const
    {
        return static_cast<int64_t>(get_number(key, double(dflt)));
    }
    /// @brief Boolean member with fallback.
    /// @param key Member key.
    /// @param dflt Value returned when absent or not a bool.
    /// @return The boolean value.
    bool get_bool(const std::string& key, bool dflt = false) const
    {
        const JValue* v = find(key);
        return v && v->is_bool() ? v->as_bool() : dflt;
    }

    /// @}

    /// @brief Serialise this value to JSON text.
    /// @param indent Spaces per level; 0 produces compact output.
    /// @return JSON text.
    std::string dump(int indent = 0) const;

  private:
    Type type_; ///< Tag selecting the active union member.
    bool bool_ = false; ///< Bool payload.
    double num_ = 0; ///< Number payload.
    std::string str_; ///< String payload.
    std::vector<JValue> arr_; ///< Array payload.
    std::vector<std::pair<std::string, JValue>> obj_; ///< Object payload in insertion order.
};

/// @brief Strict JSON parser.
/// @param text JSON source text.
/// @return Parsed value, or an Error carrying the byte offset of the failure.
Result<JValue> json_parse(const std::string& text);

/// @brief YAML → JSON projection (scalars, maps, sequences).
///
/// Used to validate YAML configuration against JSON Schemas.
///
/// @param yaml_text YAML source text.
/// @return Equivalent JSON value, or an Error describing the YAML failure.
Result<JValue> json_from_yaml_text(const std::string& yaml_text);

/// @brief JSON Schema subset validator used on every config load (REQ §4, §9).
class JsonValidator
{
  public:
    /// @brief One validation problem, located by JSON-pointer-style path.
    struct Issue
    {
        std::string path; ///< Pointer-ish location, e.g. /network/ifaces/2/mode.
        std::string message; ///< Human-readable description of the violation.
    };
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
    static bool validate(const JValue& schema, const JValue& instance, std::vector<Issue>* issues);
};

} // namespace instr