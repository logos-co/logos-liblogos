#ifndef LOGOS_PROTOCOL_GATE_H
#define LOGOS_PROTOCOL_GATE_H

#include <cstdlib>
#include <string>

// ---------------------------------------------------------------------------
// Protocol-version load gate (pure decision logic, std-only).
//
// Every module built by a current logos-module-builder carries the
// logos-protocol semver it was compiled against in its embedded metadata
// (`logos_protocol_version`). One rule governs Logos load/call
// compatibility: two participants interoperate iff they share the same
// protocol MAJOR. The host (this library) refuses to load a module whose
// major differs from its own; modules with no stamp predate the scheme and
// load permissively ("legacy") so the existing fleet never hard-fails.
//
// The caller (ModuleManager::loadModuleInternal) extracts the stamp from the
// plugin metadata pre-load and logs according to the decision.
// ---------------------------------------------------------------------------

namespace LogosCore {

enum class ProtocolGateDecision {
    Allow,        // same major — compatible
    AllowLegacy,  // no/unparseable stamp — pre-protocol module, load + warn
    Refuse,       // different major — incompatible, do not load
};

struct ProtocolGateResult {
    ProtocolGateDecision decision;
    int moduleMajor = -1;  // -1 when absent/unparseable
};

// Parse the MAJOR component of "MAJOR.MINOR.PATCH". Returns -1 when the
// string does not start with a non-negative integer.
inline int protocolVersionMajor(const std::string& version)
{
    if (version.empty()) return -1;
    char* end = nullptr;
    const long major = std::strtol(version.c_str(), &end, 10);
    if (end == version.c_str() || major < 0) return -1;
    return static_cast<int>(major);
}

inline ProtocolGateResult evaluateProtocolGate(const std::string& moduleVersion,
                                               int hostMajor)
{
    const int moduleMajor = protocolVersionMajor(moduleVersion);
    if (moduleMajor < 0)
        return {ProtocolGateDecision::AllowLegacy, -1};
    if (moduleMajor == hostMajor)
        return {ProtocolGateDecision::Allow, moduleMajor};
    return {ProtocolGateDecision::Refuse, moduleMajor};
}

} // namespace LogosCore

#endif // LOGOS_PROTOCOL_GATE_H
