// Locale-dependent SDP regression and privacy-safe diagnostic classifiers.
#include <locale>
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

#include "libtransmission/webtorrent-diagnostics.h"
#include "libtransmission/webtorrent-endpoint.h"
#include "test-fixtures.h"

using WebTorrentDiagnosticsTest = ::tr::test::TransmissionTest;

namespace
{
class GroupedNumbers : public std::numpunct<char>
{
protected:
    char do_thousands_sep() const override { return ','; }
    std::string do_grouping() const override { return "\3"; }
};

struct LocaleRestore
{
    std::locale old = std::locale{};

    LocaleRestore()
    {
        std::locale::global(std::locale{ old, new GroupedNumbers{} });
    }

    ~LocaleRestore()
    {
        std::locale::global(old);
    }
};
} // namespace

TEST_F(WebTorrentDiagnosticsTest, RejectsCommaFormattedMediaPortWithoutLeakingSdp)
{
    using tr_webtorrent_diagnostics::sdp_issue;
    EXPECT_EQ(sdp_issue("v=0\r\nm=application 64,129 UDP/DTLS/SCTP webrtc-datachannel\r\n"),
        "invalid-media-port");
    EXPECT_EQ(sdp_issue("m=application 64129 UDP/DTLS/SCTP webrtc-datachannel"),
        "no-obvious-numeric-sdp-error");
    EXPECT_EQ(sdp_issue("m=application 70000 UDP/DTLS/SCTP webrtc-datachannel"),
        "invalid-media-port");
    EXPECT_EQ(sdp_issue("m=application 00009 UDP/DTLS/SCTP webrtc-datachannel"),
        "no-obvious-numeric-sdp-error");
}

TEST_F(WebTorrentDiagnosticsTest, RejectsCommaFormattedIceCandidatesAndOversizedNumbers)
{
    using tr_webtorrent_diagnostics::sdp_issue;
    EXPECT_EQ(
        sdp_issue("a=candidate:1 1 UDP 2,114,977,791 192.0.2.1 50,310 typ host"),
        "invalid-ice-candidate-number");
    EXPECT_EQ(
        sdp_issue("a=candidate:1 1 UDP 2114977791 192.0.2.1 50310 typ host"),
        "no-obvious-numeric-sdp-error");
    EXPECT_EQ(
        sdp_issue("m=application 999999999999999999999999999999999999 UDP/DTLS/SCTP webrtc-datachannel"),
        "invalid-media-port");
}

TEST_F(WebTorrentDiagnosticsTest, NativeLibdatachannelUsesClassicLocaleForWireNumbers)
{
    LocaleRestore restore{};

    auto const application = rtc::Description::Application{};
    auto const sdp = application.generateSdp("\r\n", "IP4 192.0.2.1", 64129);
    EXPECT_NE(sdp.find("m=application 64129 "), std::string::npos);
    EXPECT_EQ(sdp.find("64,129"), std::string::npos);

    auto const candidate = rtc::Candidate{
        "candidate:1 1 UDP 2114977791 192.0.2.1 50310 typ host" };
    auto const serialized = candidate.candidate();
    EXPECT_NE(serialized.find("2114977791"), std::string::npos);
    EXPECT_NE(serialized.find("50310"), std::string::npos);
    EXPECT_EQ(serialized.find(','), std::string::npos);
}

TEST_F(WebTorrentDiagnosticsTest, ParsesSelectedRealIceEndpointsForIpv4AndIpv6)
{
    using tr_webtorrent_endpoint::parse;

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

TEST_F(WebTorrentDiagnosticsTest, RejectsMissingOrMalformedSelectedIceEndpoints)
{
    using tr_webtorrent_endpoint::parse;

    EXPECT_FALSE(parse(""));
    EXPECT_FALSE(parse("127.0.0.1"));
    EXPECT_FALSE(parse("0.0.0.0:1000"));
    EXPECT_FALSE(parse("203.0.113.10:0"));
    EXPECT_FALSE(parse("203.0.113.10:70000"));
    EXPECT_FALSE(parse("203.0.113.10:123notport"));
    EXPECT_FALSE(parse("203.0.113.10:-1"));
    EXPECT_FALSE(parse("not_an_ip:1000"));
}

TEST_F(WebTorrentDiagnosticsTest, ClassifiesLibraryErrorsWithoutLeakingTheException)
{
    using tr_webtorrent_diagnostics::library_error;
    EXPECT_EQ(library_error("Invalid integer 192.168.1.43 in description"), "invalid-description-integer");
    EXPECT_EQ(library_error("Unexpected remote answer description in signaling state stable"),
        "invalid-signaling-state");
    EXPECT_EQ(library_error("Invalid candidate address sensitive.example:55000"), "invalid-candidate-address");
    EXPECT_EQ(library_error("Invalid candidate format"), "invalid-candidate-format");
    EXPECT_EQ(library_error("private ICE password and SDP content abc123"), "other-library-error");
}

TEST_F(WebTorrentDiagnosticsTest, ClassifiesClientsWithoutExposingIdentifiers)
{
    using tr_webtorrent_diagnostics::client_family;
    EXPECT_EQ(client_family("qBittorrent 5.3.0"), "qbittorrent");
    EXPECT_EQ(client_family("WebTorrent 0.3.0.0"), "webtorrent");
    EXPECT_EQ(client_family("BiglyBT 4.1.0.0"), "biglybt");
    EXPECT_EQ(client_family("Deluge 2.2.0.dev"), "deluge");
    EXPECT_EQ(client_family("unknown-user-controlled-secret"), "other");
}
