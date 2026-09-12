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
 * @file lib/common/src/daemon_skeleton.cpp
 * @brief Implementation of the standard daemon lifecycle skeleton: argument parsing, signal handling, graceful shutdown, and systemd readiness notification (REQ §).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/common/daemon_skeleton.h"

#include <cstdlib>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <thread>
#include <functional>

namespace instr
{

/// @brief Parse the shared daemon arguments (@p daemon_name fills DaemonArgs::name).
/// @param argc Argc from main().
/// @param argv Argv from main().
/// @param daemon_name Identity used for the usage line.
/// @return Parsed arguments; help=true when --help was present.
DaemonArgs parse_daemon_args(int argc, char** argv, const std::string& daemon_name)
{
    DaemonArgs a;
    a.name = daemon_name;
    // Environment first so a container can select the simulated platform for
    // every daemon at once; an explicit flag still wins.
    if (const char* env = ::getenv("INSTR_PLATFORM")) a.platform = env;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { a.help = true; }
        else if (arg == "--socket" && i + 1 < argc) { a.socket_override = argv[++i]; }
        else if (arg == "--root" && i + 1 < argc) { a.root = argv[++i]; }
        else if (arg == "--hub" && i + 1 < argc) { a.hub_socket = argv[++i]; }
        else if (arg == "--platform" && i + 1 < argc) { a.platform = argv[++i]; }
    }
    return a;
}

static std::atomic<bool>* g_stop = nullptr;

/// @brief Signal handler entry: sets the global stop flag.
static void on_signal(int)
{
    if (g_stop) g_stop->store(true);
}

/// @brief Install SIGINT/SIGTERM handlers that set @p stop_flag (graceful shutdown).
///
/// SIGPIPE is ignored (write errors are handled via Result).
///
/// @param stop_flag Atomic set on SIGINT/SIGTERM.
void install_signal_handlers(std::atomic<bool>& stop_flag)
{
    g_stop = &stop_flag;
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);
}

/// @brief systemd readiness notification via NOTIFY_SOCKET (datagram, no libsystemd dependency).
/// @param status e.g. "READY=1".
/// @return False when NOTIFY_SOCKET is unset (running unsupervised).
bool sd_notify(const std::string& status)
{
    const char* path = ::getenv("NOTIFY_SOCKET");
    if (!path || !*path) return false;
    std::string addr = path;
    int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    struct sockaddr_un sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (addr[0] == '@')
    {
        // Abstract socket: replace leading '@' with NUL.
        addr[0] = '\0';
        std::memcpy(sa.sun_path, addr.data(), addr.size());
    }
    else { std::strncpy(sa.sun_path, addr.c_str(), sizeof(sa.sun_path) - 1); }
    std::string msg = status;
    ssize_t n = ::sendto(fd, msg.data(), msg.size(), 0, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa));
    ::close(fd);
    return n >= 0;
}

/// @brief Block until @p stop_flag is set or @p heartbeat returns false.
///
/// The heartbeat runs every @p poll_ms and normally performs periodic work;
/// returning false ends the loop early.
///
/// @param stop_flag Atomic watched for shutdown.
/// @param heartbeat Periodic work callback.
/// @param poll_ms Sleep between heartbeat invocations.
void run_until_stop(const std::atomic<bool>& stop_flag, const std::function<bool()>& heartbeat, int poll_ms)
{
    while (!stop_flag.load())
    {
        if (heartbeat && !heartbeat()) break;
        struct timespec ts;
        ts.tv_sec = poll_ms / 1000;
        ts.tv_nsec = (poll_ms % 1000) * 1'000'000L;
        nanosleep(&ts, nullptr);
    }
}

} // namespace instr