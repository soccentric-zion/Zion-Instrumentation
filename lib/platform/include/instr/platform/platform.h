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
 * @file lib/platform/include/instr/platform/platform.h
 * @brief platform abstraction layer (REQ §5, R9)
 *
 * The only place target variation exists. One interface; implementations:
 * SimulatedPlatform — complete simulation for hosts/CI/fault injection.
 * NativePlatform    — standard Linux sysfs/procfs paths shared by the
 * nine targets; board specifics come from descriptors.
 * No #ifdef for a target anywhere in daemon code: the same binaries ship on
 * every target.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <functional>
#include <string>
#include <vector>

#include "instr/common/error.h"

namespace instr
{
/// @brief Platform abstraction: board and hardware access behind IPlatform (REQ §5).
namespace platform
{

/// @brief Factory-provisioned identity (first-boot key generation, REQ §6.2).
struct FactoryIdentity
{
    std::string serial; ///< Device serial number.
    std::string hw_revision; ///< Hardware revision string.
    std::string board_variant; ///< Board variant identifier.
    std::string mfg_date; ///< Manufacture date.
    std::string hostname_base; ///< Hostname derived on first boot.
    std::string identity_secret; ///< Factory-provisioned HMAC key (never logged).
    bool valid = false; ///< False when the board carries no identity.
};

/// @brief A storage device with wear/health indicators (§5).
struct StorageDevice
{
    std::string name; ///< Kernel name, e.g. "mmcblk0".
    std::string device_path; ///< /dev path.
    std::string kind; ///< emmc / sd / nvme / usb.
    uint64_t bytes = 0; ///< Capacity in bytes.
    double wear_pct = -1; ///< Wear estimate; -1 when unknown.
    uint32_t lifetime_days_estimate = 0; ///< Remaining-life estimate; 0 when unknown.
    // SMART / NVMe health, where the device exposes it (§5 wear indicators).
    int32_t temperature_c = -1; ///< Reported temperature; -1 when unknown.
    uint64_t power_on_hours = 0; ///< Lifetime power-on hours.
    uint64_t reallocated_sectors = 0; ///< SMART reallocated-sector count.
    uint64_t media_errors = 0; ///< NVMe media-error count.
    bool smart_ok = true; ///< False once the device reports failing.
    bool smart_available = false; ///< True when health data is exposed.
};

/// @brief Partition topology (§5): which partitions exist, where they mount, how full they are.
///
/// The budgets in §6 are meaningless without knowing which partition /pst and
/// the spool actually live on.
struct Partition
{
    std::string name; ///< Partition name, e.g. mmcblk0p3.
    std::string device_path; ///< /dev path.
    std::string parent; ///< Parent block device.
    std::string mount_point; ///< Mount point; empty when not mounted.
    std::string fs_type; ///< Filesystem type.
    uint64_t bytes = 0; ///< Partition size in bytes.
    uint64_t used_bytes = 0; ///< Bytes in use.
    bool read_only = false; ///< True when mounted read-only.
};

/// @brief Time sources this board actually has (§5, §7.1).
///
/// istate reports the source it is using; without this it cannot tell "no
/// GNSS lock yet" from "this board has no GNSS receiver".
struct TimeSourceInfo
{
    std::string kind; ///< gnss / ptp / ntp / rtc.
    bool present = false; ///< Hardware/daemon exists on this board.
    bool locked = false; ///< Currently disciplined.
    double offset_ms = 0; ///< Reported offset from true time.
    double quality = 0; ///< 0..1, source-specific confidence.
};

/// @brief Measured-boot attestation state (§10); absent support is reported honestly, never faked.
struct MeasuredBootState
{
    bool available = false; ///< True when a mechanism exists on this board.
    std::string mechanism; ///< tpm2 / optee / none.
    std::map<uint32_t, std::string> pcrs; ///< PCR index → hex digest.
    bool secure_boot_enabled = false; ///< Secure-boot state.
};

/// @brief One thermal zone reading.
struct ThermalZone
{
    std::string id; ///< Zone identifier.
    std::string label; ///< Human-readable label.
    double temp_c = 0; ///< Temperature in Celsius.
};

/// @brief One power source reading.
struct PowerSource
{
    std::string id; ///< Source identifier.
    std::string kind; ///< ac / battery.
    double percent = 0; ///< Charge percentage.
    bool charging = false; ///< True when actively charging.
};

/// @brief One network interface's state.
struct IfaceInfo
{
    std::string name; ///< Interface name.
    bool up = false; ///< Administratively up.
    bool link = false; ///< Carrier present.
    std::string mac; ///< MAC address.
    std::vector<std::string> addrs; ///< Assigned addresses.
};

/// @brief A/B boot-slot state (rollback and golden boot, REQ §5).
struct BootSlotState
{
    std::string active; ///< "A" / "B".
    std::string inactive; ///< The other slot.
    std::string pending; ///< Staged slot awaiting confirmation; empty if none.
    std::string golden; ///< Fallback slot.
    uint32_t boot_attempts = 0; ///< Attempts on the pending slot.
};

/// @brief Memory pressure snapshot.
struct MemInfo
{
    uint64_t total_bytes = 0; ///< Physical RAM.
    uint64_t available_bytes = 0; ///< Usable without swapping.
    uint64_t swap_total_bytes = 0; ///< Swap configured.
    uint64_t swap_free_bytes = 0; ///< Swap unused.
};

/// @brief CPU utilisation and frequency snapshot.
struct CpuSnapshot
{
    uint32_t cores = 1; ///< Logical core count.
    std::vector<uint64_t> busy_ticks; ///< Busy ticks per core.
    std::vector<uint64_t> total_ticks; ///< Total ticks per core.
    std::vector<double> freq_mhz; ///< Current frequency per core.
    std::string governor; ///< Scaling governor name.
};

/// @brief Platform abstraction: everything the daemons need from the board/hardware.
class IPlatform
{
  public:
    virtual ~IPlatform() = default;

    /// @brief Factory identity for first-boot key generation (REQ §6.2).
    /// @return The factory identity, or an error when the board carries none.
    virtual Result<FactoryIdentity> read_factory_identity() = 0;
    /// @brief True when the CSPRNG is seeded and safe for key generation.
    /// @return True when the CSPRNG is seeded.
    virtual bool entropy_available() = 0;

    /// @brief All storage devices with wear/health data.
    /// @return Storage devices with wear/health data.
    virtual std::vector<StorageDevice> storage_devices() = 0;
    /// @brief Partition topology including mount points and usage (§5).
    /// @return Partition topology.
    virtual std::vector<Partition> partitions() = 0;
    /// @brief Memory pressure snapshot.
    /// @return The memory snapshot.
    virtual MemInfo memory_info() = 0;
    /// @brief CPU utilisation and frequency snapshot.
    /// @return The CPU snapshot.
    virtual CpuSnapshot cpu_snapshot() = 0;
    /// @brief One-minute load average.
    /// @return One-minute load average.
    virtual double load_average_1m() = 0;
    /// @brief Total process count.
    /// @return Total process count.
    virtual uint32_t process_count() = 0;
    /// @brief Uptime in seconds.
    /// @return Uptime in seconds.
    virtual uint64_t uptime_s() = 0;
    /// @brief Number of boots recorded.
    /// @return Recorded boot count.
    virtual uint32_t boot_count() = 0;
    /// @brief True when the previous shutdown was clean.
    /// @return True when the previous shutdown was clean.
    virtual bool last_shutdown_clean() = 0;

    /// @brief Thermal zone readings.
    /// @return Thermal zone readings.
    virtual std::vector<ThermalZone> thermal_zones() = 0;
    /// @brief Power source readings.
    /// @return Power source readings.
    virtual std::vector<PowerSource> power_sources() = 0;

    /// @brief Configure the hardware watchdog timeout (0 disarms where supported).
    /// @param seconds Watchdog timeout in seconds (0 disarms where supported).
    /// @return ok(), or the device error.
    virtual Error watchdog_set_timeout_s(int seconds) = 0;
    /// @brief Pet the hardware watchdog.
    /// @return ok(), or the device error.
    virtual Error watchdog_pet() = 0;

    /// @brief True when a time source is currently disciplined.
    /// @return True when a source is disciplined.
    virtual bool time_locked() = 0;
    /// @brief Which time sources this board has, and their state (§5, §7.1).
    /// @return Time-source inventory with state.
    virtual std::vector<TimeSourceInfo> time_sources() = 0;

    /// @brief Enumerate network interfaces.
    /// @return Network interface states.
    virtual std::vector<IfaceInfo> interfaces() = 0;

    /// @brief Current A/B boot-slot state.
    /// @return The boot-slot state, or an error.
    virtual Result<BootSlotState> boot_slots() = 0;
    /// @brief Select a boot slot; @p trial marks it pending confirmation.
    /// @param slot Slot to select ("A"/"B").
    /// @param trial Mark the slot pending confirmation when true.
    /// @return ok(), or the error encountered.
    virtual Error set_boot_slot(const std::string& slot, bool trial) = 0;
    /// @brief Confirm the currently pending slot as good.
    /// @return ok(), or the error encountered.
    virtual Error confirm_boot_slot() = 0;

    /// @brief Drive a GPIO line (Hardware daemon only, R7).
    /// @param line GPIO line number.
    /// @param value Value to drive.
    /// @return ok(), or the error encountered.
    virtual Error gpio_set(int line, bool value) = 0;
    /// @brief Read a GPIO line.
    /// @param line GPIO line number.
    /// @return The line state, or an error.
    virtual Result<bool> gpio_get(int line) = 0;
    /// @brief Set an LED steady on/off.
    /// @param name LED name.
    /// @param on True to light the LED.
    /// @return ok(), or the error encountered.
    virtual Error led_set(const std::string& name, bool on) = 0;
    /// @brief Blink an LED with the given duty cycle.
    /// @param name LED name.
    /// @param on_ms On duration in ms.
    /// @param off_ms Off duration in ms.
    /// @param repeats Number of blinks.
    /// @return ok(), or the error encountered.
    virtual Error led_blink(const std::string& name, int on_ms, int off_ms, int repeats) = 0;
    /// @brief Power-cycle a USB port.
    /// @param port USB port to power-cycle.
    /// @return ok(), or the error encountered.
    virtual Error usb_reset(const std::string& port) = 0;
    /// @brief Run a CAN loopback health check on @p iface.
    /// @param iface CAN interface to test.
    /// @return True when the loopback passes, or an error.
    virtual Result<bool> can_loopback_test(const std::string& iface) = 0;
    // CAN send/receive primitives (§5). send returns Unavailable where the
    // board has no CAN controller; recv returns the frames buffered since the
    // previous call, so a health check can prove the bus actually carries data.
    struct CanFrame
    {
        uint32_t id = 0; ///< CAN identifier.
        bool extended = false; ///< True for 29-bit extended IDs.
        std::vector<uint8_t> data; ///< Payload bytes (up to 8).
    };
    /// @brief Send one CAN frame on @p iface.
    /// @param iface CAN interface.
    /// @param frame Frame to send.
    /// @return ok(), or the error encountered.
    virtual Error can_send(const std::string& iface, const CanFrame& frame) = 0;
    /// @brief Drain up to @p max_frames frames buffered since the previous call.
    /// @param iface CAN interface.
    /// @param max_frames Maximum frames returned.
    /// @return Buffered frames, or an error.
    virtual Result<std::vector<CanFrame>> can_recv(const std::string& iface, size_t max_frames) = 0;
    /// @brief Send a control command to a display.
    /// @param cmd Display command.
    /// @param arg Command argument.
    /// @return ok(), or the error encountered.
    virtual Error display_control(const std::string& cmd, const std::string& arg) = 0;
    /// @brief Probe display presence/capability.
    /// @return Probe result text, or an error.
    virtual Result<std::string> display_probe() = 0;

    /// @brief Start a systemd unit (istate requests, REQ §7.1).
    /// @param unit systemd unit to start.
    /// @return ok(), or the error encountered.
    virtual Error unit_start(const std::string& unit) = 0;
    /// @brief Stop a systemd unit.
    /// @param unit systemd unit to stop.
    /// @return ok(), or the error encountered.
    virtual Error unit_stop(const std::string& unit) = 0;
    /// @brief Restart a systemd unit.
    /// @param unit systemd unit to restart.
    /// @return ok(), or the error encountered.
    virtual Error unit_restart(const std::string& unit) = 0;
    /// @brief Query whether a unit is active.
    /// @param unit systemd unit to query.
    /// @return True when active, or an error.
    virtual Result<bool> unit_active(const std::string& unit) = 0;

    /// @brief Kernel log source (Faults daemon, R6).
    ///
    /// from_start = true:  read the full ring from the beginning (boot backfill)
    //                      and position the follow cursor at the end.
    // from_start = false: follow — return only lines not yet returned, so a
    //                      poller never re-reports a fault it already saw.
/// @param from_start True to read the full ring from the beginning and position the follow cursor at the end.
/// @return Kernel log lines, or an error.
    virtual Result<std::vector<std::string>> kernel_log_lines(bool from_start) = 0;
    /// @brief Application log sink / journal access for LogManager.
    /// @param unit Unit whose journal to read.
    /// @param max_lines Maximum lines returned.
    /// @return Log lines, or an error.
    virtual Result<std::vector<std::string>> journal_lines(const std::string& unit, size_t max_lines) = 0;

    /// @brief Render/apply a network plan (interfaces, firewall, ssh windows, wg, time).
    ///
    /// The simulated platform records the plan; native shells out only when
    /// INSTR_APPLY_NETWORK=1 (never silently mutates a host).
    /// @param plan_json Rendered network plan (interfaces, firewall, ssh windows, wg, time).
    /// @return ok(), or the error encountered.
    virtual Error apply_network_plan(const std::string& plan_json) = 0;
    /// @brief True when HQ is currently reachable.
    /// @return True when HQ is currently reachable.
    virtual bool hq_reachable() = 0;

    /// @brief Measured-boot attestation state (§10).
    /// @return Measured-boot attestation state (§10).
    virtual MeasuredBootState measured_boot() = 0;

    /// @brief Directory coredumps are written to.
    /// @return The coredump directory path.
    virtual std::string coredump_dir() = 0;
    /// @brief Human-readable platform name ("native" / "simulated" / board name).
    /// @return The platform name ("native" / "simulated" / board name).
    virtual std::string platform_name() = 0;
};

} // namespace platform
} // namespace instr