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
 * @file lib/ctrl/include/instr/ctrl/ctrl.h
 * @brief raw-UDS control server/client framework (§3.2)
 *
 * Each daemon exposes <daemon>-ctrl.sock, mode 0600, root-only. The ctrl
 * server dispatches from the same command table the gRPC surface uses.
 * Streaming ops (live tail) push frames until the client disconnects or the
 * op's timeout elapses.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "instr/common/command_table.h"
#include "instr/common/frames.h"

namespace instr
{
/// @brief Raw-UDS control server/client framework (§3.2).
namespace ctrl
{

/// @brief Write-side helper for streaming ops: frames JSON lines to a client socket.
class StreamSink
{
  public:
    /// @brief Bind the sink to an accepted connection descriptor.
    /// @param fd Accepted connection descriptor.
    explicit StreamSink(int fd) : fd_(fd)
    {
    }
    /// @brief Send one JSON line as a length-prefixed frame.
    /// @param json_line Payload to send.
    /// @return False once the client has disconnected (latched in closed()).
    bool send(const std::string& json_line);
    /// @brief True once a send failed because the client went away.
    bool closed() const
    {
        return closed_;
    }

  private:
    int fd_; ///< Client connection descriptor.
    std::atomic<bool> closed_{false}; ///< Latched on the first failed send.
};

/// @brief Unix-socket control server dispatching from a shared CommandTable.
///
/// Exposes <daemon>-ctrl.sock, mode 0600, root-only. Streaming ops push
/// frames until the client disconnects or the op's timeout elapses.
class CtrlServer
{
  public:
    /// @brief Construct a server over @p table listening on @p socket_path.
    /// @param table Command table driving dispatch (not owned).
    /// @param socket_path Unix socket to bind.
    CtrlServer(const CommandTable* table, std::string socket_path);
    /// @brief Stop the server and join all worker threads.
    /// @return Stop the server and join all worker threads.
    ~CtrlServer();

    /// @brief Bind and listen; starts the accept thread.
    /// @return ok() on success, otherwise a diagnosable error.
    Error start();
    /// @brief Stop accepting, close all connections and join workers.
    void stop();
    /// @brief True between start() and stop().
    bool running() const
    {
        return running_;
    }

    /// @brief Register a streaming handler for @p op (kept separate from the table).
    ///
    /// Payload ops stay table-driven; streaming ops (live tail) push frames
    /// through @p h's sink.
    ///
    /// @param op Operation name.
    /// @param h Handler receiving the args JSON and the client sink.
    void set_stream_handler(const std::string& op, std::function<void(const std::string& args, StreamSink&)> h)
    {
        streams_[op] = std::move(h);
    }

    /// @brief How many connection threads are currently tracked.
    ///
    /// Exposed so the reaping in serve_accept() is testable: this grew
    /// without bound for the life of the daemon before the done-flag reap
    /// replaced a joinable() check that could never fire.
    size_t tracked_connections() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return conn_threads_.size();
    }

  private:
    /// @brief Accept loop: accepts, reaps finished connection threads, spawns new ones.
    void serve_accept();
    /// @brief Serve one connection: read frames, dispatch to table or stream handler.
    /// @param fd Connection descriptor to serve.
    void serve_conn(int fd);

    const CommandTable* table_; ///< Command registry driving dispatch (not owned).
    std::string path_; ///< Bound socket path.
    /// Atomic: the accept thread reads this while stop() closes and clears it
    // from another thread. As a plain int that is a data race (TSan flags it),
    // and on a weakly-ordered target — half this fleet is aarch64 — the accept
    // loop can miss the close and spin on a stale descriptor.
    std::atomic<int> listen_fd_{-1};
    std::atomic<bool> running_{false}; ///< Server lifecycle flag.
    std::thread accept_thread_; ///< Thread running serve_accept().
    // Each connection thread flags itself done on exit; serve_accept() joins
    // the flagged ones before spawning more. A plain vector<thread> pruned by
    // joinable() never shrinks — a thread stays joinable after its function
    // returns — so that leaked one handle and one unjoined OS thread per
    // connection, for the life of the daemon.
    struct Conn
    {
        std::thread thread; ///< Worker serving one connection.
        std::shared_ptr<std::atomic<bool>> done; ///< Set by the worker on exit; reaped on accept.
    };
    std::vector<Conn> conn_threads_; ///< Recently accepted connections awaiting reap.
    std::map<std::string, std::function<void(const std::string&, StreamSink&)>> streams_; ///< Streaming handlers by op.
    mutable std::mutex mu_; ///< Guards conn_threads_.
};

/// @brief Unix-socket control client for talking to a CtrlServer.
class CtrlClient
{
  public:
    /// @brief Construct a client bound to @p socket_path (does not connect yet).
    /// @param socket_path Control socket path to connect to.
    explicit CtrlClient(std::string socket_path);
    /// @brief Close any open connection.
    /// @return Close any open connection.
    ~CtrlClient();

    /// @brief Connect to the control socket.
    /// @return ok() on success, otherwise a diagnosable error.
    Error connect();
    /// @brief Close the connection (idempotent).
    void close();

    /// @brief Single request/response call.
    /// @param op Operation name.
    /// @param json_args Raw JSON arguments string.
    /// @return The server's result envelope.
    CommandResult call(const std::string& op, const std::string& json_args);
    /// @brief Streaming call: one callback per streamed frame until end or error.
    /// @param op Operation name.
    /// @param json_args Raw JSON arguments string.
    /// @param on_frame Invoked with each streamed JSON frame.
    /// @return ok() when the stream ended cleanly, otherwise the failure.
    Error call_stream(const std::string& op, const std::string& json_args, const std::function<void(const std::string& json)>& on_frame);

  private:
    /// @brief Write all of @p data to the socket, looping over partial writes.
    /// @param data Bytes to send.
    /// @return True when all bytes were written.
    bool send_all(const std::string& data);
    /// @brief Read one length-prefixed frame from the socket.
    /// @param out Receives the frame payload.
    /// @return True when a frame was read.
    bool recv_frame(std::string* out);

    std::string path_; ///< Socket path to connect to.
    int fd_ = -1; ///< Connection descriptor; -1 when closed.
    // One decoder per connection: a single read() often carries several frames
    // (a stream burst), and a fresh decoder per call would discard the tail.
    frames::FrameDecoder dec_;
};

// ---- bulk data transfer (REQ §7.7) ----
// "Bulk transfer by handle — the control API carries a handle and metadata;
// the payload never traverses istate's control path."
//
// A bundle can be tens of megabytes. Base64ing it into a JSON control
// response (as this used to do) inflates it by a third, buffers all of it in
// memory at both ends, and routes it through the path that carries alarms
// and liveness — which is the traffic that must not be held up behind a
// support archive.
//
// So the payload gets its own socket: the control response names a handle,
// the caller connects to the bulk socket, sends the handle, and the server
// streams raw bytes until close. Nothing is framed and nothing is encoded.
/// @brief Bulk data transfer by handle on its own socket (REQ §7.7).
///
/// A bundle can be tens of megabytes: base64ing it into a JSON control
/// response inflates it by a third, buffers it all in memory, and would route
/// it through the path that carries alarms and liveness. The control response
/// names a handle instead; the caller connects to the bulk socket, sends the
/// handle, and the server streams raw bytes until close — unframed, unencoded.
class BulkServer
{
  public:
    /// @brief Maps a handle to an absolute file path (error for unknown handles).
    using Resolver = std::function<Result<std::string>(const std::string& handle)>;

    /// @brief Construct a bulk server on @p socket_path resolving handles via @p resolve.
    /// @param socket_path Bulk socket path to bind.
    /// @param resolve Handle → file-path resolver.
    BulkServer(std::string socket_path, Resolver resolve) : path_(std::move(socket_path)), resolve_(std::move(resolve))
    {
    }
    /// @brief Stop the server and join the accept thread.
    /// @return Stop the server and join the accept thread.
    ~BulkServer();

    /// @brief Bind, listen and start the accept thread.
    /// @return ok(), or a bind/listen error.
    Error start();
    /// @brief Stop accepting and close the socket.
    void stop();
    /// @brief Bound socket path.
    /// @return The bound socket path.
    const std::string& path() const
    {
        return path_;
    }
    /// @brief Total bytes served across all transfers.
    uint64_t served() const
    {
        return served_.load();
    }

  private:
    /// @brief Accept loop: one connection at a time, streaming one handle.
    void serve_accept();
    /// @brief Read a handle from @p fd and stream the resolved file until close.
    /// @param fd Accepted connection descriptor.
    void serve_one(int fd);

    std::string path_; ///< Bound socket path.
    Resolver resolve_; ///< Handle → file path resolver.
    /// Atomic: the accept thread reads this while stop() closes and clears it
    // from another thread. As a plain int that is a data race (TSan flags it),
    // and on a weakly-ordered target — half this fleet is aarch64 — the accept
    // loop can miss the close and spin on a stale descriptor.
    std::atomic<int> listen_fd_{-1};
    std::atomic<bool> running_{false}; ///< Server lifecycle flag.
    std::atomic<uint64_t> served_{0}; ///< Bytes served so far.
    std::thread accept_thread_; ///< Thread running serve_accept().
};

/// @brief Read a payload by handle from a bulk socket, chunk by chunk.
///
/// @p sink is called with each chunk as it arrives, so a caller can stream to
/// disk or onward to HQ without holding the whole bundle in memory.
///
/// @param socket_path Bulk socket to connect to.
/// @param handle Handle named by the control response.
/// @param sink Called per chunk; return false to stop early.
/// @return Total bytes received, or an error.
Result<uint64_t> bulk_fetch(const std::string& socket_path, const std::string& handle, const std::function<bool(const char* data, size_t len)>& sink);

/// @brief Shared _ctrl tool main: --help, --help <op>, --json, --socket PATH, completion.
/// @param argc Argc from main().
/// @param argv Argv from main().
/// @param table Command table for the tool.
/// @param prog_name Tool name used in the usage line.
/// @param default_socket Socket path used unless --socket overrides.
/// @return Process exit code.
int tool_main(int argc, char** argv, const CommandTable& table, const std::string& prog_name, const std::string& default_socket);

} // namespace ctrl
} // namespace instr