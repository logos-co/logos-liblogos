#ifndef MODULE_STATE_OBSERVER_H
#define MODULE_STATE_OBSERVER_H

// ─────────────────────────────────────────────────────────────────────────────
// The module-lifecycle observer.
//
// WHAT THIS IS
//   The one place in liblogos that turns "a module changed state" into a
//   structured, sequenced fact. Today load/unload/crash are spdlog::info lines
//   (module_manager.cpp:342, :379, :319-321) and registry membership changes
//   are silent (module_registry.cpp discover/prune), so every consumer polls —
//   logos-basecamp runs a 2s QTimer and infers module state from
//   package-install events.
//
// WHAT THIS IS NOT
//   It does not talk to any module, and it does not know that `modules_state`
//   exists. It hands transitions to a SINK. Wiring a sink that pushes them to
//   the modules_state module is Stage 3 and lives elsewhere.
//
//   With no sink installed this is nearly free: record() early-outs before it
//   allocates or takes a lock. That matters — liblogos must not pay for a
//   feed nobody is consuming, and a host with no modules_state loaded is the
//   normal case.
//
// ── THE TWO RULES THAT MATTER ────────────────────────────────────────────────
//
// 1. NEVER DISPATCH UNDER loadMutex().
//
//    The seam points are inside loadModuleInternal / unloadModuleInternalLocked,
//    both of which run with ModuleManager's loadMutex() held. A sink that
//    performs an RPC — which is exactly what Stage 3's sink does — would then
//    be making a synchronous outbound call from inside the load path while
//    holding the lock that every other load and unload needs.
//
//    That is the shape of two failures already paid for in this codebase: the
//    ui-host startup token deadlock (mutual synchronous requestModule, missed
//    10s deadline, SIGKILL), and the ~417s Basecamp startup stall caused by a
//    synchronous call to a module that was not there.
//
//    So the discipline is the same one modules_state uses on its own side:
//
//        record() under the caller's lock  ->  buffer
//        flush() after the lock is released ->  dispatch
//
//    record() only appends. flush() is what calls the sink, and callers must
//    invoke it outside the lock span. See the ScopedFlush helper.
//
// 2. ONE SEQ COUNTER, FOR BOTH DELTAS AND SNAPSHOTS.
//
//    modules_state applies a transition iff its seq is strictly greater than
//    what it already stored for that module, and it keeps a seq TOMBSTONE for a
//    module that left the listing. That puts a requirement on this side which
//    is not obvious from over there: snapshot record seqs must come from the
//    SAME counter as transition seqs.
//
//    Stamp them from anything else — a per-snapshot counter, or zero — and the
//    tombstone is either unreachably high (a real later delta is dropped
//    forever) or trivially low (a stale delta resurrects a module that is
//    gone). nextSeq() is that single counter and there is no other.
//
// ── THREAD SAFETY ────────────────────────────────────────────────────────────
//
//   Transitions originate on at least two threads. Load and unload run on
//   whichever thread called the C API; a module DYING is detected on the
//   container's background asio thread (the onTerminated callback), and that
//   one is not under loadMutex() at all. Every method here is safe to call
//   from any thread.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace logos {

// The state vocabulary, mirroring modules_state. Free constants rather than an
// enum because the wire type is `tstr` and the contract's forward-compatibility
// rule requires an unrecognised state to be tolerated, not treated as an error.
//
// kAbsent is EVENT-ONLY: it may appear as the from/to of a transition, and it
// must never be reported as a record's resting state.
namespace module_state {
constexpr const char* kAbsent   = "absent";
constexpr const char* kUnloaded = "unloaded";
constexpr const char* kLoading  = "loading";
constexpr const char* kLoaded   = "loaded";
constexpr const char* kStopping = "stopping";
constexpr const char* kError    = "error";
}  // namespace module_state

// One lifecycle transition, already sequenced.
//
// `instance` and `pid` answer different questions and are both carried:
// instance is the host's persistence identity and is STABLE across load/unload
// cycles, so it cannot tell you a module died and came back; pid is the process
// incarnation, and a changed pid between two reads IS that answer.
struct ModuleTransition {
    std::string module;
    std::optional<std::string> instance;
    std::optional<int64_t> pid;
    std::string oldState;
    std::string newState;
    std::optional<std::string> reason;
    uint64_t seq = 0;
};

class ModuleStateObserver {
public:
    // Called with a batch of transitions, always OUTSIDE loadMutex().
    //
    // The sink must not block: it is still called on the thread that performed
    // the load, unload or termination. Stage 3's sink posts an async RPC and
    // returns.
    using Sink = std::function<void(const std::vector<ModuleTransition>&)>;

    static ModuleStateObserver& instance();

    // Install (or clear, with a default-constructed Sink) the consumer.
    void setSink(Sink sink);

    // True when a sink is installed. Callers may use this to skip building
    // arguments they would otherwise throw away.
    bool hasSink() const;

    // The single monotonic counter. Deltas AND snapshot records are stamped
    // from it — see rule 2 above.
    uint64_t nextSeq();

    // Buffer one transition. Cheap, allocation-free when no sink is installed,
    // and safe to call with any caller lock held. Does NOT dispatch.
    //
    // A no-op transition (oldState == newState) is dropped here so the sink's
    // contract matches modules_state's: old_state is never equal to new_state.
    void record(const std::string& module,
                const std::string& oldState,
                const std::string& newState,
                std::optional<std::string> instance = std::nullopt,
                std::optional<int64_t> pid = std::nullopt,
                std::optional<std::string> reason = std::nullopt);

    // Dispatch everything buffered so far. MUST be called with no caller lock
    // held. Safe to call when nothing is buffered.
    void flush();

    // Drop anything buffered without dispatching. For tests.
    void clearPending();

private:
    ModuleStateObserver() = default;

    mutable std::mutex m_mutex;
    std::vector<ModuleTransition> m_pending;
    Sink m_sink;
    uint64_t m_seq = 0;
};

// Flushes on scope exit. Declare it in the scope that ENCLOSES the lock guard,
// so the lock is released first:
//
//     ModuleStateObserver::ScopedFlush flusher;   // destroyed second
//     std::lock_guard lock(loadMutex());          // destroyed first
//
// Declaring them the other way round dispatches under the lock, which is
// exactly what rule 1 forbids.
struct ScopedModuleStateFlush {
    ~ScopedModuleStateFlush() { ModuleStateObserver::instance().flush(); }
};

}  // namespace logos

#endif  // MODULE_STATE_OBSERVER_H
