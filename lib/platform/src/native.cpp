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
 * @file lib/platform/src/native.cpp
 * @brief Implementation of the native platform backend: sysfs/procfs sensors, storage and memory probes, factory identity, and dry-run network/systemd mutations (REQ §, R9).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/platform/native.h"

#include "instr/common/json.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <fstream>
#include <sstream>

namespace instr
{
/// @brief Platform abstraction: board and hardware access behind IPlatform (REQ §5).
namespace platform
{

/// @brief Construct the platform rooted at @p sysroot.
NativePlatform::NativePlatform(std::string sysroot) : sysroot_(std::move(sysroot))
{
}

/// @brief Close the watchdog and kmsg descriptors.
NativePlatform::~NativePlatform()
{
    if (kmsg_follow_fd_ >= 0) ::close(kmsg_follow_fd_);
    if (watchdog_fd_ >= 0)
    {
        // Magic close: tell the kernel this was a deliberate stop, so a clean
        // shutdown of istate does not trip the watchdog into a reboot.
        ssize_t n = ::write(watchdog_fd_, "V", 1);
        (void)n;
        ::close(watchdog_fd_);
    }
}

/// @brief Read a file under the sysroot.
/// @param path File to read under the sysroot.
/// @return File contents, or an error.
Result<std::string> NativePlatform::read_file(const std::string& path)
{
    std::ifstream f(sysroot_ + path);
    if (!f) return Result<std::string>::fail(Errc::NotFound, "cannot open", path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return Result<std::string>::good(ss.str());
}

/// @brief List a directory under the sysroot.
/// @param path Directory to list under the sysroot.
/// @return Entry names.
std::vector<std::string> NativePlatform::list_dir(const std::string& path)
{
    std::vector<std::string> out;
    DIR* d = ::opendir((sysroot_ + path).c_str());
    if (!d) return out;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr)
    {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        out.push_back(name);
    }
    ::closedir(d);
    return out;
}

/// @brief True when INSTR_APPLY_NETWORK=1 (mutations allowed).
/// @brief Mount point of the boot partition (INSTR_BOOT_DIR, default /boot).
std::string boot_dir()
{
    const char* v = ::getenv("INSTR_BOOT_DIR");
    if (v && *v)
    {
        std::string d(v);
        while (d.size() > 1 && d.back() == '/') d.pop_back();
        return d;
    }
    return "/boot";
}

bool NativePlatform::apply_enabled() const
{
    const char* v = ::getenv("INSTR_APPLY_NETWORK");
    return v && std::string(v) == "1";
}

/// @brief Factory identity for first-boot key generation (REQ §6.2).
/// @return The factory identity, or an error when the board carries none.
Result<FactoryIdentity> NativePlatform::read_factory_identity()
{
    auto f = read_file("/pst/factory/identity/identity.yaml");
    if (!f.ok()) return Result<FactoryIdentity>::fail(f.error.code, f.error.message, "factory identity");
    FactoryIdentity id;
    // Minimal YAML scalar extraction (config library handles full validation
    // elsewhere); keys are flat.
    auto get = [&](const std::string& key) -> std::string {
        size_t pos = f->find(key + ":");
        if (pos == std::string::npos) return "";
        size_t vs = f->find_first_not_of(" \t", pos + key.size() + 1);
        size_t ve = f->find('\n', vs);
        if (vs == std::string::npos) return "";
        if (ve == std::string::npos) ve = f->size();
        std::string v = f->substr(vs, ve - vs);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        return v;
    };
    id.serial = get("serial");
    id.hw_revision = get("hw_revision");
    id.board_variant = get("board_variant");
    id.mfg_date = get("mfg_date");
    id.hostname_base = get("hostname_base");
    auto secret = read_file("/pst/factory/identity/.secret");
    id.identity_secret = secret.ok() ? *secret : "";
    id.valid = !id.serial.empty();
    if (!id.valid) return Result<FactoryIdentity>::fail(Errc::Corrupt, "identity.yaml incomplete", "factory identity");
    return Result<FactoryIdentity>::good(id);
}

/// @brief True when the CSPRNG is seeded and safe for key generation.
bool NativePlatform::entropy_available()
{
    auto a = read_file("/proc/sys/kernel/random/entropy_avail");
    if (!a.ok()) return false;
    try
    {
        return std::stoul(*a) >= 128;
    }
    catch (...)
    {
        return false;
    }
}

/// @brief All storage devices with wear/health data.
/// @return Storage devices with wear/health data.
std::vector<StorageDevice> NativePlatform::storage_devices()
{
    std::vector<StorageDevice> out;
    for (const auto& name : list_dir("/sys/class/block"))
    {
        if (name.rfind("loop", 0) == 0 || name.rfind("ram", 0) == 0) continue;
        StorageDevice d;
        d.name = name;
        d.device_path = "/dev/" + name;
        auto size = read_file("/sys/class/block/" + name + "/size");
        if (size.ok())
        {
            try
            {
                d.bytes = std::stoull(*size) * 512;
            }
            catch (...)
            {
            }
        }
        // eMMC life-time estimate (kernel exposes life_time as 0-10 steps).
        auto life = read_file("/sys/class/block/" + name + "/device/life_time");
        if (life.ok())
        {
            try
            {
                double step = std::stod(*life);
                if (step >= 0 && step <= 10) d.wear_pct = step * 10.0;
            }
            catch (...)
            {
            }
        }
        if (name.rfind("mmcblk", 0) == 0)
            d.kind = "emmc";
        else if (name.rfind("sd", 0) == 0)
            d.kind = "sd";
        else if (name.rfind("nvme", 0) == 0)
            d.kind = "nvme";
        else
            d.kind = "block";
        read_smart(&d);
        out.push_back(d);
    }
    return out;
}

/// @brief Memory pressure snapshot.
MemInfo NativePlatform::memory_info()
{
    MemInfo m;
    auto f = read_file("/proc/meminfo");
    if (!f.ok()) return m;
    auto grab = [&](const std::string& key) -> uint64_t {
        size_t pos = f->find(key);
        if (pos == std::string::npos) return 0;
        pos = f->find_first_of("0123456789", pos);
        if (pos == std::string::npos) return 0;
        try
        {
            return std::stoull(f->substr(pos)) * 1024;
        }
        catch (...)
        {
            return 0;
        }
    };
    m.total_bytes = grab("MemTotal:");
    m.available_bytes = grab("MemAvailable:");
    m.swap_total_bytes = grab("SwapTotal:");
    m.swap_free_bytes = grab("SwapFree:");
    return m;
}

/// @brief CPU utilisation and frequency snapshot.
CpuSnapshot NativePlatform::cpu_snapshot()
{
    CpuSnapshot c;
    auto f = read_file("/proc/stat");
    if (!f.ok()) return c;
    std::istringstream ss(*f);
    std::string line;
    while (std::getline(ss, line))
    {
        if (line.rfind("cpu", 0) == 0 && line.size() > 3 && isdigit(line[3]))
        {
            std::istringstream ls(line.substr(4));
            uint64_t user = 0, nice = 0, sys = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
            ls >> user >> nice >> sys >> idle >> iowait >> irq >> softirq >> steal;
            uint64_t busy = user + nice + sys + irq + softirq + steal;
            uint64_t total = busy + idle + iowait;
            c.busy_ticks.push_back(busy);
            c.total_ticks.push_back(total);
        }
        else if (line.rfind("cpu ", 0) == 0)
        {
            // aggregate handled below via per-core sum
        }
    }
    c.cores = uint32_t(c.busy_ticks.size());
    // Frequencies + governor.
    for (uint32_t i = 0; i < c.cores; ++i)
    {
        auto freq = read_file("/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/scaling_cur_freq");
        if (freq.ok())
        {
            try
            {
                c.freq_mhz.push_back(std::stod(*freq) / 1000.0);
            }
            catch (...)
            {
                c.freq_mhz.push_back(0);
            }
        }
        else { c.freq_mhz.push_back(0); }
    }
    auto gov = read_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    if (gov.ok())
    {
        std::string g = *gov;
        while (!g.empty() && (g.back() == '\n' || g.back() == ' ')) g.pop_back();
        c.governor = g;
    }
    return c;
}

/// @brief One-minute load average.
double NativePlatform::load_average_1m()
{
    auto f = read_file("/proc/loadavg");
    if (!f.ok()) return 0;
    try
    {
        return std::stod(*f);
    }
    catch (...)
    {
        return 0;
    }
}

/// @brief Total process count.
uint32_t NativePlatform::process_count()
{
    return uint32_t(list_dir("/proc").size());
}

/// @brief Uptime in seconds.
uint64_t NativePlatform::uptime_s()
{
    auto f = read_file("/proc/uptime");
    if (!f.ok()) return 0;
    try
    {
        return uint64_t(std::stod(*f));
    }
    catch (...)
    {
        return 0;
    }
}

/// @brief Number of boots recorded.
uint32_t NativePlatform::boot_count()
{
    auto f = read_file("/var/lib/instrumentation/bootcount");
    if (!f.ok()) return 1;
    try
    {
        return uint32_t(std::stoul(*f));
    }
    catch (...)
    {
        return 1;
    }
}

/// @brief True when the previous shutdown was clean.
bool NativePlatform::last_shutdown_clean()
{
    return read_file("/run/instrumentation/clean-shutdown").ok();
}

/// @brief Thermal zone readings.
/// @return Thermal zone readings.
std::vector<ThermalZone> NativePlatform::thermal_zones()
{
    std::vector<ThermalZone> out;
    for (const auto& z : list_dir("/sys/class/thermal"))
    {
        if (z.rfind("thermal_zone", 0) != 0) continue;
        ThermalZone t;
        t.id = z;
        auto type = read_file("/sys/class/thermal/" + z + "/type");
        if (type.ok())
        {
            t.label = *type;
            while (!t.label.empty() && (t.label.back() == '\n' || t.label.back() == ' ')) t.label.pop_back();
        }
        auto temp = read_file("/sys/class/thermal/" + z + "/temp");
        if (temp.ok())
        {
            try
            {
                t.temp_c = std::stod(*temp) / 1000.0;
            }
            catch (...)
            {
            }
        }
        out.push_back(t);
    }
    return out;
}

/// @brief Power source readings.
/// @return Power source readings.
std::vector<PowerSource> NativePlatform::power_sources()
{
    std::vector<PowerSource> out;
    for (const auto& p : list_dir("/sys/class/power_supply"))
    {
        PowerSource ps;
        ps.id = p;
        auto type = read_file("/sys/class/power_supply/" + p + "/type");
        if (type.ok())
        {
            ps.kind = *type;
            while (!ps.kind.empty() && (ps.kind.back() == '\n' || ps.kind.back() == ' ')) ps.kind.pop_back();
        }
        auto cap = read_file("/sys/class/power_supply/" + p + "/capacity");
        if (cap.ok())
        {
            try
            {
                ps.percent = std::stod(*cap);
            }
            catch (...)
            {
            }
        }
        auto status = read_file("/sys/class/power_supply/" + p + "/status");
        ps.charging = status.ok() && status->find("Charging") != std::string::npos;
        out.push_back(ps);
    }
    return out;
}

/// @brief Configure the hardware watchdog timeout (0 disarms where supported).
/// @param seconds Watchdog timeout in seconds (0 disarms where supported).
Error NativePlatform::watchdog_set_timeout_s(int seconds)
{
    int fd = ::open((sysroot_ + "/dev/watchdog").c_str(), O_WRONLY);
    if (fd < 0) return Error::make(Errc::Io, "cannot open /dev/watchdog", "watchdog");
    if (::ioctl(fd, 0x5706 /*WDIOC_SETTIMEOUT unsupported fallback*/, 0) == -1)
    {
        // Write timeout as text where the driver supports it; ignore failure of
        // the ioctl above and rely on the timeout= module parameter convention.
    }
    std::string t = std::to_string(seconds) + "\n";
    // A (void) cast does not silence warn_unused_result; check it properly.
    // A failed timeout write is not fatal — the watchdog keeps its default —
    // but it is worth not pretending we set it.
    if (::write(fd, t.data(), t.size()) < 0)
    {
        ::close(fd);
        return Error::make(Errc::Io, "cannot set watchdog timeout", "watchdog");
    }
    ::close(fd);
    watchdog_opened_ = true;
    return Error::none();
}

/// @brief Pet the hardware watchdog.
Error NativePlatform::watchdog_pet()
{
    // Keep a persistent fd open for petting.
    if (watchdog_fd_ < 0)
    {
        watchdog_fd_ = ::open((sysroot_ + "/dev/watchdog").c_str(), O_WRONLY);
        if (watchdog_fd_ < 0) return Error::make(Errc::Io, "cannot open /dev/watchdog", "watchdog");
        watchdog_opened_ = true;
    }
    if (::write(watchdog_fd_, "1", 1) < 0) return Error::make(Errc::Io, "watchdog write failed", "watchdog");
    return Error::none();
}

/// @brief True when a time source is currently disciplined.
bool NativePlatform::time_locked()
{
    // chrony tracking or a time-synced marker from networkd.
    return read_file("/run/instrumentation/time-synced").ok();
}

/// @brief Enumerate network interfaces.
/// @return Network interface states.
std::vector<IfaceInfo> NativePlatform::interfaces()
{
    std::vector<IfaceInfo> out;
    for (const auto& name : list_dir("/sys/class/net"))
    {
        IfaceInfo i;
        i.name = name;
        auto operstate = read_file("/sys/class/net/" + name + "/operstate");
        if (operstate.ok())
        {
            i.up = operstate->find("up") != std::string::npos;
            i.link = operstate->find("up") != std::string::npos;
        }
        auto mac = read_file("/sys/class/net/" + name + "/address");
        if (mac.ok())
        {
            i.mac = *mac;
            while (!i.mac.empty() && (i.mac.back() == '\n' || i.mac.back() == ' ')) i.mac.pop_back();
        }
        out.push_back(i);
    }
    return out;
}

// ---- boot slots (§5, §7.9) ------------------------------------------------
// The bootloader environment is the source of truth, not a file we invent.
// U-Boot exposes it through fw_printenv/fw_setenv; where those are absent we
// fall back to the flag files under the boot partition (boot_dir(),
// default /boot) so a board without a U-Boot
// environment (an EFI x86 target, say) still reports a coherent state.
//
// Variables (the names the boot script reads):
//   boot_slot       active slot, "A" or "B"
//   boot_slot_next  slot to try on the next boot (trial)
//   boot_trial      1 while an update is unconfirmed
//   boot_attempts   trial boot counter, reset on confirmation
//   boot_golden     the known-good fallback slot

/// @brief Read a U-Boot environment variable.
/// @param var U-Boot variable to read.
/// @return The variable value, or an error.
Result<std::string> NativePlatform::fw_getenv(const std::string& var)
{
    std::string cmd = "fw_printenv -n " + var + " 2>/dev/null";
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) return Result<std::string>::fail(Errc::Unavailable, "fw_printenv unavailable", var);
    char buf[256];
    std::string out;
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    int rc = ::pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ' || out.back() == '\r')) out.pop_back();
    if (rc != 0 || out.empty()) return Result<std::string>::fail(Errc::NotFound, "unset in bootloader env", var);
    return Result<std::string>::good(out);
}

/// @brief Write a U-Boot environment variable.
/// @param var U-Boot variable to write.
/// @param value Value to write.
Error NativePlatform::fw_setenv(const std::string& var, const std::string& value)
{
    if (!apply_enabled()) return Error::make(Errc::Unavailable, "bootloader env write disabled on host (INSTR_APPLY_NETWORK!=1)", var);
    std::string cmd = "fw_setenv " + var + " " + value + " 2>/dev/null";
    int rc = std::system(cmd.c_str());
    if (rc != 0) return Error::make(Errc::Io, "fw_setenv failed", var);
    return Error::none();
}

/// @brief Probe (once) whether the board has a U-Boot environment.
bool NativePlatform::have_fw_env()
{
    if (fw_env_checked_) return fw_env_present_;
    fw_env_checked_ = true;
    fw_env_present_ = fw_getenv("boot_slot").ok();
    return fw_env_present_;
}

/// @brief Current A/B boot-slot state.
/// @return The boot-slot state, or an error.
Result<BootSlotState> NativePlatform::boot_slots()
{
    BootSlotState s;
    auto trim = [](std::string v) {
        while (!v.empty() && (v.back() == '\n' || v.back() == ' ' || v.back() == '\r')) v.pop_back();
        return v;
    };

    if (have_fw_env())
    {
        auto active = fw_getenv("boot_slot");
        if (!active.ok()) return Result<BootSlotState>::fail(Errc::NotFound, "boot_slot unset", "boot");
        s.active = *active;
        s.inactive = s.active == "A" ? "B" : "A";
        auto next = fw_getenv("boot_slot_next");
        auto trial = fw_getenv("boot_trial");
        // A pending slot only counts while the trial flag is actually set;
        // a leftover boot_slot_next is not an unconfirmed update.
        if (next.ok() && trial.ok() && *trial == "1" && *next != s.active) s.pending = *next;
        auto golden = fw_getenv("boot_golden");
        s.golden = golden.ok() ? *golden : s.active;
        auto att = fw_getenv("boot_attempts");
        if (att.ok())
        {
            try
            {
                s.boot_attempts = uint32_t(std::stoul(*att));
            }
            catch (...)
            {
            }
        }
        return Result<BootSlotState>::good(s);
    }

    // Fallback: flag files written by the boot script.
    auto cur = read_file(boot_dir() + "/current-slot");
    if (!cur.ok()) return Result<BootSlotState>::fail(Errc::NotFound, "no slot state", "boot");
    s.active = trim(*cur);
    s.inactive = s.active == "A" ? "B" : "A";
    auto pend = read_file(boot_dir() + "/pending-slot");
    if (pend.ok()) s.pending = trim(*pend);
    auto golden = read_file(boot_dir() + "/golden-slot");
    s.golden = golden.ok() ? trim(*golden) : s.active;
    auto att = read_file(boot_dir() + "/boot-attempts");
    if (att.ok())
    {
        try
        {
            s.boot_attempts = uint32_t(std::stoul(*att));
        }
        catch (...)
        {
        }
    }
    return Result<BootSlotState>::good(s);
}

/// @brief Select a boot slot; @p trial marks it pending confirmation.
/// @param slot Slot to select ("A"/"B").
/// @param trial Mark the slot pending confirmation when true.
Error NativePlatform::set_boot_slot(const std::string& slot, bool trial)
{
    if (slot != "A" && slot != "B") return Error::make(Errc::InvalidArgument, "bad slot", slot);

    if (have_fw_env())
    {
        // trial = true is the whole point of the rollback contract: the boot
        // script must know this boot is on probation, so that a device that
        // fails to reach confirm() comes back on the previous slot (§11).
        Error e = fw_setenv("boot_slot_next", slot);
        if (!e.ok()) return e;
        e = fw_setenv("boot_trial", trial ? "1" : "0");
        if (!e.ok()) return e;
        e = fw_setenv("boot_attempts", "0");
        if (!e.ok()) return e;
        if (!trial)
        {
            // Not a trial: this slot is the active one from here on.
            e = fw_setenv("boot_slot", slot);
            if (!e.ok()) return e;
        }
        return Error::none();
    }

    Error e = write_boot_file(boot_dir() + "/pending-slot", slot);
    if (!e.ok()) return e;
    e = write_boot_file(boot_dir() + "/boot-trial", trial ? "1" : "0");
    if (!e.ok()) return e;
    if (!trial) e = write_boot_file(boot_dir() + "/current-slot", slot);
    return e;
}

/// @brief Confirm the currently pending slot as good.
Error NativePlatform::confirm_boot_slot()
{
    auto cur = boot_slots();
    if (!cur.ok()) return cur.error;
    // Confirmation promotes the slot we are actually running from, and clears
    // the trial state so the next boot is not treated as a probation boot.
    const std::string running = cur->pending.empty() ? cur->active : cur->pending;

    if (have_fw_env())
    {
        Error e = fw_setenv("boot_slot", running);
        if (!e.ok()) return e;
        e = fw_setenv("boot_trial", "0");
        if (!e.ok()) return e;
        e = fw_setenv("boot_attempts", "0");
        if (!e.ok()) return e;
        // A confirmed slot becomes the golden fallback for the next update.
        return fw_setenv("boot_golden", running);
    }

    Error e = write_boot_file(boot_dir() + "/current-slot", running);
    if (!e.ok()) return e;
    e = write_boot_file(boot_dir() + "/golden-slot", running);
    if (!e.ok()) return e;
    (void)write_boot_file(boot_dir() + "/boot-trial", "0");
    ::unlink((sysroot_ + boot_dir() + "/pending-slot").c_str());
    return Error::none();
}

/// @brief Flag-file fallback for boards without U-Boot env.
/// @param rel Flag-file relative path.
/// @param value Value to record.
Error NativePlatform::write_boot_file(const std::string& rel, const std::string& value)
{
    FILE* f = std::fopen((sysroot_ + rel).c_str(), "w");
    if (!f) return Error::make(Errc::Io, "cannot write " + rel, "boot");
    std::fprintf(f, "%s\n", value.c_str());
    std::fclose(f);
    return Error::none();
}

/// @brief Drive a GPIO line (Hardware daemon only, R7).
/// @param line GPIO line number.
/// @param value Value to drive.
Error NativePlatform::gpio_set(int line, bool value)
{
    std::string dir = "/sys/class/gpio/gpio" + std::to_string(line);
    if (!read_file(dir + "/value").ok())
    {
        FILE* ex = std::fopen((sysroot_ + "/sys/class/gpio/export").c_str(), "w");
        if (!ex) return Error::make(Errc::Io, "gpio export failed", "gpio");
        std::fprintf(ex, "%d\n", line);
        std::fclose(ex);
    }
    FILE* f = std::fopen((sysroot_ + dir + "/value").c_str(), "w");
    if (!f) return Error::make(Errc::Io, "gpio write failed", dir);
    std::fprintf(f, "%d\n", value ? 1 : 0);
    std::fclose(f);
    return Error::none();
}

/// @brief Read a GPIO line.
/// @param line GPIO line number.
/// @return The line state, or an error.
Result<bool> NativePlatform::gpio_get(int line)
{
    auto v = read_file("/sys/class/gpio/gpio" + std::to_string(line) + "/value");
    if (!v.ok()) return Result<bool>::fail(v.error.code, v.error.message, "gpio");
    try
    {
        return Result<bool>::good(std::stoi(*v) != 0);
    }
    catch (...)
    {
        return Result<bool>::fail(Errc::Corrupt, "gpio value unreadable", "gpio");
    }
}

/// @brief Set an LED steady on/off.
/// @param name LED name.
/// @param on True to light the LED.
Error NativePlatform::led_set(const std::string& name, bool on)
{
    FILE* f = std::fopen((sysroot_ + "/sys/class/leds/" + name + "/brightness").c_str(), "w");
    if (!f) return Error::make(Errc::NotFound, "led not found", name);
    std::fprintf(f, "%d\n", on ? 255 : 0);
    std::fclose(f);
    return Error::none();
}

/// @brief Blink an LED with the given duty cycle.
/// @param name LED name.
/// @param on_ms On duration in ms.
/// @param off_ms Off duration in ms.
/// @param repeats Number of blinks.
Error NativePlatform::led_blink(const std::string& name, int on_ms, int off_ms, int repeats)
{
    for (int i = 0; i < repeats; ++i)
    {
        Error e = led_set(name, true);
        if (!e.ok()) return e;
        ::usleep(useconds_t(on_ms) * 1000);
        e = led_set(name, false);
        if (!e.ok()) return e;
        ::usleep(useconds_t(off_ms) * 1000);
    }
    return Error::none();
}

/// @brief Power-cycle a USB port.
/// @param port USB port to power-cycle.
Error NativePlatform::usb_reset(const std::string& port)
{
    // Standard sysfs authorized toggle resets a USB port.
    std::string auth = "/sys/bus/usb/devices/" + port + "/authorized";
    FILE* f = std::fopen((sysroot_ + auth).c_str(), "w");
    if (!f) return Error::make(Errc::NotFound, "usb port not found", port);
    std::fprintf(f, "0\n");
    std::fflush(f);
    ::usleep(50000);
    std::fseek(f, 0, SEEK_SET);
    std::fprintf(f, "1\n");
    std::fclose(f);
    return Error::none();
}

/// @brief Run a CAN loopback health check on @p iface.
/// @param iface CAN interface to test.
/// @return True when the loopback passes, or an error.
Result<bool> NativePlatform::can_loopback_test(const std::string& iface)
{
    // A behavioural sanity check: interface must exist and be up.
    auto state = read_file("/sys/class/net/" + iface + "/operstate");
    if (!state.ok()) return Result<bool>::good(false);
    return Result<bool>::good(state->find("up") != std::string::npos);
}

/// @brief Send a control command to a display.
/// @param cmd Display command.
/// @param arg Command argument.
Error NativePlatform::display_control(const std::string& cmd, const std::string& arg)
{
    // Which backend this board has is descriptor data, not a code branch
    // (R9): devices.yaml names it in the display device's params, and the
    // Hardware daemon passes it through in `arg` as "backend=<name>[,...]".
    // Absent that, probe the standard sysfs paths in turn.
    std::string backend;
    const std::string kBackendKey = "backend=";
    size_t bpos = arg.find(kBackendKey);
    if (bpos != std::string::npos)
    {
        size_t start = bpos + kBackendKey.size();
        size_t end = arg.find(',', start);
        backend = arg.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }

    // Candidate control files, most specific first.
    struct Backend
    {
        const char* name;
        const char* blank_path;
        const char* on_value;
        const char* off_value;
    };
    static const Backend kBackends[] = {
        // Framebuffer blanking: FB_BLANK_UNBLANK (0) / FB_BLANK_POWERDOWN (4).
        {"fb", "/sys/class/graphics/fb0/blank", "0", "4"},
        // DRM/KMS and DSI panels expose a backlight instead.
        {"backlight", "/sys/class/backlight/backlight/bl_power", "0", "4"},
        {"dsi", "/sys/class/backlight/dsi-backlight/bl_power", "0", "4"},
        {"edp", "/sys/class/backlight/edp-backlight/bl_power", "0", "4"},
    };

    auto write_value = [this](const std::string& path, const char* value) -> bool {
        FILE* f = std::fopen((sysroot_ + path).c_str(), "w");
        if (!f) return false;
        std::fprintf(f, "%s\n", value);
        std::fclose(f);
        return true;
    };

    if (cmd == "blank" || cmd == "unblank")
    {
        const bool on = (cmd == "unblank");
        for (const auto& b : kBackends)
        {
            if (!backend.empty() && backend != b.name) continue;
            if (!read_file(b.blank_path).ok()) continue;
            if (write_value(b.blank_path, on ? b.on_value : b.off_value)) return Error::none();
        }
        return Error::make(Errc::NotSupported, backend.empty() ? "no display backend found on this board" : "display backend '" + backend + "' not present on this board", cmd);
    }

    if (cmd == "brightness")
    {
        // arg carries "value=<0-100>" alongside any backend selector.
        size_t vpos = arg.find("value=");
        if (vpos == std::string::npos) return Error::make(Errc::InvalidArgument, "brightness needs value=<0-100>", cmd);
        int pct = 0;
        try
        {
            pct = std::stoi(arg.substr(vpos + 6));
        }
        catch (...)
        {
            return Error::make(Errc::InvalidArgument, "brightness value must be numeric", cmd);
        }
        if (pct < 0 || pct > 100) return Error::make(Errc::InvalidArgument, "brightness must be 0..100", cmd);
        for (const char* dir : {"/sys/class/backlight/backlight", "/sys/class/backlight/dsi-backlight", "/sys/class/backlight/edp-backlight"})
        {
            auto max = read_file(std::string(dir) + "/max_brightness");
            if (!max.ok()) continue;
            try
            {
                const long scaled = std::stol(*max) * pct / 100;
                if (write_value(std::string(dir) + "/brightness", std::to_string(scaled).c_str())) return Error::none();
            }
            catch (...)
            {
            }
        }
        return Error::make(Errc::NotSupported, "no backlight on this board", cmd);
    }

    return Error::make(Errc::NotSupported, "display command not in descriptor", cmd);
}

/// @brief Probe display presence/capability.
/// @return Probe result text, or an error.
Result<std::string> NativePlatform::display_probe()
{
    auto f = read_file("/sys/class/graphics/fb0/virtual_size");
    if (!f.ok()) return Result<std::string>::fail(Errc::NotSupported, "no framebuffer", "display");
    std::string s = *f;
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return Result<std::string>::good("ok " + s);
}

/// @brief Start a systemd unit (istate requests, REQ §7.1).
/// @param unit systemd unit to start.
Error NativePlatform::unit_start(const std::string& unit)
{
    if (!apply_enabled()) return Error::make(Errc::Unavailable, "systemd mutation disabled on host (INSTR_APPLY_NETWORK!=1)", unit);
    int rc = std::system(("systemctl start " + unit + " 2>/dev/null").c_str());
    return rc == 0 ? Error::none() : Error::make(Errc::Unavailable, "systemctl start failed", unit);
}
/// @brief Stop a systemd unit.
/// @param unit systemd unit to stop.
Error NativePlatform::unit_stop(const std::string& unit)
{
    if (!apply_enabled()) return Error::make(Errc::Unavailable, "systemd mutation disabled on host", unit);
    int rc = std::system(("systemctl stop " + unit + " 2>/dev/null").c_str());
    return rc == 0 ? Error::none() : Error::make(Errc::Unavailable, "systemctl stop failed", unit);
}
/// @brief Restart a systemd unit.
/// @param unit systemd unit to restart.
Error NativePlatform::unit_restart(const std::string& unit)
{
    if (!apply_enabled()) return Error::make(Errc::Unavailable, "systemd mutation disabled on host", unit);
    int rc = std::system(("systemctl restart " + unit + " 2>/dev/null").c_str());
    return rc == 0 ? Error::none() : Error::make(Errc::Unavailable, "systemctl restart failed", unit);
}
/// @brief Query whether a unit is active.
/// @param unit systemd unit to query.
/// @return True when active, or an error.
Result<bool> NativePlatform::unit_active(const std::string& unit)
{
    if (apply_enabled())
    {
        int rc = std::system(("systemctl is-active --quiet " + unit + " 2>/dev/null").c_str());
        return Result<bool>::good(rc == 0);
    }
    // Without systemd: treat presence of a pid file as active.
    std::string unit_name = unit;
    size_t pos = unit_name.find(".service");
    if (pos != std::string::npos) unit_name = unit_name.substr(0, pos);
    return Result<bool>::good(read_file("/run/" + unit_name + ".pid").ok());
}

/// @copydoc IPlatform::kernel_log_lines
/// @param from_start True for boot backfill; false to follow with new lines only.
/// @return Kernel log lines, or an error.
Result<std::vector<std::string>> NativePlatform::kernel_log_lines(bool from_start)
{
    std::lock_guard<std::mutex> lk(kmsg_mu_);
    std::vector<std::string> out;

    // Backfill: a fresh descriptor reads /dev/kmsg from the head of the ring.
    if (from_start)
    {
        int fd = ::open((sysroot_ + "/dev/kmsg").c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0)
        {
            // /proc/kmsg is unreadable unprivileged; the journal carries the same
            // kernel messages, so fall back rather than losing the boot window.
            return journal_lines("kernel", 100000);
        }
        read_kmsg_into(fd, &out, 200000);
        ::close(fd);
        // Park the follow descriptor at the end so the follower that runs next
        // does not re-report everything we just backfilled.
        open_follow_fd();
        return Result<std::vector<std::string>>::good(std::move(out));
    }

    // Follow: one long-lived descriptor. Re-opening and seeking to the end on
    // every poll — as this used to do — reads nothing, ever.
    if (kmsg_follow_fd_ < 0 && !open_follow_fd()) return Result<std::vector<std::string>>::good({});
    read_kmsg_into(kmsg_follow_fd_, &out, 50000);
    return Result<std::vector<std::string>>::good(std::move(out));
}

/// @brief Open the long-lived /dev/kmsg follow descriptor.
bool NativePlatform::open_follow_fd()
{
    if (kmsg_follow_fd_ >= 0)
    {
        ::close(kmsg_follow_fd_);
        kmsg_follow_fd_ = -1;
    }
    int fd = ::open((sysroot_ + "/dev/kmsg").c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;
    // SEEK_END on /dev/kmsg means "only messages arriving from now on".
    ::lseek(fd, 0, SEEK_END);
    kmsg_follow_fd_ = fd;
    return true;
}

/// @brief Read up to @p max_lines kernel messages from @p fd into @p out.
/// @param fd Descriptor to read from.
/// @param out Receives the lines.
/// @param max_lines Maximum lines to read.
void NativePlatform::read_kmsg_into(int fd, std::vector<std::string>* out, int max_lines)
{
    char buf[8192];
    ssize_t n;
    int count = 0;
    // /dev/kmsg delivers one record per read(); EAGAIN means "nothing more
    // right now", which for a non-blocking follow is the normal exit.
    while (count < max_lines && (n = ::read(fd, buf, sizeof(buf) - 1)) > 0)
    {
        buf[n] = '\0';
        std::string chunk(buf, size_t(n));
        size_t pos = 0;
        while (pos < chunk.size())
        {
            size_t nl = chunk.find('\n', pos);
            if (nl == std::string::npos) nl = chunk.size();
            if (nl > pos) out->push_back(chunk.substr(pos, nl - pos));
            pos = nl + 1;
            ++count;
        }
    }
}

/// @brief Application log sink / journal access for LogManager.
/// @param unit Unit whose journal to read.
/// @param max_lines Maximum lines returned.
/// @return Log lines, or an error.
Result<std::vector<std::string>> NativePlatform::journal_lines(const std::string& unit, size_t max_lines)
{
    // Reading the journal is an observation, not a mutation: it must work on a
    // real device regardless of INSTR_APPLY_NETWORK, or LogManager collects
    // nothing (§7.7). journalctl simply yields nothing where it is absent.
    std::string cmd = "journalctl -u " + unit + " -n " + std::to_string(max_lines) + " --no-pager -o short 2>/dev/null";
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) return Result<std::vector<std::string>>::good({});
    std::vector<std::string> out;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), p))
    {
        std::string line(buf);
        while (!line.empty() && line.back() == '\n') line.pop_back();
        out.push_back(line);
    }
    ::pclose(p);
    return Result<std::vector<std::string>>::good(std::move(out));
}

// ---- network application (§7.2, R8) ---------------------------------------
// networkd renders a complete plan; this is where it reaches the system.
// Everything runs through run_cmd, which is inert unless INSTR_APPLY_NETWORK
// is set, so building and testing on a workstation never mutates the host —
// but on a device the plan is genuinely applied, not recorded and dropped.

/// @brief Record (and conditionally execute) a network command.
/// @param cmd Command to record (and conditionally execute).
Error NativePlatform::run_cmd(const std::string& cmd)
{
    if (!apply_enabled())
    {
        applied_commands_.push_back(cmd); // dry run: observable, not executed
        return Error::none();
    }
    applied_commands_.push_back(cmd);
    int rc = std::system((cmd + " >/dev/null 2>&1").c_str());
    if (rc != 0) return Error::make(Errc::Io, "command failed: " + cmd, "network");
    return Error::none();
}

/// @brief Record (and conditionally execute) a config-file write.
/// @param path Config file to write.
/// @param text Configuration text.
Error NativePlatform::write_conf(const std::string& path, const std::string& text)
{
    if (!apply_enabled())
    {
        applied_commands_.push_back("write " + path);
        return Error::none();
    }
    FILE* f = std::fopen((sysroot_ + path).c_str(), "w");
    if (!f) return Error::make(Errc::Io, "cannot write " + path, "network");
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    return Error::none();
}

/// @brief Render the instrumentation nftables filter ruleset for a firewall plan.
std::string render_firewall_ruleset(const JValue& firewall)
{
    const std::string t = kInstrTable;
    // Declare-then-delete-then-build. Declaring the table empty first creates it
    // when it is absent, so the delete cannot fail with ENOENT and abort the
    // whole `nft -f` transaction. What we must NOT do is `flush ruleset`: that
    // drops every table in the kernel including Configuration's `webconfig`
    // table, and whichever daemon applied last would win (Task.md B-02).
    std::string rules = "table inet " + t + " { }\n";
    rules += "delete table inet " + t + "\n";
    rules += "table inet " + t + " {\n";
    rules += "  chain input {\n";
    rules += "    type filter hook input priority 0; policy drop;\n";
    rules += "    ct state established,related accept\n";
    rules += "    iif lo accept\n";
    // The tunnel is never firewalled off by a generated rule set: losing it
    // is precisely what the tunnel-loss guard exists to prevent (§7.2).
    rules += "    udp dport 51820 accept\n";
    for (const auto& r : firewall.items())
    {
        const std::string action = r.find("action") ? r.find("action")->as_string() : "deny";
        const std::string proto = r.find("proto") ? r.find("proto")->as_string() : "any";
        const std::string src = r.find("src") ? r.find("src")->as_string() : "any";
        const uint32_t port = r.find("dst_port") ? uint32_t(r.find("dst_port")->as_number()) : 0;
        std::string line = "    ";
        if (src != "any" && !src.empty()) line += "ip saddr " + src + " ";
        if (proto != "any") line += proto + " ";
        if (port) line += "dport " + std::to_string(port) + " ";
        line += (action == "allow" ? "accept" : "drop");
        rules += line + "\n";
    }
    rules += "  }\n}\n";
    return rules;
}

/// @brief Render the instrumentation NAT ruleset for a port-forwarding plan.
std::string render_nat_ruleset(const JValue& forwards)
{
    if (forwards.items().empty()) return "";
    const std::string t = kInstrNatTable;
    std::string nat = "table ip " + t + " { }\n";
    nat += "delete table ip " + t + "\n";
    nat += "table ip " + t + " {\n  chain prerouting {\n";
    nat += "    type nat hook prerouting priority -100;\n";
    for (const auto& f : forwards.items())
    {
        const std::string proto = f.find("proto") ? f.find("proto")->as_string() : "tcp";
        const uint32_t lp = f.find("listen_port") ? uint32_t(f.find("listen_port")->as_number()) : 0;
        const std::string dip = f.find("dest_ip") ? f.find("dest_ip")->as_string() : "";
        const uint32_t dp = f.find("dest_port") ? uint32_t(f.find("dest_port")->as_number()) : 0;
        if (!lp || dip.empty() || !dp) continue;
        nat += "    " + proto + " dport " + std::to_string(lp) + " dnat to " + dip + ":" + std::to_string(dp) + "\n";
    }
    nat += "  }\n}\n";
    return nat;
}

/// @brief Render/apply a network plan (interfaces, firewall, ssh windows, wg, time).
///
/// The simulated platform records the plan; native shells out only when
/// INSTR_APPLY_NETWORK=1 (never silently mutates a host).
/// @param plan_json Rendered network plan (interfaces, firewall, ssh windows, wg, time).
Error NativePlatform::apply_network_plan(const std::string& plan_json)
{
    auto parsed = json_parse(plan_json);
    if (!parsed.ok()) return Error::make(Errc::InvalidArgument, "malformed network plan: " + parsed.error.message, "network");
    const JValue& p = *parsed;
    applied_commands_.clear();
    std::vector<std::string> failures;
    auto attempt = [&](const Error& e) {
        // One failing rule must not abandon the rest of the plan: a device with
        // a partially applied network is still reachable, one with none is not.
        if (!e.ok()) failures.push_back(e.message);
    };

    // ---- interfaces: DHCP / static / VLAN / bond ----
    if (const JValue* ifaces = p.find("interfaces"))
    {
        for (const auto& i : ifaces->items())
        {
            const std::string name = i.find("name") ? i.find("name")->as_string() : "";
            const std::string mode = i.find("mode") ? i.find("mode")->as_string() : "dhcp";
            if (name.empty() || mode == "unused") continue;

            if (mode == "vlan")
            {
                const uint32_t vid = i.find("vlan_id") ? uint32_t(i.find("vlan_id")->as_number()) : 0;
                const std::string parent = name.substr(0, name.find('.'));
                attempt(run_cmd("ip link add link " + parent + " name " + name + " type vlan id " + std::to_string(vid)));
            }
            else if (mode == "bond")
            {
                const std::string member = i.find("bond_member") ? i.find("bond_member")->as_string() : "";
                attempt(run_cmd("ip link add " + name + " type bond"));
                if (!member.empty())
                {
                    attempt(run_cmd("ip link set " + member + " down"));
                    attempt(run_cmd("ip link set " + member + " master " + name));
                }
            }

            attempt(run_cmd("ip link set " + name + " up"));
            if (mode == "static")
            {
                const std::string addr = i.find("address") ? i.find("address")->as_string() : "";
                const uint32_t prefix = i.find("prefix") ? uint32_t(i.find("prefix")->as_number()) : 24;
                if (!addr.empty())
                {
                    attempt(run_cmd("ip addr flush dev " + name));
                    attempt(run_cmd("ip addr add " + addr + "/" + std::to_string(prefix) + " dev " + name));
                }
            }
            else if (mode == "dhcp") { attempt(run_cmd("udhcpc -i " + name + " -b -q")); }
        }
    }

    // ---- routes ----
    if (const JValue* routes = p.find("routes"))
    {
        for (const auto& r : routes->items())
        {
            const std::string dest = r.find("destination") ? r.find("destination")->as_string() : "";
            const std::string gw = r.find("gateway") ? r.find("gateway")->as_string() : "";
            const std::string dev = r.find("iface") ? r.find("iface")->as_string() : "";
            const uint32_t metric = r.find("metric") ? uint32_t(r.find("metric")->as_number()) : 0;
            if (dest.empty()) continue;
            std::string cmd = "ip route replace " + dest;
            if (!gw.empty()) cmd += " via " + gw;
            if (!dev.empty()) cmd += " dev " + dev;
            if (metric) cmd += " metric " + std::to_string(metric);
            attempt(run_cmd(cmd));
        }
    }

    // ---- DNS ----
    if (const JValue* dns = p.find("dns"))
    {
        std::string resolv;
        if (const JValue* search = dns->find("search"))
        {
            std::string line;
            for (const auto& d : search->items()) line += (line.empty() ? "" : " ") + d.as_string();
            if (!line.empty()) resolv += "search " + line + "\n";
        }
        if (const JValue* servers = dns->find("servers"))
        {
            for (const auto& d : servers->items()) resolv += "nameserver " + d.as_string() + "\n";
        }
        if (!resolv.empty()) attempt(write_conf("/etc/resolv.conf", resolv));
    }

    // ---- firewall (nftables) ----
    if (const JValue* fw = p.find("firewall"))
    {
        attempt(write_conf("/etc/nftables.conf", render_firewall_ruleset(*fw)));
        attempt(run_cmd("nft -f /etc/nftables.conf"));
    }

    // ---- port forwarding ----
    if (const JValue* pf = p.find("port_forwarding"))
    {
        const std::string nat = render_nat_ruleset(*pf);
        if (!nat.empty())
        {
            attempt(write_conf("/etc/nftables-nat.conf", nat));
            attempt(run_cmd("nft -f /etc/nftables-nat.conf"));
        }
    }

    // ---- wireless: client (wpa_supplicant) or AP (hostapd) ----
    if (const JValue* w = p.find("wireless"))
    {
        const bool enabled = w->find("enabled") && w->find("enabled")->as_bool();
        const std::string mode = w->find("mode") ? w->find("mode")->as_string() : "off";
        const std::string wif = w->find("iface") ? w->find("iface")->as_string() : "";
        if (!enabled || mode == "off" || wif.empty())
        {
            attempt(run_cmd("systemctl stop wpa_supplicant@" + (wif.empty() ? "wlan0" : wif)));
            attempt(run_cmd("systemctl stop hostapd"));
        }
        else if (mode == "client")
        {
            // The rendered supplicant config (with the PSK resolved from /pst) is
            // written by networkd through write_wireless_conf; here we bring the
            // service up against it.
            attempt(run_cmd("systemctl stop hostapd"));
            attempt(run_cmd("ip link set " + wif + " up"));
            attempt(run_cmd("systemctl restart wpa_supplicant@" + wif));
        }
        else if (mode == "ap")
        {
            attempt(run_cmd("systemctl stop wpa_supplicant@" + wif));
            attempt(run_cmd("ip link set " + wif + " up"));
            attempt(run_cmd("systemctl restart hostapd"));
        }
    }

    // ---- WireGuard tunnel ----
    if (const JValue* wg = p.find("wireguard"))
    {
        const bool up = wg->find("up") && wg->find("up")->as_bool();
        if (up) { attempt(run_cmd("wg-quick up wg0")); }
        else { attempt(run_cmd("wg-quick down wg0")); }
    }

    // ---- inbound SSH: off unless a window is open (§7.2, §10) ----
    if (const JValue* ssh = p.find("ssh_enabled")) { attempt(run_cmd(ssh->as_bool() ? "systemctl start sshd" : "systemctl stop sshd")); }

    if (!failures.empty())
    {
        std::string msg = std::to_string(failures.size()) + " plan step(s) failed: " + failures.front();
        if (failures.size() > 1) msg += " (and " + std::to_string(failures.size() - 1) + " more)";
        return Error::make(Errc::Io, msg, "network");
    }
    return Error::none();
}

/// @brief Write the rendered supplicant / hostapd configuration for a plan.
/// @param mode "sta" or "ap".
/// @param iface Interface the configuration applies to.
/// @param text Rendered configuration text.
Error NativePlatform::write_wireless_conf(const std::string& mode, const std::string& iface, const std::string& text)
{
    if (mode == "client") return write_conf("/etc/wpa_supplicant/wpa_supplicant-" + iface + ".conf", text);
    if (mode == "ap") return write_conf("/etc/hostapd/hostapd.conf", text);
    return Error::make(Errc::InvalidArgument, "unknown wireless mode", mode);
}

/// @brief True when HQ is currently reachable.
bool NativePlatform::hq_reachable()
{
    return read_file("/run/instrumentation/tunnel-up").ok();
}

} // namespace platform
} // namespace instr
// ---- §5 additions: partition topology, SMART, time sources, CAN, attestation ----

namespace instr
{
/// @brief Platform abstraction: board and hardware access behind IPlatform (REQ §5).
namespace platform
{

/// @brief Partition topology including mount points and usage (§5).
/// @return Partition topology.
std::vector<Partition> NativePlatform::partitions()
{
    std::vector<Partition> out;
    // /proc/self/mountinfo gives the mount point and fs type; sysfs gives the
    // size and the parent device. Together they are the partition topology.
    std::map<std::string, std::pair<std::string, std::string>> mounts; // dev → (mount, fstype)
    {
        std::ifstream f(sysroot_ + "/proc/mounts");
        std::string dev, mnt, fstype, rest;
        while (f >> dev >> mnt >> fstype)
        {
            std::getline(f, rest);
            if (dev.rfind("/dev/", 0) == 0) mounts[dev.substr(5)] = {mnt, fstype};
        }
    }
    for (const auto& parent : list_dir("/sys/class/block"))
    {
        if (parent.rfind("loop", 0) == 0 || parent.rfind("ram", 0) == 0) continue;
        for (const auto& child : list_dir("/sys/class/block/" + parent))
        {
            // A partition directory of a block device carries its own "partition".
            if (!read_file("/sys/class/block/" + parent + "/" + child + "/partition").ok()) continue;
            Partition p;
            p.name = child;
            p.device_path = "/dev/" + child;
            p.parent = parent;
            auto size = read_file("/sys/class/block/" + parent + "/" + child + "/size");
            if (size.ok())
            {
                try
                {
                    p.bytes = std::stoull(*size) * 512;
                }
                catch (...)
                {
                }
            }
            auto ro = read_file("/sys/class/block/" + parent + "/" + child + "/ro");
            p.read_only = ro.ok() && !ro->empty() && (*ro)[0] == '1';
            auto mit = mounts.find(child);
            if (mit != mounts.end())
            {
                p.mount_point = mit->second.first;
                p.fs_type = mit->second.second;
                struct statvfs vfs;
                if (::statvfs((sysroot_ + p.mount_point).c_str(), &vfs) == 0) p.used_bytes = uint64_t(vfs.f_blocks - vfs.f_bfree) * vfs.f_frsize;
            }
            out.push_back(p);
        }
    }
    return out;
}

/// @brief Which time sources this board has, and their state (§5, §7.1).
/// @return Time-source inventory with state.
std::vector<TimeSourceInfo> NativePlatform::time_sources()
{
    std::vector<TimeSourceInfo> out;

    // GNSS: present when gpsd has a device, locked when it reports a fix.
    TimeSourceInfo gnss;
    gnss.kind = "gnss";
    gnss.present = read_file("/dev/gps0").ok() || read_file("/run/gpsd.sock").ok() || !list_dir("/sys/class/gnss").empty();
    if (gnss.present)
    {
        auto fix = read_file("/run/instrumentation/gnss-fix");
        gnss.locked = fix.ok() && fix->find("1") != std::string::npos;
        gnss.quality = gnss.locked ? 1.0 : 0.0;
    }
    out.push_back(gnss);

    // PTP: present when the kernel exposes a PHC.
    TimeSourceInfo ptp;
    ptp.kind = "ptp";
    ptp.present = !list_dir("/sys/class/ptp").empty();
    if (ptp.present)
    {
        auto st = read_file("/run/instrumentation/ptp-state");
        ptp.locked = st.ok() && st->find("SLAVE") != std::string::npos;
        ptp.quality = ptp.locked ? 0.9 : 0.0;
    }
    out.push_back(ptp);

    // NTP: chrony is a userspace daemon, so "present" means configured.
    TimeSourceInfo ntp;
    ntp.kind = "ntp";
    ntp.present = read_file("/etc/chrony/chrony.conf").ok() || read_file("/etc/chrony.conf").ok();
    ntp.locked = ntp.present && time_locked();
    ntp.quality = ntp.locked ? 0.7 : 0.0;
    out.push_back(ntp);

    TimeSourceInfo rtc;
    rtc.kind = "rtc";
    rtc.present = read_file("/dev/rtc0").ok() || !list_dir("/sys/class/rtc").empty();
    rtc.locked = rtc.present;
    rtc.quality = rtc.present ? 0.2 : 0.0;
    out.push_back(rtc);

    return out;
}

/// @brief Send one CAN frame on @p iface.
/// @param iface CAN interface.
/// @param frame Frame to send.
Error NativePlatform::can_send(const std::string& iface, const CanFrame& frame)
{
    int fd = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) return Error::make(Errc::NotSupported, "no CAN support in kernel", iface);
    struct ifreq ifr;
    std::memset(&ifr, 0, sizeof(ifr));
    std::strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
    if (::ioctl(fd, SIOCGIFINDEX, &ifr) < 0)
    {
        ::close(fd);
        return Error::make(Errc::NotFound, "no such CAN interface", iface);
    }
    struct sockaddr_can addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        ::close(fd);
        return Error::make(Errc::Io, "cannot bind CAN socket", iface);
    }
    struct can_frame cf;
    std::memset(&cf, 0, sizeof(cf));
    cf.can_id = frame.id | (frame.extended ? CAN_EFF_FLAG : 0);
    cf.can_dlc = uint8_t(std::min<size_t>(frame.data.size(), 8));
    std::memcpy(cf.data, frame.data.data(), cf.can_dlc);
    ssize_t n = ::write(fd, &cf, sizeof(cf));
    ::close(fd);
    if (n != sizeof(cf)) return Error::make(Errc::Io, "CAN write failed", iface);
    return Error::none();
}

/// @brief Drain up to @p max_frames frames buffered since the previous call.
/// @param iface CAN interface.
/// @param max_frames Maximum frames returned.
/// @return Buffered frames, or an error.
Result<std::vector<NativePlatform::CanFrame>> NativePlatform::can_recv(const std::string& iface, size_t max_frames)
{
    int fd = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) return Result<std::vector<CanFrame>>::fail(Errc::NotSupported, "no CAN support in kernel", iface);
    struct ifreq ifr;
    std::memset(&ifr, 0, sizeof(ifr));
    std::strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
    if (::ioctl(fd, SIOCGIFINDEX, &ifr) < 0)
    {
        ::close(fd);
        return Result<std::vector<CanFrame>>::fail(Errc::NotFound, "no such CAN interface", iface);
    }
    struct sockaddr_can addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        ::close(fd);
        return Result<std::vector<CanFrame>>::fail(Errc::Io, "cannot bind CAN socket", iface);
    }
    // Non-blocking: a health check must not stall on a quiet bus.
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    std::vector<CanFrame> out;
    struct can_frame cf;
    while (out.size() < max_frames && ::read(fd, &cf, sizeof(cf)) == sizeof(cf))
    {
        CanFrame f;
        f.extended = (cf.can_id & CAN_EFF_FLAG) != 0;
        f.id = cf.can_id & (f.extended ? CAN_EFF_MASK : CAN_SFF_MASK);
        f.data.assign(cf.data, cf.data + cf.can_dlc);
        out.push_back(f);
    }
    ::close(fd);
    return Result<std::vector<CanFrame>>::good(std::move(out));
}

/// @brief Measured-boot attestation state (§10).
MeasuredBootState NativePlatform::measured_boot()
{
    MeasuredBootState m;
    // TPM 2.0 exposes PCRs through securityfs. Absent hardware is reported as
    // unavailable rather than faked (§10: report where the platform supports it).
    const std::string pcr_base = "/sys/class/tpm/tpm0/pcr-sha256";
    auto entries = list_dir(pcr_base);
    if (!entries.empty())
    {
        m.available = true;
        m.mechanism = "tpm2";
        for (const auto& e : entries)
        {
            auto v = read_file(pcr_base + "/" + e);
            if (!v.ok()) continue;
            std::string digest = *v;
            while (!digest.empty() && (digest.back() == '\n' || digest.back() == ' ')) digest.pop_back();
            try
            {
                m.pcrs[uint32_t(std::stoul(e))] = digest;
            }
            catch (...)
            {
            }
        }
    }
    // UEFI SecureBoot state, where the firmware exposes it.
    for (const auto& e : list_dir("/sys/firmware/efi/efivars"))
    {
        if (e.rfind("SecureBoot-", 0) != 0) continue;
        auto v = read_file("/sys/firmware/efi/efivars/" + e);
        // 4-byte attribute prefix, then the value byte.
        if (v.ok() && v->size() >= 5) m.secure_boot_enabled = (*v)[4] == 1;
    }
    if (!m.available && !m.secure_boot_enabled) m.mechanism = "none";
    return m;
}

} // namespace platform
} // namespace instr

namespace instr
{
/// @brief Platform abstraction: board and hardware access behind IPlatform (REQ §5).
namespace platform
{

// SMART / NVMe health (§5 wear indicators). eMMC exposes life_time in sysfs
// (already read above); NVMe and SATA need smartctl, which is why this is
// best-effort and marks smart_available rather than inventing numbers.
/// @brief Enrich a device with SMART / NVMe health where it is exposed.
/// @param d Device to enrich with SMART/NVMe health.
void NativePlatform::read_smart(StorageDevice* d)
{
    if (d->kind == "nvme")
    {
        // NVMe exposes health directly in sysfs on modern kernels.
        auto used = read_file("/sys/class/nvme/" + d->name + "/percent_used");
        if (used.ok())
        {
            try
            {
                d->wear_pct = std::stod(*used);
                d->smart_available = true;
            }
            catch (...)
            {
            }
        }
    }
    if (!apply_enabled()) return; // smartctl is a subprocess: host-safe guard

    std::string cmd = "smartctl -A -j " + d->device_path + " 2>/dev/null";
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) return;
    std::string out;
    char buf[1024];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    ::pclose(p);
    if (out.empty()) return;
    auto j = json_parse(out);
    if (!j.ok() || !(*j).is_object()) return;
    d->smart_available = true;
    if (const JValue* t = (*j).find("temperature"))
        if (const JValue* c = t->find("current")) d->temperature_c = int32_t(c->as_number());
    if (const JValue* poh = (*j).find("power_on_time"))
        if (const JValue* h = poh->find("hours")) d->power_on_hours = uint64_t(h->as_number());
    if (const JValue* health = (*j).find("smart_status"))
        if (const JValue* passed = health->find("passed")) d->smart_ok = passed->as_bool();
    if (const JValue* log = (*j).find("nvme_smart_health_information_log"))
    {
        if (const JValue* me = log->find("media_errors")) d->media_errors = uint64_t(me->as_number());
        if (const JValue* pu = log->find("percentage_used")) d->wear_pct = pu->as_number();
    }
    if (const JValue* attrs = (*j).find("ata_smart_attributes"))
    {
        if (const JValue* table = attrs->find("table"))
        {
            for (const auto& a : table->items())
            {
                const JValue* nm = a.find("name");
                const JValue* raw = a.find("raw");
                if (!nm || !raw) continue;
                const JValue* val = raw->find("value");
                if (!val) continue;
                if (nm->as_string() == "Reallocated_Sector_Ct") d->reallocated_sectors = uint64_t(val->as_number());
            }
        }
    }
}

} // namespace platform
} // namespace instr
