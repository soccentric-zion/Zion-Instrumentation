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
 * @file daemons/hardware/src/daemon.cpp
 * @brief hardware daemon facade: core state, shared command table construction, and spoke wiring (REQ §7.4).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/hardware/daemon.h"

namespace instr
{
/// @brief Hardware daemon: device controls and quick health checks (§7.4, R7).
namespace hardware
{

/// @brief Render the board device inventory as JSON.
/// @return The board's device inventory as JSON.
std::string HardwareCore::descriptor_summary_json() const
{
    JValue arr = JValue::array();
    for (const auto& d : mgr_.devices())
    {
        JValue o = JValue::object();
        o.set("name", JValue(d.name));
        o.set("kind", JValue(d.kind));
        JValue controls = JValue::array();
        for (const auto& c : d.controls) controls.push(JValue(c));
        o.set("controls", controls);
        JValue checks = JValue::array();
        for (const auto& c : d.checks) checks.push(JValue(c.name));
        o.set("checks", checks);
        arr.push(o);
    }
    return arr.dump();
}

/// @brief Build the hardware ctrl command table.
/// @param core Domain core the table reads from.
/// @return The command table.
CommandTable build_hardware_table(HardwareCore* core)
{
    CommandTable t;
    t.add({"list-devices", "list board devices from the descriptor", {}, [core](const std::string&) { return CommandResult::good(core->descriptor_summary_json()); }});
    t.add({"check", "run the quick health checks for one device", {{"device", true, "device name from devices.yaml"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               std::string dev = (*a).get_string("device");
               auto results = core->manager().check_device(dev);
               if (results.empty() && !core->manager().find(dev)) return CommandResult::err(Errc::NotFound, "unknown device: " + dev);
               JValue arr = JValue::array();
               for (const auto& r : results)
               {
                   JValue o = JValue::object();
                   o.set("device", JValue(r.device));
                   o.set("check", JValue(r.check));
                   o.set("state", JValue(check_state_name(r.state)));
                   o.set("detail", JValue(r.detail));
                   arr.push(o);
               }
               return CommandResult::good(arr.dump());
           }});
    t.add({"check-all", "run quick health checks for every device", {}, [core](const std::string&) {
               auto results = core->manager().check_all();
               JValue arr = JValue::array();
               for (const auto& r : results)
               {
                   JValue o = JValue::object();
                   o.set("device", JValue(r.device));
                   o.set("check", JValue(r.check));
                   o.set("state", JValue(check_state_name(r.state)));
                   o.set("detail", JValue(r.detail));
                   arr.push(o);
               }
               return CommandResult::good(arr.dump());
           }});
    t.add({"gpio-set", "drive a GPIO line", {{"device", true, "gpio device name"}, {"value", true, "true|false"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               std::map<std::string, std::string> m;
               m["value"] = (*a).get_string("value");
               Error e = core->manager().control((*a).get_string("device"), "gpio_set", m);
               return e.ok() ? CommandResult::good(std::string("{\"set\":true}")) : CommandResult::from_error(e);
           }});
    t.add({"gpio-get", "read a GPIO line", {{"device", true, "gpio device name"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               auto v = core->manager().gpio_read((*a).get_string("device"));
               if (!v.ok()) return CommandResult::from_error(v.error);
               JValue o = JValue::object();
               o.set("value", JValue(*v));
               return CommandResult::good(o.dump());
           }});
    t.add({"led-set", "set a board LED", {{"device", true, "led name"}, {"on", true, "true|false"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               std::map<std::string, std::string> m;
               m["on"] = (*a).get_string("on");
               Error e = core->manager().control((*a).get_string("device"), "led_set", m);
               return e.ok() ? CommandResult::good(std::string("{\"set\":true}")) : CommandResult::from_error(e);
           }});
    t.add({"led-blink", "blink a board LED", {{"device", true, "led name"}, {"on_ms", false, "on ms"}, {"off_ms", false, "off ms"}, {"repeats", false, "repeat count"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               std::map<std::string, std::string> m;
               for (const char* k : {"on_ms", "off_ms", "repeats"})
                   if ((*a).find(k)) m[k] = (*a).find(k)->as_string();
               Error e = core->manager().control((*a).get_string("device"), "led_blink", m);
               return e.ok() ? CommandResult::good(std::string("{\"blinking\":true}")) : CommandResult::from_error(e);
           }});
    t.add({"usb-reset", "reset a USB device", {{"device", true, "usb device name"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               Error e = core->manager().control((*a).get_string("device"), "usb_reset", {});
               return e.ok() ? CommandResult::good(std::string("{\"reset\":true}")) : CommandResult::from_error(e);
           }});
    t.add({"display-cmd", "control the display", {{"device", true, "display name"}, {"cmd", true, "blank|unblank"}, {"arg", false, "argument"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               std::map<std::string, std::string> m;
               m["cmd"] = (*a).get_string("cmd");
               m["arg"] = (*a).get_string("arg");
               Error e = core->manager().control((*a).get_string("device"), "display_control", m);
               return e.ok() ? CommandResult::good(std::string("{\"ok\":true}")) : CommandResult::from_error(e);
           }});
    t.add({"can-send", "transmit a CAN frame", {{"device", true, "can device name"}, {"id", true, "frame id (0x123 or decimal)"}, {"data", false, "payload as a hex byte string, e.g. deadbeef"}, {"extended", false, "true for a 29-bit extended id"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               std::map<std::string, std::string> m;
               m["id"] = (*a).get_string("id");
               m["data"] = (*a).get_string("data");
               m["extended"] = (*a).get_bool("extended") ? "true" : "false";
               Error e = core->manager().control((*a).get_string("device"), "can_send", m);
               return e.ok() ? CommandResult::good(std::string("{\"sent\":true}")) : CommandResult::from_error(e);
           }});
    t.add({"can-recv", "read CAN frames buffered since the last call", {{"device", true, "can device name"}, {"max", false, "max frames (default 16)"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               size_t max = size_t((*a).get_number("max", 16));
               auto r = core->manager().can_receive((*a).get_string("device"), max);
               if (!r.ok()) return CommandResult::from_error(r.error);
               JValue arr = JValue::array();
               for (const auto& f : *r)
               {
                   JValue o = JValue::object();
                   o.set("id", JValue(double(f.id)));
                   o.set("extended", JValue(f.extended));
                   static const char* kHex = "0123456789abcdef";
                   std::string hex;
                   for (uint8_t b : f.data)
                   {
                       hex.push_back(kHex[b >> 4]);
                       hex.push_back(kHex[b & 0xf]);
                   }
                   o.set("data", JValue(hex));
                   arr.push(o);
               }
               JValue out = JValue::object();
               out.set("frames", arr);
               return CommandResult::good(out.dump());
           }});
    t.add({"periodic-interval", "set the periodic check interval", {{"seconds", true, "interval in seconds"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               double s = (*a).get_number("seconds", 0);
               if (s <= 0) return CommandResult::err(Errc::InvalidArgument, "seconds must be > 0");
               core->manager().set_periodic_interval_s(s);
               return CommandResult::good(std::string("{\"set\":true}"));
           }});
    return t;
}

/// @brief Wire the channel, config sink and handlers together.
/// @param core Domain core.
/// @param log Logger.
/// @param table Command table.
/// @param hub_target istate gRPC target.
/// @param build_version Build version to advertise.
HardwareSpoke::HardwareSpoke(HardwareCore* core, Logger* log, const CommandTable* table, const std::string& hub_target, const std::string& build_version) : core_(core), log_(log), table_(table)
{
    using ChannelT = SpokeChannel<hardware::HardwareHub, hardware::HardwareSpokeFrame, hardware::HardwareHubFrame>;
    ChannelT::Options o;
    o.target = hub_target;
    o.daemon = "hardware";
    o.build_version = build_version;
    o.capabilities = {"controls", "quick-checks", "periodic-schedule"};
    channel_ = std::make_unique<ChannelT>(std::move(o), *log);
    channel_->set_hub_handler([this](const hardware::HardwareHubFrame& hf) {
        if (hf.has_command())
        {
            common::Ack ack = dispatch_to_ack(table_, hf.command());
            channel_->send_ack(ack);
        }
    });
    core_->set_result_sink([this](const CheckResult& r) { send_result(r); });
    // Until a schema is installed, deliveries still apply — just unvalidated.
    // A packaging slip must not leave a daemon running unconfigured (R10).
    set_schema(JValue::object());
}

/// @brief Install the schema every config delivery must satisfy.
/// @param schema Schema every config delivery must satisfy.
void HardwareSpoke::set_schema(JValue schema)
{
    config_sink_ = std::make_unique<ConfigSink>("hardware", std::move(schema), [this](const ConfigDocument& doc) { return core_->apply_config(doc); });
    channel_->set_config_handler([this](const common::ConfigDelivery& delivery) { return config_sink_->deliver(delivery.yaml(), delivery.revision()); });
}

/// @brief Start the transport channel.
void HardwareSpoke::start()
{
    channel_->start();
}
/// @brief Stop the transport channel.
void HardwareSpoke::stop()
{
    channel_->stop();
}

/// @brief Queue a check result to istate.
/// @param r Check result to queue.
void HardwareSpoke::send_result(const CheckResult& r)
{
    channel_->enqueue([r](hardware::HardwareSpokeFrame& f) {
        auto* res = f.mutable_result();
        res->set_device(r.device);
        res->set_check(r.check);
        switch (r.state)
        {
        case CheckState::Pass: res->set_state(common::HEALTH_PASS); break;
        case CheckState::Degraded: res->set_state(common::HEALTH_DEGRADED); break;
        case CheckState::Fail: res->set_state(common::HEALTH_FAIL); break;
        case CheckState::NotPresent: res->set_state(common::HEALTH_NOT_PRESENT); break;
        default: res->set_state(common::HEALTH_UNKNOWN); break;
        }
        res->set_detail(r.detail);
        res->set_duration_ms(r.duration_ms);
    });
}

} // namespace hardware
} // namespace instr