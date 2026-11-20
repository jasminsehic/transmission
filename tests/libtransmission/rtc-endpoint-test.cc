// This file is part of Transmission.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

// Validate selected WebRTC ICE endpoints independently of tracker signaling.
#include "libtransmission/rtc-endpoint.h"
#include "test-fixtures.h"

using RtcEndpointTest = ::tr::test::TransmissionTest;

TEST_F(RtcEndpointTest, ParsesSelectedRealIceEndpointsForIpv4AndIpv6)
{
    using tr_rtc_endpoint::parse;

    auto const ipv4 = parse("203.0.113.19:53421");
    ASSERT_TRUE(ipv4);
    EXPECT_TRUE(ipv4->address().is_ipv4());
    EXPECT_EQ(ipv4->address().display_name(), "203.0.113.19");
    EXPECT_EQ(ipv4->port().host(), 53421U);

    // libjuice returns unbracketed IPv6; also accept the bracketed form.
    auto const ipv6 = parse("2001:db8::42:62000");
    ASSERT_TRUE(ipv6);
    EXPECT_TRUE(ipv6->address().is_ipv6());
    EXPECT_EQ(ipv6->address().display_name(), "2001:db8::42");
    EXPECT_EQ(ipv6->port().host(), 62000U);

    auto const bracketed = parse("[2001:db8::42]:62000");
    ASSERT_TRUE(bracketed);
    EXPECT_EQ(bracketed->address(), ipv6->address());
    EXPECT_EQ(bracketed->port(), ipv6->port());
}

TEST_F(RtcEndpointTest, RejectsMissingOrMalformedSelectedIceEndpoints)
{
    using tr_rtc_endpoint::parse;

    EXPECT_FALSE(parse(""));
    EXPECT_FALSE(parse("127.0.0.1"));
    EXPECT_FALSE(parse("0.0.0.0:1000"));
    EXPECT_FALSE(parse("203.0.113.10:0"));
    EXPECT_FALSE(parse("203.0.113.10:70000"));
    EXPECT_FALSE(parse("203.0.113.10:123notport"));
    EXPECT_FALSE(parse("203.0.113.10:-1"));
    EXPECT_FALSE(parse("not_an_ip:1000"));
}
