// This file is part of Transmission.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wextra-semi"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include <rtc/description.hpp>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "libtransmission/net.h"

namespace tr_rtc_network
{
[[nodiscard]] inline std::optional<std::string> bind_address(
    std::string_view ipv4,
    std::string_view ipv6,
    std::size_t peer_sequence)
{
    auto const parse = [](std::string_view value, bool const want_ipv4) -> std::optional<std::string>
    {
        if (value.empty())
        {
            return {};
        }

        auto const address = tr_address::from_string(value);
        if (!address || address->is_ipv4() != want_ipv4)
        {
#if !defined(_WIN32) || !defined(__clang_analyzer__)
            throw std::invalid_argument("Invalid WebRTC bind address");
#else
            return {};
#endif
        }
        return address->is_any() ? std::nullopt : std::optional<std::string>{ address->display_name() };
    };

    auto const v4 = parse(ipv4, true);
    auto const v6 = parse(ipv6, false);
    if (v4 && v6)
    {
        return peer_sequence % 2U == 1U ? v4 : v6;
    }
    return v4 ? v4 : v6;
}

template<typename IsBlocked>
[[nodiscard]] std::size_t filter_blocklisted_candidates(rtc::Description& description, IsBlocked const& is_blocked)
{
    auto const ended = description.ended();
    auto candidates = description.extractCandidates();
    auto const original_size = candidates.size();
    std::erase_if(
        candidates,
        [&](rtc::Candidate const& candidate)
        {
            // Parsed SDP candidates may not be resolved yet; inspect the numeric address directly.
            auto const raw = candidate.candidate();
            auto offset = std::size_t{ 0 };
            for (int field = 0; field < 4; ++field)
            {
                offset = raw.find(' ', offset);
                if (offset == std::string::npos)
                {
                    return false;
                }
                ++offset;
            }
            auto const end = raw.find(' ', offset);
            auto const address = tr_address::from_string(std::string_view{ raw }.substr(offset, end - offset));
            return address && std::invoke(is_blocked, *address);
        });
    auto const removed = original_size - candidates.size();
    description.addCandidates(std::move(candidates));
    if (ended)
    {
        description.endCandidates();
    }
    return removed;
}
} // namespace tr_rtc_network
