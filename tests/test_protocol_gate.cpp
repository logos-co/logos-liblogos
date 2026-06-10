#include <gtest/gtest.h>

#include "logos_core/protocol_gate.h"
#include "logos_protocol.h"

// Gate tests (land with the gate, per the versioning design):
//   (a) a module stamped with a different MAJOR is refused,
//   (b) an unstamped (legacy) module is allowed permissively,
//   (c) equal major / different minor loads silently.
// The stamp values are exercised against both a fixed major and the real
// LOGOS_PROTOCOL_VERSION_MAJOR the host links.

using LogosCore::evaluateProtocolGate;
using LogosCore::protocolVersionMajor;
using LogosCore::ProtocolGateDecision;

TEST(ProtocolGate, RefusesDifferentMajor)
{
    const auto r = evaluateProtocolGate("2.0.0", 1);
    EXPECT_EQ(r.decision, ProtocolGateDecision::Refuse);
    EXPECT_EQ(r.moduleMajor, 2);

    // Against the real host major: a deliberately bumped stamp is refused.
    const int hostMajor = LOGOS_PROTOCOL_VERSION_MAJOR;
    const std::string bumped = std::to_string(hostMajor + 1) + ".0.0";
    EXPECT_EQ(evaluateProtocolGate(bumped, hostMajor).decision,
              ProtocolGateDecision::Refuse);
}

TEST(ProtocolGate, MissingStampLoadsPermissively)
{
    const auto r = evaluateProtocolGate("", LOGOS_PROTOCOL_VERSION_MAJOR);
    EXPECT_EQ(r.decision, ProtocolGateDecision::AllowLegacy);
    EXPECT_EQ(r.moduleMajor, -1);
}

TEST(ProtocolGate, UnparseableStampLoadsPermissively)
{
    EXPECT_EQ(evaluateProtocolGate("garbage", 1).decision,
              ProtocolGateDecision::AllowLegacy);
    EXPECT_EQ(evaluateProtocolGate("-1.0.0", 1).decision,
              ProtocolGateDecision::AllowLegacy);
}

TEST(ProtocolGate, EqualMajorDifferentMinorAllowsSilently)
{
    const int hostMajor = LOGOS_PROTOCOL_VERSION_MAJOR;
    const std::string newerMinor =
        std::to_string(hostMajor) + ".99.7";
    const auto r = evaluateProtocolGate(newerMinor, hostMajor);
    EXPECT_EQ(r.decision, ProtocolGateDecision::Allow);
    EXPECT_EQ(r.moduleMajor, hostMajor);
}

TEST(ProtocolGate, HostOwnVersionAllows)
{
    EXPECT_EQ(evaluateProtocolGate(LOGOS_PROTOCOL_VERSION_STRING,
                                   LOGOS_PROTOCOL_VERSION_MAJOR).decision,
              ProtocolGateDecision::Allow);
}

TEST(ProtocolGate, MajorParser)
{
    EXPECT_EQ(protocolVersionMajor("0.1.0"), 0);
    EXPECT_EQ(protocolVersionMajor("12.3.4"), 12);
    EXPECT_EQ(protocolVersionMajor("1"), 1);
    EXPECT_EQ(protocolVersionMajor(""), -1);
    EXPECT_EQ(protocolVersionMajor("x.y.z"), -1);
}
