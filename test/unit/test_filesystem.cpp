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
 * @file test/unit/test_filesystem.cpp
 * @brief simulated filesystem fault injection surface and ctrl
 *
 * server/client loopback (REQ §12).
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <fstream>

#include <gtest/gtest.h>

#include "instr/common/filesystem.h"
#include <chrono>
#include <thread>
#include "instr/ctrl/ctrl.h"

using namespace instr;

/// @brief The in-memory filesystem implements the IFileSystem primitives faithfully.
TEST(SimulatedFs, BasicFileOperations)
{
    SimulatedFileSystem fs;
    EXPECT_FALSE(fs.exists("/a/b.txt"));
    EXPECT_TRUE(fs.write("/a/b.txt", "hello").ok());
    EXPECT_TRUE(fs.exists("/a/b.txt"));
    auto r = fs.read("/a/b.txt");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(*r, "hello");
    EXPECT_TRUE(fs.rename("/a/b.txt", "/a/c.txt").ok());
    EXPECT_FALSE(fs.exists("/a/b.txt"));
    EXPECT_TRUE(fs.remove("/a/c.txt", false).ok());
}

/// @brief Write-failure injection makes storage errors structural, not bolted on.
TEST(SimulatedFs, WriteFailureInjection)
{
    SimulatedFileSystem fs;
    fs.fail_writes_after(2); // third write onward fails
    EXPECT_TRUE(fs.write("/f1", "a").ok());
    EXPECT_TRUE(fs.write("/f2", "b").ok());
    auto e = fs.write("/f3", "c");
    EXPECT_FALSE(e.ok());
    EXPECT_EQ(e.code, Errc::Io);
    EXPECT_NE(e.message.find("simulated"), std::string::npos);
}

/// @brief The full-disk injector lets budget/ENOSPC paths be tested deterministically.
TEST(SimulatedFs, FullDiskInjection)
{
    SimulatedFileSystem fs;
    fs.set_free_space(10);
    // free_space is the budget input used by spool/budget logic.
    auto free = fs.free_space("/pst");
    ASSERT_TRUE(free.ok());
    EXPECT_EQ(*free, 10u);
}

/// @brief Read-corruption injection simulates bit rot for fallback testing.
TEST(SimulatedFs, CorruptReadInjection)
{
    SimulatedFileSystem fs;
    fs.write("/f", "important-data");
    fs.corrupt_reads_of("/f", 1);
    auto r = fs.read("/f");
    ASSERT_TRUE(r.ok());
    EXPECT_NE(*r, "important-data"); // first byte flipped
}

/// @brief Recursive removal and directory listing behave like the POSIX names they mirror.
TEST(SimulatedFs, RecursiveRemoveAndList)
{
    SimulatedFileSystem fs;
    fs.write("/pst/system/istate/config.yaml", "a: 1");
    fs.write("/pst/system/istate/origins.yaml", "a: default");
    fs.write("/pst/state/spool/x.seg", "data");
    auto files = fs.list_dir("/pst/system/istate");
    ASSERT_TRUE(files.ok());
    EXPECT_EQ(files->size(), 2u);
    EXPECT_TRUE(fs.remove("/pst/system", true).ok());
    EXPECT_FALSE(fs.exists("/pst/system/istate/config.yaml"));
    EXPECT_TRUE(fs.exists("/pst/state/spool/x.seg")); // state untouched
}

// ---- ctrl server/client loopback over a real UDS socket ----

/// @brief A full client↔server request/response round trip over the unix socket.
TEST(CtrlLoopback, RequestResponseOverUds)
{
    CommandTable t;
    t.add({"ping", "liveness", {}, [](const std::string&) { return CommandResult::good(std::string("{\"pong\":true}")); }});
    t.add({"echo", "echo args", {{"msg", false, "text"}}, [](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               return CommandResult::good((*a).find("msg") ? (*a).find("msg")->as_string() : "");
           }});
    std::string path = "/tmp/instr-test-ctrl-" + std::to_string(::getpid()) + ".sock";
    // Mode 0600 root-only per §3.2 (enforced by the server on bind).
    ctrl::CtrlServer server(&t, path);
    ASSERT_TRUE(server.start().ok());

    ctrl::CtrlClient client(path);
    ASSERT_TRUE(client.connect().ok());
    auto r1 = client.call("ping", "{}");
    EXPECT_TRUE(r1.ok);
    auto r2 = client.call("echo", "{\"msg\":\"hello world\"}");
    ASSERT_TRUE(r2.ok);
    EXPECT_EQ(r2.json_data, "\"hello world\"");
    auto r3 = client.call("missing", "{}");
    EXPECT_FALSE(r3.ok);
    EXPECT_EQ(r3.error_code, "not-found");
    client.close();
    server.stop();
}

/// @brief A streaming op pushes frames until the client disconnects.
TEST(CtrlLoopback, StreamHandler)
{
    CommandTable t;
    std::string path2 = "/tmp/instr-test-ctrl-stream-" + std::to_string(::getpid()) + ".sock";
    ctrl::CtrlServer server(&t, path2);
    server.set_stream_handler("tail", [](const std::string& args, ctrl::StreamSink& sink) {
        (void)args;
        for (int i = 0; i < 5; ++i)
            if (!sink.send("{\"line\":" + std::to_string(i) + "}")) return;
    });
    ASSERT_TRUE(server.start().ok());
    ctrl::CtrlClient client(path2);
    ASSERT_TRUE(client.connect().ok());
    std::vector<std::string> got;
    (void)client.call_stream("tail", "{}", [&](const std::string& f) { got.push_back(f); });
    EXPECT_EQ(got.size(), 5u);
    client.close();
    server.stop();
}
// ---------------------------------------------------------------------------
// §7.7: "Bulk transfer by handle — the control API carries a handle and
// metadata; the payload never traverses istate's control path."
//
// bundle-fetch used to base64 the whole archive into its JSON response,
// which inflates it by a third, buffers it at both ends, and pushes it
// through the path that carries alarms and liveness. These pin the split.
// ---------------------------------------------------------------------------

/// @brief Bulk payloads stream raw over the bulk socket by handle, never the control path (§7.7).
TEST(BulkTransfer, PayloadStreamsOverTheBulkSocket)
{
    // A payload big enough that nobody would want it inside a JSON envelope.
    const std::string path = "/tmp/instr-bulk-payload-" + std::to_string(::getpid()) + ".bin";
    std::string payload;
    payload.reserve(512 * 1024);
    for (int i = 0; i < 512 * 1024; ++i) payload += char('A' + (i % 26));
    {
        std::ofstream out(path, std::ios::binary);
        out.write(payload.data(), std::streamsize(payload.size()));
    }

    const std::string sock = "/tmp/instr-bulk-" + std::to_string(::getpid()) + ".sock";
    ctrl::BulkServer server(sock, [&](const std::string& handle) {
        if (handle != "bundle-abc") return Result<std::string>::fail(Errc::NotFound, "no such bundle", handle);
        return Result<std::string>::good(path);
    });
    ASSERT_TRUE(server.start().ok());

    std::string received;
    auto n = ctrl::bulk_fetch(sock, "bundle-abc", [&](const char* d, size_t len) {
        received.append(d, len);
        return true;
    });
    ASSERT_TRUE(n.ok()) << n.error.str();
    EXPECT_EQ(*n, payload.size());
    EXPECT_EQ(received, payload) << "payload did not survive the transfer intact";
    EXPECT_EQ(server.served(), 1u);

    server.stop();
    ::unlink(path.c_str());
}

/// @brief An unknown handle yields an error, so a handle cannot read arbitrary files.
TEST(BulkTransfer, UnknownHandleYieldsNothing)
{
    const std::string sock = "/tmp/instr-bulk-unk-" + std::to_string(::getpid()) + ".sock";
    ctrl::BulkServer server(sock, [](const std::string& h) { return Result<std::string>::fail(Errc::NotFound, "no such bundle", h); });
    ASSERT_TRUE(server.start().ok());
    std::string received;
    auto n = ctrl::bulk_fetch(sock, "does-not-exist", [&](const char* d, size_t len) {
        received.append(d, len);
        return true;
    });
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(*n, 0u);
    EXPECT_TRUE(received.empty());
    server.stop();
}

/// @brief A sink that stops early must not hang the server or the client.
TEST(BulkTransfer, ASinkMayStopEarlyWithoutHangingTheTransfer)
{
    // A caller streaming to HQ over a tunnel that drops must be able to
    // abandon the transfer without the socket wedging.
    const std::string path = "/tmp/instr-bulk-early-" + std::to_string(::getpid()) + ".bin";
    {
        std::ofstream out(path, std::ios::binary);
        const std::string chunk(256 * 1024, 'x');
        out.write(chunk.data(), std::streamsize(chunk.size()));
    }
    const std::string sock = "/tmp/instr-bulk-early-" + std::to_string(::getpid()) + ".sock";
    ctrl::BulkServer server(sock, [&](const std::string&) { return Result<std::string>::good(path); });
    ASSERT_TRUE(server.start().ok());
    size_t seen = 0;
    auto n = ctrl::bulk_fetch(sock, "h", [&](const char*, size_t len) {
        seen += len;
        return false; // stop after the first chunk
    });
    EXPECT_TRUE(n.ok());
    EXPECT_GT(seen, 0u);
    server.stop();
    ::unlink(path.c_str());
}

/// @brief Connection threads are joined and reaped on accept; tracking does not grow with connections served.
TEST(CtrlLoopback, FinishedConnectionsAreReapedRatherThanAccumulated)
{
    // A _ctrl tool connects, asks one question and goes away. On a fielded
    // device that happens on a timer for years.
    //
    // serve_accept() used to prune with `if (!it->joinable())`, which is never
    // true — a std::thread stays joinable after its function returns, right up
    // until it is joined or detached. The erase branch was dead, so the vector
    // grew by one entry, and one unjoined OS thread, per connection ever
    // accepted, in all ten daemons.
    CommandTable t;
    t.add({"ping", "liveness", {}, [](const std::string&) { return CommandResult::good(std::string("{\"pong\":true}")); }});
    std::string path = "/tmp/instr-test-ctrl-reap-" + std::to_string(::getpid()) + ".sock";
    ctrl::CtrlServer server(&t, path);
    ASSERT_TRUE(server.start().ok());

    const int kConnections = 60;
    for (int i = 0; i < kConnections; ++i)
    {
        ctrl::CtrlClient client(path);
        ASSERT_TRUE(client.connect().ok()) << "connection " << i;
        EXPECT_TRUE(client.call("ping", "{}").ok);
        client.close();
        // Give the server a moment to notice the close before the next accept,
        // which is when reaping happens.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Reaping runs on accept, so a couple of recent entries may still be
    // tracked. What must not happen is growth proportional to the number of
    // connections served.
    EXPECT_LT(server.tracked_connections(), size_t(kConnections / 4)) << "connection threads are accumulating: " << server.tracked_connections() << " tracked after " << kConnections << " closed connections";
    server.stop();
    EXPECT_EQ(server.tracked_connections(), 0u) << "stop() must join and clear every connection";
}
