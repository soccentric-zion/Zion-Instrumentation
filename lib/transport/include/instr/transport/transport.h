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
 * @file lib/transport/include/instr/transport/transport.h
 * @brief gRPC-over-UDS transport plumbing (R3: one wire
 *
 * format; §3.1 spokes dial istate, register, stay connected).
 * 
 * SpokeChannel<Stub, SpokeFrame, HubFrame>  — spoke client side:
 * dial → Register (schema verified) → bidi Channel; writer thread pumps
 * frames; reader thread dispatches HubFrames; reconnect with jittered
 * backoff; liveness ping answered automatically.
 * 
 * HubServiceBase<Service, SpokeFrame, HubFrame> — istate server side per
 * spoke socket (Service = <X>Hub::Service, the generated nested service
 * class); converts typed frames to GenericSpokeMsg so the istate core
 * stays proto-agnostic.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <grpcpp/support/sync_stream.h>

#include "common.pb.h"
#include "instr/common/backoff.h"
#include "instr/common/command_table.h"
#include "instr/common/json.h"
#include "instr/common/log.h"

namespace instr
{
/// @brief gRPC transport between the istate hub and daemon spokes (§3.1).
namespace transport
{

/// @brief Proto schema version of this suite; istate refuses mismatched registrations (REQ §3.1, §11).
constexpr uint32_t kProtoSchemaVersion = 2; // v2: config delivery (§9)

/// @brief Type-erased spoke→hub message used by the transport layer.
struct GenericSpokeMsg
{
    std::string daemon; ///< Which spoke sent it.
    std::string type; ///< e.g. "sample", "fault", "batch", "event".
    std::string payload_json; ///< Opaque JSON payload.
    common::TimeStamp ts; ///< Record timestamp.
};

/// @brief Type-erased hub→spoke message (oneof: command / backpressure / relay / time_sync / config).
struct GenericHubMsg
{
    bool has_command = false; ///< command is present.
    common::Command command; ///< Control command for the spoke.
    bool has_backpressure = false; ///< backpressure is present.
    common::Backpressure backpressure; ///< HQ backpressure signal.
    bool has_relay = false; ///< relay is present.
    common::RelayedData relay; ///< Data relayed from HQ.
    bool has_time_sync = false; ///< time_sync is present.
    common::TimeStamp time_sync; ///< HQ wall-clock correction.
    bool has_config = false; ///< config is present.
    common::ConfigDelivery config; ///< Configuration push for the spoke.
};

/// @brief Spoke-side gRPC bidi channel to istate: registration, streaming, config delivery and liveness.
///
/// @tparam StubOuter Generated service stub factory.
/// @tparam SpokeFrame Spoke→hub frame type.
/// @tparam HubFrame Hub→spoke frame type.
template <typename StubOuter, typename SpokeFrame, typename HubFrame> class SpokeChannel
{
  public:
    /// @brief Channel parameters.
    struct Options
    {
        std::string target; ///< "unix:/run/instrumentation/<daemon>.sock".
        std::string daemon; ///< Spoke identity used in the handshake.
        std::string build_version; ///< Build version reported at registration.
        uint32_t proto_version = kProtoSchemaVersion; ///< Proto schema version to advertise.
        std::vector<std::string> capabilities; ///< Capabilities advertised at registration.
        double backoff_base_s = 1.0; ///< Reconnect backoff base.
        double backoff_max_s = 30.0; ///< Reconnect backoff cap.
    };

    /// @brief Construct a channel with @p opts logging to @p log.
    /// @param opts Channel parameters.
    /// @param backoff_base_s Reconnect backoff base (unused after opts; kept for compatibility).
    /// @param backoff_max_s Reconnect backoff cap (unused after opts).
    SpokeChannel(Options opts, Logger& log) : opts_(std::move(opts)), log_(log), backoff_(opts_.backoff_base_s, opts_.backoff_max_s)
    {
    }

    /// @brief Callback invoked with every non-config hub frame.
    using HubHandler = std::function<void(const HubFrame&)>;
    /// @brief Callback that fills one spoke frame (mutates the oneof payload).
    using MakeFrame = std::function<void(SpokeFrame&)>;
    /// @brief Applies a delivered config; empty string on success, diagnosable reason on rejection.
    ///
    /// The spoke keeps its previous config on rejection and never opens /pst
    /// itself (R4).
    using ConfigHandler = std::function<std::string(const common::ConfigDelivery&)>;

    /// @brief Install the hub-frame handler (commands, relays, time sync).
    /// @param h Hub-frame callback.
    void set_hub_handler(HubHandler h)
    {
        hub_handler_ = std::move(h);
    }
    /// @brief Install the config-delivery handler.
    /// @param h Config-delivery callback.
    void set_config_handler(ConfigHandler h)
    {
        config_handler_ = std::move(h);
    }

    /// @brief Start the channel thread (reconnect loop).
    void start()
    {
        running_ = true;
        thread_ = std::thread([this] { run_loop(); });
    }
    /// @brief Stop the channel: cancel the in-flight RPC, drop queued frames and join.
    void stop()
    {
        running_ = false;
        // Cancel the in-flight RPC. The reader is parked in rw->Read(), which
        // does not return merely because a flag changed — on a quiet channel
        // (no commands, no pings) it would block indefinitely and the join
        // below would never complete. On a device that is a daemon that ignores
        // SIGTERM until systemd escalates to SIGKILL.
        {
            std::lock_guard<std::mutex> lk(ctx_mu_);
            if (active_ctx_) active_ctx_->TryCancel();
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            queue_.clear();
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    /// @brief True while the hub has accepted our registration.
    bool registered() const
    {
        return registered_;
    }

    /// @brief Enqueue a spoke frame (copied); non-blocking.
    /// @param fill Callback that populates the frame.
    void enqueue(const std::function<void(SpokeFrame&)>& fill)
    {
        SpokeFrame f;
        fill(f);
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(std::move(f));
        cv_.notify_one();
    }

    /// @brief Queue an ack frame.
    /// @param ack Ack frame to queue.
    void send_ack(const common::Ack& ack)
    {
        enqueue([&](SpokeFrame& f) { *f.mutable_ack() = ack; });
    }
    /// @brief Queue a liveness pong answering @p nonce.
    /// @param nonce Nonce being answered.
    void send_pong(uint64_t nonce)
    {
        enqueue([&](SpokeFrame& f) { f.mutable_pong()->set_nonce(nonce); });
    }
    /// @brief Queue a status JSON frame.
    /// @param status_json Status JSON to queue.
    void send_status_json(const std::string& status_json)
    {
        enqueue([&](SpokeFrame& f) { f.set_status_json(status_json); });
    }
    /// @brief Ask istate for this daemon's configuration (§R4: istate is the sole reader of /pst).
    void request_config()
    {
        enqueue([this](SpokeFrame& f) { f.mutable_config_request()->set_daemon(opts_.daemon); });
    }
    /// @brief Revision of the last config successfully applied (0 = none yet).
    uint64_t config_revision() const
    {
        return config_revision_.load();
    }

  private:
    /// @brief Reconnect loop: session attempts with jittered backoff between them.
    void run_loop()
    {
        while (running_.load())
        {
            if (try_session())
            {
                backoff_.reset();
                continue; // clean shutdown of session → reconnect promptly
            }
            double d = backoff_.next();
            log_.info(opts_.daemon, "reconnecting in " + std::to_string(d) + "s");
            // Sleep in small slices so stop() is responsive.
            double slept = 0;
            while (slept < d && running_.load())
            {
                struct timespec ts
                {
                    0, 100 * 1000 * 1000
                };
                nanosleep(&ts, nullptr);
                slept += 0.1;
            }
        }
    }

    /// @brief One registration+stream session; returns true for a clean session end.
    bool try_session()
    {
        auto creds = grpc::InsecureChannelCredentials(); // UDS; TLS terminates at the tunnel
        auto channel = grpc::CreateCustomChannel(opts_.target, creds, grpc::ChannelArguments());
        auto stub = StubOuter::NewStub(channel);
        registered_ = false;

        // Registration handshake (§3.1): daemon name, build version, proto schema
        // version, capabilities. Mismatch → refuse; never guess.
        grpc::ClientContext rctx;
        // Bound the handshake so a hub that accepts the connection but never
        // answers cannot wedge the reconnect loop.
        rctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        common::RegistrationRequest req;
        req.set_daemon(opts_.daemon);
        req.set_build_version(opts_.build_version);
        req.set_proto_schema_version(opts_.proto_version);
        for (const auto& c : opts_.capabilities) req.add_capabilities(c);
        common::RegistrationReply reply;
        grpc::Status rs = stub->Register(&rctx, req, &reply);
        if (!rs.ok()) return false;
        if (!reply.accepted())
        {
            log_.critical(opts_.daemon, "registration refused: " + reply.reason());
            // Proto version mismatch is fatal until versions align; keep backing
            // off so the operator sees it but the device keeps running (R10).
            return false;
        }
        registered_ = true;

        grpc::ClientContext cctx;
        {
            std::lock_guard<std::mutex> lk(ctx_mu_);
            if (!running_.load()) return true; // stopped between register and stream
            active_ctx_ = &cctx;
        }
        // The context outlives the stream below; clear the pointer before it
        // goes out of scope so stop() never cancels a dangling context.
        struct CtxGuard
        {
            SpokeChannel* self;
            ~CtxGuard()
            {
                std::lock_guard<std::mutex> lk(self->ctx_mu_);
                self->active_ctx_ = nullptr;
            }
        } ctx_guard{this};
        auto rw = stub->Channel(&cctx);
        // Writer thread.
        std::atomic<bool> stream_ok{true};
        std::thread writer([this, &rw, &stream_ok] {
            while (running_.load() && stream_ok.load())
            {
                SpokeFrame f;
                {
                    std::unique_lock<std::mutex> lk(mu_);
                    cv_.wait_for(lk, std::chrono::milliseconds(100), [this] { return !queue_.empty(); });
                    if (queue_.empty()) continue;
                    f = std::move(queue_.front());
                    queue_.pop_front();
                }
                if (!rw->Write(f))
                {
                    stream_ok = false;
                    return;
                }
            }
        });
        // Reader loop (this thread).
        // Registration is complete: ask for our configuration straight away, so
        // the daemon is running on /pst state rather than compiled defaults.
        request_config();

        HubFrame hf;
        while (running_.load() && rw->Read(&hf))
        {
            if (hf.has_config())
            {
                std::string reason;
                if (config_handler_) reason = config_handler_(hf.config());
                if (reason.empty())
                {
                    config_revision_ = hf.config().revision();
                    log_.info(opts_.daemon, "config applied, revision " + std::to_string(hf.config().revision()));
                }
                else
                {
                    // Loud rejection: keep the previous config and tell istate why.
                    log_.error(opts_.daemon, "config delivery rejected: " + reason);
                }
                SpokeFrame ack;
                common::ConfigAck* ca = ack.mutable_config_ack();
                ca->set_daemon(opts_.daemon);
                ca->set_revision(hf.config().revision());
                ca->set_applied(reason.empty());
                ca->set_reason(reason);
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    queue_.push_back(std::move(ack));
                }
                cv_.notify_one();
            }
            if (hub_handler_) hub_handler_(hf);
            // Built-in liveness: a command op "liveness-ping" must be answered even
            // when the domain layer is busy — respond here directly.
            if (hf.has_command() && hf.command().op() == "liveness-ping")
            {
                // nonce arrives in json_args {"nonce": N}
                auto args = json_parse(hf.command().json_args());
                uint64_t nonce = 0;
                if (args.ok())
                {
                    if (const JValue* n = (*args).find("nonce")) nonce = uint64_t(n->as_number());
                }
                SpokeFrame pong;
                pong.mutable_pong()->set_nonce(nonce);
                std::lock_guard<std::mutex> lk(mu_);
                queue_.push_back(std::move(pong));
                cv_.notify_one();
            }
        }
        stream_ok = false;
        cv_.notify_all();
        writer.join();
        registered_ = false;
        return true;
    }

    Options opts_; ///< Channel parameters.
    Logger& log_; ///< Logger for reconnect and rejection events.
    HubHandler hub_handler_; ///< Hub-frame callback (domain layer).
    JitteredBackoff backoff_; ///< Reconnect backoff sequence.
    std::thread thread_; ///< Thread running run_loop().
    std::mutex mu_; ///< Guards the outbound queue.
    std::condition_variable cv_; ///< Signals queue producers/consumers.
    std::deque<SpokeFrame> queue_; ///< Outbound frames awaiting the writer.
    std::atomic<bool> running_{false}; ///< Lifecycle flag.
    std::atomic<bool> registered_{false}; ///< True between accepted registration and session end.
    ConfigHandler config_handler_; ///< Config-delivery callback.
    std::atomic<uint64_t> config_revision_{0}; ///< Last successfully applied config revision.
    std::mutex ctx_mu_; ///< Guards active_ctx_.
    grpc::ClientContext* active_ctx_ = nullptr; ///< In-flight stream context (guarded by ctx_mu_); stop() cancels it.
};

/// @brief istate-side hub service base: registration handshake plus one bidi stream per spoke.
///
/// @tparam Service Generated <X>Hub::Service type.
/// @tparam SpokeFrame Spoke→hub frame type.
/// @tparam HubFrame Hub→spoke frame type.
template <typename Service, typename SpokeFrame, typename HubFrame> class HubServiceBase : public Service
{
  public:
    /// @brief Inbound spoke-frame consumer (istate core ingest).
    using FrameIn = std::function<void(const GenericSpokeMsg&)>;
    /// @brief Outbound hub-frame source (enqueue for the spoke).
    using FrameOut = std::function<bool(const GenericHubMsg&)>;

    /// @brief Construct the hub for @p daemon reporting @p build_version.
    /// @param daemon Accepted spoke identity.
    /// @param build_version Build version returned in the handshake.
    HubServiceBase(std::string daemon, std::string build_version) : daemon_(std::move(daemon)), build_version_(std::move(build_version))
    {
    }

    /// @brief Install the inbound frame consumer.
    /// @param f Inbound frame consumer.
    void set_ingest(FrameIn f)
    {
        ingest_ = std::move(f);
    }
    /// @brief Install the outbound frame source.
    /// @param f Outbound frame source.
    void set_outbound(FrameOut f)
    {
        outbound_ = std::move(f);
    }
    /// @brief Reports each registration attempt (accepted or refused) to istate.
    ///
    /// The orchestrator records what actually attached (build version, proto
    /// version, capabilities) and raises the alarm a mismatch requires (§3.1,
    /// §11). Without this the handshake result never leaves the transport.
    using RegistrationFn = std::function<void(const common::RegistrationRequest&, bool accepted)>;
    /// @brief Install the registration-reporting callback.
    /// @param f Registration-reporting callback.
    void set_registration_fn(RegistrationFn f)
    {
        on_register_ = std::move(f);
    }
    /// @brief The daemon identity this hub accepts registrations from.
    /// @return The accepted spoke identity.
    const std::string& daemon() const
    {
        return daemon_;
    }

    /// @brief Registration RPC: validates daemon name and proto version, reports the outcome.
    /// @param req Registration request.
    /// @param reply Reply to populate.
    /// @return gRPC status (always OK; acceptance is in @p reply).
    grpc::Status Register(grpc::ServerContext*, const common::RegistrationRequest* req, common::RegistrationReply* reply) override
    {
        if (req->daemon() != daemon_)
        {
            reply->set_accepted(false);
            reply->set_reason("daemon name mismatch: " + req->daemon() + " != " + daemon_);
            if (on_register_) on_register_(*req, false);
            return grpc::Status::OK;
        }
        if (req->proto_schema_version() != kProtoSchemaVersion)
        {
            // Proto version mismatch: refuse, alarm raised by istate core, never
            // guess or silently degrade (REQ §3.1, §11).
            reply->set_accepted(false);
            reply->set_reason("proto schema version mismatch: spoke " + std::to_string(req->proto_schema_version()) + " vs istate " + std::to_string(kProtoSchemaVersion));
            refused_ = true;
            if (on_register_) on_register_(*req, false);
            return grpc::Status::OK;
        }
        registered_ = true;
        if (on_register_) on_register_(*req, true);
        reply->set_accepted(true);
        reply->set_proto_schema_version(kProtoSchemaVersion);
        reply->set_istate_version(build_version_);
        return grpc::Status::OK;
    }

    /// @brief Bidi channel RPC: reader on this thread, writer thread draining the outbound queue.
    /// @param stream Bidi stream to serve.
    /// @return gRPC status when the session ends.
    grpc::Status Channel(grpc::ServerContext*, grpc::ServerReaderWriter<HubFrame, SpokeFrame>* stream) override
    {
        // Reader (this thread) + writer thread.
        std::atomic<bool> open{true};
        std::thread writer([this, stream, &open] {
            while (open.load())
            {
                GenericHubMsg m;
                bool have = false;
                {
                    std::unique_lock<std::mutex> lk(mu_);
                    cv_.wait_for(lk, std::chrono::milliseconds(100), [this] { return !out_.empty(); });
                    if (!out_.empty())
                    {
                        m = std::move(out_.front());
                        out_.pop_front();
                        have = true;
                    }
                }
                if (!have) continue;
                HubFrame f;
                apply(f, m);
                if (!stream->Write(f))
                {
                    open = false;
                    return;
                }
            }
        });
        SpokeFrame sf;
        while (open.load() && stream->Read(&sf))
        {
            if (ingest_) ingest_(to_generic(sf));
        }
        open = false;
        cv_.notify_all();
        writer.join();
        registered_ = false;
        return grpc::Status::OK;
    }

    /// @brief Queue a hub message for the spoke.
    /// @param m Hub message to queue.
    void push(const GenericHubMsg& m)
    {
        std::lock_guard<std::mutex> lk(mu_);
        out_.push_back(m);
        cv_.notify_one();
    }
    /// @brief True while the spoke is registered on this hub.
    bool spoke_registered() const
    {
        return registered_;
    }
    /// @brief True when a registration was refused (proto mismatch) since start.
    bool refused() const
    {
        return refused_;
    }

  protected:
    /// @brief Convert a typed spoke frame into the generic transport message.
    /// @param f Typed spoke frame to convert.
    /// @return The generic transport message.
    virtual GenericSpokeMsg to_generic(const SpokeFrame& f) = 0;
    /// @brief Apply a generic hub message onto a typed hub frame.
    /// @param f Typed hub frame to fill.
    /// @param m Generic hub message.
    virtual void apply(HubFrame& f, const GenericHubMsg& m) = 0;

    std::string daemon_; ///< Accepted spoke identity.
    std::string build_version_; ///< istate build version returned in the handshake.
    FrameIn ingest_; ///< Inbound frame consumer.
    FrameOut outbound_; ///< Outbound frame source.
    std::mutex mu_; ///< Guards the outbound queue.
    std::condition_variable cv_; ///< Signals queue producers/consumers.
    std::deque<GenericHubMsg> out_; ///< Outbound messages awaiting the writer.
    std::atomic<bool> registered_{false}; ///< Current registration state.
    std::atomic<bool> refused_{false}; ///< Latched on a proto-version refusal.
    RegistrationFn on_register_; ///< Registration-reporting callback.
};

// ---- istate ↔ HQ session client (HqPeer) ----
// Defined against istate.proto types by the istate daemon transport; shared
// helpers live here.

/// @brief UDS target helper: "unix:<path>".
/// @param path Socket path.
/// @return "unix:<path>".
inline std::string uds_target(const std::string& path)
{
    return "unix:" + path;
}

/// @brief Shared command handling for every spoke: dispatch through the daemon's command table.
///
/// Produces an Ack frame (same table as _ctrl, §3.2).
/// @param table Command table to dispatch through (may be null).
/// @param cmd Command to run.
/// @return The ack frame carrying the result.
inline common::Ack dispatch_to_ack(const CommandTable* table, const common::Command& cmd)
{
    CommandResult r = table ? table->dispatch(cmd.op(), cmd.json_args()) : CommandResult::err(Errc::Unavailable, "no table");
    common::Ack ack;
    ack.set_id(cmd.id());
    ack.set_ok(r.ok);
    if (r.ok) { ack.set_json_payload(r.json_data); }
    else
    {
        ack.set_code(r.error_code);
        ack.set_message(r.error_message);
    }
    return ack;
}

/// @brief Copy a Stamp into a proto TimeStamp.
/// @param ts TimeStamp to populate.
/// @param s Stamp to copy from.
inline void set_ts(common::TimeStamp* ts, const Stamp& s)
{
    ts->set_mono_ns(s.mono_ns);
    ts->set_boot_id(s.boot_id);
    ts->set_wall_unix_ns(s.wall_ns);
    ts->set_sequence(s.seq);
    ts->set_wall_valid(s.wall_valid);
}

} // namespace transport
} // namespace instr