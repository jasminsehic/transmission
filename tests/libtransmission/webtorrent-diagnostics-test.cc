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
