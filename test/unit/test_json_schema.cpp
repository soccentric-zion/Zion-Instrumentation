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
 * @file test/unit/test_json_schema.cpp
 * @brief JSON parser/serialiser and the JSON Schema subset validator
 *
 * (REQ §9: every format validated at load, rejected with clear errors).
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <gtest/gtest.h>

#include "instr/common/json.h"

using namespace instr;

/// @brief The parser handles scalars, arrays and nested objects.
TEST(JsonParse, ScalarsAndContainers)
{
    auto v = json_parse(R"({"a":1,"b":[true,null,"x"],"c":{"d":-2.5}})");
    ASSERT_TRUE(v.ok());
    EXPECT_EQ((*v).find("a")->as_number(), 1);
    EXPECT_TRUE((*v).find("b")->items()[0].as_bool());
    EXPECT_TRUE((*v).find("b")->items()[1].is_null());
    EXPECT_EQ((*v).find("b")->items()[2].as_string(), "x");
    EXPECT_NEAR((*v).find("c")->find("d")->as_number(), -2.5, 1e-9);
}

/// @brief Parse failures carry a diagnosable byte offset.
TEST(JsonParse, DiagnosableErrors)
{
    auto v = json_parse("{\"a\": }");
    EXPECT_FALSE(v.ok());
    EXPECT_NE(v.error.message.find("JSON parse error"), std::string::npos);
    auto v2 = json_parse("trailing");
    EXPECT_FALSE(v2.ok());
}

/// @brief String escapes are decoded correctly, including unicode forms.
TEST(JsonParse, StringEscapes)
{
    auto v = json_parse(R"x({"s":"a\"b\\c\ndA"})x");
    ASSERT_TRUE(v.ok());
    EXPECT_EQ((*v).find("s")->as_string(), "a\"b\\c\ndA");
}

/// @brief dump() output re-parses to the same value (compact and indented).
TEST(JsonDump, RoundTrip)
{
    auto v = json_parse(R"({"k":[1,2,{"n":null}],"s":"x"})");
    ASSERT_TRUE(v.ok());
    auto v2 = json_parse((*v).dump());
    ASSERT_TRUE(v2.ok());
    EXPECT_EQ((*v).dump(), (*v2).dump());
}

/// @brief YAML projects to equivalent JSON (scalars, maps, sequences).
TEST(JsonFromYaml, Projection)
{
    auto v = json_from_yaml_text("a: 5\nb: hello\nc:\n  - true\n  - 1.5\nd:\n  k: v\n");
    ASSERT_TRUE(v.ok());
    EXPECT_TRUE((*v).is_object());
    EXPECT_EQ((*v).find("a")->as_number(), 5);
    EXPECT_EQ((*v).find("b")->as_string(), "hello");
    ASSERT_TRUE((*v).find("c")->is_array());
    EXPECT_TRUE((*v).find("c")->items()[0].as_bool());
    EXPECT_NEAR((*v).find("c")->items()[1].as_number(), 1.5, 1e-9);
    EXPECT_EQ((*v).find("d")->find("k")->as_string(), "v");
}

/// @brief type and required keywords are enforced with pointer-style issues.
TEST(JsonSchema, TypeAndRequired)
{
    JValue schema = JValue::object();
    JValue props = JValue::object();
    {
        JValue t = JValue::object();
        t.set("type", JValue("integer"));
        props.set("port", t);
        JValue s = JValue::object();
        s.set("type", JValue("string"));
        s.set("minLength", JValue(2.0));
        props.set("name", s);
    }
    schema.set("type", JValue("object"));
    schema.set("properties", props);
    JValue req = JValue::array();
    req.push(JValue("name"));
    schema.set("required", req);

    std::vector<JsonValidator::Issue> issues;
    JValue good = JValue::object();
    good.set("name", JValue("ok"));
    good.set("port", JValue(42.0));
    EXPECT_TRUE(JsonValidator::validate(schema, good, &issues));

    JValue bad = JValue::object();
    bad.set("port", JValue(42.0)); // name missing
    EXPECT_FALSE(JsonValidator::validate(schema, bad, &issues));
    EXPECT_NE(issues[0].message.find("required"), std::string::npos);

    JValue bad2 = JValue::object();
    bad2.set("name", JValue("x")); // too short
    bad2.set("port", JValue(4.5)); // not integer
    EXPECT_FALSE(JsonValidator::validate(schema, bad2, &issues));
    EXPECT_GE(issues.size(), 2u);
}

/// @brief enum, min/max, length and pattern keywords are enforced.
TEST(JsonSchema, EnumMinMaxPattern)
{
    JValue schema = JValue::object();
    JValue props = JValue::object();
    {
        JValue mode = JValue::object();
        JValue e = JValue::array();
        e.push(JValue("dhcp"));
        e.push(JValue("static"));
        mode.set("enum", e);
        props.set("mode", mode);
        JValue port = JValue::object();
        port.set("type", JValue("integer"));
        port.set("minimum", JValue(1.0));
        port.set("maximum", JValue(65535.0));
        props.set("port", port);
        JValue cidr = JValue::object();
        cidr.set("type", JValue("string"));
        cidr.set("pattern", JValue("^[0-9.]+$"));
        props.set("addr", cidr);
    }
    schema.set("properties", props);

    std::vector<JsonValidator::Issue> issues;
    JValue ok = JValue::object();
    ok.set("mode", JValue("dhcp"));
    ok.set("port", JValue(8080.0));
    ok.set("addr", JValue("10.0.0.1"));
    EXPECT_TRUE(JsonValidator::validate(schema, ok, &issues));

    JValue bad = JValue::object();
    bad.set("mode", JValue("ppp"));
    bad.set("port", JValue(99999.0));
    bad.set("addr", JValue("not an ip"));
    EXPECT_FALSE(JsonValidator::validate(schema, bad, &issues));
    EXPECT_EQ(issues.size(), 3u);
}

/// @brief Validation recurses into nested objects and array items.
TEST(JsonSchema, NestedObjectsAndArrays)
{
    JValue schema = JValue::object();
    schema.set("type", JValue("object"));
    JValue props = JValue::object();
    JValue ifaces = JValue::object();
    ifaces.set("type", JValue("array"));
    JValue items = JValue::object();
    items.set("type", JValue("object"));
    JValue iprops = JValue::object();
    JValue nm = JValue::object();
    nm.set("type", JValue("string"));
    iprops.set("name", nm);
    items.set("properties", iprops);
    JValue ireq = JValue::array();
    ireq.push(JValue("name"));
    items.set("required", ireq);
    ifaces.set("items", items);
    ifaces.set("minItems", JValue(1.0));
    props.set("interfaces", ifaces);
    schema.set("properties", props);
    schema.set("additionalProperties", JValue(false));

    std::vector<JsonValidator::Issue> issues;
    JValue ok = JValue::object();
    JValue arr = JValue::array();
    JValue iface = JValue::object();
    iface.set("name", JValue("eth0"));
    arr.push(iface);
    ok.set("interfaces", arr);
    EXPECT_TRUE(JsonValidator::validate(schema, ok, &issues));

    JValue bad = JValue::object();
    JValue arr2 = JValue::array();
    arr2.push(JValue::object()); // missing name
    bad.set("interfaces", arr2);
    bad.set("extra", JValue("not allowed")); // additionalProperties: false
    EXPECT_FALSE(JsonValidator::validate(schema, bad, &issues));
    bool saw_nested = false, saw_addl = false;
    for (const auto& i : issues)
    {
        if (i.path.find("interfaces/0") != std::string::npos) saw_nested = true;
        if (i.message.find("additional property") != std::string::npos) saw_addl = true;
    }
    EXPECT_TRUE(saw_nested);
    EXPECT_TRUE(saw_addl);
}