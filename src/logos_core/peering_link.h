#ifndef LOGOS_PEERING_LINK_H
#define LOGOS_PEERING_LINK_H

// The engine's side of peering: it configures peering_module, turns its
// imports into facade records it loads, and gives exported modules a tls_tcp
// listener. peering_module hears of each facade or export before its host
// starts, and after it stops. Everything here is a no-op without a config.

#include <cstdint>
#include <string>

namespace logos::peering_link {

// Protected input, before start: the peering configuration, a JSON object.
bool setConfig(const std::string& json, std::string& error);
bool configured();

// At start, after modules_state: loads peering_module (and peering_identity
// with it), configures it, follows its events and loads the imports.
void start();
// At teardown: stops following and forgets everything.
void stop();

// `transportSet` plus a tls_tcp listener on `host`; an empty set keeps the
// host's default local transport beside it.
std::string withExportListener(const std::string& transportSet, const std::string& host);

// A facade or an export announced to peering_module for one load. Unless the
// load commits, its end is announced when this goes out of scope.
class Announcement {
public:
    Announcement() = default;
    Announcement(std::string name, std::string role, std::int64_t epoch);
    Announcement(Announcement&& other) noexcept;
    Announcement& operator=(Announcement&&) = delete;
    ~Announcement();

    explicit operator bool() const { return !m_role.empty(); }
    const std::string& role() const { return m_role; }
    void commit();

private:
    std::string m_name;
    std::string m_role;
    std::int64_t m_epoch = 0;
    bool m_committed = false;
};

// Before `name`'s host is spawned. An export (a native module peering_module
// exports, not placed in-process) gains its listener in `transportSet`.
Announcement beforeSpawn(const std::string& name, const std::string& format, bool inProcess,
                         std::string& transportSet);
// The module's host exited, after its load committed.
void exited(const std::string& name);
// A facade's load committed: its reflected state applies from now on.
void facadeLoaded(const std::string& name);

} // namespace logos::peering_link

#endif // LOGOS_PEERING_LINK_H
