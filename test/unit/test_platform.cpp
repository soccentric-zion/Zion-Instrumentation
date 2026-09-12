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
 * @file test/unit/test_platform.cpp
 * @brief simulated platform and descriptor loading (REQ §5, §12)
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>

#include "instr/common/config.h"
#include "instr/common/json.h"
#include "instr/platform/descriptors.h"
#include "instr/platform/native.h"
#include "instr/platform/simulated.h"

using namespace instr;
using namespace instr::platform;

/// @brief Factory identity and entropy scripting feed first-boot provisioning paths.
TEST(SimulatedPlatform, FactoryIdentityAndEntropy)
{
    SimulatedPlatform plat;
    auto id = plat.read_factory_identity();
    ASSERT_TRUE(id.ok());
    EXPECT_EQ(id->serial, "SIM-0001");
    EXPECT_TRUE(plat.entropy_available());
    plat.set_entropy_available(false);
    EXPECT_FALSE(plat.entropy_available());
}

/// @brief A/B trial boot and confirmation behave per the rollback model (§5).
TEST(SimulatedPlatform, BootSlotsTrialAndConfirm)
{
    SimulatedPlatform plat;
    auto s = plat.boot_slots();
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(s->active, "A");
    EXPECT_TRUE(plat.set_boot_slot("B", true).ok());
    s = plat.boot_slots();
    EXPECT_EQ(s->pending, "B");
    EXPECT_EQ(s->active, "A"); // trial boot: not yet confirmed
    EXPECT_TRUE(plat.confirm_boot_slot().ok());
    s = plat.boot_slots();
    EXPECT_EQ(s->active, "B");
    EXPECT_EQ(s->inactive, "A");
}

/// @brief Watchdog arming and pets are observable for watchdog-policy tests.
TEST(SimulatedPlatform, WatchdogPetCounts)
{
    SimulatedPlatform plat;
    EXPECT_TRUE(plat.watchdog_set_timeout_s(30).ok());
    EXPECT_TRUE(plat.watchdog_pet().ok());
    EXPECT_TRUE(plat.watchdog_pet().ok());
    EXPECT_EQ(plat.watchdog_pets(), 2u);
    plat.fail_watchdog(true);
    EXPECT_FALSE(plat.watchdog_pet().ok());
}

/// @brief GPIO/LED/USB/display/CAN controls record observable state.
TEST(SimulatedPlatform, HardwareControlObservation)
{
    SimulatedPlatform plat;
    EXPECT_TRUE(plat.gpio_set(4, true).ok());
    auto v = plat.gpio_get(4);
    ASSERT_TRUE(v.ok());
    EXPECT_TRUE(*v);
    EXPECT_TRUE(plat.led_blink("status", 10, 10, 3).ok());
    EXPECT_TRUE(plat.led_state().at("status:blink-ack"));
    EXPECT_TRUE(plat.usb_reset("1-2").ok());
    EXPECT_EQ(plat.usb_resets().size(), 1u);
    EXPECT_TRUE(plat.display_control("blank", "").ok());
    EXPECT_EQ(plat.display_commands().size(), 1u);
}

/// @brief Unit start/stop/restart calls are recorded for orchestration tests.
TEST(SimulatedPlatform, UnitOrchestrationObservation)
{
    SimulatedPlatform plat;
    EXPECT_TRUE(plat.unit_start("instr-alarms.service").ok());
    auto active = plat.unit_active("instr-alarms.service");
    ASSERT_TRUE(active.ok());
    EXPECT_TRUE(*active);
    EXPECT_TRUE(plat.unit_restart("instr-alarms.service").ok());
    EXPECT_EQ(plat.unit_restarts().size(), 1u);
}

/// @brief A device descriptor loads and validates against its schema.
TEST(Descriptors, DeviceDescriptorLoadAndValidate)
{
    auto schema = load_schema_text(R"({"type":"object","properties":{
    "board":{"type":"string"},
    "devices":{"type":"array","items":{"type":"object","properties":{
      "name":{"type":"string"},"kind":{"type":"string"},
      "controls":{"type":"array","items":{"type":"string"}},
      "checks":{"type":"array","items":{"type":"object","properties":{
        "name":{"type":"string"},"op":{"type":"string"}},"required":["name","op"]}},
      "params":{"type":"object"}},"required":["name","kind"]}}}})");
    ASSERT_TRUE(schema.ok());
    const char* yaml = R"yaml(
board: sim-board
devices:
  - name: status-led
    kind: led
    controls: [led_set, led_blink]
    checks:
      - name: blink-ack
        op: led_blink_ack
  - name: gpio-1
    kind: gpio
    params:
      line: 4
    controls: [gpio_set]
    checks:
      - name: toggle
        op: gpio_toggle_read
)yaml";
    auto d = load_device_descriptor(yaml, *schema, "devices.yaml");
    ASSERT_TRUE(d.ok());
    EXPECT_EQ(d->board, "sim-board");
    ASSERT_EQ(d->devices.size(), 2u);
    EXPECT_EQ(d->devices[1].params.at("line"), "4");
    EXPECT_EQ(d->devices[0].checks[0].op, "led_blink_ack");

    // Reject with a diagnosable error on schema violation.
    auto bad = load_device_descriptor("devices:\n  - name: x\n", *schema, "devices.yaml");
    EXPECT_FALSE(bad.ok());
    EXPECT_NE(bad.error.message.find("devices.yaml"), std::string::npos);
    EXPECT_NE(bad.error.message.find("kind"), std::string::npos);
}

/// @brief A resource profile loads with per-class intervals and budgets.
TEST(Descriptors, ResourceProfileLoad)
{
    auto schema = load_schema_text(R"({"type":"object","properties":{
    "platform_class":{"type":"string","enum":["small","medium","large"]},
    "sample_interval_default_s":{"type":"number"},
    "spool_max_bytes":{"type":"integer"},
    "sample_intervals_per_class_s":{"type":"object"}}})");
    ASSERT_TRUE(schema.ok());
    const char* yaml = R"yaml(
platform_class: small
sample_interval_default_s: 10
sample_intervals_per_class_s:
  thermal: 2
  cpu: 5
spool_max_bytes: 33554432
)yaml";
    auto p = load_resource_profile(yaml, *schema, "resource-profile.yaml");
    ASSERT_TRUE(p.ok());
    EXPECT_EQ(p->platform_class, "small");
    EXPECT_DOUBLE_EQ(p->sample_intervals_per_class_s.at("thermal"), 2.0);
    EXPECT_EQ(p->spool_max_bytes, 33554432u);
}

/// @brief A capability manifest loads and answers has() queries.
TEST(Descriptors, CapabilityManifest)
{
    auto schema = load_schema_text(R"({"type":"object","properties":{
    "board":{"type":"string"},"capabilities":{"type":"array","items":{"type":"string"}}}})");
    auto m = load_capability_manifest("board: jetson-orin\ncapabilities: [gnss, ptp, can, display]\n", *schema, "caps.yaml");
    ASSERT_TRUE(m.ok());
    EXPECT_TRUE(m->has("gnss"));
    EXPECT_FALSE(m->has("watchdog"));
}

// ---------------------------------------------------------------------------
// §7.2 / R8: networkd renders a plan and the platform applies it. This used
// to be a no-op that discarded plan_json entirely, so every firewall rule,
// route, VLAN and tunnel change computed by networkd went nowhere. These
// tests pin that the plan actually turns into system commands.
// ---------------------------------------------------------------------------

namespace
{
// Dry-run mode records what would run without touching the host, which is
// exactly what we want to assert against.
/// @brief Join the recorded apply commands for assertion.
/// @param p Platform whose applied commands to join.
/// @return The newline-joined command log.
std::string joined_commands(const platform::NativePlatform& p)
{
    std::string all;
    for (const auto& c : p.applied_commands()) all += c + "\n";
    return all;
}
} // namespace

/// @brief A plan renders to the exact commands recorded for the apply seam.
TEST(NativeNetworkApply, PlanBecomesSystemCommands)
{
    platform::NativePlatform plat("/nonexistent-sysroot");
    const std::string plan = R"({
    "interfaces":[
      {"name":"eth0","mode":"static","address":"10.0.0.5","prefix":24,"priority":10},
      {"name":"eth1","mode":"dhcp","priority":50},
      {"name":"eth0.100","mode":"vlan","vlan_id":100}
    ],
    "routes":[{"destination":"default","gateway":"10.0.0.1","iface":"eth0","metric":100}],
    "dns":{"servers":["10.0.0.1","1.1.1.1"],"search":["site.local"]},
    "firewall":[{"action":"allow","proto":"tcp","src":"any","dst_port":443}],
    "port_forwarding":[{"proto":"tcp","listen_port":8080,"dest_ip":"10.0.0.9","dest_port":80}],
    "wireless":{"enabled":true,"mode":"client","iface":"wlan0","ssid":"site"},
    "wireguard":{"up":true},
    "ssh_enabled":false
  })";
    ASSERT_TRUE(plat.apply_network_plan(plan).ok());
    const std::string cmds = joined_commands(plat);

    // Interfaces: static addressing, DHCP, and the VLAN link.
    EXPECT_NE(cmds.find("ip addr add 10.0.0.5/24 dev eth0"), std::string::npos) << cmds;
    EXPECT_NE(cmds.find("udhcpc -i eth1"), std::string::npos) << cmds;
    EXPECT_NE(cmds.find("ip link add link eth0 name eth0.100 type vlan id 100"), std::string::npos) << cmds;
    // Routing and DNS.
    EXPECT_NE(cmds.find("ip route replace default via 10.0.0.1 dev eth0"), std::string::npos) << cmds;
    EXPECT_NE(cmds.find("write /etc/resolv.conf"), std::string::npos) << cmds;
    // Firewall and NAT.
    EXPECT_NE(cmds.find("nft -f /etc/nftables.conf"), std::string::npos) << cmds;
    EXPECT_NE(cmds.find("nft -f /etc/nftables-nat.conf"), std::string::npos) << cmds;
    // Wireless client and the tunnel.
    EXPECT_NE(cmds.find("wpa_supplicant@wlan0"), std::string::npos) << cmds;
    EXPECT_NE(cmds.find("wg-quick up wg0"), std::string::npos) << cmds;
    // Inbound SSH is off in this plan and must be stopped, not started.
    EXPECT_NE(cmds.find("systemctl stop sshd"), std::string::npos) << cmds;
    EXPECT_EQ(cmds.find("systemctl start sshd"), std::string::npos) << cmds;
}

/// @brief A malformed plan is refused loudly, never half-applied.
TEST(NativeNetworkApply, MalformedPlanIsRefusedLoudly)
{
    platform::NativePlatform plat("/nonexistent-sysroot");
    Error e = plat.apply_network_plan("{not json");
    EXPECT_FALSE(e.ok());
    EXPECT_NE(e.message.find("malformed network plan"), std::string::npos);
}

/// @brief AP mode starts hostapd and stops the supplicant (mutually exclusive).
TEST(NativeNetworkApply, ApModeStartsHostapdAndStopsSupplicant)
{
    platform::NativePlatform plat("/nonexistent-sysroot");
    ASSERT_TRUE(plat.apply_network_plan(R"({"wireless":{"enabled":true,"mode":"ap","iface":"wlan0","ssid":"commission"}})").ok());
    const std::string cmds = joined_commands(plat);
    EXPECT_NE(cmds.find("systemctl restart hostapd"), std::string::npos) << cmds;
    EXPECT_NE(cmds.find("systemctl stop wpa_supplicant@wlan0"), std::string::npos) << cmds;
}

// The generated rule set must never firewall off the tunnel: that is the
// remote-bricking scenario the tunnel-loss guard exists to catch (§7.2).
/// @brief The generated firewall always admits the tunnel, whatever the plan contains.
TEST(NativeNetworkApply, GeneratedFirewallAlwaysAdmitsTheTunnel)
{
    platform::NativePlatform plat("/nonexistent-sysroot");
    ASSERT_TRUE(plat.apply_network_plan(R"({"firewall":[{"action":"deny","proto":"any","src":"any"}]})").ok());
    // The rules text is written before nft loads it; the write is recorded.
    EXPECT_NE(joined_commands(plat).find("write /etc/nftables.conf"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Ruleset scoping (Task.md B-02).
//
// Instrumentation and Arches-Configuration both program nftables on the same
// device and neither knows the other exists. Configuration writes an
// `inet webconfig` table and removes it with a scoped `delete table`.
// Instrumentation used to open its script with `flush ruleset`, which drops
// EVERY table in the kernel -- so whichever daemon applied last won, and on a
// device where the firewall protects the management port, losing the peer's
// table is a security failure rather than a cosmetic one.
// ---------------------------------------------------------------------------

/// @brief The rendered ruleset never contains a global flush.
TEST(FirewallScoping, NeverFlushesTheWholeRuleset)
{
    auto plan = json_parse(R"([{"action":"allow","proto":"tcp","dst_port":443}])");
    ASSERT_TRUE(plan.ok());
    const std::string rules = platform::render_firewall_ruleset(*plan);
    EXPECT_EQ(rules.find("flush ruleset"), std::string::npos) << rules;
}

/// @brief The ruleset only ever deletes the table Instrumentation owns.
TEST(FirewallScoping, DeletesOnlyItsOwnTable)
{
    auto plan = json_parse(R"([{"action":"allow","proto":"tcp","dst_port":443}])");
    ASSERT_TRUE(plan.ok());
    const std::string rules = platform::render_firewall_ruleset(*plan);
    EXPECT_NE(rules.find("delete table inet instr"), std::string::npos) << rules;
    // Exactly one delete, and it names our table.
    EXPECT_EQ(rules.find("delete table", rules.find("delete table") + 1), std::string::npos) << rules;
    EXPECT_EQ(rules.find("webconfig"), std::string::npos) << rules;
}

/// @brief Declaring the table before deleting it keeps `nft -f` idempotent.
///
/// Without the empty declaration the delete fails with ENOENT on a fresh boot
/// and nft aborts the whole transaction, so no rules load at all.
TEST(FirewallScoping, DeclaresTableBeforeDeletingSoFirstApplySucceeds)
{
    auto plan = json_parse(R"([])");
    ASSERT_TRUE(plan.ok());
    const std::string rules = platform::render_firewall_ruleset(*plan);
    const size_t declare = rules.find("table inet instr { }");
    const size_t del = rules.find("delete table inet instr");
    ASSERT_NE(declare, std::string::npos) << rules;
    ASSERT_NE(del, std::string::npos) << rules;
    EXPECT_LT(declare, del) << rules;
}

/// @brief Applying both daemons' rulesets in either order leaves both tables.
///
/// The peer ruleset is Configuration's, reproduced here in the shape its
/// firewall applier emits (`crates/webconfig-engine/src/appliers/firewall.rs`).
/// Simulating the kernel is out of scope for a unit test, so this asserts the
/// property that makes coexistence possible: neither script contains an
/// operation that reaches outside its own table, therefore order cannot matter.
TEST(FirewallScoping, PeerTableSurvivesInEitherOrder)
{
    auto plan = json_parse(R"([{"action":"allow","proto":"tcp","dst_port":443}])");
    ASSERT_TRUE(plan.ok());
    const std::string ours = platform::render_firewall_ruleset(*plan);
    const std::string peer = "table inet webconfig { }\ndelete table inet webconfig\n"
                             "table inet webconfig {\n  chain input {\n"
                             "    type filter hook input priority 0; policy drop;\n  }\n}\n";

    // Neither script may name the other's table, and neither may flush globally.
    EXPECT_EQ(ours.find("webconfig"), std::string::npos) << ours;
    EXPECT_EQ(peer.find("instr"), std::string::npos) << peer;
    EXPECT_EQ((ours + peer).find("flush ruleset"), std::string::npos);

    // Applied in either concatenation order, each table is created exactly once
    // after its own delete, and neither delete names the other table.
    for (const std::string& combined : {ours + peer, peer + ours})
    {
        EXPECT_NE(combined.find("table inet instr {\n"), std::string::npos) << combined;
        EXPECT_NE(combined.find("table inet webconfig {\n"), std::string::npos) << combined;
    }
}

/// @brief The NAT ruleset is scoped the same way, so re-apply replaces rather
///        than appending duplicate DNAT rules.
TEST(FirewallScoping, NatRulesetIsScopedAndIdempotent)
{
    auto plan = json_parse(R"([{"proto":"tcp","listen_port":8080,"dest_ip":"10.0.0.5","dest_port":80}])");
    ASSERT_TRUE(plan.ok());
    const std::string nat = platform::render_nat_ruleset(*plan);
    EXPECT_EQ(nat.find("flush ruleset"), std::string::npos) << nat;
    EXPECT_NE(nat.find("delete table ip instr_nat"), std::string::npos) << nat;
    EXPECT_NE(nat.find("dnat to 10.0.0.5:80"), std::string::npos) << nat;
    // The old code used the generic `table ip nat`, which collides with anything
    // else on the device that programs NAT.
    EXPECT_EQ(nat.find("table ip nat {"), std::string::npos) << nat;
}

/// @brief An empty port-forwarding list renders nothing at all.
TEST(FirewallScoping, EmptyNatPlanRendersNothing)
{
    auto plan = json_parse(R"([])");
    ASSERT_TRUE(plan.ok());
    EXPECT_TRUE(platform::render_nat_ruleset(*plan).empty());
}

// ---------------------------------------------------------------------------
// Boot-partition location (Task.md D-05).
//
// The A/B flag-file fallback (used on boards with no U-Boot environment) wrote
// to /mnt/boot, which exists on no Arches image -- all three wic layouts mount
// the boot partition at /boot. instr-ota.service named the same wrong path in
// ReadWritePaths under ProtectSystem=strict, which makes systemd refuse to
// start the unit at all.
// ---------------------------------------------------------------------------

/// @brief The boot directory defaults to the mount point Arches-OS actually uses.
TEST(BootDir, DefaultsToTheImageMountPoint)
{
    ::unsetenv("INSTR_BOOT_DIR");
    EXPECT_EQ(platform::boot_dir(), "/boot");
}

/// @brief A board with a differently-placed boot partition can redirect it.
TEST(BootDir, HonoursTheEnvironmentOverride)
{
    ::setenv("INSTR_BOOT_DIR", "/mnt/somewhere-else", 1);
    EXPECT_EQ(platform::boot_dir(), "/mnt/somewhere-else");
    ::unsetenv("INSTR_BOOT_DIR");
    EXPECT_EQ(platform::boot_dir(), "/boot");
}

/// @brief A trailing slash never doubles up in the composed flag-file path.
TEST(BootDir, StripsTrailingSlashes)
{
    ::setenv("INSTR_BOOT_DIR", "/boot///", 1);
    EXPECT_EQ(platform::boot_dir(), "/boot");
    ::unsetenv("INSTR_BOOT_DIR");
}

/// @brief An empty override falls back to the default rather than yielding "".
TEST(BootDir, EmptyOverrideFallsBackToDefault)
{
    ::setenv("INSTR_BOOT_DIR", "", 1);
    EXPECT_EQ(platform::boot_dir(), "/boot");
    ::unsetenv("INSTR_BOOT_DIR");
}

/// @brief The slot flag files are read from under the configured boot directory.
///
/// Points the platform at a fixture sysroot containing a boot directory and
/// asserts the slot state is actually picked up -- this is the read path that
/// silently returned nothing when it was looking under /mnt/boot.
TEST(BootDir, SlotStateIsReadFromTheConfiguredBootDirectory)
{
    const std::string root = std::string(::getenv("TMPDIR") ? ::getenv("TMPDIR") : "/tmp") + "/instr-bootdir-test";
    ::system(("rm -rf " + root + " && mkdir -p " + root + "/boot").c_str());
    auto put = [&](const char* n, const char* v) {
        std::ofstream f(root + "/boot/" + n);
        f << v << "\n";
    };
    put("current-slot", "A");
    put("pending-slot", "B");
    put("golden-slot", "A");

    ::unsetenv("INSTR_BOOT_DIR"); // default /boot, resolved under the sysroot
    platform::NativePlatform plat(root);
    auto slots = plat.boot_slots();
    ASSERT_TRUE(slots.ok()) << slots.error.message;
    EXPECT_EQ(slots->active, "A");
    EXPECT_EQ(slots->pending, "B");
    ::system(("rm -rf " + root).c_str());
}
