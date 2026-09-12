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
 * @file daemons/monitord/include/instr/monitord/spoke.h
 * @brief monitord spoke runner: periodic sampling loop, spoke channel to istate,
 *
 * ctrl server. The daemon main wires these; the harness does the same
 * in-process.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "instr/ctrl/ctrl.h"
#include "instr/monitord/transport.h"
#include "monitord.grpc.pb.h"

namespace instr
{
/// @brief monitord: passive resource sampling and wear projection (§7.3).
namespace monitord
{
using namespace domain;
using namespace instr::transport;
/// @brief Pure domain logic for this daemon: no transport, no sockets, fully unit-testable.
namespace domain
{

/// @brief Spoke: transport to istate, schema-validated config sink and the sampling thread.
class MonitorSpoke
{
  public:
    /// @brief Wire the channel, config sink and handlers together.
    /// @param core Domain core.
    /// @param log Logger.
    /// @param table Command table.
    /// @param hub_target istate gRPC target.
    /// @param build_version Build version to advertise.
    MonitorSpoke(MonitorCore* core, Logger* log, const CommandTable* table, const std::string& hub_target, const std::string& build_version);

    /// @brief Start the channel and the sampling thread.
    void start();
    /// @brief Stop the channel and the sampling thread.
    void stop();
    /// @brief True while registered with istate.
    bool registered() const
    {
        return channel_->registered();
    }
    /// @brief Install the schema every config delivery must satisfy (R4, §9).
    /// @param schema Schema every config delivery must satisfy.
    void set_schema(JValue schema);
    /// @brief Revision of the last config applied.
    uint64_t config_revision() const
    {
        return channel_->config_revision();
    }

    /// @brief Number of frames handed to the transport (for tests).
    uint64_t frames_enqueued() const
    {
        return frames_.load();
    }

  private:
    /// @brief Periodic sampling loop driving sample_once() into the channel.
    void sample_loop();

    MonitorCore* core_;                     ///< Domain core (not owned).
    Logger* log_;                           ///< Logger (not owned).
    const CommandTable* table_;             ///< Command table (not owned).
    std::unique_ptr<ConfigSink> config_sink_;    ///< Validates and applies config deliveries.
    std::unique_ptr<SpokeChannel<monitord::MonitorHub, monitord::MonitorSpokeFrame, monitord::MonitorHubFrame>> channel_;   ///< gRPC channel to istate.
    std::atomic<bool> running_{false};           ///< Sampling-thread lifecycle flag.
    std::thread sampler_thread_;            ///< Thread running sample_loop().
    std::mutex sample_mu_;                  ///< Guards the sampling wait.
    std::condition_variable sample_cv_;     ///< Signals the sampling wait.
    std::atomic<uint64_t> frames_{0};            ///< Frames enqueued so far.
};

/// @brief Build the spoke channel with hub-frame dispatch wired to the command table.
std::unique_ptr<SpokeChannel<monitord::MonitorHub, monitord::MonitorSpokeFrame, monitord::MonitorHubFrame>> make_monitor_channel(MonitorCore* core, const CommandTable* table, Logger* log, const std::string& hub_target, const std::string& build_version,
                                                                                                                                 SpokeChannel<monitord::MonitorHub, monitord::MonitorSpokeFrame, monitord::MonitorHubFrame>::Options opts);

} // namespace domain
} // namespace monitord
} // namespace instr