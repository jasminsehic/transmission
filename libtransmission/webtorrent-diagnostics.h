// Privacy-safe validation of numeric SDP fields for WebRTC diagnostics.
// Deliberately never logs or returns SDP payloads, addresses or credentials.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <cstddef>
#include <string_view>

namespace tr_webtorrent_diagnostics
{

[[nodiscard]] inline std::string_view library_error(std::string_view message)
{
    // Library error text may contain an address, ICE credentials or the full SDP.
    // Only return fixed reason labels, never the original message.
    auto lower = std::string{ message };
    for (auto& c : lower)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    auto const has = [&lower](std::string_view value)
    {
        return lower.find(value) != std::string::npos;
    };

    if (has("invalid integer"))
    {
        return "invalid-description-integer";
    }
    if (has("invalid media description"))
    {
        return "invalid-media-description";
    }
    if (has("invalid candidate format"))
    {
        return "invalid-candidate-format";
    }
    if (has("invalid candidate address"))
    {
        return "invalid-candidate-address";
    }
    if (has("unexpected remote") || has("unexpected local") || has("signaling state"))
    {
        return "invalid-signaling-state";
    }
    if (has("ice-ufrag") || has("ice-pwd") || has("ice credentials"))
    {
        return "invalid-ice-credentials";
    }
    if (has("fingerprint"))
    {
        return "invalid-fingerprint";
    }
    if (has("no datachannel or track"))
    {
        return "missing-datachannel";
    }
    if (has("peer connection is closed"))
    {
        return "closed-peer-connection";
    }
    if (has("ice transport"))
    {
        return "ice-transport-error";
    }
    if (has("dtls transport"))
    {
        return "dtls-transport-error";
    }
    if (has("sctp transport"))
    {
        return "sctp-transport-error";
    }
    return "other-library-error";
}

[[nodiscard]] inline std::string_view client_family(std::string_view name)
{
    // A peer's version or user agent is untrusted and should not appear in diagnostics.
    if (name.starts_with("qBittorrent"))
    {
        return "qbittorrent";
    }
    if (name.starts_with("WebTorrent"))
    {
        return "webtorrent";
    }
    if (name.starts_with("Deluge"))
    {
        return "deluge";
    }
    if (name.starts_with("Transmission"))
    {
        return "transmission";
    }
    if (name.starts_with("BiglyBT"))
    {
        return "biglybt";
    }
    if (name.starts_with("libtorrent"))
    {
        return "libtorrent";
    }
    return "other";
}

inline bool decimal(std::string_view value, uint64_t limit)
{
    if (value.empty())
    {
        return false;
    }

    auto parsed = uint64_t{ 0 };
    for (auto const digit : value)
    {
        if (digit < '0' || digit > '9')
        {
            return false;
        }
        auto const n = static_cast<uint64_t>(digit - '0');
        if (n > limit || parsed > (limit - n) / 10U)
        {
            return false;
        }
        parsed = parsed * 10U + n;
    }
    return true;
}

inline std::string_view sdp_issue(std::string_view sdp)
{
    for (auto offset = size_t{ 0 }; offset < sdp.size();)
    {
        auto const end = sdp.find_first_of("\r\n", offset);
        auto const line = sdp.substr(offset, (end == std::string_view::npos ? sdp.size() : end) - offset);
        offset = end == std::string_view::npos ? sdp.size() : end + 1U;

        if (line.starts_with("m="))
        {
            auto const media_end = line.find_first_of(" \t", 2U);
            auto const port_begin = media_end == std::string_view::npos ? media_end : line.find_first_not_of(" \t", media_end);
            if (port_begin == std::string_view::npos)
            {
                return "malformed-media-line";
            }
            auto const port_end = line.find_first_of(" \t", port_begin);
            if (!decimal(line.substr(port_begin, port_end - port_begin), 65535))
            {
                return "invalid-media-port";
            }
        }
        else if (line.starts_with("a=candidate:"))
        {
            auto fields = std::array<std::string_view, 6>{};
            auto cursor = size_t{ 0 };
            for (auto& field : fields)
            {
                cursor = line.find_first_not_of(" \t", cursor);
                if (cursor == std::string_view::npos)
                {
                    return "malformed-ice-candidate";
                }
                auto const next = line.find_first_of(" \t", cursor);
                field = line.substr(cursor, next - cursor);
                cursor = next == std::string_view::npos ? next : next + 1U;
            }

            if (!decimal(fields[1], 256) || !decimal(fields[3], UINT32_MAX) || !decimal(fields[5], 65535))
            {
                return "invalid-ice-candidate-number";
            }
        }
    }
    return "no-obvious-numeric-sdp-error";
}
} // namespace tr_webtorrent_diagnostics
