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
 * @file daemons/hardware/test/test_domain.cpp
 * @brief hardware domain (REQ §12)
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <gtest/gtest.h>

#include "instr/common/timestamps.h"
#include "instr/hardware/domain.h"
#include "instr/platform/simulated.h"

using namespace instr;
using namespace instr::hardware;
using namespace instr::hardware::domain;

/// @brief A small board descriptor exercising checks, controls and CAN.
/// @return A small board descriptor exercising checks, controls and CAN.
static platform::DeviceDescriptor test_descriptor()
{
    platform::DeviceDescriptor d;
    d.board = "sim-board";
    {
        platform::DeviceSpec s;
        s.name = "status-led";
        s.kind = "led";
        s.controls = {"led_set", "led_blink"};
        s.checks = {{"blink-ack", "led_blink_ack", {}}, {"presence", "presence", {}}};
        d.devices.push_back(s);
    }
    {
        platform::DeviceSpec s;
        s.name = "gpio-1";
        s.kind = "gpio";
        s.params["line"] = "4";
        s.controls = {"gpio_set"};
        s.checks = {{"toggle", "gpio_toggle_read", {}}};
        d.devices.push_back(s);
    }
    {
        platform::DeviceSpec s;
        s.name = "can0";
        s.kind = "can";
        s.params["iface"] = "can0";
        s.controls = {"can_send", "can_recv"};
        s.checks = {{"loopback", "can_loopback", {}}};
        d.devices.push_back(s);
    }
    {
        platform::DeviceSpec s;
        s.name = "display";
        s.kind = "display";
        s.controls = {"display_control"};
        s.checks = {{"presence", "presence", {}}, {"probe", "display_probe", {}}};
        d.devices.push_back(s);
    }
    return d;
}

/// @brief A GPIO toggle-read check passes against the simulated platform.
TEST(HardwareChecks, GpioToggleReadPasses)
{
    platform::SimulatedPlatform plat;
    ManualTimeSource ts;
    DeviceManager mgr(plat, ts, test_descriptor());
    auto r = mgr.check_device("gpio-1");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].state, CheckState::Pass);
    EXPECT_NE(r[0].detail.find("toggle+read ok"), std::string::npos);
}

/// @brief A stuck GPIO line fails its check.
TEST(HardwareChecks, GpioStuckFails)
{
    platform::SimulatedPlatform plat;
    plat.fail_gpio(true);
    ManualTimeSource ts;
    DeviceManager mgr(plat, ts, test_descriptor());
    auto r = mgr.check_device("gpio-1");
    EXPECT_EQ(r[0].state, CheckState::Fail);
}

/// @brief An injected CAN failure fails the loopback check.
TEST(HardwareChecks, CanLoopbackFailsWhenInjected)
{
    platform::SimulatedPlatform plat;
    plat.fail_can(true);
    ManualTimeSource ts;
    DeviceManager mgr(plat, ts, test_descriptor());
    auto r = mgr.check_device("can0");
    EXPECT_EQ(r[0].state, CheckState::Fail);
}

/// @brief A missing display reports NotPresent, never Unknown.
TEST(HardwareChecks, DisplayNotPresent)
{
    platform::SimulatedPlatform plat;
    plat.fail_display(true);
    ManualTimeSource ts;
    DeviceManager mgr(plat, ts, test_descriptor());
    auto r = mgr.check_device("display");
    // presence probe fails → not-present; probe check fails
    bool saw_not_present = false, saw_fail = false;
    for (const auto& c : r)
    {
        if (c.state == CheckState::NotPresent) saw_not_present = true;
        if (c.state == CheckState::Fail) saw_fail = true;
    }
    EXPECT_TRUE(saw_not_present);
    EXPECT_TRUE(saw_fail);
}

/// @brief Controls are gated by the board descriptor — undeclared ops do not reach the platform.
TEST(HardwareControls, DescriptorGatesControls)
{
    platform::SimulatedPlatform plat;
    ManualTimeSource ts;
    DeviceManager mgr(plat, ts, test_descriptor());
    // gpio-1 declares gpio_set only — led_set is rejected.
    std::map<std::string, std::string> m;
    m["on"] = "true";
    EXPECT_FALSE(mgr.control("gpio-1", "led_set", m).ok());
    // Declared control works.
    m.clear();
    m["value"] = "true";
    EXPECT_TRUE(mgr.control("gpio-1", "gpio_set", m).ok());
    auto v = mgr.gpio_read("gpio-1");
    ASSERT_TRUE(v.ok());
    EXPECT_TRUE(*v);
}

/// @brief LED blink operations are acknowledged by the platform seam.
TEST(HardwareControls, LedBlinkAcknowledged)
{
    platform::SimulatedPlatform plat;
    ManualTimeSource ts;
    DeviceManager mgr(plat, ts, test_descriptor());
    std::map<std::string, std::string> m;
    m["repeats"] = "3";
    EXPECT_TRUE(mgr.control("status-led", "led_blink", m).ok());
    auto* sim = dynamic_cast<platform::SimulatedPlatform*>(&plat);
    ASSERT_NE(sim, nullptr);
    EXPECT_TRUE(sim->led_state().at("status-led:blink-ack"));
}

/// @brief An unknown device is a loud error.
TEST(HardwareControls, UnknownDeviceIsLoudError)
{
    platform::SimulatedPlatform plat;
    ManualTimeSource ts;
    DeviceManager mgr(plat, ts, test_descriptor());
    auto e = mgr.control("nonexistent", "led_set", {});
    EXPECT_EQ(e.code, Errc::NotFound);
    EXPECT_NE(e.message.find("nonexistent"), std::string::npos);
}

/// @brief Periodic checks run once per interval, not per tick.
TEST(HardwareSchedule, DueChecksRunOncePerInterval)
{
    platform::SimulatedPlatform plat;
    ManualTimeSource ts;
    DeviceManager mgr(plat, ts, test_descriptor());
    mgr.set_periodic_interval_s(10);
    auto first = mgr.due_checks(ts.mono_ns());
    EXPECT_FALSE(first.empty());
    auto soon = mgr.due_checks(ts.mono_ns() + 1'000'000ull); // 1 ms later
    EXPECT_TRUE(soon.empty());
    ts.mono += 11ull * 1'000'000'000ull;
    auto later = mgr.due_checks(ts.mono_ns());
    EXPECT_FALSE(later.empty());
}
// ---- CAN control (§7.4) -----------------------------------------------------
//
// §7.4 requires Hardware to drive its devices, "not read-only", and names CAN
// in the managed set. control() previously branched on gpio/led/usb/display
// only, so a CAN device could be loopback-checked and nothing else — the
// platform's can_send/can_recv were unreachable from any daemon.

/// @brief can_send reaches the platform with the composed frame.
TEST(HardwareCan, SendsAFrameToThePlatform)
{
    platform::SimulatedPlatform plat;
    ManualTimeSource ts;
    DeviceManager mgr(plat, ts, test_descriptor());
    Error e = mgr.control("can0", "can_send", {{"id", "0x123"}, {"data", "deadbeef"}});
    ASSERT_TRUE(e.ok()) << e.str();
    const auto& sent = plat.can_sent("can0");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].id, 0x123u);
    ASSERT_EQ(sent[0].data.size(), 4u);
    EXPECT_EQ(sent[0].data[0], 0xde);
    EXPECT_EQ(sent[0].data[3], 0xef);
}

/// @brief Decimal ids and 29-bit extended frames are accepted.
TEST(HardwareCan, DecimalIdsAndExtendedFramesAreAccepted)
{
    platform::SimulatedPlatform plat;
    ManualTimeSource ts;
    DeviceManager mgr(plat, ts, test_descriptor());
    ASSERT_TRUE(mgr.control("can0", "can_send", {{"id", "291"}, {"extended", "true"}}).ok());
    const auto& sent = plat.can_sent("can0");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].id, 291u);
    EXPECT_TRUE(sent[0].extended);
}

/// @brief A malformed CAN payload is rejected with a diagnosable error.
TEST(HardwareCan, MalformedPayloadIsRejectedDiagnosably)
{
    platform::SimulatedPlatform plat;
    ManualTimeSource ts;
    DeviceManager mgr(plat, ts, test_descriptor());
    // Odd length, non-hex, and a classic frame over 8 bytes are all errors —
    // §14: every rejected input produces a diagnosable message.
    EXPECT_FALSE(mgr.control("can0", "can_send", {{"id", "1"}, {"data", "abc"}}).ok());
    EXPECT_FALSE(mgr.control("can0", "can_send", {{"id", "1"}, {"data", "zzzz"}}).ok());
    EXPECT_FALSE(mgr.control("can0", "can_send", {{"id", "1"}, {"data", "00112233445566778899"}}).ok());
    EXPECT_FALSE(mgr.control("can0", "can_send", {{"data", "00"}}).ok()) << "id is required";
    EXPECT_TRUE(plat.can_sent("can0").empty()) << "nothing rejected may reach the bus";
}

/// @brief can_receive drains the frames buffered since the previous call.
TEST(HardwareCan, ReceivesBufferedFrames)
{
    platform::SimulatedPlatform plat;
    ManualTimeSource ts;
    platform::IPlatform::CanFrame f;
    f.id = 0x7ff;
    f.data = {0x01, 0x02};
    plat.push_can_frame("can0", f);
    DeviceManager mgr(plat, ts, test_descriptor());
    auto r = mgr.can_receive("can0", 8);
    ASSERT_TRUE(r.ok()) << r.error.str();
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].id, 0x7ffu);
}

/// @brief An undeclared control op is refused before the platform is touched.
TEST(HardwareCan, UndeclaredControlIsRefused)
{
    // Board knowledge is data: a control the descriptor does not declare is
    // not available, whatever the platform could do.
    platform::DeviceDescriptor d = test_descriptor();
    for (auto& dev : d.devices)
        if (dev.name == "can0") dev.controls.clear();
    platform::SimulatedPlatform plat;
    ManualTimeSource ts;
    DeviceManager mgr(plat, ts, d);
    EXPECT_FALSE(mgr.control("can0", "can_send", {{"id", "1"}}).ok());
    EXPECT_FALSE(mgr.can_receive("can0", 1).ok());
}

/// @brief Receiving on a non-CAN device is refused.
TEST(HardwareCan, ReceiveOnANonCanDeviceIsRefused)
{
    platform::SimulatedPlatform plat;
    ManualTimeSource ts;
    DeviceManager mgr(plat, ts, test_descriptor());
    EXPECT_FALSE(mgr.can_receive("status-led", 1).ok());
    EXPECT_FALSE(mgr.can_receive("no-such-device", 1).ok());
}
