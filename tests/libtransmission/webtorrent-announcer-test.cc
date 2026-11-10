// Local WebSocket announce integration. No public tracker or network is needed.
#include <atomic>
#include <chrono>
#include <memory>
#include <string>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wextra-semi"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include <rtc/rtc.hpp>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#define LIBTRANSMISSION_ANNOUNCER_MODULE

#include "libtransmission/announcer-common.h"
#include "libtransmission/announcer-ws.h"
#include "libtransmission/quark.h"
#include "libtransmission/session.h"
#include "libtransmission/webtorrent-protocol.h"

#include "test-fixtures.h"

using WebTorrentAnnouncerTest = ::tr::test::SessionTest;
using namespace std::chrono_literals;

TEST_F(WebTorrentAnnouncerTest, DisabledByDefault)
{
    auto announcer = tr_announcer_ws{ session_ };
    auto request = tr_announce_request{};
    request.info_hash.fill(std::byte{ 'A' });
    request.peer_id.fill('B');
    request.announce_url = tr_interned_string{ "ws://127.0.0.1:9000" };

    auto completed = std::atomic<bool>{ false };
    auto accepted = std::atomic<bool>{ true };
    session_->run_in_session_thread(
        [&]
        {
            announcer.announce(
                request,
                [&](tr_announce_response const& response)
                {
                    accepted = response.did_connect;
                    completed = true;
                });
        });

    EXPECT_TRUE(tr::test::waitFor([&] { return completed.load(); }, 2000));
    EXPECT_FALSE(accepted);
}

class WebTorrentAnnouncerEnabledTest : public tr::test::SessionTest
{
protected:
    void SetUp() override
    {
        auto* const settings_map = settings()->get_if<tr_variant::Map>();
        settings_map->insert_or_assign(TR_KEY_webtorrent_enabled, true);
        settings_map->insert_or_assign(TR_KEY_webtorrent_ice_servers, tr_variant::Vector{});
        tr::test::SessionTest::SetUp();
    }
};

TEST_F(WebTorrentAnnouncerEnabledTest, AnnounceOverLocalWebSocket)
{
    auto config = rtc::WebSocketServer::Configuration{};
    config.port = 0;
    config.bindAddress = "127.0.0.1";
    auto server = std::make_shared<rtc::WebSocketServer>(config);

    auto const expected_hash = std::string(20, 'A');
    auto const response = std::string{ R"({"action":"announce","info_hash":")" } +
        expected_hash + R"(","interval":125,"complete":2,"incomplete":1})";

    auto seen = std::atomic<int>{ 0 };
    server->onClient(
        [&](std::shared_ptr<rtc::WebSocket> socket)
        {
            auto weak = std::weak_ptr<rtc::WebSocket>{ socket };
            socket->onMessage(
                [](rtc::binary) {},
                [weak, &seen, response](std::string message)
                {
                    if (message.find(R"("action":"announce")") != std::string::npos)
                    {
                        ++seen;
                        if (auto ws = weak.lock())
                        {
                            ws->send(response);
                        }
                    }
                });
        });

    auto announcer = tr_announcer_ws{ session_ };
    auto request = tr_announce_request{};
    request.info_hash.fill(std::byte{ 'A' });
    request.peer_id.fill('B');
    request.announce_url = tr_interned_string{ "ws://127.0.0.1:" + std::to_string(server->port()) };
    request.numwant = 0;

    auto completed = std::atomic<bool>{ false };
    auto succeeded = std::atomic<bool>{ false };
    auto seeders = std::atomic<int64_t>{ -1 };
    session_->run_in_session_thread(
        [&]
        {
            announcer.announce(
                request,
                [&](tr_announce_response const& answer)
                {
                    succeeded = answer.did_connect && answer.errmsg.empty();
                    seeders = answer.seeders.value_or(-1);
                    completed = true;
                });
        });

    ASSERT_TRUE(tr::test::waitFor([&] { return completed.load(); }, 12000));
    EXPECT_TRUE(succeeded);
    EXPECT_EQ(seeders, 2);
    EXPECT_EQ(seen.load(), 1);

    session_->run_in_session_thread([&] { announcer.upkeep(); });
    server->stop();
}
