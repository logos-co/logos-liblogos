#ifndef LOGOS_MODULE_NAME_VALIDATION_H
#define LOGOS_MODULE_NAME_VALIDATION_H

// Qt-free allowlist validator for module names.
//
// A module's name originates from untrusted plugin metadata (the embedded
// Qt MetaData JSON read by ModuleLib::LogosModule::getModuleName) and then
// flows into security-sensitive sinks that treat it as a single path
// segment — most notably the per-module token-handoff Unix socket
// ("logos_token_<name>" -> qtCompatibleTempDir()/<name>, see
// unix_socket_path.h) used to deliver each module's auth token, plus the
// in-memory registry map key and the LogosAPI RPC target.
//
// Without validation, a name containing '/' or ".." escapes the temp dir
// and changes where the parent connect()s to hand off the token and where
// the child QLocalServer binds — enabling path traversal and token-handoff
// redirection by a malicious installed module (CWE-22). The single source
// of truth for "is this a safe module identifier" lives here so every
// consumer (registry trust boundary + both socket sinks) shares one rule.
//
// Rule: non-empty, at most 64 bytes, and every byte is one of
// [A-Za-z0-9_-]. This rejects '/', '\\', '.', "..", NUL, whitespace and
// any other separator outright. The charset already excludes "." and "..";
// they are re-checked defensively for clarity.

#include <cstddef>
#include <string>

namespace logos {

inline bool isValidModuleName(const std::string& name) {
    if (name.empty() || name.size() > 64)
        return false;
    for (unsigned char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') ||
                        c == '_' || c == '-';
        if (!ok)
            return false;
    }
    if (name == "." || name == "..")
        return false;
    return true;
}

}  // namespace logos

#endif  // LOGOS_MODULE_NAME_VALIDATION_H
