// WebRTC signaling orchestrator: non-trickle SDP after ICE gathering completes.
// All maps and callbacks are accessed on Transmission's session event thread.
#include <algorithm>
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

using namespace std::chrono_literals;

namespace
{
constexpr auto OfferLifetime = 120s;
constexpr auto GatheringLifetime = 20s;
constexpr auto MaxOutstandingPeers = size_t{ 128 };
constexpr auto MaxOffersPerAnnounce = size_t{ 4 };

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
    uint32_t next_virtual_peer = 0;

    tr_socket_address allocate_address()
    {
        // Internal identity only: RFC 2544 benchmark range is not dialed and
        // must not enter classic PEX. Avoid collision with real TCP endpoints.
        auto const id = ++next_virtual_peer;
        auto const host_a = (id >> 8U) & 0xFFU;
        auto const host_b = id & 0xFFU;
        auto const port = 1024U + (id % 64000U);
        // IPv4 dotted address must contain four octets (198.18.a.b).
        return *tr_socket_address::from_string(fmt::format("198.18.{}.{}:{}", host_a, host_b, port));
    }

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
            , id{ std::move(key) }
            , tracker_url{ std::move(tracker) }
            , info_hash{ std::move(hash) }
            , incoming{ is_incoming }
            , pc{ std::make_shared<rtc::PeerConnection>(ice_config()) }
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

        static rtc::Configuration ice_config()
        {
            auto config = rtc::Configuration{};
            config.iceServers.emplace_back("stun:stun.l.google.com:19302");
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
                    if (state == rtc::PeerConnection::GatheringState::Complete)
                    {
                        if (auto peer = weak.lock())
                        {
                            peer->dispatch([](Peer& p) { p.gathering_complete(); });
                        }
                    }
                });
            pc->onStateChange(
                [weak](rtc::PeerConnection::State state)
                {
                    if (state == rtc::PeerConnection::State::Failed ||
                        state == rtc::PeerConnection::State::Closed)
                    {
                        if (auto peer = weak.lock())
                        {
                            peer->dispatch([](Peer& p) { p.remove(); });
                        }
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

        void start_outgoing(std::shared_ptr<Batch> const& batch_in)
        {
            batch = batch_in;
            dc = pc->createDataChannel("webtorrent");
            attach_channel(dc);
            pc->setLocalDescription(rtc::Description::Type::Offer);
        }

        void start_incoming(
            tr_webtorrent::Signal const& signal,
            std::string our_peer,
            tr_webtorrent_signaling::SendAnswer send)
        {
            remote_offer_id = signal.offer_id;
            remote_peer_id = signal.peer_id;
            our_peer_id = std::move(our_peer);
            send_answer = std::move(send);
            pc->setRemoteDescription(rtc::Description(signal.sdp, rtc::Description::Type::Offer));
            pc->setLocalDescription(rtc::Description::Type::Answer);
        }

        void gathering_complete()
        {
            if (description_sent)
            {
                return;
            }
            auto const desc = pc->localDescription();
            if (!desc)
            {
                return;
            }
            description_sent = true;
            auto const sdp = desc->generateSdp();
            if (incoming)
            {
                if (send_answer)
                {
                    send_answer(
                        tr_webtorrent::encode_answer(info_hash, our_peer_id, remote_peer_id, remote_offer_id, sdp));
                }
            }
            else if (auto pending = batch.lock())
            {
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
            attached = true;
            auto const socket_address = parent->allocate_address();
            auto const weak_state = std::weak_ptr<State>{ parent };
            auto socket = tr_peer_socket_rtc::create(
                socket_address,
                dc,
                [weak_state](std::function<void()> callback)
                {
                    if (auto state = weak_state.lock(); state && !state->closing)
                    {
                        state->session->queue_session_thread(std::move(callback));
                    }
                });
            parent->session->addIncoming(std::move(socket));
        }

        void on_answer(std::string const& sdp)
        {
            if (incoming || answered || sdp.empty())
            {
                return;
            }
            answered = true;
            pc->setRemoteDescription(rtc::Description(sdp, rtc::Description::Type::Answer));
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
        try
        {
            auto id = new_offer_id();
            while (state_->peers.find(id) != state_->peers.end())
            {
                id = new_offer_id();
            }
            auto peer = std::make_shared<State::Peer>(state_, id, tracker_url, hash_str, false);
            state_->peers.emplace(peer->id, peer);
            peer->init();
            peer->start_outgoing(batch);
        }
        catch (std::exception const&)
        {
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

    try
    {
        auto id = new_offer_id();
        while (state_->peers.find(id) != state_->peers.end())
        {
            id = new_offer_id();
        }
        auto peer = std::make_shared<State::Peer>(state_, id, tracker_url, signal.info_hash, true);
        state_->peers.emplace(peer->id, peer);
        peer->init();
        peer->start_incoming(signal, our_peer_id, std::move(send_answer));
    }
    catch (std::exception const&)
    {
        // Remote SDP may be malformed. Never report it or retain a bad peer.
    }
}

void tr_webtorrent_signaling::upkeep()
{
    auto const now = std::chrono::steady_clock::now();
    for (auto it = state_->batches.begin(); it != state_->batches.end();)
    {
        auto& batch = *it;
        if (batch->remaining == 0 || batch->deadline <= now)
        {
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
            it = state_->peers.erase(it);
        }
        else
        {
            ++it;
        }
    }
}
