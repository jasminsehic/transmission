// WebRTC offer/answer lifecycle, isolated from WebSocket tracker transport.
#pragma once
#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "libtransmission/types.h"
#include "libtransmission/webtorrent-protocol.h"

struct tr_session;

class tr_webtorrent_signaling final
{
public:
    using OffersReady = std::function<void(std::vector<tr_webtorrent::Offer>)>;
    using SendAnswer = std::function<void(std::string)>;

    explicit tr_webtorrent_signaling(tr_session* session);
    ~tr_webtorrent_signaling();
    tr_webtorrent_signaling(tr_webtorrent_signaling const&) = delete;
    tr_webtorrent_signaling& operator=(tr_webtorrent_signaling const&) = delete;

    void make_offers(tr_sha1_digest_t const& hash, std::string tracker_url, size_t count, OffersReady on_ready);
    void on_signal(
        tr_webtorrent::Signal const& signal,
        std::string const& tracker_url,
        std::string const& our_peer_id,
        SendAnswer send_answer);
    void upkeep();

private:
    struct State;
    std::shared_ptr<State> state_;
};
