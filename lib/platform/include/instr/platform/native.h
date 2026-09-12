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
 * @file lib/platform/include/instr/platform/native.h
 * @brief native platform implementation
 *
 * Reads standard Linux interfaces (sysfs/procfs, /dev/watchdog, boot
 * counters) that are common across the nine targets; board-specific device
 * topology comes from descriptors, never from code (R9). Network/systemd
 * mutations are dry-run recorded unless INSTR_APPLY_NETWORK=1 so the same
 * binary is safe in CI and on a laptop.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <mutex>
#include <string>

#include "instr/common/json.h"
#include "instr/platform/platform.h"

namespace instr
{
/// @brief Platform abstraction: board and hardware access behind IPlatform (REQ §5).
namespace platform
{

/// @brief Mount point of the boot partition, where the A/B slot flag files live.
///
/// Arches-OS mounts the boot partition at `/boot` on all three wic layouts
/// (`meta/meta-arches/wic/*.wks`, `part /boot`). This used to be hardcoded to
/// `/mnt/boot`, which exists on no Arches image -- so on a board without a
/// U-Boot environment the flag-file fallback wrote slot state into a
/// non-existent directory and every write failed. instr-ota.service also named
/// `/mnt/boot` in ReadWritePaths under ProtectSystem=strict, which makes
/// systemd refuse to start the unit outright. See Task.md D-05.
///
/// Overridable via INSTR_BOOT_DIR so a board whose boot partition lands
/// elsewhere, and the test suite, can point it somewhere else without a rebuild.
///
/// @return The boot mount point, without a trailing slash.
std::string boot_dir();

/// @brief Name of the nftables table Instrumentation owns, and the only one it
///        is ever permitted to create or destroy.
///
/// Instrumentation shares the kernel's ruleset with Arches-Configuration, which
/// writes its own `webconfig` table. Neither daemon knows the other exists, so
/// scoping is the entire safety property here: a global `flush ruleset` would
/// drop the peer's table along with our own, and on a device where the firewall
/// is what protects the management port that is a security failure, not merely
/// a functional one. See Task.md B-02.
inline constexpr const char* kInstrTable = "instr";
/// @brief Name of the NAT table Instrumentation owns (port forwarding).
inline constexpr const char* kInstrNatTable = "instr_nat";

/// @brief Render the instrumentation nftables filter ruleset for a firewall plan.
///
/// The rendered text is idempotent and scoped: it declares the table empty
/// (creating it when absent, so the delete below cannot fail with ENOENT),
/// deletes exactly that table, then rebuilds it. No other table in the kernel
/// is touched.
///
/// @param firewall The plan's `firewall` array.
/// @return nftables script text suitable for `nft -f`.
std::string render_firewall_ruleset(const JValue& firewall);

/// @brief Render the instrumentation NAT ruleset for a port-forwarding plan.
///
/// Scoped and idempotent on the same terms as render_firewall_ruleset; without
/// the delete, re-applying a plan appends duplicate DNAT rules rather than
/// replacing them.
///
/// @param forwards The plan's `port_forwarding` array.
/// @return nftables script text, or an empty string when there is nothing to do.
std::string render_nat_ruleset(const JValue& forwards);

/// @brief Native (real-board) platform implementation reading /proc, /sys, SMART and U-Boot env.
class NativePlatform : public IPlatform
{
  public:
    /// @brief Tear down open descriptors (watchdog, kmsg follow).
    /// @return Tear down open descriptors (watchdog, kmsg follow).
    ~NativePlatform() override;
    /// @brief Construct the platform rooted at @p sysroot ("/" on a device).
    explicit NativePlatform(std::string sysroot = "/");

    /// @brief Test seam: redirect file reads to a fixture directory.
    /// @param root Directory to read files under.
    void set_sysroot(const std::string& root)
    {
        sysroot_ = root;
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
    /// @copydoc IPlatform::kernel_log_lines
    /// @param from_start True for boot backfill; false to follow with new lines only.
    /// @return Kernel log lines, or an error.
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
    /// @brief Write the rendered supplicant / hostapd configuration for a plan.
    ///
    /// The PSK is resolved by networkd from /pst immediately before this call.
    ///
    /// @param mode "sta" or "ap".
    /// @param iface Interface the configuration applies to.
    /// @param text Rendered configuration text.
    /// @return ok(), or the error encountered.
    Error write_wireless_conf(const std::string& mode, const std::string& iface, const std::string& text);
    /// @brief What the last apply_network_plan did (or would have done on a host).
    /// @return Log of executed or intended network commands.
    const std::vector<std::string>& applied_commands() const
    {
        return applied_commands_;
    }
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
        return "native";
    }

  private:
    // Kernel-log follow state: one long-lived /dev/kmsg descriptor so that
    // polling returns only new records (see kernel_log_lines).
    /// @brief Open the long-lived /dev/kmsg follow descriptor.
    /// @return True when the condition holds; false otherwise.
    bool open_follow_fd();
    /// @brief Read up to @p max_lines kernel messages from @p fd into @p out.
    /// @param fd Descriptor to read from.
    /// @param out Receives the lines.
    /// @param max_lines Maximum lines to read.
    static void read_kmsg_into(int fd, std::vector<std::string>* out, int max_lines);
    std::mutex kmsg_mu_; ///< Serialises /dev/kmsg follow access.
    int kmsg_follow_fd_ = -1; ///< Long-lived /dev/kmsg descriptor (-1 closed).

    // Bootloader environment access (U-Boot fw_printenv/fw_setenv), with a
    // flag-file fallback for boards that have no U-Boot environment.
    /// @brief Read a U-Boot environment variable.
    /// @param var U-Boot variable to read.
    /// @return The variable value, or an error.
    Result<std::string> fw_getenv(const std::string& var);
    /// @brief Write a U-Boot environment variable.
    /// @param var U-Boot variable to write.
    /// @param value Value to write.
    /// @return ok(), or the error encountered.
    Error fw_setenv(const std::string& var, const std::string& value);
    /// @brief Probe (once) whether the board has a U-Boot environment.
    /// @return True when the condition holds; false otherwise.
    bool have_fw_env();
    /// @brief Flag-file fallback for boards without U-Boot env.
    /// @param rel Flag-file relative path.
    /// @param value Value to record.
    /// @return ok(), or the error encountered.
    Error write_boot_file(const std::string& rel, const std::string& value);
    bool fw_env_checked_ = false; ///< True after the first U-Boot env probe.
    bool fw_env_present_ = false; ///< Cached U-Boot env availability.

    // Network application helpers. Every mutation goes through run_cmd /
    // write_conf, which record what they would do and only execute when
    // INSTR_APPLY_NETWORK=1 — so a build host is never reconfigured.
    /// @brief Record (and conditionally execute) a network command.
    /// @param cmd Command to record (and conditionally execute).
    /// @return ok(), or the error encountered.
    Error run_cmd(const std::string& cmd);
    /// @brief Record (and conditionally execute) a config-file write.
    /// @param path Config file to write.
    /// @param text Configuration text.
    /// @return ok(), or the error encountered.
    Error write_conf(const std::string& path, const std::string& text);

    /// @brief Enrich a device with SMART / NVMe health where it is exposed.
    /// @param d Device to enrich with SMART/NVMe health.
    void read_smart(StorageDevice* d);

    /// @brief Read a file under the sysroot.
    /// @param path File to read under the sysroot.
    /// @return File contents, or an error.
    Result<std::string> read_file(const std::string& path);
    /// @brief List a directory under the sysroot.
    /// @param path Directory to list under the sysroot.
    /// @return Entry names.
    std::vector<std::string> list_dir(const std::string& path);
    /// @brief True when INSTR_APPLY_NETWORK=1 (mutations allowed).
    /// @return True when INSTR_APPLY_NETWORK=1 (mutations allowed).
    bool apply_enabled() const;

    std::vector<std::string> applied_commands_; ///< Log of executed/intended network commands.
    std::string sysroot_; ///< File-access root ("/" on device).
    int watchdog_fd_ = -1; ///< Open watchdog device descriptor.
    bool watchdog_opened_ = false; ///< True once the watchdog has been configured.
};

} // namespace platform
} // namespace instr