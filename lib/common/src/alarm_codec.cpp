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
 * @file lib/common/src/alarm_codec.cpp
 * @brief Implementation of the 32-bit alarm/fault identity codec: domain/module/X.733-type/index decode, encode, and canonical text form (REQ §8, locked).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/common/alarm_codec.h"

#include <cstdio>
#include <unordered_map>

namespace instr
{

/// @brief Canonical lowercase name of an alarm domain ("hardware", ...).
/// @param d Domain to name.
/// @return Static string for the domain.
const char* alarm_domain_name(AlarmDomain d)
{
    switch (d)
    {
    case AlarmDomain::Hardware: return "hardware";
    case AlarmDomain::Software: return "software";
    case AlarmDomain::Network: return "network";
    case AlarmDomain::Application: return "application";
    case AlarmDomain::Fault: return "fault";
    }
    return "unknown";
}

/// @brief Canonical lowercase name of an X.733 type.
/// @param t Type to name.
/// @return Static string for the type.
const char* x733_type_name(X733Type t)
{
    switch (t)
    {
    case X733Type::Equipment: return "equipment";
    case X733Type::Communications: return "communications";
    case X733Type::ProcessingError: return "processing-error";
    case X733Type::QualityOfService: return "quality-of-service";
    case X733Type::Environmental: return "environmental";
    }
    return "unknown";
}

/// @brief Parse an alarm domain name ("hardware" etc.).
/// @param s Name as produced by alarm_domain_name().
/// @param n Name to parse.
/// @return The parsed domain; Hardware when unknown.
AlarmDomain alarm_domain_from_name(const std::string& n)
{
    if (n == "hardware") return AlarmDomain::Hardware;
    if (n == "software") return AlarmDomain::Software;
    if (n == "network") return AlarmDomain::Network;
    if (n == "application") return AlarmDomain::Application;
    if (n == "fault") return AlarmDomain::Fault;
    return AlarmDomain::Software; // caller validates via catalog schema
}

/// @brief Parse an X.733 type name.
/// @param s Name as produced by x733_type_name().
/// @param n Name to parse.
/// @return The parsed type; Equipment when unknown.
X733Type x733_type_from_name(const std::string& n)
{
    if (n == "equipment") return X733Type::Equipment;
    if (n == "communications") return X733Type::Communications;
    if (n == "processing-error") return X733Type::ProcessingError;
    if (n == "quality-of-service") return X733Type::QualityOfService;
    if (n == "environmental") return X733Type::Environmental;
    return X733Type::ProcessingError;
}

/// @brief Module numbering: 0x01..0x0A are the ten daemons, 0x10.. are application modules.
///
/// HQ renders via the shared catalog without hardcoding.
///
/// @return Shared module-number → display-name map.
const std::map<uint8_t, std::string>& module_names()
{
    static const std::map<uint8_t, std::string> m = {
        {0x01, "istate"}, {0x02, "networkd"}, {0x03, "monitord"}, {0x04, "hardware"}, {0x05, "faults"}, {0x06, "alarms"}, {0x07, "logmanager"}, {0x08, "telemetry"}, {0x09, "ota"}, {0x0A, "backup"}, {0x10, "application-1"}, {0x11, "application-2"},
    };
    return m;
}

/// @brief Parse a module name to its number.
/// @param s Module name as listed by module_names().
/// @param name Module name as listed by module_names().
/// @return Module number; 0 when the name is unknown.
uint8_t module_from_name(const std::string& name)
{
    for (const auto& kv : module_names())
        if (kv.second == name) return kv.first;
    return 0;
}

/// @brief Unpack a 32-bit alarm ID.
/// @param id Packed identity.
/// @return The unpacked AlarmId, or an InvalidArgument error for reserved domain values.
Result<AlarmId> decode_alarm_id(uint32_t id)
{
    AlarmId a;
    if (id == 0) return Result<AlarmId>::fail(Errc::InvalidArgument, "alarm id 0 is reserved", "decode_alarm_id");
    a.domain = static_cast<AlarmDomain>((id >> 28) & 0xF);
    a.module = static_cast<uint8_t>((id >> 20) & 0xFF);
    a.type = static_cast<X733Type>((id >> 16) & 0xF);
    a.index = static_cast<uint16_t>(id & 0xFFFF);
    return Result<AlarmId>::good(a);
}

/// @brief Canonical text form of a packed ID: 0x<domain>_<module>_<type>_<index:04x>.
/// @param id Packed identity.
/// @return Text such as "0x1_4_1_0007".
std::string alarm_id_string(uint32_t id)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%x_%02x_%x_%04x", (id >> 28) & 0xF, (id >> 20) & 0xFF, (id >> 16) & 0xF, id & 0xFFFF);
    return buf;
}

} // namespace instr