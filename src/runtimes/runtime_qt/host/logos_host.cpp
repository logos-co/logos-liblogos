#include "command_line_parser.h"
#include "module_initializer.h"
#include "qt/qt_app.h"
#include "token_receiver.h"
#include "logos_api.h"
#include "interface.h"

#include <csignal>
#include <cstring>

#include <execinfo.h>
#include <unistd.h>

namespace {

// Fatal signals that indicate the loaded module faulted.
constexpr int kFatalSignals[] = { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE };

// Set once before handlers are installed; only ever read afterwards, so it is
// safe to touch from an async-signal context. Points into main()'s `args`,
// which outlives every point at which a fatal signal can be delivered.
const char* g_crashModuleName = "unknown";

// Alternate stack so a stack-overflow SIGSEGV (a common module crash) can still
// run the handler instead of immediately re-faulting.
char g_altStack[64 * 1024];

// async-signal-safe: only write(2).
void safeWrite(const char* s)
{
    if (!s) return;
    const ssize_t n = ::write(STDERR_FILENO, s, std::strlen(s));
    (void)n;
}

// Everything here is async-signal-safe: write(2), backtrace(3),
// backtrace_symbols_fd(3), signal(2)/raise(3). No malloc, no strsignal.
extern "C" void fatalSignalHandler(int sig)
{
    // "FATAL:" makes SubprocessContainer's onOutput classifier log this as
    // critical, so a module crash is unmistakable in the Basecamp log.
    safeWrite("\nFATAL: module '");
    safeWrite(g_crashModuleName);
    safeWrite("' crashed (signal ");

    char numbuf[16];
    int idx = static_cast<int>(sizeof(numbuf));
    numbuf[--idx] = '\0';
    int v = sig;
    if (v == 0) {
        numbuf[--idx] = '0';
    } else {
        while (v > 0 && idx > 0) { numbuf[--idx] = static_cast<char>('0' + v % 10); v /= 10; }
    }
    safeWrite(&numbuf[idx]);
    safeWrite("). Backtrace:\n");

    void* frames[64];
    const int n = ::backtrace(frames, 64);
    ::backtrace_symbols_fd(frames, n, STDERR_FILENO);
    safeWrite("FATAL: end backtrace\n");

    // Re-raise with the default disposition so the process still dies from the
    // real signal: this keeps WIFSIGNALED true, so SubprocessContainer sees
    // crashed=true and marks the module unloaded (host stays up).
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

void installCrashHandler(const char* moduleName)
{
    g_crashModuleName = (moduleName && *moduleName) ? moduleName : "unknown";

    stack_t ss{};
    ss.ss_sp    = g_altStack;
    ss.ss_size  = sizeof(g_altStack);
    ss.ss_flags = 0;
    ::sigaltstack(&ss, nullptr);

    struct sigaction sa{};
    sa.sa_handler = &fatalSignalHandler;
    sigemptyset(&sa.sa_mask);
    // SA_RESETHAND: if the handler itself faults, the next delivery uses the
    // default disposition and the process dies instead of looping.
    sa.sa_flags = SA_RESETHAND | SA_ONSTACK;
    for (const int s : kFatalSignals) {
        ::sigaction(s, &sa, nullptr);
    }
}

} // namespace

int main(int argc, char *argv[])
{
    ModuleArgs args = parseCommandLineArgs(argc, argv);
    if (!args.valid) {
        return 1;
    }

    installCrashHandler(args.name.c_str());

    QtApp::init(argc, argv);

    // Container concern: receive auth token via subprocess IPC (Unix socket).
    // A different container (Docker, in-process) would deliver the token
    // through its own channel; the runtime/loader doesn't care how it arrives.
    std::string authToken = SubprocessTokenReceiver::receive(args.name);
    if (authToken.empty()) {
        return 1;
    }

    // Runtime concern: load the Qt plugin and initialize LogosAPI.
    ModuleLib::LogosModule module = loadModule(args.path, args.name);
    if (!module.isValid()) {
        return 1;
    }

    PluginInterface* basePlugin = module.as<PluginInterface>();
    LogosAPI* logos_api = initializeLogosAPI(args.name, module.instance(),
                                             basePlugin, authToken, args.path,
                                             args.instancePersistencePath,
                                             args.transportSetJson);
    module.release();

    if (!logos_api) {
        return 1;
    }

    int result = QtApp::exec();
    delete logos_api;
    QtApp::cleanup();

    return result;
}
