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
 * @file daemons/hardware/src/domain.cpp
 * @brief hardware domain implementation (REQ §7.4): descriptor-driven device checks and controls, check state tracking, and board-specific behaviour from data.
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/hardware/domain.h"

#include <cctype>

#include "instr/platform/simulated.h"

#include <algorithm>
#include <chrono>

namespace instr
{
/// @brief Hardware daemon: device controls and quick health checks (§7.4, R7).
namespace hardware
{
/// @brief Pure domain logic for this daemon: no transport, no sockets, fully unit-testable.
namespace domain
{

/// @brief Canonical name of a check state.
/// @param s Check state to name.
/// @return Canonical lowercase name.
const char* check_state_name(CheckState s)
{
    switch (s)
    {
    case CheckState::Pass: return "pass";
    case CheckState::Fail: return "fail";
    case CheckState::Degraded: return "degraded";
    case CheckState::NotPresent: return "not-present";
    case CheckState::Unknown: return "unknown";
    }
    return "unknown";
}

/// @brief Look up a device spec by name.
/// @param name Device name to look up.
/// @return Pointer to the spec, or nullptr.
const platform::DeviceSpec* DeviceManager::find(const std::string& name) const
{
    for (const auto& d : desc_.devices)
        if (d.name == name) return &d;
    return nullptr;
}

/// @brief Stamp and return a check result.
/// @param dev Device name.
/// @param chk Check name.
/// @param st Outcome state.
/// @param detail Human-readable detail.
/// @param started Start time for duration accounting.
/// @return The stamped check result.
CheckResult DeviceManager::make_result(const std::string& dev, const std::string& chk, CheckState st, const std::string& detail, uint64_t started)
{
    CheckResult r;
    r.device = dev;
    r.check = chk;
    r.state = st;
    r.detail = detail;
    r.mono_ns = ts_.mono_ns();
    r.duration_ms = (ts_.mono_ns() - started) / 1'000'000;
    return r;
}

/// @brief Dispatch a control op to the platform primitive for the device.
/// @param device Device name.
/// @param control_op Control op from the device's controls list.
/// @param args Operation arguments.
/// @return ok(), or an error for unknown device/op or platform refusal.
Error DeviceManager::control(const std::string& device, const std::string& control_op, const std::map<std::string, std::string>& args)
{
    const platform::DeviceSpec* dev = find(device);
    if (!dev) return Error::make(Errc::NotFound, "unknown device: " + device, "hardware");
    // Only controls declared in the descriptor are allowed (board knowledge is
    // data, not code).
    bool declared = false;
    for (const auto& c : dev->controls)
        if (c == control_op) declared = true;
    if (!declared) return Error::make(Errc::NotSupported, "control op not declared for device " + device + ": " + control_op, device);
    if (dev->kind == "gpio")
    {
        int line = std::stoi(dev->params.count("line") ? dev->params.at("line") : "-1");
        if (control_op == "gpio_set")
        {
            auto it = args.find("value");
            if (it == args.end()) return Error::make(Errc::InvalidArgument, "value required", device);
            return plat_.gpio_set(line, it->second == "true" || it->second == "1");
        }
    }
    else if (dev->kind == "led")
    {
        if (control_op == "led_set")
        {
            auto it = args.find("on");
            if (it == args.end()) return Error::make(Errc::InvalidArgument, "on required", device);
            return plat_.led_set(device, it->second == "true" || it->second == "1");
        }
        if (control_op == "led_blink")
        {
            int on = 100, off = 100, reps = 3;
            auto get = [&](const char* k, int d) {
                auto it = args.find(k);
                return it != args.end() ? std::stoi(it->second) : d;
            };
            on = get("on_ms", on);
            off = get("off_ms", off);
            reps = get("repeats", reps);
            return plat_.led_blink(device, on, off, reps);
        }
    }
    else if (dev->kind == "usb")
    {
        if (control_op == "usb_reset") return plat_.usb_reset(dev->params.count("port") ? dev->params.at("port") : device);
    }
    else if (dev->kind == "can")
    {
        const std::string iface = dev->params.count("iface") ? dev->params.at("iface") : device;
        if (control_op == "can_send")
        {
            // id is hex or decimal; data is a hex byte string ("deadbeef"), which
            // is how a CAN payload is written everywhere else in the toolchain.
            auto id_it = args.find("id");
            if (id_it == args.end()) return Error::make(Errc::InvalidArgument, "id required", device);
            platform::IPlatform::CanFrame frame;
            frame.id = uint32_t(std::stoul(id_it->second, nullptr, 0));
            auto ext = args.find("extended");
            frame.extended = ext != args.end() && (ext->second == "true" || ext->second == "1");
            auto data_it = args.find("data");
            if (data_it != args.end())
            {
                const std::string& hex = data_it->second;
                if (hex.size() % 2 != 0) return Error::make(Errc::InvalidArgument, "data must be an even-length hex string", device);
                for (size_t i = 0; i < hex.size(); i += 2)
                {
                    if (!std::isxdigit(static_cast<unsigned char>(hex[i])) || !std::isxdigit(static_cast<unsigned char>(hex[i + 1]))) return Error::make(Errc::InvalidArgument, "data is not hex", device);
                    frame.data.push_back(uint8_t(std::stoul(hex.substr(i, 2), nullptr, 16)));
                }
            }
            if (frame.data.size() > 8 && !frame.extended) return Error::make(Errc::InvalidArgument, "classic CAN payload exceeds 8 bytes", device);
            return plat_.can_send(iface, frame);
        }
        if (control_op == "can_recv")
        {
            // Declared so the descriptor can gate it; the frames come back through
            // can_receive(), which has a Result signature to carry them.
            auto r = plat_.can_recv(iface, 1);
            return r.ok() ? Error::none() : r.error;
        }
    }
    else if (dev->kind == "display")
    {
        if (control_op == "display_control")
        {
            auto it = args.find("cmd");
            if (it == args.end()) return Error::make(Errc::InvalidArgument, "cmd required", device);
            return plat_.display_control(it->second, args.count("arg") ? args.at("arg") : "");
        }
    }
    return Error::make(Errc::NotSupported, "control op not implemented for kind " + dev->kind, control_op);
}

/// @brief Drain buffered CAN frames for a device.
/// @param device CAN device to drain.
/// @param max_frames Maximum frames returned.
/// @return Buffered frames, or an error.
Result<std::vector<platform::IPlatform::CanFrame>> DeviceManager::can_receive(const std::string& device, size_t max_frames)
{
    using Ret = Result<std::vector<platform::IPlatform::CanFrame>>;
    const platform::DeviceSpec* dev = find(device);
    if (!dev) return Ret::fail(Errc::NotFound, "unknown device: " + device, "hardware");
    if (dev->kind != "can") return Ret::fail(Errc::NotSupported, "not a can device", device);
    bool declared = false;
    for (const auto& c : dev->controls)
        if (c == "can_recv") declared = true;
    if (!declared) return Ret::fail(Errc::NotSupported, "control op not declared for device " + device + ": can_recv", device);
    const std::string iface = dev->params.count("iface") ? dev->params.at("iface") : device;
    return plat_.can_recv(iface, max_frames == 0 ? 1 : max_frames);
}

/// @brief Read a GPIO line for a device.
/// @param device Device whose GPIO to read.
/// @return The line state, or an error.
Result<bool> DeviceManager::gpio_read(const std::string& device)
{
    const platform::DeviceSpec* dev = find(device);
    if (!dev) return Result<bool>::fail(Errc::NotFound, "unknown device: " + device, "hardware");
    if (dev->kind != "gpio") return Result<bool>::fail(Errc::NotSupported, "not a gpio device", device);
    int line = std::stoi(dev->params.count("line") ? dev->params.at("line") : "-1");
    return plat_.gpio_get(line);
}

/// @brief Run one quick health check on the platform.
/// @param dev Device to check.
/// @param chk Check to run.
/// @return The check outcome.
CheckResult DeviceManager::run_check(const platform::DeviceSpec& dev, const platform::DeviceCheck& chk)
{
    uint64_t started = ts_.mono_ns();
    if (chk.op == "presence")
    {
        // Cheap presence detection per kind.
        if (dev.kind == "gpio")
        {
            int line = std::stoi(dev.params.count("line") ? dev.params.at("line") : "-1");
            auto v = plat_.gpio_get(line);
            if (!v.ok()) return make_result(dev.name, chk.name, CheckState::NotPresent, v.error.message, started);
            return make_result(dev.name, chk.name, CheckState::Pass, "line readable", started);
        }
        if (dev.kind == "display")
        {
            auto p = plat_.display_probe();
            if (!p.ok()) return make_result(dev.name, chk.name, CheckState::NotPresent, p.error.message, started);
            return make_result(dev.name, chk.name, CheckState::Pass, *p, started);
        }
        return make_result(dev.name, chk.name, CheckState::Pass, "assumed present", started);
    }
    if (chk.op == "gpio_toggle_read")
    {
        int line = std::stoi(dev.params.count("line") ? dev.params.at("line") : "-1");
        // Behavioural sanity: set high, read back; set low, read back.
        Error e1 = plat_.gpio_set(line, true);
        if (!e1.ok()) return make_result(dev.name, chk.name, CheckState::Fail, e1.message, started);
        auto r1 = plat_.gpio_get(line);
        if (!r1.ok() || !*r1) return make_result(dev.name, chk.name, CheckState::Degraded, "set-high readback mismatch", started);
        Error e2 = plat_.gpio_set(line, false);
        if (!e2.ok()) return make_result(dev.name, chk.name, CheckState::Fail, e2.message, started);
        auto r2 = plat_.gpio_get(line);
        if (!r2.ok() || *r2) return make_result(dev.name, chk.name, CheckState::Degraded, "set-low readback mismatch", started);
        return make_result(dev.name, chk.name, CheckState::Pass, "toggle+read ok", started);
    }
    if (chk.op == "led_blink_ack")
    {
        Error e = plat_.led_blink(dev.name, 30, 30, 1);
        if (!e.ok()) return make_result(dev.name, chk.name, CheckState::Fail, e.message, started);
        // Blink request acknowledged when the platform records it.
        auto* sim = dynamic_cast<platform::SimulatedPlatform*>(&plat_);
        if (sim)
        {
            bool ack = sim->led_state().count(dev.name + ":blink-ack") && sim->led_state().at(dev.name + ":blink-ack");
            return make_result(dev.name, chk.name, ack ? CheckState::Pass : CheckState::Degraded, ack ? "blink acknowledged" : "no blink ack", started);
        }
        return make_result(dev.name, chk.name, CheckState::Pass, "blink issued", started);
    }
    if (chk.op == "can_loopback")
    {
        std::string iface = dev.params.count("iface") ? dev.params.at("iface") : dev.name;
        auto r = plat_.can_loopback_test(iface);
        if (!r.ok()) return make_result(dev.name, chk.name, CheckState::Fail, r.error.message, started);
        return make_result(dev.name, chk.name, *r ? CheckState::Pass : CheckState::Fail, *r ? "loopback frame ok" : "loopback frame lost", started);
    }
    if (chk.op == "usb_reset_ack")
    {
        std::string port = dev.params.count("port") ? dev.params.at("port") : dev.name;
        Error e = plat_.usb_reset(port);
        if (!e.ok()) return make_result(dev.name, chk.name, CheckState::Fail, e.message, started);
        return make_result(dev.name, chk.name, CheckState::Pass, "reset accepted", started);
    }
    if (chk.op == "display_probe")
    {
        auto p = plat_.display_probe();
        if (!p.ok()) return make_result(dev.name, chk.name, CheckState::Fail, p.error.message, started);
        return make_result(dev.name, chk.name, CheckState::Pass, *p, started);
    }
    return make_result(dev.name, chk.name, CheckState::Unknown, "check op not known: " + chk.op, started);
}

/// @brief Run every check for one device.
/// @param name Device to check.
/// @return Results for every check of the device.
std::vector<CheckResult> DeviceManager::check_device(const std::string& name)
{
    std::vector<CheckResult> out;
    const platform::DeviceSpec* dev = find(name);
    if (!dev) return out;
    for (const auto& chk : dev->checks) out.push_back(run_check(*dev, chk));
    last_check_mono_[name] = ts_.mono_ns();
    return out;
}

/// @brief Run every check for every device.
/// @return Results for every check of every device.
std::vector<CheckResult> DeviceManager::check_all()
{
    std::vector<CheckResult> out;
    for (const auto& dev : desc_.devices)
    {
        auto r = check_device(dev.name);
        out.insert(out.end(), r.begin(), r.end());
    }
    return out;
}

/// @brief Run checks whose periodic schedule is due.
/// @param now_mono_ns Current monotonic time.
/// @return Results for due checks.
std::vector<CheckResult> DeviceManager::due_checks(uint64_t now_mono_ns)
{
    std::vector<CheckResult> out;
    uint64_t interval_ns = uint64_t(periodic_interval_s_ * 1e9);
    for (const auto& dev : desc_.devices)
    {
        auto it = last_check_mono_.find(dev.name);
        if (it == last_check_mono_.end() || now_mono_ns - it->second >= interval_ns)
        {
            auto r = check_device(dev.name);
            out.insert(out.end(), r.begin(), r.end());
        }
    }
    return out;
}

} // namespace domain
} // namespace hardware
} // namespace instr