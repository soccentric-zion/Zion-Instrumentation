// ============================================================================
//  Arches Instrumentation — minimal ApplicationApi client
//
//  Copyright (C) 2026 Soccentric. All rights reserved.
//  This file is part of a proprietary product of Soccentric.
// ============================================================================
/**
 * @file docs/example-client/main.cpp
 * @brief The smallest complete application that raises an alarm and reports a metric.
 *
 * This is the pattern Arches-HAL, Arches-Client and Arches-Server copy to
 * satisfy R5 (an alarm is declared by an application and by nothing else).
 * Build it with the installed package:
 *
 *     find_package(instrumentation REQUIRED)
 *     instrumentation_generate_client(my_app PROTOS istate.proto)
 *
 * Three things this example is careful about, because all three are easy to
 * get wrong and two of them are documented Don'ts:
 *
 *   1. Instrumentation is NEVER a hard dependency. Every call here degrades to
 *      a log line if istate is down or absent. An application must run
 *      normally on an image built without instrumentation -- that is the
 *      failure-behaviour contract Instrumentation itself commits to, and it is
 *      why arches-client.service says After= but not Requires=.
 *
 *   2. `owner` is required on every alarm call (R5). It is how the alarm
 *      timeline attributes a condition to the application that declared it.
 *      Use one stable name per application and never vary it.
 *
 *   3. A CRITICAL alarm can come back with needs_snapshot set, which asks the
 *      application to capture bounded diagnostics. Ignoring it is not an
 *      error, but handling it is most of the value of raising the alarm.
 */

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "istate.grpc.pb.h"

namespace {

/// The application surface. Mode 0660, owned instr-istate:instr-apps -- the
/// process must be in the instr-apps group or the connect fails (see E-01).
constexpr const char* kSocket = "/run/instrumentation/application.sock";

/// One stable owner name per application, used on every alarm call.
constexpr const char* kOwner = "example-client";

/// Alarm id from catalog/alarms.yaml. Never invent one at the call site: the
/// 32-bit layout is locked and HQ decodes against the catalog.
constexpr uint32_t kExampleAlarmId = 0x10410007;  // hardware/Hardware/equipment/7

/// @brief Connect to istate, or return nullptr if it is not there.
///
/// Deliberately non-fatal: absent instrumentation is a normal state.
std::unique_ptr<instr::istate::ApplicationApi::Stub> connect()
{
    auto channel = grpc::CreateChannel(std::string("unix:") + kSocket,
                                       grpc::InsecureChannelCredentials());
    // Bounded wait: never block application startup on instrumentation.
    const auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(2);
    if (!channel->WaitForConnected(deadline))
    {
        std::cerr << "instrumentation unavailable at " << kSocket
                  << " -- continuing without it\n";
        return nullptr;
    }
    return instr::istate::ApplicationApi::NewStub(channel);
}

} // namespace

int main()
{
    auto stub = connect();
    if (!stub) return 0; // Not an error. The application does its real work regardless.

    // ---- register a metric once, at startup -------------------------------
    // Unit and type are carried through to HQ; a metric registered without
    // them loses that metadata at the boundary.
    {
        instr::istate::RegisterMetricRequest req;
        req.set_name("example_runs_total");
        req.set_unit("runs");
        req.set_type("counter");
        req.set_app(kOwner);

        grpc::ClientContext ctx;
        instr::common::Ack ack;
        const grpc::Status s = stub->RegisterMetric(&ctx, req, &ack);
        if (!s.ok()) std::cerr << "RegisterMetric failed: " << s.error_message() << "\n";
    }

    // ---- raise an alarm on a real condition -------------------------------
    {
        instr::istate::RaiseAlarmRequest req;
        req.set_alarm_id(kExampleAlarmId);
        req.set_owner(kOwner); // required (R5)
        req.set_additional_text("example condition detected");

        grpc::ClientContext ctx;
        instr::istate::AlarmAck ack;
        const grpc::Status s = stub->RaiseAlarm(&ctx, req, &ack);
        if (!s.ok())
        {
            std::cerr << "RaiseAlarm failed: " << s.error_message() << "\n";
        }
        else
        {
            std::cout << "alarm " << (ack.accepted() ? "accepted" : "rejected")
                      << " action=" << ack.action() << "\n";
            if (ack.needs_snapshot())
            {
                // A CRITICAL alarm asked for bounded diagnostics. Capture what
                // is cheap and relevant -- this is where the value is.
                std::cout << "needs_snapshot set: capture diagnostics here\n";
            }
        }
    }

    // ---- clear it when the condition goes away ----------------------------
    {
        instr::istate::ClearAlarmRequest req;
        req.set_alarm_id(kExampleAlarmId);
        req.set_owner(kOwner); // same owner that raised it

        grpc::ClientContext ctx;
        instr::istate::AlarmAck ack;
        const grpc::Status s = stub->ClearAlarm(&ctx, req, &ack);
        if (!s.ok()) std::cerr << "ClearAlarm failed: " << s.error_message() << "\n";
    }

    return 0;
}
