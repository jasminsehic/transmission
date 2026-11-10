// WebSocket tracker client. All mutable actor state runs on the session thread;
// libdatachannel network callbacks only enqueue work on that thread.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
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

#define LIBTRANSMISSION_ANNOUNCER_MODULE

#include "libtransmission/announcer-common.h"
#include "libtransmission/announcer-ws.h"
#include "libtransmission/log.h"
#include "libtransmission/session.h"
#include "libtransmission/webtorrent-protocol.h"

using namespace std::chrono_literals;

namespace
{
std::string hash_key(tr_sha1_digest_t const& hash)
{
    return { reinterpret_cast<char const*>(hash.data()), hash.size() };
}

std::string peer_key(tr_peer_id_t const& id)
{
    return { id.data(), id.size() };
}

tr_announce_response response_from(tr_announce_request const& request, bool const connected, std::string error = {})
{
    auto response = tr_announce_response{};
    response.info_hash = request.info_hash;
    response.did_connect = connected;
    response.errmsg = std::move(error);
    return response;
}

constexpr auto ResponseTimeout = 25s;
constexpr auto MaxPendingPerSocket = size_t{ 64 };
} // namespace

struct tr_announcer_ws::State : std::enable_shared_from_this<State>
{
    struct Pending
    {
        tr_announce_request request;
        tr_announce_response_func callback;
        std::string payload;
        std::chrono::steady_clock::time_point deadline;
    };

    struct Connection : std::enable_shared_from_this<Connection>
    {
        Connection(std::shared_ptr<State> const& parent, std::string tracker_url)
            : owner{ parent }
            , url{ std::move(tracker_url) }
            , ws{ std::make_shared<rtc::WebSocket>() }
        {
        }

        ~Connection()
        {
            try
            {
                ws->onOpen(nullptr);
                ws->onClosed(nullptr);
                ws->onError(nullptr);
                ws->onMessage(nullptr);
                ws->onBufferedAmountLow(nullptr);
                ws->close();
            }
            catch (std::exception const&)
            {
                // Closing a failed connection is best-effort.
            }
        }

        void dispatch(std::function<void(Connection&)> callback)
        {
            auto const state = owner.lock();
            if (!state || state->closing)
            {
                return;
            }
            auto const weak = weak_from_this();
            state->session->queue_session_thread(
                [weak, fn = std::move(callback)]()
                {
                    if (auto const strong = weak.lock())
                    {
                        if (auto const parent = strong->owner.lock(); parent && !parent->closing)
                        {
                            fn(*strong);
                        }
                    }
                });
        }

        void start()
        {
            auto const weak = weak_from_this();
            ws->onOpen(
                [weak]()
                {
                    if (auto self = weak.lock())
                    {
                        self->dispatch([](Connection& connection) { connection.on_open(); });
                    }
                });
            ws->onMessage(
                [weak](rtc::binary)
                {
                    // WebTorrent tracker signaling is JSON text only.
                },
                [weak](std::string message)
                {
                    if (auto self = weak.lock())
                    {
                        self->dispatch(
                            [text = std::move(message)](Connection& connection) { connection.on_message(text); });
                    }
                });
            ws->onBufferedAmountLow(
                [weak]()
                {
                    if (auto self = weak.lock())
                    {
                        self->dispatch([](Connection& connection) { connection.flush(); });
                    }
                });
            ws->onClosed(
                [weak]()
                {
                    if (auto self = weak.lock())
                    {
                        self->dispatch([](Connection& connection) { connection.on_closed("WebSocket tracker closed"); });
                    }
                });
            ws->onError(
                [weak](std::string error)
                {
                    if (auto self = weak.lock())
                    {
                        self->dispatch(
                            [error = std::move(error)](Connection& connection) { connection.on_closed(error); });
                    }
                });
            ws->setBufferedAmountLowThreshold(64U * 1024U);
            try
            {
                ws->open(url);
            }
            catch (std::exception const& ex)
            {
                on_closed(ex.what());
            }
        }

        void submit(Pending pending)
        {
            if (closed)
            {
                pending.callback(response_from(pending.request, false, "WebSocket connection closed"));
                return;
            }
            if (waiting.size() + outstanding_count() >= MaxPendingPerSocket)
            {
                pending.callback(response_from(pending.request, false, "Too many pending WebSocket announces"));
                return;
            }
            waiting.emplace_back(std::move(pending));
            flush();
        }

        size_t outstanding_count() const
        {
            size_t total = 0;
            for (auto const& [hash, requests] : in_flight)
            {
                total += requests.size();
            }
            return total;
        }

        void on_open()
        {
            if (closed)
            {
                return;
            }
            ready = true;
            flush();
        }

        void flush()
        {
            if (!ready || closed)
            {
                return;
            }

            while (!waiting.empty())
            {
                auto& pending = waiting.front();
                bool accepted = false;
                try
                {
                    accepted = ws->send(pending.payload);
                }
                catch (std::exception const& e)
                {
                    on_closed(e.what());
                    return;
                }
                if (!accepted)
                {
                    return; // Retry after buffered-amount-low.
                }

                auto submitted = std::move(pending);
                waiting.pop_front();
                if (submitted.request.event == TR_ANNOUNCE_EVENT_STOPPED)
                {
                    submitted.callback(response_from(submitted.request, true));
                }
                else
                {
                    in_flight[hash_key(submitted.request.info_hash)].emplace_back(std::move(submitted));
                }
            }
        }

        void on_message(std::string_view message)
        {
            if (closed)
            {
                return;
            }
            auto const signal = tr_webtorrent::parse_signal(message);
            if (!signal)
            {
                return;
            }
            if (signal->kind != tr_webtorrent::Signal::Kind::Counts &&
                signal->kind != tr_webtorrent::Signal::Kind::Failure)
            {
                // SDP events are handled by the separate orchestration stage.
                return;
            }
            auto it = in_flight.find(signal->info_hash);
            if (it == in_flight.end() || it->second.empty())
            {
                return;
            }
            auto pending = std::move(it->second.front());
            it->second.pop_front();
            if (it->second.empty())
            {
                in_flight.erase(it);
            }

            auto response = response_from(pending.request, true);
            response.interval = signal->interval;
            response.seeders = signal->complete;
            response.leechers = signal->incomplete;
            response.errmsg = signal->failure;
            pending.callback(response);
        }

        void fail_expired(std::chrono::steady_clock::time_point const now)
        {
            auto expired = std::vector<Pending>{};
            for (auto it = waiting.begin(); it != waiting.end();)
            {
                if (it->deadline <= now)
                {
                    expired.emplace_back(std::move(*it));
                    it = waiting.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            for (auto it = in_flight.begin(); it != in_flight.end();)
            {
                auto& requests = it->second;
                for (auto pending = requests.begin(); pending != requests.end();)
                {
                    if (pending->deadline <= now)
                    {
                        expired.emplace_back(std::move(*pending));
                        pending = requests.erase(pending);
                    }
                    else
                    {
                        ++pending;
                    }
                }
                if (requests.empty())
                {
                    it = in_flight.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            for (auto& pending : expired)
            {
                auto response = response_from(pending.request, true, "WebSocket announce response timed out");
                response.did_timeout = true;
                pending.callback(response);
            }
            if (!expired.empty() && !ready)
            {
                on_closed("WebSocket connection timed out");
            }
        }

        void on_closed(std::string const& reason)
        {
            if (closed)
            {
                return;
            }
            closed = true;
            ready = false;
            auto failed = std::vector<Pending>{};
            while (!waiting.empty())
            {
                failed.emplace_back(std::move(waiting.front()));
                waiting.pop_front();
            }
            for (auto& [hash, requests] : in_flight)
            {
                while (!requests.empty())
                {
                    failed.emplace_back(std::move(requests.front()));
                    requests.pop_front();
                }
            }
            in_flight.clear();
            for (auto& pending : failed)
            {
                pending.callback(response_from(pending.request, false, reason));
            }
            if (auto parent = owner.lock())
            {
                auto it = parent->connections.find(url);
                if (it != parent->connections.end() && it->second.get() == this)
                {
                    parent->connections.erase(it);
                }
            }
        }

        std::weak_ptr<State> owner;
        std::string const url;
        std::shared_ptr<rtc::WebSocket> ws;
        bool ready = false;
        bool closed = false;
        std::deque<Pending> waiting;
        std::map<std::string, std::deque<Pending>> in_flight;
    };

    explicit State(tr_session* value)
        : session{ value }
    {
    }

    tr_session* const session;
    std::atomic<bool> closing = false;
    std::map<std::string, std::shared_ptr<Connection>> connections;
};

tr_announcer_ws::tr_announcer_ws(tr_session* session)
    : state_{ std::make_shared<State>(session) }
{
}

tr_announcer_ws::~tr_announcer_ws()
{
    state_->closing = true;
    state_->connections.clear();
}

void tr_announcer_ws::announce(tr_announce_request const& request, tr_announce_response_func on_response)
{
    auto payload = tr_webtorrent::Announce{};
    payload.info_hash = hash_key(request.info_hash);
    payload.peer_id = peer_key(request.peer_id);
    payload.event = tr_announce_event_get_string(request.event);
    payload.uploaded = request.up;
    payload.downloaded = request.down;
    payload.left = request.leftUntilComplete;
    // Limit WebTorrent offer requests until actual SDP offers are available.
    payload.numwant = 0;

    auto pending = State::Pending{
        request,
        std::move(on_response),
        tr_webtorrent::encode_announce(payload),
        std::chrono::steady_clock::now() + ResponseTimeout,
    };
    auto const url = std::string{ request.announce_url.sv() };
    auto [it, inserted] = state_->connections.try_emplace(url);
    if (inserted)
    {
        it->second = std::make_shared<State::Connection>(state_, url);
    }
    auto connection = it->second;
    connection->submit(std::move(pending));
    if (inserted)
    {
        connection->start();
    }
}

void tr_announcer_ws::upkeep()
{
    auto const now = std::chrono::steady_clock::now();
    auto connections = std::vector<std::shared_ptr<State::Connection>>{};
    connections.reserve(state_->connections.size());
    for (auto const& [url, connection] : state_->connections)
    {
        connections.push_back(connection);
    }
    for (auto& connection : connections)
    {
        connection->fail_expired(now);
    }
}
