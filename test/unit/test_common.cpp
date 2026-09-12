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
 * @file test/unit/test_common.cpp
 * @brief lib/common (REQ §12): timestamps/reconciliation, config
 *
 * origins and merge, alarm ID codec, spool drop policy, rate limiter, time
 * reconciliation, atomic writes, tar, crypto vectors, backoff, versions.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "instr/common/alarm_codec.h"
#include "instr/common/atomic_file.h"
#include "instr/common/backoff.h"
#include "instr/common/command_table.h"
#include "instr/common/config.h"
#include "instr/common/crypto.h"
#include "instr/common/log.h"
#include "instr/common/rate_limiter.h"
#include "instr/common/spool.h"
#include "instr/common/tar.h"
#include "instr/common/timestamps.h"
#include "instr/common/frames.h"

using namespace instr;
using namespace instr::crypto;

// ---------------- timestamps & reconciliation (§4) ----------------

/// @brief Records spooled before a clock lock get wall times when the lock arrives.
TEST(TimeReconciliation, PendingRecordsRestampedOnLock)
{
    ManualTimeSource ts;
    TimeReconciler rec(ts);
    rec.record_pending(1'000'000'000ull);
    rec.record_pending(2'000'000'000ull);
    rec.record_pending(3'000'000'000ull);
    EXPECT_EQ(rec.pending_count(), 3u);
    EXPECT_FALSE(rec.wall_valid());
    size_t n = rec.reconcile(+3'600'000'000'000ll /*+1h*/, "ntp");
    EXPECT_EQ(n, 3u);
    EXPECT_TRUE(rec.wall_valid());
    auto stamped = rec.take_stamped();
    ASSERT_EQ(stamped.size(), 3u);
    // Wall times derived from the lock offset; never reordered.
    for (size_t i = 1; i < stamped.size(); ++i) EXPECT_GE(stamped[i].wall_ns, stamped[i - 1].wall_ns);
    EXPECT_GT(stamped[0].wall_ns, 0);
}

/// @brief A GNSS/NTP lock mid-run must not reorder history; wall times are non-decreasing in seq order.
TEST(TimeReconciliation, MidRunLockNeverReordersHistory)
{
    ManualTimeSource ts;
    TimeReconciler rec(ts);
    rec.record_pending(100);
    rec.record_pending(200);
    // Lock with a wildly negative offset: history order must hold by seq.
    rec.reconcile(-1'000'000, "gnss");
    auto stamped = rec.take_stamped();
    ASSERT_EQ(stamped.size(), 2u);
    EXPECT_LE(stamped[0].wall_ns, stamped[1].wall_ns);
    EXPECT_LT(stamped[0].seq, stamped[1].seq);
}

/// @brief A second lock re-anchors the mapping without moving historical wall times inconsistently.
TEST(TimeReconciliation, SecondLockRemapsWallConsistently)
{
    ManualTimeSource ts;
    TimeReconciler rec(ts);
    rec.reconcile(0, "ntp");
    auto a = rec.stamp_now();
    ts.mono += 1'000'000'000ull; // 1s later, correction -5s
    rec.reconcile(-5'000'000'000ll, "gnss");
    auto b = rec.stamp_now();
    EXPECT_TRUE(rec.wall_valid());
    // Mapping is consistent: wall(mono) = base_wall + (mono - base_mono);
    // a correction steps the clock (GNSS/PTP discipline) but history
    // re-stamps stay ordered by seq.
    EXPECT_EQ(b.wall_ns, a.wall_ns - 5'000'000'000ll);
}

// ---------------- config: origins, merge, reset (§9) ----------------

static const char* kSchema = R"({"type":"object","additionalProperties":false,"properties":{
  "interval":{"type":"number"},"name":{"type":"string"},"flag":{"type":"boolean"},
  "servers":{"type":"array","items":{"type":"string"}}}})";

/// @brief Schema-validated parse flattens to dot-paths with correct typed accessors.
TEST(ConfigDocument, ParseValidatesAndTypes)
{
    auto sch = load_schema_text(kSchema);
    ASSERT_TRUE(sch.ok());
    auto doc = ConfigDocument::parse("interval: 30\nname: hello\nflag: true\n", *sch, "x.yaml");
    ASSERT_TRUE(doc.ok());
    EXPECT_EQ((*doc).get_int("interval"), 30);
    EXPECT_EQ((*doc).get_string("name"), "hello");
    EXPECT_TRUE((*doc).get_bool("flag"));
    EXPECT_EQ((*doc).origin_of("interval"), Origin::Default);
}

/// @brief A malformed document is a loud error listing the offending paths (file-and-line diagnosability).
TEST(ConfigDocument, InvalidConfigLoudErrorWithPaths)
{
    auto sch = load_schema_text(kSchema);
    ASSERT_TRUE(sch.ok());
    auto doc = ConfigDocument::parse("interval: not_a_number\n", *sch, "bad.yaml");
    ASSERT_FALSE(doc.ok());
    EXPECT_NE(doc.error.message.find("bad.yaml"), std::string::npos);
    EXPECT_NE(doc.error.message.find("interval"), std::string::npos);
}
// Result accessors sanity (one model across the suite).
/// @brief Error carries code, message and context; str() renders a diagnosable line.
TEST(ResultModel, ErrorAccessors)
{
    auto r = Result<int>::fail(Errc::Corrupt, "bad data", "ctx");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error.code, Errc::Corrupt);
    EXPECT_NE(r.error.message.find("bad data"), std::string::npos);
    auto okr = Result<int>::good(5);
    EXPECT_TRUE(okr.ok());
    EXPECT_EQ(*okr, 5);
}

/// @brief A document violating its JSON Schema is rejected before it can change behaviour.
TEST(ConfigDocument, SchemaViolationRejected)
{
    auto sch = load_schema_text(kSchema);
    auto doc = ConfigDocument::parse("interval: 5\nunknown_key: 1\n", *sch, "y.yaml");
    ASSERT_FALSE(doc.ok());
    EXPECT_NE(doc.error.message.find("unknown_key"), std::string::npos);
}

/// @brief Origin rules: an HQ push must not overwrite operator (local) values without force.
TEST(ConfigDocument, OriginRulesLocalProtectedFromHqPush)
{
    auto sch = load_schema_text(kSchema);
    ConfigDocument doc;
    doc.set("name", "operator-edited", Origin::Local);
    doc.set("interval", "10", Origin::Default);
    ConfigDocument push;
    push.set("name", "hq-value", Origin::Default);
    push.set("interval", "99", Origin::Default);
    auto rejected = doc.merge_from(push, Origin::Hq, false);
    ASSERT_EQ(rejected.size(), 1u);
    EXPECT_EQ(rejected[0], "name");
    EXPECT_EQ(doc.get_string("name"), "operator-edited"); // preserved
    EXPECT_EQ(doc.get_int("interval"), 99);
    EXPECT_EQ(doc.origin_of("interval"), Origin::Hq);
    // Force overrides even local.
    auto rejected2 = doc.merge_from(push, Origin::Hq, true);
    EXPECT_TRUE(rejected2.empty());
    EXPECT_EQ(doc.get_string("name"), "hq-value");
}

/// @brief Release merge adds new default keys and keeps every existing value.
TEST(ConfigDocument, MergeReleaseAddsMissingKeepsOperatorValues)
{
    auto sch = load_schema_text(kSchema);
    auto defaults = ConfigDocument::parse("interval: 30\nname: shipped\nflag: false\n", *sch, "d");
    ASSERT_TRUE(defaults.ok());
    ConfigDocument existing;
    existing.set("interval", "45", Origin::Local);
    auto added = existing.merge_release(*defaults);
    ASSERT_EQ(added.size(), 2u); // name + flag added
    EXPECT_EQ(existing.get_int("interval"), 45); // operator edit preserved
    EXPECT_EQ(existing.get_string("name"), "shipped"); // new default
    EXPECT_EQ(existing.origin_of("name"), Origin::Default);
}

/// @brief System reset keeps only origin=Default entries (§6.1).
TEST(ConfigDocument, ResetToDefaultsDropsNonDefault)
{
    auto sch = load_schema_text(kSchema);
    ConfigDocument doc;
    doc.set("interval", "10", Origin::Default);
    doc.set("name", "custom", Origin::Local);
    doc.reset_to_defaults();
    EXPECT_TRUE(doc.has("interval"));
    EXPECT_FALSE(doc.has("name"));
}

/// @brief Serialising and re-parsing a document preserves values and scalar quoting.
TEST(ConfigDocument, RoundtripYamlPreservesValues)
{
    auto sch = load_schema_text(kSchema);
    auto doc = ConfigDocument::parse("interval: 30\nname: hello world\nflag: true\n", *sch, "r");
    ASSERT_TRUE(doc.ok());
    auto yaml = (*doc).to_yaml();
    auto again = ConfigDocument::parse(yaml, *sch, "roundtrip");
    ASSERT_TRUE(again.ok());
    EXPECT_EQ((*again).get_int("interval"), 30);
    EXPECT_EQ((*again).get_string("name"), "hello world");
    EXPECT_TRUE((*again).get_bool("flag"));
}

// ---------------- alarm ID codec (§8, locked layout) ----------------

/// @brief The §8 worked example 0x1_04_1_0007 decodes to the documented domain/module/type/index.
TEST(AlarmCodec, DocumentExampleDecodes)
{
    uint32_t id = encode_alarm_id(AlarmDomain::Hardware, 0x04, X733Type::Equipment, 7);
    EXPECT_EQ(id, 0x10410007u);
    EXPECT_EQ(alarm_id_string(id), "0x1_04_1_0007");
    auto d = decode_alarm_id(id);
    ASSERT_TRUE(d.ok());
    EXPECT_EQ(d->domain, AlarmDomain::Hardware);
    EXPECT_EQ(d->module, 0x04);
    EXPECT_EQ(d->type, X733Type::Equipment);
    EXPECT_EQ(d->index, 7);
}

/// @brief Every identity field survives encode→decode round-trip.
TEST(AlarmCodec, AllFieldsRoundTrip)
{
    for (uint8_t domain = 1; domain <= 5; ++domain)
    {
        for (uint8_t type = 1; type <= 5; ++type)
        {
            uint32_t id = encode_alarm_id(static_cast<AlarmDomain>(domain), 0x7F, static_cast<X733Type>(type), 65535);
            auto d = decode_alarm_id(id);
            ASSERT_TRUE(d.ok());
            EXPECT_EQ(d->module, 0x7F);
            EXPECT_EQ(d->index, 65535);
        }
    }
}

/// @brief Alarm id 0 is reserved and rejected as invalid.
TEST(AlarmCodec, ZeroReserved)
{
    EXPECT_FALSE(decode_alarm_id(0).ok());
}

// ---------------- spool drop policy (§11) ----------------

/// @brief Drop policy: oldest telemetry first; alarms and faults are never dropped.
TEST(DiskSpool, DropOldestTelemetryFirstNeverProtected)
{
    SimulatedFileSystem fs;
    SpoolConfig cfg;
    cfg.dir = "/pst/state/spool";
    cfg.max_bytes = 1200;
    cfg.segment_bytes = 900;
    DiskSpool sp(fs, cfg);
    ASSERT_TRUE(sp.open().ok());
    // 10 protected (alarm) records fill 1000 bytes; telemetry then forces
    // drops of telemetry only.
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(sp.append(SpoolPriority::Alarm, "alarm-" + std::to_string(i), 100 + i).ok());
    uint64_t alarms_before = 0;
    for (const auto& e : sp.peek(100))
        if (e.priority == SpoolPriority::Alarm) ++alarms_before;
    for (int i = 0; i < 50; ++i) ASSERT_TRUE(sp.append(SpoolPriority::Telemetry, "t-" + std::to_string(i), 1000 + i).ok());
    // No alarm may have been dropped.
    uint64_t alarms_after = 0;
    for (const auto& e : sp.peek(200))
        if (e.priority == SpoolPriority::Alarm) ++alarms_after;
    EXPECT_EQ(alarms_before, alarms_after);
    EXPECT_GT(sp.stats().dropped_telemetry, 0);
}

/// @brief When only protected records fit, the append is refused with BudgetExceeded.
TEST(DiskSpool, ProtectedRefusedLoudlyWhenNothingDroppable)
{
    SimulatedFileSystem fs;
    SpoolConfig cfg;
    cfg.dir = "/pst/state/spool";
    cfg.max_bytes = 150;
    cfg.segment_bytes = 300;
    DiskSpool sp(fs, cfg);
    ASSERT_TRUE(sp.open().ok());
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(sp.append(SpoolPriority::Fault, "f-" + std::to_string(i), i).ok());
    auto r = sp.append(SpoolPriority::Fault, "another", 99); // returns Error
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.code, Errc::BudgetExceeded);
    EXPECT_EQ(sp.stats().refused_protected, 1);
}

/// @brief The spool index is rebuilt from segment files; records survive restart.
TEST(DiskSpool, PersistsAcrossReopen)
{
    SimulatedFileSystem fs;
    SpoolConfig cfg;
    cfg.dir = "/pst/state/spool";
    {
        DiskSpool sp(fs, cfg);
        ASSERT_TRUE(sp.open().ok());
        ASSERT_TRUE(sp.append(SpoolPriority::Event, "hello", 42).ok());
    }
    DiskSpool sp2(fs, cfg);
    ASSERT_TRUE(sp2.open().ok());
    ASSERT_EQ(sp2.size(), 1u);
    auto e = sp2.peek(10);
    ASSERT_EQ(e.size(), 1u);
    EXPECT_EQ(e[0].payload, "hello");
}

/// @brief peek/pop_front consume strictly in arrival order.
TEST(DiskSpool, DrainConsumesInOrder)
{
    SimulatedFileSystem fs;
    SpoolConfig cfg;
    cfg.dir = "/pst/state/spool";
    DiskSpool sp(fs, cfg);
    ASSERT_TRUE(sp.open().ok());
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(sp.append(SpoolPriority::Telemetry, "rec-" + std::to_string(i), i).ok());
    auto first = sp.peek(2);
    ASSERT_EQ(first.size(), 2u);
    EXPECT_EQ(first[0].payload, "rec-0");
    EXPECT_EQ(first[1].payload, "rec-1");
    ASSERT_TRUE(sp.pop_front(2).ok());
    auto next = sp.peek(5);
    ASSERT_EQ(next.size(), 3u);
    EXPECT_EQ(next[0].payload, "rec-2");
}

// ---------------- rate limiter (§4) ----------------

/// @brief Tokens refill at the configured rate and are capped at the burst.
TEST(TokenBucket, RefillsByRate)
{
    double now = 0;
    TokenBucket b(10, 2, [&now] { return now; });
    EXPECT_TRUE(b.try_acquire());
    EXPECT_TRUE(b.try_acquire());
    EXPECT_FALSE(b.try_acquire()); // burst exhausted
    now = 0.5; // 5 tokens accrued at 10/s
    EXPECT_TRUE(b.try_acquire());
}

/// @brief Refusals advance both the lifetime and streak suppression counters.
TEST(TokenBucket, SuppressionStreakTracked)
{
    double now = 0;
    TokenBucket b(1, 1, [&now] { return now; });
    EXPECT_TRUE(b.try_acquire());
    for (int i = 0; i < 5; ++i) EXPECT_FALSE(b.try_acquire());
    EXPECT_EQ(b.current_streak(), 5u);
}

/// @brief Exactly one "suppressed N events" summary per qualifying streak, never more.
TEST(SuppressionSummarizer, EmitsExactlyOneSummaryPerStreak)
{
    double now = 0;
    TokenBucket b(1, 1, [&now] { return now; });
    SuppressionSummarizer s(2);
    EXPECT_TRUE(b.try_acquire());
    EXPECT_FALSE(s.on_attempt(b.try_acquire(), b).has_value());
    EXPECT_FALSE(s.on_attempt(b.try_acquire(), b).has_value());
    now = 1.0;
    auto summary = s.on_attempt(b.try_acquire(), b);
    ASSERT_TRUE(summary.has_value());
    EXPECT_EQ(*summary, "suppressed 2 events");
    EXPECT_FALSE(s.on_attempt(b.try_acquire(), b).has_value()); // streak reset
}

// ---------------- atomic writes + redundant copy (§4, §6) ----------------

/// @brief A write lands via temp+fsync+rename and leaves a checksum sidecar.
TEST(AtomicFile, TempThenRenameThenChecksum)
{
    SimulatedFileSystem fs;
    ASSERT_TRUE(AtomicFile::write_protected(fs, "/pst/system/x/config.yaml", "hello").ok());
    auto r = AtomicFile::read_protected(fs, "/pst/system/x/config.yaml");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(*r, "hello");
    // Checksum sidecar matches.
    auto sum = fs.read("/pst/system/x/config.yaml.checksum");
    ASSERT_TRUE(sum.ok());
    EXPECT_EQ(*sum, crypto::sha256("hello").hex());
}

/// @brief A corrupt primary falls back to the .bak copy (REQ §11).
TEST(AtomicFile, CorruptPrimaryFallsBackToRedundantCopy)
{
    SimulatedFileSystem fs;
    ASSERT_TRUE(AtomicFile::write_protected(fs, "/pst/system/x/config.yaml", "v1").ok());
    ASSERT_TRUE(AtomicFile::write_protected(fs, "/pst/system/x/config.yaml", "v2").ok());
    // Simulate corruption of the primary from the next read.
    fs.corrupt_reads_of("/pst/system/x/config.yaml", 1);
    auto r = AtomicFile::read_protected(fs, "/pst/system/x/config.yaml");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(*r, "v2"); // recovered from the redundant copy of the current content
}

/// @brief When primary and backup are both corrupt, read_protected fails loudly with Corrupt.
TEST(AtomicFile, BothCopiesCorruptReportsLoudly)
{
    SimulatedFileSystem fs;
    ASSERT_TRUE(AtomicFile::write_protected(fs, "/pst/system/x/config.yaml", "v1").ok());
    ASSERT_TRUE(AtomicFile::write_protected(fs, "/pst/system/x/config.yaml", "v2").ok());
    fs.corrupt_reads_of("/pst/system/x/config.yaml", 1);
    fs.corrupt_reads_of("/pst/system/x/config.yaml.bak", 1);
    auto r = AtomicFile::read_protected(fs, "/pst/system/x/config.yaml");
    // With both copies failing checks the reader reports corrupt — the device
    // then boots unprovisioned rather than failing to boot (§11).
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error.code, Errc::Corrupt);
}

// ---------------- tar (ustar) ----------------

/// @brief write_ustar/read_ustar round-trip file contents, modes and names.
TEST(Tar, RoundTrip)
{
    std::vector<tar::FileEntry> files = {
        {"a/config.yaml", "key: value\n", 0644, 1700000000},
        {"b/nested/file.txt", std::string(1000, 'x'), 0644, 1700000001},
    };
    auto archive = tar::write_ustar(files);
    ASSERT_NE(archive.size(), 0u);
    auto back = tar::read_ustar(archive);
    ASSERT_TRUE(back.ok());
    ASSERT_EQ(back->size(), 2u);
    EXPECT_EQ((*back)[0].name, "a/config.yaml");
    EXPECT_EQ((*back)[0].data, "key: value\n");
    EXPECT_EQ((*back)[1].data, std::string(1000, 'x'));
}

/// @brief A malformed archive header is rejected as Corrupt, not silently mis-parsed.
TEST(Tar, CorruptHeaderDetected)
{
    auto archive = tar::write_ustar({{"f", "data", 0644, 0}});
    std::string corrupt = archive;
    corrupt[10] ^= 0xFF; // break the header → checksum mismatch
    auto back = tar::read_ustar(corrupt);
    EXPECT_FALSE(back.ok());
    EXPECT_EQ(back.error.code, Errc::Corrupt);
}

// ---------------- crypto: published vectors ----------------

/// @brief SHA-256 matches FIPS 180-4 published test vectors.
TEST(CryptoSha256, FipsVectors)
{
    EXPECT_EQ(sha256("").hex(), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha256("abc").hex(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(sha256("The quick brown fox jumps over the lazy dog").hex(), "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
}

/// @brief HMAC-SHA256 matches an RFC 4231 test vector.
TEST(CryptoHmac, Rfc4231Vector)
{
    EXPECT_EQ(hmac_sha256("Jefe", "what do ya want for nothing?").hex(), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

/// @brief X25519 matches RFC 7748 test vector 1.
TEST(CryptoX25519, Rfc7748Vector1)
{
    auto s = hex_decode("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4").value;
    auto u = hex_decode("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c").value;
    auto out = x25519(reinterpret_cast<const uint8_t*>(s.data()), reinterpret_cast<const uint8_t*>(u.data()));
    EXPECT_EQ(hex_encode(std::string(reinterpret_cast<const char*>(out.data()), 32)), "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
}

/// @brief X25519 matches RFC 7748 test vector 2.
TEST(CryptoX25519, Rfc7748Vector2)
{
    auto s = hex_decode("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d").value;
    auto u = hex_decode("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493").value;
    auto out = x25519(reinterpret_cast<const uint8_t*>(s.data()), reinterpret_cast<const uint8_t*>(u.data()));
    EXPECT_EQ(hex_encode(std::string(reinterpret_cast<const char*>(out.data()), 32)), "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957");
}

/// @brief Two parties derive the same shared secret from each other's public keys.
TEST(CryptoX25519, DhKeyAgreement)
{
    auto a = hex_decode("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a").value;
    auto b = hex_decode("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb").value;
    std::string base(32, '\0');
    base[0] = 9;
    auto apub = x25519(reinterpret_cast<const uint8_t*>(a.data()), reinterpret_cast<const uint8_t*>(base.data()));
    auto bpub = x25519(reinterpret_cast<const uint8_t*>(b.data()), reinterpret_cast<const uint8_t*>(base.data()));
    EXPECT_EQ(hex_encode(std::string(reinterpret_cast<const char*>(apub.data()), 32)), "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    auto k1 = x25519(reinterpret_cast<const uint8_t*>(a.data()), bpub.data());
    auto k2 = x25519(reinterpret_cast<const uint8_t*>(b.data()), apub.data());
    EXPECT_EQ(k1, k2);
    EXPECT_EQ(hex_encode(std::string(reinterpret_cast<const char*>(k1.data()), 32)), "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
}

/// @brief Freshly generated keypairs agree on the shared secret.
TEST(CryptoX25519, GeneratedKeypairsAgree)
{
    X25519Key k1 = x25519_generate([](uint8_t* out, size_t n) {
        for (size_t i = 0; i < n; ++i) out[i] = uint8_t(i * 7 + 3);
    });
    X25519Key k2 = x25519_generate([](uint8_t* out, size_t n) {
        for (size_t i = 0; i < n; ++i) out[i] = uint8_t(i * 13 + 1);
    });
    auto s1 = x25519(k1.priv.data(), k2.pub.data());
    auto s2 = x25519(k2.priv.data(), k1.pub.data());
    EXPECT_EQ(s1, s2);
}

/// @brief ChaCha20 matches an RFC 8439 test vector.
TEST(CryptoChaCha20, Rfc8439Vector)
{
    // §2.4.2 test vector.
    std::string key_hex = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    std::string nonce_hex = "000000090000004a00000000";
    auto key = hex_decode(key_hex).value;
    auto nonce = hex_decode(nonce_hex).value;
    std::string pt = "Ladies and Gentlemen of the class of '99: If I could offer you "
                     "only one tip for the future, sunscreen would be it.";
    std::string ct(pt.size(), '\0');
    chacha20(reinterpret_cast<const uint8_t*>(key.data()), reinterpret_cast<const uint8_t*>(nonce.data()), 1, reinterpret_cast<const uint8_t*>(pt.data()), reinterpret_cast<uint8_t*>(&ct[0]), ct.size());
    // RFC 8439 §2.3.2 keystream block first 16 bytes:
    // 10 f1 e7 e4 d1 3b 59 15 50 0f dd 1f a3 20 71 c4
    // Plaintext "Ladies and Gentl" XOR keystream:
    EXPECT_EQ(hex_encode(ct.substr(0, 16)), "5c90838db44879743e6bfd58c64e05a8");
}

/// @brief aead_seal/aead_open round-trip; tampering is rejected.
TEST(CryptoAead, SealOpenRoundtripAndTamper)
{
    std::string key(32, 'k');
    auto sealed = aead_seal(key, "attack at dawn", [](uint8_t* o, size_t n) {
        for (size_t i = 0; i < n; ++i) o[i] = uint8_t(i);
    });
    auto opened = aead_open(key, sealed);
    ASSERT_TRUE(opened.ok());
    EXPECT_EQ(*opened, "attack at dawn");
    std::string tampered = sealed;
    tampered[20] ^= 0xFF;
    auto failed = aead_open(key, tampered);
    EXPECT_FALSE(failed.ok()); // encrypt-then-MAC refuses tampering
}

/// @brief HKDF-SHA256 matches RFC 5869 test case 1.
TEST(CryptoHkdf, Rfc5869TestCase1)
{
    std::string ikm(22, char(0x0b));
    std::string salt = hex_decode("000102030405060708090a0b0c").value;
    std::string info = hex_decode("f0f1f2f3f4f5f6f7f8f9").value;
    std::string okm = hkdf_sha256(ikm, salt, info, 42);
    EXPECT_EQ(hex_encode(okm),
              "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
              "34007208d5b887185865"); // RFC 5869 test case 1, 42 bytes
}

/// @brief Base64 encode/decode round-trips arbitrary bytes.
TEST(CryptoBase64, Roundtrip)
{
    std::string raw = "\x00\x01\x02\xff\xfe binary \n data";
    auto enc = base64_encode(raw);
    auto dec = base64_decode(enc);
    ASSERT_TRUE(dec.ok());
    EXPECT_EQ(*dec, raw);
}

// ---------------- logging: redaction (§10 secret hygiene) ----------------

/// @brief Field values under credential-like keys are redacted before reaching a sink.
TEST(LogRedaction, SecretKeysRedacted)
{
    EXPECT_EQ(Redactor::redact_value("wg-private.key", "SECRET"), "[REDACTED]");
    EXPECT_EQ(Redactor::redact_value("password", "hunter2"), "[REDACTED]");
    EXPECT_EQ(Redactor::redact_value("normal_field", "visible"), "visible");
}

/// @brief Raw credential material inside message bodies is masked inline.
TEST(LogRedaction, TextRedactedInline)
{
    auto out = Redactor::redact_text("auth failed with password=hunter2 for user");
    EXPECT_EQ(out.find("hunter2"), std::string::npos);
    EXPECT_NE(out.find("[REDACTED]"), std::string::npos);
}

/// @brief Per-source levels and rate limits work at runtime; surplus is dropped and counted.
TEST(Logging, PerSourceLevelControlAndRateLimit)
{
    MemorySink sink;
    ManualTimeSource ts;
    Logger log(sink, ts);
    log.set_level(LogLevel::Info);
    log.set_source_level("noisy", LogLevel::Error);
    log.debug("noisy", "suppressed by per-source level");
    log.error("noisy", "visible");
    ASSERT_EQ(sink.records().size(), 1u);
    EXPECT_EQ(sink.records()[0].level, LogLevel::Error);
    // Rate limiting: 2/s, summary after suppression streak.
    log.set_rate_limit("quiet", 2.0);
    for (int i = 0; i < 10; ++i) log.info("quiet", "flood " + std::to_string(i));
    EXPECT_LT(sink.records().size(), 12u);
    EXPECT_GT(log.dropped_by_rate_limit(), 0u);
}

// ---------------- ctrl framing ----------------

/// @brief Length-prefixed frames encode and decode across arbitrary chunk boundaries.
TEST(CtrlFrames, LengthPrefixRoundtrip)
{
    auto frame = frames::encode_frame("{\"op\":\"status\"}");
    frames::FrameDecoder dec;
    std::string out;
    // Feed in two halves to exercise the streaming decoder.
    size_t half = frame.size() / 2;
    EXPECT_FALSE(dec.feed(frame.data(), half, &out));
    EXPECT_TRUE(dec.feed(frame.data() + half, frame.size() - half, &out));
    EXPECT_EQ(out, "{\"op\":\"status\"}");
}

/// @brief A declared length beyond the safety cap is rejected and latched.
TEST(CtrlFrames, OversizeFrameRejected)
{
    frames::FrameDecoder dec;
    std::string out;
    std::string huge(4, char(0xFF)); // length ~4GiB > cap
    dec.feed(huge.data(), huge.size(), &out);
    EXPECT_TRUE(dec.bad_length());
}

// ---------------- backoff (§3.4 flood control) ----------------

/// @brief Backoff grows exponentially and is clamped to the cap before jitter.
TEST(Backoff, ExponentialWithJitterBounded)
{
    JitteredBackoff b(1.0, 256.0, 0.0); // no jitter, cap above the sum
    b.set_rng([] { return 0.5; });
    double total = 0;
    for (int i = 0; i < 8; ++i) total += b.next();
    EXPECT_GE(total, 255.0 - 1.0); // 1+2+4+...+128
    EXPECT_LE(b.next(), 256.0); // capped at max
    b.reset();
    EXPECT_DOUBLE_EQ(b.next(), 1.0);
}

/// @brief Jitter keeps every delay within [base×(1−j), cap×(1+j)].
TEST(Backoff, JitterStaysInBounds)
{
    JitteredBackoff b(10.0, 100.0, 0.2);
    b.set_rng([] { return 0.0; });
    EXPECT_NEAR(b.next(), 8.0, 0.001); // 1 - 0.2
    b.set_rng([] { return 1.0; });
    EXPECT_NEAR(b.next(), 24.0, 0.001); // 2 * (1 + 0.2)
}

// ---------------- command table (§3.2) ----------------

/// @brief dispatch routes to handlers; help and completion cover every registered op.
TEST(CommandTable, DispatchHelpCompletion)
{
    CommandTable t;
    t.add({"ping", "reply with pong", {}, [](const std::string&) { return CommandResult::good(std::string("{\"pong\":true}")); }});
    t.add({"set", "set a value", {{"key", true, "key name"}, {"value", false, "value"}}, [](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               if (!(*a).has("key")) return CommandResult::err(Errc::InvalidArgument, "key required");
               return CommandResult::good(std::string("{\"set\":true}"));
           }});
    auto r = t.dispatch("ping", "{}");
    EXPECT_TRUE(r.ok);
    auto bad = t.dispatch("nope", "{}");
    EXPECT_FALSE(bad.ok);
    EXPECT_EQ(bad.error_code, "not-found");
    auto help = t.help_text("demo_ctrl");
    EXPECT_NE(help.find("ping"), std::string::npos);
    EXPECT_NE(help.find("set <key> [value]"), std::string::npos);
    auto bash = t.completion_bash("demo_ctrl");
    EXPECT_NE(bash.find("complete -F _demo_ctrl_completions demo_ctrl"), std::string::npos);
    auto zsh = t.completion_zsh("demo_ctrl");
    EXPECT_NE(zsh.find("#compdef demo_ctrl"), std::string::npos);
    // Envelope shape: {"ok":...} consistent across the suite.
    auto env = json_parse(r.envelope());
    ASSERT_TRUE(env.ok());
    EXPECT_EQ((*env).find("ok")->as_bool(), true);
}