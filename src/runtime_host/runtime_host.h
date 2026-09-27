#ifndef RUNTIME_HOST_H
#define RUNTIME_HOST_H

#include <string>

// The runtime in a process of its own (bin/logos_runtime), and the app that
// spawned it. They talk over the runtime's stdin and stdout, one JSON object per
// line, which nothing logs:
//
//   app -> runtime   the configuration                               (first line)
//                    {"call": n, "op": "process_module", "path": p}
//                    {"reply": n, "text": s | null}                  (to a hook)
//   runtime -> app   {"ready": {"instance": id, "credential": c, "pid": p}}
//                    {"error": why}                                   (then it exits)
//                    {"call": n, "hook": "extension", "caller": {...}, "method": m, "args": [...]}
//                    {"call": n, "hook": "operator", "token": t, "transport": tr}
//                    {"hook": "shutdown"}
//                    {"reply": n, "text": s | null}                  (to a call)
//
// EOF on the runtime's stdin stops it: it unloads its modules in order and exits.

namespace logos::runtime_host {

// While this process has a runtime spawned, it cannot also start one itself.
bool spawned();

// Applies a spawn configuration to this process's runtime, as logos_runtime does
// with the line its app sends; the embedded runtime (logos_runtime_embed) too.
bool configure(const std::string& configJson, std::string& error);

} // namespace logos::runtime_host

#endif
