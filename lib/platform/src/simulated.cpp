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
 * @file lib/platform/src/simulated.cpp
 * @brief Implementation of the simulated platform: in-memory, scriptable reality covering every failure mode for hosts, CI, and fault-injection tests (REQ §, §11).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/platform/simulated.h"

namespace instr
{
/// @brief Platform abstraction: board and hardware access behind IPlatform (REQ §5).
namespace platform
{

/// @brief Construct a fresh simulated board with sane defaults.
SimulatedPlatform::SimulatedPlatform()
{
    identity_.serial = "SIM-0001";
    identity_.hw_revision = "rev-a";
    identity_.board_variant = "sim";
    identity_.mfg_date = "2025-01-01";
    identity_.hostname_base = "sim-device";
    identity_.identity_secret = "sim-factory-secret";
    identity_.valid = true;
    slots_.active = "A";
    slots_.inactive = "B";
    slots_.golden = "A";
    units_["instr-istate.service"] = true;
}

/// @brief Script the factory identity returned to the daemons.
/// @param id Identity the daemons will read.
void SimulatedPlatform::set_factory_identity(const FactoryIdentity& id)
{
    std::lock_guard<std::mutex> lk(mu_);
    identity_ = id;
}
/// @brief Inject a storage device into the enumeration.
/// @param d Storage device to inject.
void SimulatedPlatform::inject_storage_device(const StorageDevice& d)
{
    std::lock_guard<std::mutex> lk(mu_);
    storage_.push_back(d);
}
/// @brief Script a thermal zone reading.
/// @param id Zone id.
/// @param label Zone label.
/// @param c Temperature in Celsius.
void SimulatedPlatform::set_thermal(const std::string& id, const std::string& label, double c)
{
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& t : thermals_)
        if (t.id == id)
        {
            t.temp_c = c;
            t.label = label;
            return;
        }
    thermals_.push_back(ThermalZone{id, label, c});
}
/// @brief Script a power source reading.
/// @param id Source id.
/// @param kind ac or battery.
/// @param pct Charge percentage.
/// @param charging True when charging.
void SimulatedPlatform::set_power(const std::string& id, const std::string& kind, double pct, bool charging)
{
    std::lock_guard<std::mutex> lk(mu_);
    power_.push_back(PowerSource{id, kind, pct, charging});
}
/// @brief Inject a network interface into the enumeration.
/// @param i Interface to inject.
void SimulatedPlatform::inject_iface(const IfaceInfo& i)
{
    std::lock_guard<std::mutex> lk(mu_);
    ifaces_.push_back(i);
}
/// @brief Append one kernel log line (emits it to followers too).
/// @param line Line to append.
void SimulatedPlatform::append_kernel_line(const std::string& line)
{
    std::lock_guard<std::mutex> lk(mu_);
    kernel_log_.push_back(line);
}

/// @brief Factory identity for first-boot key generation (REQ §6.2).
/// @return The scripted factory identity.
Result<FactoryIdentity> SimulatedPlatform::read_factory_identity()
{
    std::lock_guard<std::mutex> lk(mu_);
    if (!identity_.valid) return Result<FactoryIdentity>::fail(Errc::NotFound, "factory identity not provisioned", "simulated");
    return Result<FactoryIdentity>::good(identity_);
}
/// @brief True when the CSPRNG is seeded and safe for key generation.
bool SimulatedPlatform::entropy_available()
{
    return entropy_;
}
/// @brief All storage devices with wear/health data.
/// @return The scripted storage devices.
std::vector<StorageDevice> SimulatedPlatform::storage_devices()
{
    std::lock_guard<std::mutex> lk(mu_);
    return storage_;
}
/// @brief Memory pressure snapshot.
MemInfo SimulatedPlatform::memory_info()
{
    std::lock_guard<std::mutex> lk(mu_);
    return mem_;
}
/// @brief CPU utilisation and frequency snapshot.
CpuSnapshot SimulatedPlatform::cpu_snapshot()
{
    std::lock_guard<std::mutex> lk(mu_);
    return cpu_;
}
/// @brief One-minute load average.
double SimulatedPlatform::load_average_1m()
{
    return load_;
}
/// @brief Total process count.
uint32_t SimulatedPlatform::process_count()
{
    return processes_;
}
/// @brief Uptime in seconds.
uint64_t SimulatedPlatform::uptime_s()
{
    return uptime_;
}
/// @brief Number of boots recorded.
uint32_t SimulatedPlatform::boot_count()
{
    return boot_count_;
}
/// @brief True when the previous shutdown was clean.
bool SimulatedPlatform::last_shutdown_clean()
{
    return clean_shutdown_;
}
/// @brief Thermal zone readings.
/// @return The scripted thermal zones.
std::vector<ThermalZone> SimulatedPlatform::thermal_zones()
{
    std::lock_guard<std::mutex> lk(mu_);
    return thermals_;
}
/// @brief Power source readings.
/// @return The scripted power sources.
std::vector<PowerSource> SimulatedPlatform::power_sources()
{
    std::lock_guard<std::mutex> lk(mu_);
    return power_;
}
/// @brief Configure the hardware watchdog timeout (0 disarms where supported).
/// @param seconds Watchdog timeout in seconds.
Error SimulatedPlatform::watchdog_set_timeout_s(int seconds)
{
    (void)seconds;
    if (fail_watchdog_) return Error::make(Errc::Io, "simulated watchdog open failure", "watchdog");
    watchdog_running_ = true;
    return Error::none();
}
/// @brief Pet the hardware watchdog.
Error SimulatedPlatform::watchdog_pet()
{
    if (fail_watchdog_) return Error::make(Errc::Io, "simulated watchdog pet failure", "watchdog");
    ++watchdog_pets_;
    return Error::none();
}
/// @brief True when a time source is currently disciplined.
bool SimulatedPlatform::time_locked()
{
    return time_locked_;
}
/// @brief Enumerate network interfaces.
/// @return The scripted interfaces.
std::vector<IfaceInfo> SimulatedPlatform::interfaces()
{
    std::lock_guard<std::mutex> lk(mu_);
    return ifaces_;
}
/// @brief Current A/B boot-slot state.
/// @return The scripted boot-slot state.
Result<BootSlotState> SimulatedPlatform::boot_slots()
{
    if (fail_boot_slots_) return Result<BootSlotState>::fail(Errc::Io, "simulated bootctl failure", "boot");
    std::lock_guard<std::mutex> lk(mu_);
    return Result<BootSlotState>::good(slots_);
}
/// @brief Select a boot slot; @p trial marks it pending confirmation.
/// @param slot Slot to select.
/// @param trial Mark the slot pending confirmation when true.
Error SimulatedPlatform::set_boot_slot(const std::string& slot, bool trial)
{
    if (fail_boot_slots_) return Error::make(Errc::Io, "simulated set-slot failure", "boot");
    std::lock_guard<std::mutex> lk(mu_);
    if (slot != "A" && slot != "B") return Error::make(Errc::InvalidArgument, "bad slot", slot);
    if (trial) slots_.pending = slot;
    return Error::none();
}
/// @brief Confirm the currently pending slot as good.
Error SimulatedPlatform::confirm_boot_slot()
{
    if (fail_boot_slots_) return Error::make(Errc::Io, "simulated confirm failure", "boot");
    std::lock_guard<std::mutex> lk(mu_);
    if (!slots_.pending.empty())
    {
        slots_.inactive = slots_.active;
        slots_.active = slots_.pending;
        slots_.pending.clear();
    }
    return Error::none();
}
/// @brief Drive a GPIO line (Hardware daemon only, R7).
/// @param line GPIO line number.
/// @param value Value to drive.
Error SimulatedPlatform::gpio_set(int line, bool value)
{
    if (fail_gpio_) return Error::make(Errc::Io, "simulated gpio failure", "gpio");
    std::lock_guard<std::mutex> lk(mu_);
    gpios_[line] = value;
    return Error::none();
}
/// @brief Read a GPIO line.
/// @param line GPIO line number.
/// @return The scripted line state.
Result<bool> SimulatedPlatform::gpio_get(int line)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = gpios_.find(line);
    if (it == gpios_.end()) return Result<bool>::fail(Errc::NotFound, "gpio not configured", "gpio");
    return Result<bool>::good(it->second);
}
/// @brief Set an LED steady on/off.
/// @param name LED name.
/// @param on True to light the LED.
Error SimulatedPlatform::led_set(const std::string& name, bool on)
{
    std::lock_guard<std::mutex> lk(mu_);
    leds_[name] = on;
    return Error::none();
}
/// @brief Blink an LED with the given duty cycle.
/// @param name LED name.
/// @param on_ms On duration in ms.
/// @param off_ms Off duration in ms.
/// @param repeats Number of blinks.
Error SimulatedPlatform::led_blink(const std::string& name, int on_ms, int off_ms, int repeats)
{
    std::lock_guard<std::mutex> lk(mu_);
    (void)on_ms;
    (void)off_ms;
    // Behavioural sanity: acknowledge the blink request.
    leds_[name + ":blink-ack"] = repeats > 0;
    return Error::none();
}
/// @brief Power-cycle a USB port.
/// @param port USB port to reset.
Error SimulatedPlatform::usb_reset(const std::string& port)
{
    if (fail_usb_) return Error::make(Errc::Io, "simulated usb reset failure", port);
    std::lock_guard<std::mutex> lk(mu_);
    usb_resets_.push_back(port);
    return Error::none();
}
/// @brief Run a CAN loopback health check on @p iface.
/// @param iface CAN interface to test.
/// @return True (loopback echoes by default).
Result<bool> SimulatedPlatform::can_loopback_test(const std::string& iface)
{
    if (fail_can_) return Result<bool>::good(false);
    (void)iface;
    return Result<bool>::good(true);
}
/// @brief Send a control command to a display.
/// @param cmd Display command.
/// @param arg Command argument.
Error SimulatedPlatform::display_control(const std::string& cmd, const std::string& arg)
{
    if (fail_display_) return Error::make(Errc::Io, "simulated display failure", cmd);
    std::lock_guard<std::mutex> lk(mu_);
    display_cmds_.push_back(cmd + ":" + arg);
    return Error::none();
}
/// @brief Probe display presence/capability.
/// @return The scripted probe result.
Result<std::string> SimulatedPlatform::display_probe()
{
    if (fail_display_) return Result<std::string>::fail(Errc::Io, "simulated display probe failure", "display");
    return Result<std::string>::good("ok 1280x800");
}
/// @brief Start a systemd unit (istate requests, REQ §7.1).
/// @param unit Unit to start.
Error SimulatedPlatform::unit_start(const std::string& unit)
{
    if (fail_unit_start_) return Error::make(Errc::Unavailable, "simulated systemctl failure", unit);
    std::lock_guard<std::mutex> lk(mu_);
    units_[unit] = true;
    started_units_.push_back(unit);
    return Error::none();
}
/// @brief Stop a systemd unit.
/// @param unit Unit to stop.
Error SimulatedPlatform::unit_stop(const std::string& unit)
{
    std::lock_guard<std::mutex> lk(mu_);
    units_[unit] = false;
    return Error::none();
}
/// @brief Restart a systemd unit.
/// @param unit Unit to restart.
Error SimulatedPlatform::unit_restart(const std::string& unit)
{
    std::lock_guard<std::mutex> lk(mu_);
    units_[unit] = true;
    restarts_.push_back(unit);
    return Error::none();
}
/// @brief Query whether a unit is active.
/// @param unit Unit to query.
/// @return The scripted unit state.
Result<bool> SimulatedPlatform::unit_active(const std::string& unit)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = units_.find(unit);
    return Result<bool>::good(it != units_.end() && it->second);
}
/// @copydoc IPlatform::kernel_log_lines
/// @param from_start True to read from the beginning; false to follow with new lines only.
/// @return The scripted kernel log lines.
Result<std::vector<std::string>> SimulatedPlatform::kernel_log_lines(bool from_start)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (from_start)
    {
        // Backfill hands back the whole ring and parks the cursor at the end, so
        // the follower that runs next does not re-report the boot window.
        follow_cursor_ = kernel_log_.size();
        return Result<std::vector<std::string>>::good(kernel_log_);
    }
    if (follow_cursor_ > kernel_log_.size()) follow_cursor_ = kernel_log_.size();
    std::vector<std::string> fresh(kernel_log_.begin() + long(follow_cursor_), kernel_log_.end());
    follow_cursor_ = kernel_log_.size();
    return Result<std::vector<std::string>>::good(std::move(fresh));
}
/// @brief Application log sink / journal access for LogManager.
/// @param unit Unit whose journal to read.
/// @param max_lines Maximum lines returned.
/// @return The scripted journal lines.
Result<std::vector<std::string>> SimulatedPlatform::journal_lines(const std::string& unit, size_t max_lines)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = journal_.find(unit);
    if (it == journal_.end()) return Result<std::vector<std::string>>::good({});
    std::vector<std::string> out = it->second;
    if (out.size() > max_lines) out.erase(out.begin(), out.end() - long(max_lines));
    return Result<std::vector<std::string>>::good(std::move(out));
}
/// @brief Render/apply a network plan (interfaces, firewall, ssh windows, wg, time).
///
/// The simulated platform records the plan; native shells out only when
/// INSTR_APPLY_NETWORK=1 (never silently mutates a host).
/// @param plan_json Network plan to record.
Error SimulatedPlatform::apply_network_plan(const std::string& plan_json)
{
    std::lock_guard<std::mutex> lk(mu_);
    applied_plans_.push_back(plan_json);
    return Error::none();
}
/// @brief True when HQ is currently reachable.
bool SimulatedPlatform::hq_reachable()
{
    return hq_reachable_;
}

} // namespace platform
} // namespace instr
// ---- §5 additions: partition topology, time sources, CAN, measured boot ----
// The simulated platform must be complete enough to run the whole suite on a
// build host with no hardware, so each of these has a working default rather
// than an empty stub.

namespace instr
{
/// @brief Platform abstraction: board and hardware access behind IPlatform (REQ §5).
namespace platform
{

/// @brief Partition topology including mount points and usage (§5).
/// @return The scripted partition topology.
std::vector<Partition> SimulatedPlatform::partitions()
{
    std::lock_guard<std::mutex> lk(mu_);
    if (!partitions_.empty()) return partitions_;
    // A representative A/B layout with a separate /pst and a state partition —
    // the topology §6 recommends, so budget logic has something real to read.
    std::vector<Partition> out;
    auto mk = [](const char* name, const char* mount, uint64_t total, uint64_t used, bool ro) {
        Partition p;
        p.name = name;
        p.device_path = std::string("/dev/") + name;
        p.parent = "mmcblk0";
        p.mount_point = mount;
        p.fs_type = ro ? "squashfs" : "ext4";
        p.bytes = total;
        p.used_bytes = used;
        p.read_only = ro;
        return p;
    };
    out.push_back(mk("mmcblk0p1", "/boot", 256ull << 20, 64ull << 20, false));
    out.push_back(mk("mmcblk0p2", "/", 2ull << 30, 1ull << 30, true)); // slot A
    out.push_back(mk("mmcblk0p3", "", 2ull << 30, 0, true)); // slot B
    out.push_back(mk("mmcblk0p4", "/pst", 512ull << 20, 32ull << 20, false));
    out.push_back(mk("mmcblk0p5", "/pst/state", 1ull << 30, 64ull << 20, false));
    return out;
}

/// @brief Which time sources this board has, and their state (§5, §7.1).
/// @return The scripted time-source inventory.
std::vector<TimeSourceInfo> SimulatedPlatform::time_sources()
{
    std::lock_guard<std::mutex> lk(mu_);
    if (!time_sources_.empty()) return time_sources_;
    std::vector<TimeSourceInfo> out;
    TimeSourceInfo ntp;
    ntp.kind = "ntp";
    ntp.present = true;
    ntp.locked = time_locked_;
    ntp.offset_ms = 0.5;
    ntp.quality = time_locked_ ? 0.8 : 0.0;
    out.push_back(ntp);
    TimeSourceInfo rtc;
    rtc.kind = "rtc";
    rtc.present = true;
    rtc.locked = true;
    rtc.quality = 0.2;
    out.push_back(rtc);
    // GNSS and PTP are absent unless a test declares them: reporting a source
    // the board does not have is exactly the dishonesty §5.1 forbids.
    return out;
}

/// @brief Send one CAN frame on @p iface.
/// @param iface CAN interface.
/// @param frame Frame to send.
Error SimulatedPlatform::can_send(const std::string& iface, const CanFrame& frame)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = can_present_.find(iface);
    if (it != can_present_.end() && !it->second) return Error::make(Errc::NotSupported, "no CAN controller", iface);
    can_tx_[iface].push_back(frame);
    // Loopback by default: a health check that sends and reads back sees its
    // own frame, which is the cheap behavioural check §7.4 asks for.
    can_rx_[iface].push_back(frame);
    return Error::none();
}

/// @brief Drain up to @p max_frames frames buffered since the previous call.
/// @param iface CAN interface.
/// @param max_frames Maximum frames returned.
/// @return Queued receive frames.
Result<std::vector<SimulatedPlatform::CanFrame>> SimulatedPlatform::can_recv(const std::string& iface, size_t max_frames)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = can_present_.find(iface);
    if (it != can_present_.end() && !it->second) return Result<std::vector<CanFrame>>::fail(Errc::NotSupported, "no CAN controller", iface);
    auto& q = can_rx_[iface];
    std::vector<CanFrame> out;
    while (!q.empty() && out.size() < max_frames)
    {
        out.push_back(q.front());
        q.erase(q.begin());
    }
    return Result<std::vector<CanFrame>>::good(std::move(out));
}

/// @brief Measured-boot attestation state (§10).
MeasuredBootState SimulatedPlatform::measured_boot()
{
    std::lock_guard<std::mutex> lk(mu_);
    return measured_boot_;
}

} // namespace platform
} // namespace instr
