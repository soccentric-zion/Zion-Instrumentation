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
 * @file lib/common/include/instr/common/alarm_codec.h
 * @brief alarm/fault 32-bit identity (REQ §8, locked)
 *
 * [31:28] domain | [27:20] module | [19:16] X.733 type | [15:0] index.
 * An ID is decodable without the catalog:
 * 0x1_04_1_0007 = hardware / Hardware daemon / equipment / alarm 7.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <cstdint>
#include <map>
#include <string>

#include "instr/common/error.h"

namespace instr
{

/// @brief Top-level alarm/fault domain (identity bits [31:28]).
enum class AlarmDomain : uint8_t
{
    Hardware = 1, ///< Physical equipment faults.
    Software = 2, ///< Daemon/software faults.
    Network = 3, ///< Network faults.
    Application = 4, ///< Colocated-application alarms (R5).
    Fault = 5, ///< Faults daemon internal.
};

/// @brief X.733 event type (identity bits [19:16]).
enum class X733Type : uint8_t
{
    Equipment = 1, ///< Physical/equipment-related.
    Communications = 2, ///< Transmission/link problems.
    ProcessingError = 3, ///< Software/processing problem.
    QualityOfService = 4, ///< Performance/QoS degradation.
    Environmental = 5, ///< Environmental conditions.
};

/// @brief Canonical lowercase name of an alarm domain ("hardware", ...).
/// @param d Domain to name.
/// @return Static string for the domain.
const char* alarm_domain_name(AlarmDomain d);

/// @brief Canonical lowercase name of an X.733 type.
/// @param t Type to name.
/// @return Static string for the type.
const char* x733_type_name(X733Type t);

/// @brief Parse an alarm domain name ("hardware" etc.).
/// @param s Name as produced by alarm_domain_name().
/// @return The parsed domain; Hardware when unknown.
AlarmDomain alarm_domain_from_name(const std::string& s);

/// @brief Parse an X.733 type name.
/// @param s Name as produced by x733_type_name().
/// @return The parsed type; Equipment when unknown.
X733Type x733_type_from_name(const std::string& s);

/// @brief Module numbering: 0x01..0x0A are the ten daemons, 0x10.. are application modules.
///
/// HQ renders via the shared catalog without hardcoding.
///
/// @return Shared module-number → display-name map.
const std::map<uint8_t, std::string>& module_names();

/// @brief Parse a module name to its number.
/// @param s Module name as listed by module_names().
/// @return Module number; 0 when the name is unknown.
uint8_t module_from_name(const std::string&);

/// @brief A structured alarm identity (the four 32-bit ID fields unpacked).
struct AlarmId
{
    AlarmDomain domain; ///< Identity bits [31:28].
    uint8_t module; ///< Identity bits [27:20] (daemon or application module).
    X733Type type; ///< Identity bits [19:16] (X.733 event type).
    uint16_t index; ///< Identity bits [15:0] (alarm number within module).
    /// @brief Structural equality for map/set keys and tests.
    bool operator==(const AlarmId& o) const
    {
        return domain == o.domain && module == o.module && type == o.type && index == o.index;
    }
};

/// @brief Pack the four identity fields into the 32-bit alarm ID.
///
/// Layout: [31:28] domain | [27:20] module | [19:16] X.733 type | [15:0] index.
/// An ID is decodable without the catalog:
/// 0x1_04_1_0007 = hardware / Hardware daemon / equipment / alarm 7.
///
/// @param d Domain nibble.
/// @param module Module byte.
/// @param t X.733 type nibble.
/// @param index Alarm index within the module.
/// @return The packed 32-bit identity.
constexpr uint32_t encode_alarm_id(AlarmDomain d, uint8_t module, X733Type t, uint16_t index)
{
    return (static_cast<uint32_t>(d) << 28) | (static_cast<uint32_t>(module) << 20) | (static_cast<uint32_t>(t) << 16) | index;
}

/// @brief Unpack a 32-bit alarm ID.
/// @param id Packed identity.
/// @return The unpacked AlarmId, or an InvalidArgument error for reserved domain values.
Result<AlarmId> decode_alarm_id(uint32_t id);

/// @brief Canonical text form of a packed ID: 0x<domain>_<module>_<type>_<index:04x>.
/// @param id Packed identity.
/// @return Text such as "0x1_4_1_0007".
std::string alarm_id_string(uint32_t id);

} // namespace instr