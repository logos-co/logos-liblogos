#ifndef EMBEDDED_CORE_SERVICE_H
#define EMBEDDED_CORE_SERVICE_H

// core_service, the runtime's control surface, compiled into liblogos and
// published as a module of its own: every consumer reaches it the same way, and
// each method answers only the callers its scope admits.

#include <string>

namespace logos::core_service {

using ShutdownHandler = void (*)(void* userData);
// The operator a token names, as a heap string (lp_string_free), or NULL.
using OperatorResolver = char* (*)(const char* token, const char* transport, void* userData);
// An embedder method's result as heap JSON (lp_string_free), or NULL when the
// method is not the embedder's. It authorizes by `callerJson` itself.
using Extension = char* (*)(const char* callerJson, const char* method, const char* argsJson,
                            void* userData);

// Protected input, taken before start.
void setTransports(const std::string& json);
void setShutdownHandler(ShutdownHandler handler, void* userData);
void setOperatorResolver(OperatorResolver resolver, void* userData);
void setExtension(Extension extension, const std::string& methodsJson, void* userData);
void setShellIdentity(const std::string& name);
std::string shellIdentity();
void resetConfiguration();

// Publishes it, after capability_module; stop() withdraws it.
bool start();
void stop();

} // namespace logos::core_service

#endif
