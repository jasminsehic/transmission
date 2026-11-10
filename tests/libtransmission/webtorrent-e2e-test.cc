// Fully local WebTorrent tracker -> ICE -> DataChannel -> BitTorrent handshake.
// No Internet STUN servers, public trackers, or third-party WebTorrent clients.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
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

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "libtransmission/announce-list.h"
#include "libtransmission/quark.h"
#include "libtransmission/session.h"
#include "libtransmission/torrent.h"
#include "libtransmission/types.h"
#include "libtransmission/webtorrent-protocol.h"

#include "test-fixtures.h"

using namespace std::chrono_literals;

class WebTorrentEndToEndTest : public tr::test::SessionTest
{
protected:
    void SetUp() override
    {
        auto* const settings_map = settings()->get_if<tr_variant::Map>();
        settings_map->insert_or_assign(TR_KEY_webtorrent_enabled, true);
        // Loopback tests should never contact a public STUN/TURN service.
        settings_map->insert_or_assign(TR_KEY_webtorrent_ice_servers, tr_variant::Vector{});
        tr::test::SessionTest::SetUp();
    }
};

struct WebTorrentTestTracker
{
    std::mutex mutex;
    std::vector<std::shared_ptr<rtc::WebSocket>> sockets;
    std::vector<std::shared_ptr<rtc::PeerConnection>> peers;
    std::vector<std::shared_ptr<rtc::DataChannel>> channels;
    std::atomic<bool> saw_announce = false;
    std::atomic<bool> got_handshake = false;
    std::string const remote_peer_id = "-TEST00-123456789012";
};

TEST_F(WebTorrentEndToEndTest, BrowserStylePeerCompletesBitTorrentHandshake)
{
    auto cfg = rtc::WebSocketServer::Configuration{};
    cfg.port = 0;
    cfg.bindAddress = "127.0.0.1";
    auto server = std::make_shared<rtc::WebSocketServer>(cfg);
    auto state = std::make_shared<WebTorrentTestTracker>();

    server->onClient(
        [state](std::shared_ptr<rtc::WebSocket> ws)
        {
            {
                auto guard = std::lock_guard{ state->mutex };
                state->sockets.emplace_back(ws);
            }
            auto weak_ws = std::weak_ptr<rtc::WebSocket>{ ws };
            ws->onMessage(
                [](rtc::binary) {},
                [state, weak_ws](std::string json)
                {
                    auto document = rapidjson::Document{};
                    document.Parse(json.c_str());
                    if (document.HasParseError() || !document.IsObject() ||
                        !document.HasMember("action") || !document["action"].IsString() ||
                        std::string_view{ document["action"].GetString() } != "announce" ||
                        !document.HasMember("info_hash") || !document["info_hash"].IsString())
                    {
                        return;
                    }
                    state->saw_announce = true;

                    // A tracker counts reply must use the same binary info_hash.
                    auto response_buffer = rapidjson::StringBuffer{};
                    auto writer = rapidjson::Writer<rapidjson::StringBuffer>{ response_buffer };
                    writer.StartObject();
                    writer.Key("action");
                    writer.String("announce");
                    writer.Key("info_hash");
                    writer.String(document["info_hash"].GetString(), document["info_hash"].GetStringLength());
                    writer.Key("interval");
                    writer.Int(125);
                    writer.Key("complete");
                    writer.Int(1);
                    writer.Key("incomplete");
                    writer.Int(1);
                    writer.EndObject();
                    if (auto socket = weak_ws.lock())
                    {
                        socket->send(std::string{ response_buffer.GetString(), response_buffer.GetSize() });
                    }

                    if (!document.HasMember("offers") || !document["offers"].IsArray() ||
                        document["offers"].Empty())
                    {
                        return;
                    }
                    auto const& first = document["offers"][0];
                    if (!first.IsObject() || !first.HasMember("offer") || !first["offer"].IsObject() ||
                        !first["offer"].HasMember("sdp") || !first["offer"]["sdp"].IsString() ||
                        !first.HasMember("offer_id") || !first["offer_id"].IsString() ||
                        !document.HasMember("peer_id") || !document["peer_id"].IsString())
                    {
                        return;
                    }

                    auto const hash = tr_webtorrent::decode_binary(
                        { document["info_hash"].GetString(), document["info_hash"].GetStringLength() });
                    auto const client_id = tr_webtorrent::decode_binary(
                        { document["peer_id"].GetString(), document["peer_id"].GetStringLength() });
                    auto const offer_id = tr_webtorrent::decode_binary(
                        { first["offer_id"].GetString(), first["offer_id"].GetStringLength() });
                    if (!hash || !client_id || !offer_id || hash->size() != 20U || client_id->size() != 20U)
                    {
                        return;
                    }
                    // Respond to one of the offers as a browser-style WebRTC peer.
                    auto remote = std::make_shared<rtc::PeerConnection>(rtc::Configuration{});
                    {
                        auto guard = std::lock_guard{ state->mutex };
                        state->peers.emplace_back(remote);
                    }
                    remote->onDataChannel(
                        [state, hash = *hash](std::shared_ptr<rtc::DataChannel> channel)
                        {
                            {
                                auto guard = std::lock_guard{ state->mutex };
                                state->channels.emplace_back(channel);
                            }
                            auto weak = std::weak_ptr<rtc::DataChannel>{ channel };
                            channel->onOpen(
                                [weak, hash, remote_id = state->remote_peer_id]()
                                {
                                    if (auto dc = weak.lock())
                                    {
                                        auto hs = std::string{};
                                        hs.reserve(68);
                                        hs.push_back(19);
                                        hs += "BitTorrent protocol";
                                        hs.append(8, '\0');
                                        hs += hash;
                                        hs += remote_id;
                                        auto wire = rtc::binary{};
                                        wire.reserve(hs.size());
                                        for (unsigned char const c : hs)
                                        {
                                            wire.emplace_back(static_cast<std::byte>(c));
                                        }
                                        dc->send(std::move(wire));
                                    }
                                });
                            channel->onMessage(
                                [state](rtc::binary message)
                                {
                                    if (message.size() >= 68U && message.front() == std::byte{ 19 })
                                    {
                                        state->got_handshake = true;
                                    }
                                },
                                [](std::string) {});
                        });

                    auto const weak_remote = std::weak_ptr<rtc::PeerConnection>{ remote };
                    remote->onGatheringStateChange(
                        [weak_remote, weak_ws, hash = *hash, peer_id = *client_id,
                         offer_id = *offer_id, remote_id = state->remote_peer_id](
                            rtc::PeerConnection::GatheringState gathering)
                        {
                            if (gathering != rtc::PeerConnection::GatheringState::Complete)
                            {
                                return;
                            }
                            auto pc = weak_remote.lock();
                            auto ws = weak_ws.lock();
                            if (!pc || !ws)
                            {
                                return;
                            }
                            if (auto desc = pc->localDescription())
                            {
                                ws->send(tr_webtorrent::encode_answer(
                                    hash, remote_id, peer_id, offer_id, desc->generateSdp()));
                            }
                        });

                    remote->setRemoteDescription(rtc::Description(
                        first["offer"]["sdp"].GetString(), rtc::Description::Type::Offer));
                    remote->setLocalDescription(rtc::Description::Type::Answer);
                });
        });

    auto* const tor = zeroTorrentInit(ZeroTorrentState::NoFiles);
    ASSERT_NE(tor, nullptr);
    auto local_tracker = tr_announce_list{};
    ASSERT_TRUE(local_tracker.add("ws://127.0.0.1:" + std::to_string(server->port()) + "/"));
    ASSERT_TRUE(tor->set_announce_list(std::move(local_tracker)));
    tr_torrentStartNow(tor);

    EXPECT_TRUE(tr::test::waitFor([state] { return state->saw_announce.load(); }, 15000));
    EXPECT_TRUE(tr::test::waitFor([state] { return state->got_handshake.load(); }, 25000))
        << "A WebRTC answer was accepted, but the BitTorrent handshake did not complete";

    tr_torrentStop(tor);
    server->onClient(nullptr);
    {
        auto guard = std::lock_guard{ state->mutex };
        for (auto& channel : state->channels)
        {
            channel->onOpen(nullptr);
            channel->onMessage(nullptr);
        }
        for (auto& pc : state->peers)
        {
            pc->resetCallbacks();
            pc->close();
        }
        for (auto& ws : state->sockets)
        {
            ws->onMessage(nullptr);
            ws->close();
        }
        state->channels.clear();
        state->peers.clear();
        state->sockets.clear();
    }
    server->stop();
}
