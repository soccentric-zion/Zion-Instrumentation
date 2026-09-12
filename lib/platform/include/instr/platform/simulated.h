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
 * @file lib/platform/include/instr/platform/simulated.h
 * @brief simulated platform (REQ §5)
 *
 * Complete enough to run the entire suite on a build host with no hardware;
 * the integration harness and fault-injection tests depend on it. All state
 * is in-memory and scriptable, with injectors for every §11 failure mode.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "instr/platform/platform.h"

namespace instr
{
/// @brief Platform abstraction: board and hardware access behind IPlatform (REQ §5).
namespace platform
{

/// @brief Scripted in-memory platform: every board behaviour is test-controlled.
///
/// Backs the whole-suite simulated mode (REQ §5) and unit tests (§12 fault
/// injection).
class SimulatedPlatform : public IPlatform
{
  public:
    /// @brief Construct a fresh simulated board with sane defaults.
    SimulatedPlatform();

    /// @brief Script the factory identity returned to the daemons.
    /// @param id Identity the daemons will read.
    void set_factory_identity(const FactoryIdentity& id);
    /// @brief Script entropy availability.
    /// @param v Entropy availability to script.
    void set_entropy_available(bool v)
    {
        entropy_ = v;
    }
    /// @brief Inject a storage device into the enumeration.
    /// @param d Storage device to inject.
    void inject_storage_device(const StorageDevice& d);
    /// @brief Script a thermal zone reading.
    /// @param id Zone id.
    /// @param label Zone label.
    /// @param c Temperature in Celsius.
    void set_thermal(const std::string& id, const std::string& label, double c);
    /// @brief Script a power source reading.
    /// @param id Source id.
    /// @param kind ac or battery.
    /// @param pct Charge percentage.
    /// @param charging True when charging.
    void set_power(const std::string& id, const std::string& kind, double pct, bool charging);
    /// @brief Inject a network interface into the enumeration.
    /// @param i Interface to inject.
    void inject_iface(const IfaceInfo& i);
    void set_time_locked(bool v)
    {
        time_locked_ = v;
    }
    void set_hq_reachable(bool v)
    {
        hq_reachable_ = v;
    }
    void set_mem(const MemInfo& m)
    {
        mem_ = m;
    }
    /// @brief Script the CPU snapshot.
    /// @param c CPU snapshot to script.
    void set_cpu(const CpuSnapshot& c)
    {
        cpu_ = c;
    }
    /// @brief Script the load average.
    /// @param l Load average to script.
    void set_load(double l)
    {
        load_ = l;
    }
    /// @brief Script the process count.
    /// @param n Process count to script.
    void set_processes(uint32_t n)
    {
        processes_ = n;
    }
    void set_uptime_s(uint64_t s)
    {
        uptime_ = s;
    }
    /// @brief Script the boot count.
    /// @param n Boot count to script.
    void set_boot_count(uint32_t n)
    {
        boot_count_ = n;
    }
    /// @brief Script the last-shutdown-clean state.
    /// @param v Last-shutdown-clean state to script.
    void set_last_shutdown_clean(bool v)
    {
        clean_shutdown_ = v;
    }

    /// @brief Fault injection (REQ §12): watchdog failures.
    /// @param v True to fail watchdog operations.
    void fail_watchdog(bool v)
    {
        fail_watchdog_ = v;
    }
    /// @brief Fault injection: GPIO failures.
    /// @param v True to fail GPIO operations.
    void fail_gpio(bool v)
    {
        fail_gpio_ = v;
    }
    /// @brief Fault injection: USB reset failures.
    /// @param v True to fail USB reset operations.
    void fail_usb(bool v)
    {
        fail_usb_ = v;
    }
    /// @brief Fault injection: CAN failures.
    /// @param v True to fail CAN operations.
    void fail_can(bool v)
    {
        fail_can_ = v;
    }
    /// @brief Fault injection: display failures.
    /// @param v True to fail display operations.
    void fail_display(bool v)
    {
        fail_display_ = v;
    }
    /// @brief Fault injection: unit start failures.
    /// @param v True to fail unit starts.
    void fail_unit_start(bool v)
    {
        fail_unit_start_ = v;
    }
    /// @brief Fault injection: boot-slot operation failures.
    /// @param v True to fail boot-slot operations.
    void fail_boot_slot_ops(bool v)
    {
        fail_boot_slots_ = v;
    }
    /// @brief Replace the whole scripted kernel log.
    /// @param lines Kernel log lines to script.
    void set_kernel_log(const std::vector<std::string>& lines)
    {
        kernel_log_ = lines;
    }
    /// @brief Append one kernel log line (emits it to followers too).
    /// @param line Line to append.
    void append_kernel_line(const std::string& line);

    /// @brief Network plans applied (or recorded) so far — test observation.
    /// @return Network plans applied (or recorded) so far.
    const std::vector<std::string>& applied_plans() const
    {
        return applied_plans_;
    }
    /// @brief Watchdog pets counted so far.
    uint64_t watchdog_pets() const
    {
        return watchdog_pets_;
    }
    /// @brief True between watchdog_set_timeout_s() and disarm.
    bool watchdog_running() const
    {
        return watchdog_running_;
    }
    /// @brief Current LED states.
    /// @return Current LED states.
    const std::map<std::string, bool>& led_state() const
    {
        return leds_;
    }
    /// @brief Current GPIO states.
    /// @return Current GPIO states.
    const std::map<int, bool>& gpio_state() const
    {
        return gpios_;
    }
    /// @brief Units started so far.
    /// @return Units started so far.
    const std::vector<std::string>& started_units() const
    {
        return started_units_;
    }
    /// @brief Current unit states.
    /// @return Current unit states.
    const std::map<std::string, bool>& unit_states() const
    {
        return units_;
    }
    /// @brief USB ports reset so far.
    /// @return USB ports reset so far.
    const std::vector<std::string>& usb_resets() const
    {
        return usb_resets_;
    }
    /// @brief Display commands received so far.
    /// @return Display commands received so far.
    const std::vector<std::string>& display_commands() const
    {
        return display_cmds_;
    }
    /// @brief Units restarted so far.
    /// @return Units restarted so far.
    const std::vector<std::string>& unit_restarts() const
    {
        return restarts_;
    }

/// @brief Factory identity for first-boot key generation (REQ §6.2).
/// @return The factory identity, or an error when the board carries none.
    Result<FactoryIdentity> read_factory_identity() override;
/// @brief True when the CSPRNG is seeded and safe for key generation.
    /// @return True when the CSPRNG is seeded.
    bool entropy_available() override;
/// @brief All storage devices with wear/health data.
/// @return Storage devices with wear/health data.
    std::vector<StorageDevice> storage_devices() override;
/// @brief Partition topology including mount points and usage (§5).
/// @return Partition topology.
    std::vector<Partition> partitions() override;
/// @brief Memory pressure snapshot.
    /// @return The memory snapshot.
    MemInfo memory_info() override;
/// @brief CPU utilisation and frequency snapshot.
    /// @return The CPU snapshot.
    CpuSnapshot cpu_snapshot() override;
/// @brief One-minute load average.
    /// @return One-minute load average.
    double load_average_1m() override;
/// @brief Total process count.
    /// @return Total process count.
    uint32_t process_count() override;
/// @brief Uptime in seconds.
    /// @return Uptime in seconds.
    uint64_t uptime_s() override;
/// @brief Number of boots recorded.
    /// @return Recorded boot count.
    uint32_t boot_count() override;
/// @brief True when the previous shutdown was clean.
    /// @return True when the previous shutdown was clean.
    bool last_shutdown_clean() override;
/// @brief Thermal zone readings.
/// @return Thermal zone readings.
    std::vector<ThermalZone> thermal_zones() override;
/// @brief Power source readings.
/// @return Power source readings.
    std::vector<PowerSource> power_sources() override;
/// @brief Configure the hardware watchdog timeout (0 disarms where supported).
/// @param seconds Watchdog timeout in seconds (0 disarms where supported).
    /// @return ok(), or the device error.
    Error watchdog_set_timeout_s(int seconds) override;
/// @brief Pet the hardware watchdog.
    /// @return ok(), or the device error.
    Error watchdog_pet() override;
/// @brief True when a time source is currently disciplined.
    /// @return True when a source is disciplined.
    bool time_locked() override;
/// @brief Which time sources this board has, and their state (§5, §7.1).
/// @return Time-source inventory with state.
    std::vector<TimeSourceInfo> time_sources() override;
/// @brief Enumerate network interfaces.
/// @return Network interface states.
    std::vector<IfaceInfo> interfaces() override;
/// @brief Current A/B boot-slot state.
/// @return The boot-slot state, or an error.
    Result<BootSlotState> boot_slots() override;
/// @brief Select a boot slot; @p trial marks it pending confirmation.
/// @param slot Slot to select ("A"/"B").
/// @param trial Mark the slot pending confirmation when true.
    /// @return ok(), or the error encountered.
    Error set_boot_slot(const std::string& slot, bool trial) override;
/// @brief Confirm the currently pending slot as good.
    /// @return ok(), or the error encountered.
    Error confirm_boot_slot() override;
/// @brief Drive a GPIO line (Hardware daemon only, R7).
/// @param line GPIO line number.
/// @param value Value to drive.
    /// @return ok(), or the error encountered.
    Error gpio_set(int line, bool value) override;
/// @brief Read a GPIO line.
/// @param line GPIO line number.
/// @return The line state, or an error.
    Result<bool> gpio_get(int line) override;
/// @brief Set an LED steady on/off.
/// @param name LED name.
/// @param on True to light the LED.
    /// @return ok(), or the error encountered.
    Error led_set(const std::string& name, bool on) override;
/// @brief Blink an LED with the given duty cycle.
/// @param name LED name.
/// @param on_ms On duration in ms.
/// @param off_ms Off duration in ms.
/// @param repeats Number of blinks.
    /// @return ok(), or the error encountered.
    Error led_blink(const std::string& name, int on_ms, int off_ms, int repeats) override;
/// @brief Power-cycle a USB port.
/// @param port USB port to power-cycle.
    /// @return ok(), or the error encountered.
    Error usb_reset(const std::string& port) override;
/// @brief Run a CAN loopback health check on @p iface.
/// @param iface CAN interface to test.
/// @return True when the loopback passes, or an error.
    Result<bool> can_loopback_test(const std::string& iface) override;
/// @brief Send one CAN frame on @p iface.
/// @param iface CAN interface.
/// @param frame Frame to send.
    /// @return ok(), or the error encountered.
    Error can_send(const std::string& iface, const CanFrame& frame) override;
/// @brief Drain up to @p max_frames frames buffered since the previous call.
/// @param iface CAN interface.
/// @param max_frames Maximum frames returned.
/// @return Buffered frames, or an error.
    Result<std::vector<CanFrame>> can_recv(const std::string& iface, size_t max_frames) override;
/// @brief Send a control command to a display.
/// @param cmd Display command.
/// @param arg Command argument.
    /// @return ok(), or the error encountered.
    Error display_control(const std::string& cmd, const std::string& arg) override;
/// @brief Probe display presence/capability.
/// @return Probe result text, or an error.
    Result<std::string> display_probe() override;
/// @brief Start a systemd unit (istate requests, REQ §7.1).
/// @param unit systemd unit to start.
    /// @return ok(), or the error encountered.
    Error unit_start(const std::string& unit) override;
/// @brief Stop a systemd unit.
/// @param unit systemd unit to stop.
    /// @return ok(), or the error encountered.
    Error unit_stop(const std::string& unit) override;
/// @brief Restart a systemd unit.
/// @param unit systemd unit to restart.
    /// @return ok(), or the error encountered.
    Error unit_restart(const std::string& unit) override;
/// @brief Query whether a unit is active.
/// @param unit systemd unit to query.
/// @return True when active, or an error.
    Result<bool> unit_active(const std::string& unit) override;
    /// @brief Kernel log source.
    /// @param from_start True to read from the beginning; false to follow with new lines only.
    /// @return The scripted kernel log lines.
    Result<std::vector<std::string>> kernel_log_lines(bool from_start) override;
/// @brief Application log sink / journal access for LogManager.
/// @param unit Unit whose journal to read.
/// @param max_lines Maximum lines returned.
/// @return Log lines, or an error.
    Result<std::vector<std::string>> journal_lines(const std::string& unit, size_t max_lines) override;
/// @brief Render/apply a network plan (interfaces, firewall, ssh windows, wg, time).
///
/// The simulated platform records the plan; native shells out only when
/// INSTR_APPLY_NETWORK=1 (never silently mutates a host).
/// @param plan_json Rendered network plan (interfaces, firewall, ssh windows, wg, time).
    /// @return ok(), or the error encountered.
    Error apply_network_plan(const std::string& plan_json) override;
/// @brief True when HQ is currently reachable.
    /// @return True when HQ is currently reachable.
    bool hq_reachable() override;
/// @brief Measured-boot attestation state (§10).
    /// @return Measured-boot attestation state (§10).
    MeasuredBootState measured_boot() override;
/// @brief Directory coredumps are written to.
/// @return The coredump directory path.
    std::string coredump_dir() override
    {
        return "/var/lib/instrumentation/coredumps";
    }
/// @brief Human-readable platform name ("native" / "simulated" / board name).
/// @return The platform name ("native" / "simulated" / board name).
    std::string platform_name() override
    {
        return "simulated";
    }
    /// @}

    /// @brief Script per-unit journal contents for LogManager tests.
    /// @param j Per-unit journal contents to script.
    void set_journal(const std::map<std::string, std::vector<std::string>>& j)
    {
        journal_ = j;
    }
    /// @brief Fault-injection / scenario knobs for the newer platform surfaces.
    /// @{
    /// @brief Script the partition topology.
    /// @param p Partition topology to script.
    void set_partitions(std::vector<Partition> p)
    {
        partitions_ = std::move(p);
    }
    /// @brief Script the time-source inventory.
    /// @param t Time-source inventory to script.
    void set_time_sources(std::vector<TimeSourceInfo> t)
    {
        time_sources_ = std::move(t);
    }
    /// @brief Script the measured-boot state.
    /// @param m Measured-boot state to script.
    void set_measured_boot(MeasuredBootState m)
    {
        measured_boot_ = std::move(m);
    }
    /// @brief Queue a frame a can_recv on this interface will hand back
    /// (loopback by default: whatever was sent comes back, which is what a
    /// health check expects).
    /// @param iface Interface to queue the frame on.
    /// @param f Frame to hand to can_recv.
    void push_can_frame(const std::string& iface, const CanFrame& f)
    {
        can_rx_[iface].push_back(f);
    }
    /// @brief Frames sent on this interface so far (test observation).
    /// @param iface Interface to inspect.
    /// @return Frames sent on this interface so far.
    const std::vector<CanFrame>& can_sent(const std::string& iface)
    {
        return can_tx_[iface];
    }
    /// @brief Script CAN controller presence for an interface.
    /// @param iface CAN interface.
    /// @param present Controller presence to script.
    void set_can_present(const std::string& iface, bool present)
    {
        can_present_[iface] = present;
    }
    /// @}

  private:
    std::mutex mu_; ///< Serialises scripted state across threads.
    // Follow cursor: index of the first kernel line not yet handed out in
    // follow mode (see kernel_log_lines).
    size_t follow_cursor_ = 0; ///< Kernel-log follow position.
    std::vector<Partition> partitions_; ///< Scripted partition topology.
    std::vector<TimeSourceInfo> time_sources_; ///< Scripted time sources.
    MeasuredBootState measured_boot_; ///< Scripted attestation state.
    std::map<std::string, std::vector<CanFrame>> can_rx_; ///< Queued receive frames per interface.
    std::map<std::string, std::vector<CanFrame>> can_tx_; ///< Recorded sent frames per interface.
    std::map<std::string, bool> can_present_; ///< CAN controller presence per interface.
    FactoryIdentity identity_; ///< Scripted factory identity.
    bool entropy_ = true; ///< Scripted entropy availability.
    std::vector<StorageDevice> storage_; ///< Scripted storage devices.
    std::vector<ThermalZone> thermals_; ///< Scripted thermal zones.
    std::vector<PowerSource> power_; ///< Scripted power sources.
    std::vector<IfaceInfo> ifaces_; ///< Scripted interfaces.
    MemInfo mem_; ///< Scripted memory snapshot.
    CpuSnapshot cpu_; ///< Scripted CPU snapshot.
    double load_ = 0.5; ///< Scripted load average.
    uint32_t processes_ = 80; ///< Scripted process count.
    uint64_t uptime_ = 0; ///< Scripted uptime.
    uint32_t boot_count_ = 1; ///< Scripted boot count.
    bool clean_shutdown_ = true; ///< Scripted last-shutdown state.
    bool time_locked_ = false; ///< Scripted time-lock state.
    bool hq_reachable_ = false; ///< Scripted HQ reachability.
    bool fail_watchdog_ = false, fail_gpio_ = false, fail_usb_ = false; ///< Fault-injection flags.
    bool fail_can_ = false, fail_display_ = false, fail_unit_start_ = false; ///< Fault-injection flags.
    bool fail_boot_slots_ = false; ///< Fault-injection flag for boot-slot ops.
    bool watchdog_running_ = false; ///< Simulated watchdog armed state.
    uint64_t watchdog_pets_ = 0; ///< Pets counted.
    std::map<std::string, bool> leds_; ///< LED states.
    std::map<int, bool> gpios_; ///< GPIO states.
    std::map<std::string, bool> units_; ///< Unit states.
    std::vector<std::string> started_units_; ///< Units started so far.
    std::vector<std::string> usb_resets_; ///< USB resets so far.
    std::vector<std::string> display_cmds_; ///< Display commands so far.
    std::vector<std::string> restarts_; ///< Unit restarts so far.
    std::vector<std::string> kernel_log_; ///< Scripted kernel log.
    std::map<std::string, std::vector<std::string>> journal_; ///< Scripted per-unit journal.
    std::vector<std::string> applied_plans_; ///< Recorded network plans.
    BootSlotState slots_; ///< Simulated A/B slot state.
};

} // namespace platform
} // namespace instr