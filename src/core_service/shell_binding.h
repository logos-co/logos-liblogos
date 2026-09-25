#ifndef SHELL_BINDING_H
#define SHELL_BINDING_H

// The embedder's own identity (a "shell": Basecamp, logoscore, ...), admitted by
// capability_module like any consumer, and the calls it makes as that identity.

namespace logos::shell_binding {

// At start, after core_service: admits the configured shell identity, if any.
bool prepare();
// At cleanup: retires it and closes whatever the embedder left open.
void shutdown();

} // namespace logos::shell_binding

#endif
