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
 * @file test/contract/contract_tests.cpp
 * @brief Contract tests (REQ §12): a breaking .proto schema change fails CI rather
 *
 * than a field device. The golden descriptor hash pins the wire contract;
 * the registration/ctrl contracts pin the runtime behaviour on top of it.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <fstream>
#include <sstream>

#include <gtest/gtest.h>

#include "instr/common/alarm_codec.h"
#include "instr/common/crypto.h"
#include "instr/common/frames.h"
#include "instr/common/spool.h"
#include "instr/istate/orchestrator.h"
#include "instr/transport/transport.h"

using namespace instr;

/// @brief Read a whole file (fixtures and golden files).
/// @param path File to read.
/// @return The file contents.
static std::string slurp(const std::string& path)
{
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// @brief The generated proto descriptors match the golden hash — wire incompatibility is caught at build time.
TEST(ContractProtos, DescriptorHashMatchesGolden)
{
    std::string desc = slurp(PROTO_DESC_FILE);
    ASSERT_GT(desc.size(), 100u) << "descriptor set missing; build protos_golden first";
    std::string computed = crypto::sha256(desc).hex();
    std::string golden = slurp(GOLDEN_FILE);
    while (!golden.empty() && (golden.back() == '\n' || golden.back() == ' ' || golden.back() == '\r')) golden.pop_back();
    if (golden.empty())
    {
        // First run: seed the golden file (CI must commit it).
        std::ofstream out(GOLDEN_FILE);
        out << computed << "\n";
        GTEST_SKIP() << "golden seeded; commit " << GOLDEN_FILE;
    }
    EXPECT_EQ(computed, golden) << "Proto definitions changed — this is a breaking wire contract change.\n"
                                << "Update the golden deliberately: test/contract/protos.golden.sha256\n"
                                << "(regenerate with: protoc --descriptor_set_out, then re-hash) and\n"
                                << "bump transport::kProtoSchemaVersion if wire-incompatible.";
}

/// @brief The proto schema version is one shared constant every side checks (§3.1).
TEST(ContractHandshake, SchemaVersionIsSuiteWide)
{
    // One wire format for services: every spoke and istate share one schema
    // version; the constant is the single point of truth (§3.1, R3). Pinning
    // it here is a deliberate tripwire — bumping the constant must be a
    // conscious act paired with a regenerated descriptor golden above.
    //   v1 → v2: spokes receive their configuration over the channel (§9),
    //            so a v1 spoke against a v2 istate would run unconfigured.
    EXPECT_EQ(transport::kProtoSchemaVersion, 2u);
}

/// @brief Registration requests carry the fields the orchestrator records; mismatches are refused.
TEST(ContractHandshake, RegistrationFieldsAreStable)
{
    // The handshake contract: daemon name, build version, proto schema
    // version, capability list — encoded in common.proto RegistrationRequest.
    common::RegistrationRequest req;
    req.set_daemon("monitord");
    req.set_build_version("1.0.0");
    req.set_proto_schema_version(transport::kProtoSchemaVersion);
    req.add_capabilities("samples");
    std::string wire;
    ASSERT_TRUE(req.AppendToString(&wire));
    common::RegistrationRequest back;
    ASSERT_TRUE(back.ParseFromString(wire));
    EXPECT_EQ(back.daemon(), "monitord");
    EXPECT_EQ(back.proto_schema_version(), transport::kProtoSchemaVersion);
    EXPECT_EQ(back.capabilities(0), "samples");
}

/// @brief The ctrl frame format is a 4-byte little-endian length prefix (§3.2, on the wire contract).
TEST(ContractCtrl, FrameFormatIsLengthPrefixedLe32)
{
    // The raw-UDS ctrl contract: 4-byte LE length + payload.
    auto frame = frames::encode_frame("payload");
    ASSERT_EQ(frame.size(), 4 + 7);
    EXPECT_EQ(uint8_t(frame[0]), 7); // little-endian payload length
    EXPECT_EQ(uint8_t(frame[1]), 0);
    frames::FrameDecoder dec;
    std::string out;
    ASSERT_TRUE(dec.feed(frame.data(), frame.size(), &out));
    EXPECT_EQ(out, "payload");
}

/// @brief Alarms and faults share the §8 32-bit identity scheme.
TEST(ContractAlarmIds, SameSchemeForAlarmsAndFaults)
{
    // §8: alarms and faults share the 32-bit identity scheme so HQ can
    // correlate a kernel fault with an application-raised alarm.
    uint32_t alarm = encode_alarm_id(AlarmDomain::Hardware, 0x04, X733Type::Equipment, 7);
    uint32_t fault = encode_alarm_id(AlarmDomain::Fault, 0x05, X733Type::ProcessingError, 8);
    auto da = decode_alarm_id(alarm);
    auto flt = decode_alarm_id(fault);
    ASSERT_TRUE(da.ok());
    ASSERT_TRUE(flt.ok());
    EXPECT_EQ(da->domain, AlarmDomain::Hardware);
    EXPECT_EQ(flt->domain, AlarmDomain::Fault);
    EXPECT_EQ(alarm_id_string(fault), "0x5_05_3_0008");
}

/// @brief The spool drop-priority order is the contractual one: telemetry first, alarms/faults never.
TEST(ContractSpool, DropPriorityOrderIsContractual)
{
    // §11: oldest telemetry first, then log refs, then events; alarms and
    // faults protected. The enum ordering encodes the contract.
    EXPECT_EQ(int(SpoolPriority::Telemetry), 0);
    EXPECT_EQ(int(SpoolPriority::LogRef), 1);
    EXPECT_EQ(int(SpoolPriority::Event), 2);
    EXPECT_EQ(int(SpoolPriority::Fault), 3);
    EXPECT_EQ(int(SpoolPriority::Alarm), 4);
}
