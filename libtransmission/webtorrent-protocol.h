// WebTorrent tracker JSON protocol. Binary identifiers use Latin-1 code points.
#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tr_webtorrent
{
struct Offer
{
    std::string offer_id;
    std::string sdp;
};

struct Announce
{
    std::string info_hash; // 20 raw bytes, NOT 40 hex digits
    std::string peer_id; // 20 raw bytes
    uint64_t uploaded = 0;
    uint64_t downloaded = 0;
    uint64_t left = 0;
    int64_t numwant = 0;
    std::string event;
    std::vector<Offer> offers;
};

struct Signal
{
    enum class Kind
    {
        Counts,
        Offer,
        Answer,
        Failure
    };

    Kind kind = Kind::Counts;
    std::string info_hash;
    std::string peer_id;
    std::string offer_id;
    std::string sdp;
    std::string failure;
    int64_t interval = 0;
    std::optional<int64_t> complete;
    std::optional<int64_t> incomplete;
};

// Each byte becomes a Unicode character in U+0000..U+00FF.
// The JSON writer then escapes control chars and emits valid UTF-8.
std::string encode_binary(std::string_view bytes);
std::optional<std::string> decode_binary(std::string_view unicode_utf8);

std::string encode_announce(Announce const& announce);
std::string encode_answer(
    std::string_view info_hash,
    std::string_view our_peer_id,
    std::string_view remote_peer_id,
    std::string_view offer_id,
    std::string_view sdp);
std::optional<Signal> parse_signal(std::string_view json);
} // namespace tr_webtorrent
