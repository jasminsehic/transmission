// WebRTC signaling orchestrator: non-trickle SDP after ICE gathering completes.
// All maps and callbacks are accessed on Transmission's session event thread.
#include <algorithm>
#include <array>
#include <string_view>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wextra-semi"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include <rtc/rtc.hpp>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <fmt/format.h>

#include "libtransmission/crypto-utils.h"
#include "libtransmission/log.h"
#include "libtransmission/net.h"
#include "libtransmission/peer-socket-rtc.h"
#include "libtransmission/session.h"
#include "libtransmission/webtorrent-signaling.h"
#include "libtransmission/webtorrent-endpoint.h"
#include "libtransmission/webtorrent-diagnostics.h"

using namespace std::chrono_literals;

namespace
{
constexpr auto OfferLifetime = 120s;
constexpr auto GatheringLifetime = 20s;
constexpr auto MaxOutstandingPeers = size_t{ 128 };
constexpr auto MaxOffersPerAnnounce = size_t{ 4 };

std::string_view candidate_type_name(rtc::Candidate::Type type)
{
    switch (type)
    {
    case rtc::Candidate::Type::Host: return "host";
    case rtc::Candidate::Type::ServerReflexive: return "srflx";
    case rtc::Candidate::Type::PeerReflexive: return "prflx";
    case rtc::Candidate::Type::Relayed: return "relay";
    default: return "unknown";
    }
}

std::string_view rtc_state_name(rtc::PeerConnection::State state)
{
    switch (state)
    {
    case rtc::PeerConnection::State::New: return "new";
    case rtc::PeerConnection::State::Connecting: return "connecting";
    case rtc::PeerConnection::State::Connected: return "connected";
    case rtc::PeerConnection::State::Disconnected: return "disconnected";
    case rtc::PeerConnection::State::Failed: return "failed";
    case rtc::PeerConnection::State::Closed: return "closed";
    default: return "unknown";
    }
}

std::string_view ice_state_name(rtc::PeerConnection::IceState state)
{
    switch (state)
    {
    case rtc::PeerConnection::IceState::New: return "new";
    case rtc::PeerConnection::IceState::Checking: return "checking";
    case rtc::PeerConnection::IceState::Connected: return "connected";
    case rtc::PeerConnection::IceState::Completed: return "completed";
    case rtc::PeerConnection::IceState::Failed: return "failed";
    case rtc::PeerConnection::IceState::Disconnected: return "disconnected";
    case rtc::PeerConnection::IceState::Closed: return "closed";
    default: return "unknown";
    }
}

std::string hash_key(tr_sha1_digest_t const& hash)
{
    return { reinterpret_cast<char const*>(hash.data()), hash.size() };
}

std::string new_offer_id()
{
    auto random = std::array<uint8_t, 10>{};
    tr_rand_buffer(random.data(), random.size());
    static constexpr auto Hex = "0123456789abcdef";
    auto result = std::string{};
    result.reserve(20);
    for (auto const byte : random)
    {
        result.push_back(Hex[byte >> 4U]);
        result.push_back(Hex[byte & 0x0FU]);
    }
    return result;
}
} // namespace

struct tr_webtorrent_signaling::State : std::enable_shared_from_this<State>
{
    struct Batch;
    struct Peer;

    explicit State(tr_session* value)
        : session{ value }
    {
    }

    ~State()
    {
        closing = true;
        peers.clear();
        batches.clear();
    }

    tr_session* const session;
    std::atomic<bool> closing = false;
    std::map<std::string, std::shared_ptr<Peer>> peers;
    std::vector<std::shared_ptr<Batch>> batches;
    uint64_t next_diagnostic_peer = 0;
    uint64_t incoming_offers = 0;
    uint64_t incoming_rejections = 0;
    uint64_t remote_answer_rejections = 0;
    uint64_t outgoing_creation_failures = 0;
    uint64_t ice_failures = 0;
    uint64_t peer_connection_failures = 0;
    uint64_t peer_expirations = 0;
    uint64_t connected_peers = 0;
    uint64_t registered_channels = 0;
    uint64_t selected_pairs = 0;
    std::array<uint64_t, 5> gathered_candidate_types{};
    std::chrono::steady_clock::time_point last_diagnostic_summary{};

    struct Batch
    {
        size_t remaining = 0;
        bool completed = false;
        std::chrono::steady_clock::time_point deadline;
        std::vector<tr_webtorrent::Offer> offers;
        tr_webtorrent_signaling::OffersReady callback;

        void complete()
        {
            if (completed)
            {
                return;
            }
            completed = true;
            auto fn = std::move(callback);
            fn(std::move(offers));
        }
    };

    struct Peer : std::enable_shared_from_this<Peer>
    {
        Peer(std::shared_ptr<State> const& parent, std::string key, std::string tracker, std::string hash, bool is_incoming)
            : owner{ parent }
            , diagnostic_id{ ++parent->next_diagnostic_peer }
            , id{ std::move(key) }
            , tracker_url{ std::move(tracker) }
            , info_hash{ std::move(hash) }
            , incoming{ is_incoming }
            , pc{ std::make_shared<rtc::PeerConnection>(ice_config(parent->session)) }
            , created{ std::chrono::steady_clock::now() }
        {
        }

        ~Peer()
        {
            try
            {
                pc->resetCallbacks();
                if (dc)
                {
                    dc->onOpen(nullptr);
                }
                pc->close();
            }
            catch (std::exception const&)
            {
                // May already be closed.
            }
        }

        static rtc::Configuration ice_config(tr_session const* session)
        {
            auto config = rtc::Configuration{};
            for (auto const& server : session->settings().webtorrent_ice_servers)
            {
                if (server.starts_with("stun:") || server.starts_with("turn:") || server.starts_with("turns:"))
                {
                    config.iceServers.emplace_back(server);
                }
            }
            return config;
        }

        void dispatch(std::function<void(Peer&)> callback)
        {
            auto const parent = owner.lock();
            if (!parent || parent->closing)
            {
                return;
            }
            auto const weak = weak_from_this();
            parent->session->queue_session_thread(
                [weak, callback = std::move(callback)]()
                {
                    if (auto peer = weak.lock())
                    {
                        if (auto parent = peer->owner.lock(); parent && !parent->closing)
                        {
                            callback(*peer);
                        }
                    }
                });
        }

        void init()
        {
            auto const weak = weak_from_this();
            pc->onGatheringStateChange(
                [weak](rtc::PeerConnection::GatheringState state)
                {
                    if (auto peer = weak.lock())
                    {
                        peer->dispatch(
                            [state](Peer& p)
                            {
                                tr_logAddDebug(
                                    fmt::format("WebRTC peer={} gathering-state={}", p.diagnostic_id,
                                        state == rtc::PeerConnection::GatheringState::Complete ? "complete" :
                                        state == rtc::PeerConnection::GatheringState::InProgress ? "in-progress" : "new"),
                                    "WebTorrent");
                                if (state == rtc::PeerConnection::GatheringState::Complete)
                                {
                                    p.gathering_complete();
                                }
                            });
                    }
                });
            pc->onLocalCandidate(
                [weak](rtc::Candidate candidate)
                {
                    if (auto peer = weak.lock())
                    {
                        peer->dispatch(
                            [type = candidate.type()](Peer& p)
                            {
                                if (auto parent = p.owner.lock())
                                {
                                    auto const slot = static_cast<size_t>(type);
                                    if (slot < parent->gathered_candidate_types.size())
                                    {
                                        ++parent->gathered_candidate_types[slot];
                                    }
                                    if (slot < p.candidate_type_seen.size() && !p.candidate_type_seen[slot])
                                    {
                                        p.candidate_type_seen[slot] = true;
                                        tr_logAddDebug(
                                            fmt::format("WebRTC peer={} local-candidate-type={}", p.diagnostic_id,
                                                candidate_type_name(type)),
                                            "WebTorrent");
                                    }
                                }
                            });
                    }
                });
            pc->onIceStateChange(
                [weak](rtc::PeerConnection::IceState state)
                {
                    if (auto peer = weak.lock())
                    {
                        peer->dispatch(
                            [state](Peer& p)
                            {
                                tr_logAddDebug(
                                    fmt::format("WebRTC peer={} ice-state={}", p.diagnostic_id,
                                        ice_state_name(state)),
                                    "WebTorrent");
                                if (state == rtc::PeerConnection::IceState::Connected ||
                                    state == rtc::PeerConnection::IceState::Completed)
                                {
                                    p.log_selected_pair();
                                }
                                if (state == rtc::PeerConnection::IceState::Failed)
                                {
                                    if (auto parent = p.owner.lock())
                                    {
                                        ++parent->ice_failures;
                                    }
                                }
                            });
                    }
                });
            pc->onStateChange(
                [weak](rtc::PeerConnection::State state)
                {
                    if (auto peer = weak.lock())
                    {
                        peer->dispatch(
                            [state](Peer& p)
                            {
                                tr_logAddDebug(
                                    fmt::format("WebRTC peer={} direction={} connection-state={}", p.diagnostic_id,
                                        p.incoming ? "incoming" : "outgoing", rtc_state_name(state)),
                                    "WebTorrent");
                                if (auto parent = p.owner.lock())
                                {
                                    if (state == rtc::PeerConnection::State::Connected)
                                    {
                                        ++parent->connected_peers;
                                        p.log_selected_pair();
                                    }
                                    if (state == rtc::PeerConnection::State::Failed)
                                    {
                                        ++parent->peer_connection_failures;
                                    }
                                }
                                if (state == rtc::PeerConnection::State::Failed ||
                                    state == rtc::PeerConnection::State::Closed)
                                {
                                    p.remove();
                                }
                            });
                    }
                });
            if (incoming)
            {
                pc->onDataChannel(
                    [weak](std::shared_ptr<rtc::DataChannel> dc)
                    {
                        if (auto peer = weak.lock())
                        {
                            peer->dispatch(
                                [dc = std::move(dc)](Peer& p) { p.attach_channel(dc); });
                        }
                    });
            }
        }

        void log_selected_pair()
        {
            if (selected_pair_reported)
            {
                return;
            }
            try
            {
                auto local = rtc::Candidate{};
                auto remote = rtc::Candidate{};
                if (!pc->getSelectedCandidatePair(&local, &remote))
                {
                    return;
                }
                selected_pair_reported = true;
                if (auto parent = owner.lock())
                {
                    ++parent->selected_pairs;
                }
                tr_logAddInfo(
                    fmt::format("WebRTC peer={} selected-pair local={} remote={}", diagnostic_id,
                        candidate_type_name(local.type()), candidate_type_name(remote.type())),
                    "WebTorrent");
            }
            catch (std::exception const&)
            {
                tr_logAddDebug(fmt::format("WebRTC peer={} selected-pair unavailable", diagnostic_id), "WebTorrent");
            }
        }

        void start_outgoing(std::shared_ptr<Batch> const& batch_in)
        {
            tr_logAddDebug("WebRTC outgoing offer started", "WebTorrent");
            batch = batch_in;
            dc = pc->createDataChannel("webtorrent");
            attach_channel(dc);
        }

        void start_incoming(
            tr_webtorrent::Signal const& signal,
            std::string our_peer,
            tr_webtorrent_signaling::SendAnswer send)
        {
            tr_logAddDebug(fmt::format("WebRTC peer={} incoming offer processing", diagnostic_id), "WebTorrent");
            remote_offer_id = signal.offer_id;
            remote_peer_id = signal.peer_id;
            our_peer_id = std::move(our_peer);
            send_answer = std::move(send);
            // libdatachannel creates the local answer automatically when the remote offer is applied.
            failure_stage = "remote-description-or-auto-answer";
            pc->setRemoteDescription(rtc::Description(signal.sdp, rtc::Description::Type::Offer));
            tr_logAddDebug(
                fmt::format("WebRTC peer={} incoming remote offer accepted; automatic answer started", diagnostic_id),
                "WebTorrent");
        }

        void gathering_complete()
        {
            tr_logAddDebug("WebRTC ICE gathering complete", "WebTorrent");
            if (description_sent)
            {
                return;
            }
            auto const desc = pc->localDescription();
            if (!desc)
            {
                tr_logAddWarn("WebRTC ICE gathering completed without local SDP", "WebTorrent");
                return;
            }
            description_sent = true;
            auto const sdp = desc->generateSdp();
            if (incoming)
            {
                tr_logAddDebug("WebRTC incoming answer ready", "WebTorrent");
                if (send_answer)
                {
                    send_answer(
                        tr_webtorrent::encode_answer(info_hash, our_peer_id, remote_peer_id, remote_offer_id, sdp));
                }
            }
            else if (auto pending = batch.lock())
            {
                tr_logAddTrace("WebRTC outgoing offer ready", "WebTorrent");
                pending->offers.emplace_back(tr_webtorrent::Offer{ id, sdp });
                if (pending->remaining > 0)
                {
                    --pending->remaining;
                }
                if (pending->remaining == 0)
                {
                    pending->complete();
                }
            }
        }

        void attach_channel(std::shared_ptr<rtc::DataChannel> channel)
        {
            if (attached || !channel)
            {
                return;
            }
            dc = std::move(channel);
            auto const weak = weak_from_this();
            dc->onOpen(
                [weak]()
                {
                    if (auto peer = weak.lock())
                    {
                        peer->dispatch([](Peer& p) { p.on_open(); });
                    }
                });
            if (dc->isOpen())
            {
                on_open();
            }
        }

        void on_open()
        {
            if (attached || !dc || !dc->isOpen())
            {
                return;
            }
            auto const parent = owner.lock();
            if (!parent || parent->closing)
            {
                return;
            }
            // Follow libtorrent RC_2_1: get the selected remote ICE endpoint
            // from libdatachannel instead of inventing a fake socket address.
            auto const remote_address = pc->remoteAddress();
            auto const socket_address =
                remote_address ? tr_webtorrent_endpoint::parse(*remote_address) : std::nullopt;
            if (!socket_address)
            {
                tr_logAddWarn(
                    fmt::format("WebRTC peer={} selected ICE remote endpoint unavailable or invalid", diagnostic_id),
                    "WebTorrent");
                remove();
                return;
            }
            attached = true;
            tr_logAddInfo(
                fmt::format("WebRTC peer={} data channel open; registering BitTorrent peer (remote={})",
                    diagnostic_id, socket_address->address().is_ipv6() ? "ipv6" : "ipv4"),
                "WebTorrent");
            auto const weak_state = std::weak_ptr<State>{ parent };
            auto socket = tr_peer_socket_rtc::create(
                *socket_address,
                dc,
                [weak_state](std::function<void()> callback)
                {
                    if (auto state = weak_state.lock(); state && !state->closing)
                    {
                        state->session->queue_session_thread(std::move(callback));
                    }
                });
            parent->session->addIncoming(std::move(socket));
            ++parent->registered_channels;
            tr_logAddDebug(fmt::format("WebRTC peer={} incoming peer socket submitted to session", diagnostic_id), "WebTorrent");
        }

        void on_answer(std::string const& sdp)
        {
            if (incoming || answered || sdp.empty())
            {
                return;
            }
            pc->setRemoteDescription(rtc::Description(sdp, rtc::Description::Type::Answer));
            answered = true;
            tr_logAddDebug(fmt::format("WebRTC peer={} remote answer accepted", diagnostic_id), "WebTorrent");
        }

        void remove()
        {
            if (auto parent = owner.lock())
            {
                auto it = parent->peers.find(id);
                if (it != parent->peers.end() && it->second.get() == this)
                {
                    parent->peers.erase(it);
                }
            }
        }

        std::weak_ptr<State> owner;
        uint64_t const diagnostic_id;
        std::string const id;
        std::string const tracker_url;
        std::string const info_hash;
        bool const incoming;
        std::shared_ptr<rtc::PeerConnection> pc;
        std::shared_ptr<rtc::DataChannel> dc;
        std::weak_ptr<Batch> batch;
        std::chrono::steady_clock::time_point const created;
        tr_webtorrent_signaling::SendAnswer send_answer;
        std::string remote_peer_id;
        std::string remote_offer_id;
        std::string our_peer_id;
        bool answered = false;
        bool description_sent = false;
        bool attached = false;
        bool selected_pair_reported = false;
        std::array<bool, 5> candidate_type_seen{};
        std::string_view failure_stage = "create-peer";
    };
};

tr_webtorrent_signaling::tr_webtorrent_signaling(tr_session* session)
    : state_{ std::make_shared<State>(session) }
{
}

tr_webtorrent_signaling::~tr_webtorrent_signaling()
{
    state_->closing = true;
    state_->peers.clear();
    state_->batches.clear();
}

void tr_webtorrent_signaling::make_offers(
    tr_sha1_digest_t const& hash,
    std::string tracker_url,
    size_t count,
    OffersReady on_ready)
{
    count = std::min({ count, MaxOffersPerAnnounce, MaxOutstandingPeers - std::min(MaxOutstandingPeers, state_->peers.size()) });
    tr_logAddDebug(fmt::format("WebRTC offer batch requested: {}", count), "WebTorrent");
    if (count == 0)
    {
        on_ready({});
        return;
    }
    auto batch = std::make_shared<State::Batch>();
    batch->remaining = count;
    batch->deadline = std::chrono::steady_clock::now() + GatheringLifetime;
    batch->callback = std::move(on_ready);
    state_->batches.emplace_back(batch);
    auto const hash_str = hash_key(hash);

    for (size_t i = 0; i < count; ++i)
    {
        auto peer = std::shared_ptr<State::Peer>{};
        try
        {
            auto id = new_offer_id();
            while (state_->peers.find(id) != state_->peers.end())
            {
                id = new_offer_id();
            }
            peer = std::make_shared<State::Peer>(state_, id, tracker_url, hash_str, false);
            state_->peers.emplace(peer->id, peer);
            peer->init();
            peer->start_outgoing(batch);
        }
        catch (std::exception const&)
        {
            ++state_->outgoing_creation_failures;
            tr_logAddWarn("WebRTC outgoing offer creation failed; see diagnostic counters", "WebTorrent");
            if (peer)
            {
                peer->remove();
            }
            if (batch->remaining != 0)
            {
                --batch->remaining;
            }
        }
    }
    if (batch->remaining == 0)
    {
        batch->complete();
    }
}

void tr_webtorrent_signaling::on_signal(
    tr_webtorrent::Signal const& signal,
    std::string const& tracker_url,
    std::string const& our_peer_id,
    SendAnswer send_answer)
{
    if (signal.kind == tr_webtorrent::Signal::Kind::Answer)
    {
        auto it = state_->peers.find(signal.offer_id);
        if (it == state_->peers.end() || it->second->incoming ||
            it->second->tracker_url != tracker_url || it->second->info_hash != signal.info_hash)
        {
            return;
        }
        try
        {
            it->second->on_answer(signal.sdp);
        }
        catch (std::exception const&)
        {
            ++state_->remote_answer_rejections;
            tr_logAddWarn(
                fmt::format("WebRTC peer={} remote answer rejected reason={}", it->second->diagnostic_id,
                    tr_webtorrent_diagnostics::sdp_issue(signal.sdp)),
                "WebTorrent");
            it->second->remove();
        }
        return;
    }
    if (signal.kind != tr_webtorrent::Signal::Kind::Offer ||
        signal.peer_id == our_peer_id || our_peer_id.size() != 20U ||
        state_->peers.size() >= MaxOutstandingPeers)
    {
        return;
    }

    ++state_->incoming_offers;
    auto peer = std::shared_ptr<State::Peer>{};
    try
    {
        auto id = new_offer_id();
        while (state_->peers.find(id) != state_->peers.end())
        {
            id = new_offer_id();
        }
        peer = std::make_shared<State::Peer>(state_, id, tracker_url, signal.info_hash, true);
        state_->peers.emplace(peer->id, peer);
        peer->init();
        peer->start_incoming(signal, our_peer_id, std::move(send_answer));
    }
    catch (std::exception const&)
    {
        ++state_->incoming_rejections;
        tr_logAddWarn(
            fmt::format("WebRTC incoming offer rejected peer={} stage={} reason={}",
                peer ? peer->diagnostic_id : 0,
                peer ? peer->failure_stage : "create-peer",
                tr_webtorrent_diagnostics::sdp_issue(signal.sdp)),
            "WebTorrent");
        // A rejected SDP must not occupy a pending peer slot.
        if (peer)
        {
            peer->remove();
        }
    }
}

void tr_webtorrent_signaling::upkeep()
{
    auto const now = std::chrono::steady_clock::now();
    if (state_->last_diagnostic_summary.time_since_epoch().count() == 0 ||
        now - state_->last_diagnostic_summary >= 60s)
    {
        state_->last_diagnostic_summary = now;
        auto const& types = state_->gathered_candidate_types;
        tr_logAddInfo(
            fmt::format("WebRTC diagnostics incoming={}/rejected={} remote-answers-rejected={} "
                "outgoing-creation-failures={} ice-failures={} peer-failures={} "
                "expired={} connected={} registered={} selected-pairs={} "
                "local-candidates=host:{}/srflx:{}/prflx:{}/relay:{}/unknown:{}",
                state_->incoming_offers, state_->incoming_rejections, state_->remote_answer_rejections,
                state_->outgoing_creation_failures, state_->ice_failures, state_->peer_connection_failures,
                state_->peer_expirations, state_->connected_peers, state_->registered_channels,
                state_->selected_pairs, types[1], types[2], types[3], types[4], types[0]),
            "WebTorrent");
    }
    for (auto it = state_->batches.begin(); it != state_->batches.end();)
    {
        auto& batch = *it;
        if (batch->remaining == 0 || batch->deadline <= now)
        {
            if (batch->remaining != 0)
            {
                tr_logAddWarn(fmt::format("WebRTC ICE gathering timed out for {} offers", batch->remaining), "WebTorrent");
            }
            batch->complete();
            it = state_->batches.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (auto it = state_->peers.begin(); it != state_->peers.end();)
    {
        auto const& peer = it->second;
        if (!peer->attached && now - peer->created >= OfferLifetime)
        {
            ++state_->peer_expirations;
            it = state_->peers.erase(it);
        }
        else
        {
            ++it;
        }
    }
}
