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
 * @file daemons/istate/include/instr/istate/hubs.h
 * @brief gRPC server wiring: nine per-spoke hub services (one socket per
 *
 * daemon, mode 0660 owned instr:<daemon>), the Configuration API (§3.3),
 * and the HQ session client (istate dials HqPeer over the tunnel, §3.4).
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <grpcpp/grpcpp.h>

#include "alarms.grpc.pb.h"
#include "backup.grpc.pb.h"
#include "faults.grpc.pb.h"
#include "hardware.grpc.pb.h"
#include "istate.grpc.pb.h"
#include "logmanager.grpc.pb.h"
#include "monitord.grpc.pb.h"
#include "networkd.grpc.pb.h"
#include "ota.grpc.pb.h"
#include "telemetry.grpc.pb.h"

#include "instr/istate/core.h"

namespace instr
{
/// @brief istate: /pst owner, spoke orchestrator and HQ northbound bridge (R4, §7.1).
namespace istate
{

/// @brief Builds and runs the full gRPC server for istate (all hub services).
///
/// Hub impls derive from the generated nested `<X>Hub::Service` classes.
/// Socket paths are /run/instrumentation/<daemon>.sock (created by istate, §3.1)
/// plus configuration.sock.
///
/// @param core istate core the hubs ingest into.
/// @param run_dir Directory holding the sockets.
/// @param services Receives the built hub services (for the server's lifetime).
/// @return The started server, or nullptr when startup failed.
std::unique_ptr<grpc::Server> build_hub_server(IstateCore& core, const std::string& run_dir, std::vector<std::unique_ptr<grpc::Service>>* services);

/// @brief HQ session client: dials HqPeer over the tunnel, pushes spooled data,
/// receives config pushes / OTA commands / backpressure.
class HqClient
{
  public:
    /// @brief Construct the client over @p hq_target.
    /// @param core istate core.
    /// @param log Logger.
    /// @param hq_target HQ tunnel target.
    HqClient(IstateCore& core, Logger& log, const std::string& hq_target);
    /// @brief Start the session loop.
    void start();
    /// @brief Stop the session loop.
    void stop();
    /// @brief True while a session is established.
    bool connected() const
    {
        return connected_;
    }

  private:
    /// @brief Reconnect loop with jittered backoff.
    void run_loop();
    /// @brief One session attempt.
    /// @return True when the condition holds; false otherwise.
    bool try_session();

    IstateCore& core_;               ///< istate core (not owned).
    Logger& log_;                     ///< Logger (not owned).
    std::string target_;              ///< HQ tunnel target.
    std::thread thread_;              ///< Thread running run_loop().
    std::atomic<bool> running_{false};      ///< Lifecycle flag.
    std::atomic<bool> connected_{false};    ///< Session state.
    JitteredBackoff backoff_{2.0, 60.0};   ///< Reconnect backoff.
};

} // namespace istate
} // namespace instr