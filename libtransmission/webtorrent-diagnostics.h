// Privacy-safe validation of numeric SDP fields for WebRTC diagnostics.
// Deliberately never logs or returns SDP payloads, addresses or credentials.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace tr_webtorrent_diagnostics
{
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
