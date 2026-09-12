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
 * @file lib/ctrl/src/ctrl.cpp
 * @brief Implementation of the raw-UDS control server, client, streaming sink, and tool_main dispatch for the *_ctrl tools (REQ §3.2).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/ctrl/ctrl.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>

namespace instr
{
/// @brief Raw-UDS control server/client framework (§3.2).
namespace ctrl
{

// ---------------- StreamSink ----------------

/// @brief Frame one JSON line to the client; latches closed() on failure.
/// @param json_line JSON payload line to frame and send.
/// @return False once the client disconnects.
bool StreamSink::send(const std::string& json_line)
{
    std::string frame = frames::encode_frame(json_line);
    size_t off = 0;
    while (off < frame.size())
    {
        // MSG_NOSIGNAL: a _ctrl client that disconnects mid-stream (a tail cut
        // short) must close the connection, not signal the daemon to death.
        ssize_t n = ::send(fd_, frame.data() + off, frame.size() - off, MSG_NOSIGNAL);
        if (n <= 0)
        {
            closed_ = true;
            return false;
        }
        off += size_t(n);
    }
    return true;
}

// ---------------- CtrlServer ----------------

/// @brief Construct a server over @p table on @p socket_path.
/// @param table Command table driving dispatch.
/// @param socket_path Socket to bind.
CtrlServer::CtrlServer(const CommandTable* table, std::string socket_path) : table_(table), path_(std::move(socket_path))
{
}

/// @brief Stop and join everything.
CtrlServer::~CtrlServer()
{
    stop();
}

/// @brief Bind, listen and start the accept thread.
Error CtrlServer::start()
{
    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return Error::make(Errc::Io, "socket() failed", path_);
    ::unlink(path_.c_str()); // stale socket from a previous run
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path_.size() >= sizeof(addr.sun_path)) return Error::make(Errc::InvalidArgument, "socket path too long", path_);
    std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) return Error::make(Errc::Io, "bind failed: " + std::string(strerror(errno)), path_);
    // Mode 0600 root-only (REQ §3.2).
    ::chmod(path_.c_str(), 0600);
    if (::listen(listen_fd_, 16) != 0) return Error::make(Errc::Io, "listen failed", path_);
    running_ = true;
    accept_thread_ = std::thread([this] { serve_accept(); });
    return Error::none();
}

/// @brief Stop accepting, close the socket and join all workers.
void CtrlServer::stop()
{
    if (!running_.exchange(false))
    {
        if (listen_fd_ >= 0)
        {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        return;
    }
    if (listen_fd_ >= 0)
    {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) accept_thread_.join();
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& c : conn_threads_)
            if (c.thread.joinable()) c.thread.join();
        conn_threads_.clear();
    }
    ::unlink(path_.c_str());
}

/// @brief Accept loop: reap finished connections, accept and spawn workers.
void CtrlServer::serve_accept()
{
    while (running_.load())
    {
        int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0)
        {
            if (!running_.load()) break;
            continue;
        }
        std::lock_guard<std::mutex> lk(mu_);
        // Reap connections that have finished. Testing joinable() here (as this
        // once did) never reaps anything: a thread remains joinable after its
        // function returns, so the erase branch was dead and the vector grew by
        // one unjoined thread per connection for the life of the daemon. The
        // done flag is what actually distinguishes finished from running.
        for (auto it = conn_threads_.begin(); it != conn_threads_.end();)
        {
            if (it->done->load())
            {
                if (it->thread.joinable()) it->thread.join();
                it = conn_threads_.erase(it);
            }
            else { ++it; }
        }
        auto done = std::make_shared<std::atomic<bool>>(false);
        conn_threads_.push_back(Conn{std::thread([this, fd, done] {
                                         serve_conn(fd);
                                         done->store(true);
                                     }),
                                     done});
    }
}

/// @brief Serve one connection until close.
/// @param fd Connection descriptor to serve.
void CtrlServer::serve_conn(int fd)
{
    frames::FrameDecoder dec;
    char buf[8192];
    bool done = false;
    while (running_.load() && !done)
    {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        // A single read may carry several pipelined requests; drain them all
        // before blocking again.
        std::string payload;
        for (bool more = dec.feed(buf, size_t(n), &payload); more; more = dec.feed(nullptr, 0, &payload))
        {
            // Request: {"op":"...","args":{...},"stream":false}
            auto req = json_parse(payload);
            if (!req.ok() || !(*req).is_object())
            {
                std::string resp = CommandResult::err(Errc::InvalidArgument, "malformed ctrl request").envelope();
                (void)StreamSink(fd).send(resp);
                continue;
            }
            std::string op;
            std::string args;
            bool stream = false;
            if (const JValue* v = (*req).find("op")) op = v->as_string();
            if (const JValue* v = (*req).find("args")) args = v->is_object() ? v->dump() : "";
            if (const JValue* v = (*req).find("stream")) stream = v->as_bool();

            auto sit = streams_.find(op);
            if (stream && sit != streams_.end())
            {
                StreamSink sink(fd);
                sit->second(args, sink);
                done = true; // stream finished → close the connection
                break;
            }
            StreamSink sink(fd);
            if (!sink.send(table_->dispatch(op, args).envelope()))
            {
                done = true;
                break;
            }
        }
    }
    ::close(fd);
}

// ---------------- CtrlClient ----------------

/// @brief Construct a client for @p socket_path (not yet connected).
CtrlClient::CtrlClient(std::string socket_path) : path_(std::move(socket_path))
{
}

/// @brief Close the connection if open.
CtrlClient::~CtrlClient()
{
    close();
}

/// @brief Connect to the control socket (mode is checked at the server).
Error CtrlClient::connect()
{
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) return Error::make(Errc::Io, "socket() failed", path_);
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path_.size() >= sizeof(addr.sun_path)) return Error::make(Errc::InvalidArgument, "socket path too long", path_);
    std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
    dec_ = frames::FrameDecoder();
    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        int saved = errno;
        ::close(fd_);
        fd_ = -1;
        return Error::make(Errc::Unavailable, "cannot connect to ctrl socket: " + std::string(strerror(saved)), path_);
    }
    return Error::none();
}

/// @brief Close the connection (idempotent).
void CtrlClient::close()
{
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
}

/// @brief Loop over partial writes until all bytes are sent.
/// @param data Bytes to send.
/// @return True when all bytes were written.
bool CtrlClient::send_all(const std::string& data)
{
    size_t off = 0;
    while (off < data.size())
    {
        ssize_t n = ::send(fd_, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += size_t(n);
    }
    return true;
}

/// @brief Read one length-prefixed frame.
/// @param out Receives the frame payload.
/// @return True when a frame was read.
bool CtrlClient::recv_frame(std::string* out)
{
    char buf[8192];
    while (true)
    {
        // Drain frames already buffered from an earlier read before blocking.
        if (dec_.feed(nullptr, 0, out)) return true;
        ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n <= 0) return false;
        if (dec_.feed(buf, size_t(n), out)) return true;
    }
}

/// @brief Single request/response round trip.
/// @param op Operation name.
/// @param json_args Raw JSON argument string.
/// @return The server's result envelope.
CommandResult CtrlClient::call(const std::string& op, const std::string& json_args)
{
    if (fd_ < 0) return CommandResult::err(Errc::Unavailable, "not connected", "");
    JValue req = JValue::object();
    req.set("op", JValue(op));
    auto args = json_parse(json_args.empty() ? "{}" : json_args);
    req.set("args", args.ok() && (*args).is_object() ? *args : JValue::object());
    req.set("stream", JValue(false));
    if (!send_all(frames::encode_frame(req.dump()))) return CommandResult::err(Errc::Io, "ctrl write failed", "");
    std::string resp;
    if (!recv_frame(&resp)) return CommandResult::err(Errc::Io, "ctrl read failed", "");
    auto parsed = json_parse(resp);
    if (!parsed.ok()) return CommandResult::err(Errc::Corrupt, "malformed ctrl response: " + parsed.error.message, "");
    // Parse the envelope back into CommandResult.
    const JValue& v = *parsed;
    CommandResult r;
    if (const JValue* ok = v.find("ok")) r.ok = ok->as_bool();
    if (r.ok)
    {
        if (const JValue* d = v.find("data")) r.json_data = d->dump();
    }
    else
    {
        if (const JValue* e = v.find("error"))
        {
            if (const JValue* c = e->find("code")) r.error_code = c->as_string();
            if (const JValue* m = e->find("message")) r.error_message = m->as_string();
        }
    }
    return r;
}

/// @brief Streaming call: one callback per frame until the server closes.
/// @param op Operation name.
/// @param json_args Raw JSON argument string.
/// @param on_frame Per-frame callback.
/// @return ok() when the stream ended cleanly.
Error CtrlClient::call_stream(const std::string& op, const std::string& json_args, const std::function<void(const std::string&)>& on_frame)
{
    if (fd_ < 0) return Error::make(Errc::Unavailable, "not connected", path_);
    JValue req = JValue::object();
    req.set("op", JValue(op));
    auto args = json_parse(json_args.empty() ? "{}" : json_args);
    req.set("args", args.ok() && (*args).is_object() ? *args : JValue::object());
    req.set("stream", JValue(true));
    if (!send_all(frames::encode_frame(req.dump()))) return Error::make(Errc::Io, "ctrl write failed", path_);
    std::string resp;
    while (recv_frame(&resp)) { on_frame(resp); }
    return Error::none();
}

// ---------------- bulk transfer (REQ §7.7) ----------------

/// @brief Stop and join the accept thread.
BulkServer::~BulkServer()
{
    stop();
}

/// @brief Bind, listen and start the accept loop.
Error BulkServer::start()
{
    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return Error::make(Errc::Io, "socket() failed", path_);
    ::unlink(path_.c_str());
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path_.size() >= sizeof(addr.sun_path)) return Error::make(Errc::InvalidArgument, "socket path too long", path_);
    std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) return Error::make(Errc::Io, "bind failed: " + std::string(strerror(errno)), path_);
    // Same 0600 root-only posture as the ctrl socket: a support bundle can
    // contain anything the device logged.
    ::chmod(path_.c_str(), 0600);
    if (::listen(listen_fd_, 8) != 0) return Error::make(Errc::Io, "listen failed", path_);
    running_ = true;
    accept_thread_ = std::thread([this] { serve_accept(); });
    return Error::none();
}

/// @brief Stop accepting and close the socket.
void BulkServer::stop()
{
    if (!running_.exchange(false))
    {
        if (listen_fd_ >= 0)
        {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        return;
    }
    if (listen_fd_ >= 0)
    {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) accept_thread_.join();
    ::unlink(path_.c_str());
}

/// @brief Accept loop serving one bulk connection at a time.
void BulkServer::serve_accept()
{
    while (running_.load())
    {
        int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0)
        {
            if (!running_.load()) break;
            continue;
        }
        serve_one(fd); // one transfer at a time: bulk is not latency-sensitive
        ::close(fd);
    }
}

/// @brief Read a handle and stream the resolved file until close.
/// @param fd Accepted connection descriptor.
void BulkServer::serve_one(int fd)
{
    // Request: one length-prefixed frame carrying the handle.
    frames::FrameDecoder dec;
    char buf[4096];
    std::string handle;
    while (running_.load())
    {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) return;
        if (dec.feed(buf, size_t(n), &handle)) break;
    }
    if (handle.empty()) return;

    auto path = resolve_ ? resolve_(handle) : Result<std::string>::fail(Errc::Unavailable, "no resolver", handle);
    if (!path.ok())
    {
        // An unknown handle closes without data; the metadata call is where a
        // caller learns whether a bundle exists.
        return;
    }
    std::FILE* f = std::fopen(path->c_str(), "rb");
    if (!f) return;
    char chunk[64 * 1024];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
    {
        size_t off = 0;
        while (off < got)
        {
            // MSG_NOSIGNAL, not write(): a caller that abandons the transfer —
            // an operator hitting Ctrl-C, a tunnel dropping mid-bundle — closes
            // its end, and a plain write() would then raise SIGPIPE and take the
            // daemon down. A viewer disconnecting must never kill LogManager.
            ssize_t w = ::send(fd, chunk + off, got - off, MSG_NOSIGNAL);
            if (w <= 0)
            {
                std::fclose(f);
                return; // caller went away: stop, nothing to report
            }
            off += size_t(w);
        }
    }
    std::fclose(f);
    ++served_;
}

/// @brief Read a payload by handle from a bulk socket, chunk by chunk.
///
/// @p sink is called with each chunk as it arrives, so a caller can stream to
/// disk or onward to HQ without holding the whole bundle in memory.
///
/// @param socket_path Bulk socket to connect to.
/// @param handle Handle named by the control response.
/// @param sink Called per chunk; return false to stop early.
/// @return Total bytes received, or an error.
Result<uint64_t> bulk_fetch(const std::string& socket_path, const std::string& handle, const std::function<bool(const char*, size_t)>& sink)
{
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return Result<uint64_t>::fail(Errc::Io, "socket() failed", socket_path);
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path))
    {
        ::close(fd);
        return Result<uint64_t>::fail(Errc::InvalidArgument, "socket path too long", socket_path);
    }
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        ::close(fd);
        return Result<uint64_t>::fail(Errc::Unavailable, "cannot connect to bulk socket", socket_path);
    }
    const std::string req = frames::encode_frame(handle);
    size_t off = 0;
    while (off < req.size())
    {
        ssize_t w = ::send(fd, req.data() + off, req.size() - off, MSG_NOSIGNAL);
        if (w <= 0)
        {
            ::close(fd);
            return Result<uint64_t>::fail(Errc::Io, "bulk request write failed", socket_path);
        }
        off += size_t(w);
    }
    uint64_t total = 0;
    char buf[64 * 1024];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0)
    {
        if (sink && !sink(buf, size_t(n))) break;
        total += uint64_t(n);
    }
    ::close(fd);
    return Result<uint64_t>::good(total);
}

// ---------------- tool main ----------------

/// @brief Print a command envelope (--json emits raw; default pretty-prints).
/// @param r Result envelope to print.
/// @param pretty Pretty-print the JSON payload when true.
static void print_envelope(const CommandResult& r, bool pretty)
{
    std::string env = r.envelope();
    if (!pretty)
    {
        std::cout << env << "\n";
        return;
    }
    auto v = json_parse(env);
    if (v.ok())
        std::cout << (*v).dump(2) << "\n";
    else
        std::cout << env << "\n";
}

/// @brief Shared _ctrl tool main: --help, --help <op>, --json, --socket PATH, completion.
/// @param argc Argc from main().
/// @param argv Argv from main().
/// @param table Command table for the tool.
/// @param prog_name Tool name used in the usage line.
/// @param default_socket Socket path used unless --socket overrides.
/// @return Process exit code.
int tool_main(int argc, char** argv, const CommandTable& table, const std::string& prog_name, const std::string& default_socket)
{
    std::string socket_path = default_socket;
    bool pretty = false;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--help" || a == "-h")
        {
            if (i + 1 < argc && table.find(argv[i + 1]))
            {
                std::cout << table.help_for(argv[i + 1]);
                return 0;
            }
            std::cout << table.help_text(prog_name);
            return 0;
        }
        if (a == "--json")
        {
            pretty = false; // --json = compact machine envelope (default); kept for explicitness
            continue;
        }
        if (a == "--pretty")
        {
            pretty = true;
            continue;
        }
        if (a == "--socket" && i + 1 < argc)
        {
            socket_path = argv[++i];
            continue;
        }
        positional.push_back(a);
    }

    if (!positional.empty() && positional[0] == "completion")
    {
        if (positional.size() < 2)
        {
            std::cerr << "usage: " << prog_name << " completion bash|zsh\n";
            return 2;
        }
        if (positional[1] == "bash")
            std::cout << table.completion_bash(prog_name);
        else if (positional[1] == "zsh")
            std::cout << table.completion_zsh(prog_name);
        else
        {
            std::cerr << "unknown shell: " << positional[1] << "\n";
            return 2;
        }
        return 0;
    }

    if (positional.empty())
    {
        std::cout << table.help_text(prog_name);
        return 0;
    }

    const std::string& op = positional[0];
    // Args: remaining positionals accepted as JSON blob, or key=value pairs.
    std::string json_args;
    if (positional.size() > 1)
    {
        // Try whole-remainder JSON first.
        std::string joined;
        for (size_t i = 1; i < positional.size(); ++i) joined += positional[i];
        auto parsed = json_parse(joined);
        if (parsed.ok() && (*parsed).is_object()) { json_args = joined; }
        else
        {
            JValue obj = JValue::object();
            for (size_t i = 1; i < positional.size(); ++i)
            {
                const std::string& kv = positional[i];
                size_t eq = kv.find('=');
                if (eq == std::string::npos) continue;
                std::string key = kv.substr(0, eq);
                std::string val = kv.substr(eq + 1);
                auto asj = json_parse(val);
                obj.set(key, asj.ok() ? *asj : JValue(val));
            }
            json_args = obj.dump();
        }
    }

    CtrlClient client(socket_path);
    Error e = client.connect();
    if (!e.ok())
    {
        std::cerr << "error: " << e.str() << "\n";
        std::cerr << "hint: start the daemon, or pass --socket PATH\n";
        return 3;
    }
    CommandResult r = client.call(op, json_args);
    print_envelope(r, pretty);
    client.close();
    return r.ok ? 0 : 1;
}

} // namespace ctrl
} // namespace instr