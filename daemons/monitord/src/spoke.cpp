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
 * @file daemons/monitord/src/spoke.cpp
 * @brief monitord spoke runner: periodic sampling loop, spoke channel to istate, and ctrl server wiring (REQ §3.1, §7.3).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/monitord/spoke.h"

#include <algorithm>

namespace instr
{
/// @brief monitord: passive resource sampling and wear projection (§7.3).
namespace monitord
{
/// @brief Pure domain logic for this daemon: no transport, no sockets, fully unit-testable.
namespace domain
{

using ChannelT = SpokeChannel<monitord::MonitorHub, monitord::MonitorSpokeFrame, monitord::MonitorHubFrame>;

/// @brief Wire the channel, config sink and handlers together.
/// @param core Domain core.
/// @param log Logger.
/// @param table Command table.
/// @param hub_target istate gRPC target.
/// @param build_version Build version to advertise.
MonitorSpoke::MonitorSpoke(MonitorCore* core, Logger* log, const CommandTable* table, const std::string& hub_target, const std::string& build_version) : core_(core), log_(log), table_(table)
{
    ChannelT::Options o;
    o.target = hub_target;
    o.daemon = "monitord";
    o.build_version = build_version;
    o.capabilities = {"samples", "wear-projection", "highres-window"};
    channel_ = std::make_unique<ChannelT>(std::move(o), *log);
    // Hub-frame handling: commands dispatch through the shared table and are
    // acked; relays/time-sync reach the core via the ingest path.
    channel_->set_hub_handler([this](const monitord::MonitorHubFrame& hf) {
        if (hf.has_command())
        {
            CommandResult r = table_->dispatch(hf.command().op(), hf.command().json_args());
            common::Ack ack;
            ack.set_id(hf.command().id());
            ack.set_ok(r.ok);
            if (r.ok) { ack.set_json_payload(r.json_data); }
            else
            {
                ack.set_code(r.error_code);
                ack.set_message(r.error_message);
            }
            channel_->send_ack(ack);
        }
    });
    // Until a schema is installed, deliveries still apply — just unvalidated.
    set_schema(JValue::object());
}

/// @brief Install the schema every config delivery must satisfy.
/// @param schema Schema every config delivery must satisfy.
void MonitorSpoke::set_schema(JValue schema)
{
    config_sink_ = std::make_unique<ConfigSink>("monitord", std::move(schema), [this](const ConfigDocument& doc) { return core_->apply_config(doc); });
    channel_->set_config_handler([this](const common::ConfigDelivery& delivery) { return config_sink_->deliver(delivery.yaml(), delivery.revision()); });
}

/// @brief Start the channel and the sampling thread.
void MonitorSpoke::start()
{
    running_ = true;
    channel_->start();
    sampler_thread_ = std::thread([this] { sample_loop(); });
}

/// @brief Stop the channel and the sampling thread.
void MonitorSpoke::stop()
{
    running_ = false;
    sample_cv_.notify_all(); // do not wait out the remaining sampling interval
    if (sampler_thread_.joinable()) sampler_thread_.join();
    channel_->stop();
}

/// @brief Periodic sampling loop driving samples into the channel.
void MonitorSpoke::sample_loop()
{
    while (running_.load())
    {
        auto samples = core_->sample_once();
        for (const auto& s : samples)
        {
            channel_->enqueue([&s](monitord::MonitorSpokeFrame& f) {
                monitord::Sample* m = f.mutable_sample();
                m->mutable_ts()->set_mono_ns(s.mono_ns);
                m->set_metric(s.metric);
                m->set_value(s.value);
                m->set_unit(s.unit);
                m->set_resource_class(s.resource_class);
                for (const auto& kv : s.tags) (*m->mutable_tags())[kv.first] = kv.second;
            });
            ++frames_;
        }
        // Wait until the nearest class deadline (min interval across classes).
        // A plain sleep here would make shutdown take up to a full sampling
        // interval — minutes on a slow-sampling board — so wait on the stop
        // signal instead and wake immediately when it arrives.
        double interval = 1e9;
        for (const char* cls : {"cpu", "memory", "storage", "thermal", "load", "system"}) interval = std::min(interval, core_->sampler().interval_for(cls));
        std::unique_lock<std::mutex> lk(sample_mu_);
        sample_cv_.wait_for(lk, std::chrono::duration<double>(interval), [this] { return !running_.load(); });
    }
}

} // namespace domain
} // namespace monitord
} // namespace instr