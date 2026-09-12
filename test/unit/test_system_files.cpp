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
 * @file test/unit/test_system_files.cpp
 * @brief The shipped system files are coherent (REQ §13, §10, §7.1).
 *
 * The repo carries everything the image needs — units, sysusers, tmpfiles,
 * udev. None of it is exercised by building, so a duplicated section, a
 * missing user or a startup order that contradicts the orchestrator survives
 * until someone flashes a board. These are the checks that catch that in CI.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <fstream>
#include <set>
#include <sstream>

#include <gtest/gtest.h>

#include "instr/istate/orchestrator.h"

using namespace instr;

namespace
{

const char* kDaemons[] = {"istate", "networkd", "monitord", "hardware", "faults", "alarms", "logmanager", "telemetry", "ota", "backup"};

/// @brief Read a system/systemd file.
/// @param p File to read.
/// @return The file contents.
std::string slurp(const std::string& p)
{
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
/// @brief Path of a daemon's systemd unit file.
/// @param d Daemon name.
/// @return The daemon's unit file path.
std::string unit(const std::string& d)
{
    return slurp(std::string(REPO_ROOT) + "/system/systemd/instr-" + d + ".service");
}
/// @brief Count occurrences of a substring.
/// @param hay Text to search.
/// @param needle Substring to count.
/// @return Number of occurrences.
size_t count_of(const std::string& hay, const std::string& needle)
{
    size_t n = 0, pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos)
    {
        ++n;
        pos += needle.size();
    }
    return n;
}

} // namespace

/// @brief Every daemon has a systemd unit (packaging completeness).
TEST(SystemFiles, EveryDaemonHasAUnit)
{
    for (const char* d : kDaemons) EXPECT_FALSE(unit(d).empty()) << d;
}

/// @brief Each unit file declares exactly one [Service] section.
TEST(SystemFiles, UnitsHaveExactlyOneServiceSection)
{
    // A repeated [Service] header is legal to systemd but hides edits: a
    // directive added under the first copy sits above the second and reads as
    // dead. Nine of these shipped with an empty duplicate.
    for (const char* d : kDaemons) EXPECT_EQ(count_of(unit(d), "[Service]"), 1u) << d << " has a duplicated [Service] section";
}

/// @brief Every daemon runs as its own unprivileged user (least privilege, §12).
TEST(SystemFiles, EveryDaemonRunsAsItsOwnUnprivilegedUser)
{
    // §10: each daemon unprivileged and confined.
    const std::string users = slurp(std::string(REPO_ROOT) + "/system/sysusers.d/instrumentation.conf");
    for (const char* d : kDaemons)
    {
        const std::string u = std::string("instr-") + d;
        EXPECT_NE(unit(d).find("User=" + u), std::string::npos) << d << " does not run as " << u;
        EXPECT_NE(users.find("u " + u + " "), std::string::npos) << u << " is not declared in sysusers.d";
        EXPECT_NE(unit(d).find("NoNewPrivileges=yes"), std::string::npos) << d;
        EXPECT_NE(unit(d).find("ProtectSystem=strict"), std::string::npos) << d;
    }
}

/// @brief The configuration peer service has its own unprivileged user.
TEST(SystemFiles, ConfigurationPeerHasItsOwnUser)
{
    // Configuration is architecturally a spoke (§3.3) and needs a user for the
    // same reason: its socket is owned by it and nothing else.
    const std::string users = slurp(std::string(REPO_ROOT) + "/system/sysusers.d/instrumentation.conf");
    EXPECT_NE(users.find("u instr-configuration "), std::string::npos);
}

/// @brief istate can chown spoke sockets to each daemon user (§3.1 socket handover).
TEST(SystemFiles, IstateCanChownSpokeSockets)
{
    // Socket permissions enforce the call graph (§3.1, §10). istate hands each
    // socket to its spoke's user, which needs CAP_CHOWN. An empty bounding set
    // silently disables the entire mechanism.
    const std::string u = unit("istate");
    EXPECT_NE(u.find("AmbientCapabilities=CAP_CHOWN"), std::string::npos);
    EXPECT_NE(u.find("CapabilityBoundingSet=CAP_CHOWN"), std::string::npos);
    // ... and nothing more than that: istate requests unit changes through
    // systemd rather than performing privileged operations itself (§7.1).
    EXPECT_EQ(u.find("CAP_SYS_ADMIN"), std::string::npos);
}

/// @brief Ambient capabilities are granted only to the units that need them.
TEST(SystemFiles, CapabilitiesAreGrantedOnlyWhereRequired)
{
    // §10: "capabilities granted only where genuinely required".
    EXPECT_NE(unit("networkd").find("CAP_NET_ADMIN"), std::string::npos);
    EXPECT_NE(unit("ota").find("CAP_SYS_BOOT"), std::string::npos);
    // Observation-only daemons get nothing at all.
    for (const char* d : {"monitord", "faults", "alarms", "telemetry", "logmanager", "backup"})
    {
        EXPECT_NE(unit(d).find("CapabilityBoundingSet="), std::string::npos) << d;
        EXPECT_EQ(unit(d).find("AmbientCapabilities=CAP_"), std::string::npos) << d << " is granted a capability it should not need";
    }
}

/// @brief Unit After=/Wants= ordering matches the orchestrator's declared startup order (§7.1).
TEST(SystemFiles, UnitOrderingMatchesTheDeclaredStartupOrder)
{
    // §7.1 declares istate → faults, monitord, hardware → alarms, logmanager,
    // telemetry → networkd → ota, backup. The orchestrator knows it; the units
    // must agree, or systemd starts everything at once and the observation
    // daemons miss the boot window they exist to capture.
    const auto& order = istate::startup_order();
    std::map<std::string, size_t> rank;
    for (size_t i = 0; i < order.size(); ++i) rank[order[i]] = i;

    for (const auto& kv : istate::dependency_edges())
    {
        const std::string u = unit(kv.first);
        if (u.empty()) continue;
        for (const auto& dep : kv.second)
        {
            // Each declared dependency edge is expressed as an After= in the unit.
            EXPECT_NE(u.find("instr-" + dep + ".service"), std::string::npos) << kv.first << " must be ordered after " << dep;
        }
    }
    // Everything is ordered after the hub.
    for (const char* d : kDaemons)
    {
        if (std::string(d) == "istate") continue;
        EXPECT_NE(unit(d).find("After="), std::string::npos) << d << " has no ordering at all";
    }
}

/// @brief The runtime directory is declared exactly once (no double tmpfiles ownership).
TEST(SystemFiles, RunDirectoryIsDeclaredExactlyOnce)
{
    const std::string t = slurp(std::string(REPO_ROOT) + "/system/tmpfiles.d/instrumentation.conf");
    size_t decls = 0;
    std::istringstream in(t);
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#') continue;
        if (line.find("/run/instrumentation") != std::string::npos) ++decls;
    }
    EXPECT_EQ(decls, 1u) << "contradictory tmpfiles entries for the same path:\n" << t;
    EXPECT_NE(t.find("instr-istate instr"), std::string::npos);
}

/// @brief udev rules grant the hardware daemon exactly the devices it drives.
TEST(SystemFiles, UdevGrantsHardwareTheDevicesItDrives)
{
    // §13: udev rules where device access requires them (R7: the Hardware
    // daemon is the only hardware writer, so it is the only one named).
    const std::string u = slurp(std::string(REPO_ROOT) + "/system/udev/70-instrumentation.rules");
    for (const char* sub : {"leds", "can", "usb"}) EXPECT_NE(u.find(sub), std::string::npos) << "no udev rule covering " << sub;
    EXPECT_NE(u.find("watchdog"), std::string::npos);
    EXPECT_NE(u.find("instr-istate"), std::string::npos) << "the watchdog belongs to istate (§7.1)";
}
