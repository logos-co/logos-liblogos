#ifndef SHELL_BINDING_H
#define SHELL_BINDING_H

// The embedder's own identity (a "shell": Basecamp, logoscore, ...), admitted by
// capability_module like any consumer, and the calls it makes as that identity.

#include <string>

struct logos_consumer;

namespace logos::shell_binding {

// At start, after core_service: admits the configured shell identity, if any.
bool prepare();
// At cleanup: retires it and closes whatever the embedder left open.
void shutdown();

// The shell of a runtime in another process, which admitted it: its credential,
// adopted here. With nothing published in this process, every call takes the
// local socket. NULL if the credential cannot be adopted.
logos_consumer* adopt(const std::string& name, const std::string& credential);
// Closes its calls and frees it; that runtime retires the identity.
void release(logos_consumer* consumer);

} // namespace logos::shell_binding

#endif
