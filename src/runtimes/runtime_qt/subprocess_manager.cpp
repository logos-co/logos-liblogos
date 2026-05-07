#include "subprocess_manager.h"

#include <boost/asio/connect_pipe.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>
#include <boost/dll/runtime_symbol_info.hpp>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace bp2  = boost::process::v2;
namespace asio = boost::asio;
namespace fs   = std::filesystem;

namespace {

// Dedicated logger for child stdout — uses a pattern that prints a
// fictional "[out]" level, so stdout lines visually align with spdlog
// output but are clearly distinguished from stderr-classified lines.
std::shared_ptr<spdlog::logger>& moduleStdoutLogger() {
    static std::shared_ptr<spdlog::logger> logger = []() {
        auto l = spdlog::stdout_color_mt("logos_module_stdout");
        l->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [out] %v");
        return l;
    }();
    return logger;
}

// ---------------------------------------------------------------------------
// logos_host_qt resolution (moved from qt_subprocess_runtime.cpp)
// ---------------------------------------------------------------------------

fs::path findInDir(const fs::path& dir) {
    for (const auto& name : {"logos_host_qt", "logos_host"}) {
        auto candidate = (dir / name).lexically_normal();
        if (fs::exists(candidate))
            return candidate;
    }
    return {};
}

std::string resolveLogosHostPath(const std::vector<std::string>& modulesDirs) {
    std::string logosHostPath;

    const char* envPath = std::getenv("LOGOS_HOST_PATH");
    if (envPath)
        logosHostPath = envPath;

    if (logosHostPath.empty()) {
        auto found = findInDir(fs::path(boost::dll::program_location().parent_path().string()));
        if (!found.empty())
            logosHostPath = found.string();
    }

    if (logosHostPath.empty() || !fs::exists(logosHostPath)) {
        if (!modulesDirs.empty()) {
            auto binDir = fs::absolute(
                fs::path(modulesDirs.front()) / ".." / "bin"
            ).lexically_normal();
            auto found = findInDir(binDir);
            if (!found.empty())
                logosHostPath = found.string();
        }
    }

    if (logosHostPath.empty() || !fs::exists(logosHostPath)) {
        spdlog::critical("logos_host_qt (or logos_host) not found - set LOGOS_HOST_PATH or place it next to the executable (last tried: {})",
                         logosHostPath);
        return {};
    }

    return logosHostPath;
}

// ---------------------------------------------------------------------------
// Background io_context: one thread, kept alive by a work guard.
// ---------------------------------------------------------------------------

struct IoRuntime {
    asio::io_context ctx;
    asio::executor_work_guard<asio::io_context::executor_type> guard;
    std::thread thread;

    IoRuntime()
        : guard(asio::make_work_guard(ctx))
        , thread([this]() { ctx.run(); })
    {}

    ~IoRuntime() {
        guard.reset();
        ctx.stop();
        if (thread.joinable()) {
            // Common case: destructor fires from the main thread at process
            // exit, ctx.run() returned cleanly, just join.
            //
            // Pathological case: destructor fires from *this very thread*.
            // Happens when an asio handler running on `thread` calls
            // exit() (e.g. the onFinished callback below crash-aborts the
            // process). exit() triggers static destruction in the calling
            // thread; that's us. join() on yourself is EDEADLK and would
            // throw a std::system_error → uncaught → terminate() → SIGABRT,
            // masking the real crash that triggered the exit() in the first
            // place. Detach instead: the OS reaps the thread on process
            // exit, no observable difference vs join in this single-process
            // scenario.
            if (thread.get_id() == std::this_thread::get_id()) {
                thread.detach();
            } else {
                thread.join();
            }
        }
    }
};

IoRuntime& ioRuntime() {
    static IoRuntime s_runtime;
    return s_runtime;
}

// ---------------------------------------------------------------------------
// ProcessEntry: owns one live child process and its read pipes.
// ---------------------------------------------------------------------------

struct ProcessEntry {
    bp2::process                           process;
    asio::readable_pipe                    out_pipe;
    asio::readable_pipe                    err_pipe;
    SubprocessManager::ProcessCallbacks    callbacks;
    std::string                            name;
    std::array<char, 4096>                 out_read_buf{};
    std::array<char, 4096>                 err_read_buf{};
    std::string                            out_line_buf;
    std::string                            err_line_buf;
    // Set by async_wait callback; used by syncKill to avoid double-waitpid.
    std::atomic<bool>                exited{false};
    std::atomic<bool>                cancelled{false};

    ProcessEntry(bp2::process proc,
                 asio::readable_pipe out_rp, asio::readable_pipe err_rp,
                 const std::string& n, const SubprocessManager::ProcessCallbacks& cb)
        : process(std::move(proc))
        , out_pipe(std::move(out_rp))
        , err_pipe(std::move(err_rp))
        , name(n)
        , callbacks(cb)
    {}
};

// ---------------------------------------------------------------------------
// Global process registry
// ---------------------------------------------------------------------------

std::unordered_map<std::string, std::shared_ptr<ProcessEntry>> s_processes;
std::mutex s_processesMutex;

// ---------------------------------------------------------------------------
// Unix domain socket path
// ---------------------------------------------------------------------------

std::string unixSocketPath(const std::string& name) {
    const char* tmp = getenv("TMPDIR");
    std::string dir;
    if (tmp && tmp[0]) {
        dir = tmp;
        while (!dir.empty() && dir.back() == '/') dir.pop_back();
    } else {
        dir = "/tmp";
    }
    return dir + "/" + name;
}

// ---------------------------------------------------------------------------
// Async read loop: reads stdout and stderr separately from child, fires
// onOutput per line with the correct isStderr flag.
// Called from the io_context thread.
// ---------------------------------------------------------------------------

void scheduleRead(std::shared_ptr<ProcessEntry> entry, bool isStderr);

void handleRead(std::shared_ptr<ProcessEntry> entry, bool isStderr,
                const boost::system::error_code& ec, std::size_t n)
{
    auto& buf      = isStderr ? entry->err_read_buf : entry->out_read_buf;
    auto& line_buf = isStderr ? entry->err_line_buf : entry->out_line_buf;

    if (n > 0) {
        line_buf.append(buf.data(), n);
        std::size_t pos = 0, nl;
        while ((nl = line_buf.find('\n', pos)) != std::string::npos) {
            std::string line = line_buf.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && entry->callbacks.onOutput)
                entry->callbacks.onOutput(entry->name, line, isStderr);
            pos = nl + 1;
        }
        line_buf.erase(0, pos);
    }

    if (!ec) {
        scheduleRead(std::move(entry), isStderr);
    } else {
        // Pipe closed — flush any remaining partial line (stripping a
        // trailing CR, same as the newline loop above).
        if (!line_buf.empty() && entry->callbacks.onOutput) {
            if (line_buf.back() == '\r') line_buf.pop_back();
            if (!line_buf.empty())
                entry->callbacks.onOutput(entry->name, line_buf, isStderr);
        }
        line_buf.clear();
    }
}

void scheduleRead(std::shared_ptr<ProcessEntry> entry, bool isStderr) {
    auto* e = entry.get();
    auto& pipe = isStderr ? e->err_pipe : e->out_pipe;
    auto& buf  = isStderr ? e->err_read_buf : e->out_read_buf;
    pipe.async_read_some(
        asio::buffer(buf),
        [entry = std::move(entry), isStderr](const boost::system::error_code& ec, std::size_t n) mutable {
            handleRead(std::move(entry), isStderr, ec, n);
        });
}

// ---------------------------------------------------------------------------
// Async wait
// ---------------------------------------------------------------------------

void scheduleWait(std::shared_ptr<ProcessEntry> entry) {
    auto* e = entry.get();
    e->process.async_wait(
        [entry = std::move(entry)](const boost::system::error_code& /*ec*/, int raw_status) mutable {
            entry->exited.store(true);

            std::string name = entry->name;
            bool was_cancelled = entry->cancelled.load();

            {
                std::lock_guard<std::mutex> lock(s_processesMutex);
                s_processes.erase(name);
            }

            if (!was_cancelled && entry->callbacks.onFinished) {
                bool crashed = false;
                int exit_code = raw_status;
#if defined(WIFEXITED)
                if (WIFSIGNALED(raw_status)) {
                    crashed    = true;
                    exit_code  = WTERMSIG(raw_status);
                } else if (WIFEXITED(raw_status)) {
                    exit_code = WEXITSTATUS(raw_status);
                }
#endif
                entry->callbacks.onFinished(name, exit_code, crashed);
            }
        });
}

// ---------------------------------------------------------------------------
// Synchronous kill
// ---------------------------------------------------------------------------

void syncKill(std::shared_ptr<ProcessEntry> entry) {
    if (!entry) return;

    entry->cancelled.store(true);

    boost::system::error_code ec;
    entry->out_pipe.close(ec); // cancel pending async_read on stdout
    entry->err_pipe.close(ec); // cancel pending async_read on stderr

    entry->process.request_exit(ec);

    auto wait = [&](std::chrono::milliseconds budget) -> bool {
        auto deadline = std::chrono::steady_clock::now() + budget;
        while (!entry->exited.load()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return true;
    };

    if (!wait(std::chrono::seconds(5))) {
        fprintf(stderr, "[SubprocessManager] Process did not terminate gracefully, killing: %s\n",
                entry->name.c_str());
        entry->process.terminate(ec);
        if (!wait(std::chrono::seconds(2))) {
            fprintf(stderr, "[SubprocessManager] Process did not respond to SIGKILL: %s\n",
                    entry->name.c_str());
        }
    }
}

} // anonymous namespace

// ===========================================================================
// ModuleRuntime interface
// ===========================================================================

bool SubprocessManager::canHandle(const LogosCore::ModuleDescriptor& desc) const
{
    return desc.format == "qt-plugin" || desc.format.empty();
}

bool SubprocessManager::load(const LogosCore::ModuleDescriptor& desc,
                              std::function<void(const std::string&)> onTerminated,
                              LogosCore::LoadedModuleHandle& out)
{
    std::string logosHostPath = resolveLogosHostPath(desc.modulesDirs);
    if (logosHostPath.empty())
        return false;

    std::vector<std::string> arguments = {
        "--name", desc.name,
        "--path", desc.path
    };

    if (!desc.instancePersistencePath.empty()) {
        arguments.push_back("--instance-persistence-path");
        arguments.push_back(desc.instancePersistencePath);
    }

    // Per-module transport set: forward the daemon-side serialized JSON
    // verbatim. The child's command_line_parser deserializes it back
    // into a LogosTransportSet and passes the result into LogosAPI's
    // explicit-transport constructor, so its provider binds every
    // listener instead of only the global default (LocalSocket).
    // bp2 takes care of escaping; we pass the JSON as a single argv
    // value, no shell quoting required.
    if (!desc.transportSetJson.empty()) {
        arguments.push_back("--transport-set");
        arguments.push_back(desc.transportSetJson);
    }

    ProcessCallbacks callbacks;

    callbacks.onFinished = [onTerminated](const std::string& pName, int exitCode, bool crashed) {
        (void)exitCode;
        if (crashed) {
            spdlog::critical("Module process crashed: {}", pName);
            exit(1);
        }
        if (onTerminated)
            onTerminated(pName);
    };

    callbacks.onError = [](const std::string& pName, bool crashed) {
        if (crashed) {
            spdlog::critical("Module process crashed: {}", pName);
            exit(1);
        }
    };

    callbacks.onOutput = [](const std::string& pName, const std::string& line, bool isStderr) {
        if (!isStderr) {
            moduleStdoutLogger()->info("[{}] {}", pName, line);
            return;
        }
        auto contains = [&](std::initializer_list<const char*> keywords) {
            for (const char* k : keywords)
                if (line.find(k) != std::string::npos) return true;
            return false;
        };
        // stderr: map common level prefixes (Qt's default message handler,
        // test/assertion frameworks, spdlog-style output from children)
        // onto spdlog levels. Default to info when no prefix is recognised.
        if (contains({"Critical:", "CRITICAL:", "Fatal:", "FATAL:"}))
            spdlog::critical("[{}] {}", pName, line);
        else if (contains({"Error:", "ERROR:", "FAILED:"}))
            spdlog::error("[{}] {}", pName, line);
        else if (contains({"Warning:", "WARNING:"}))
            spdlog::warn("[{}] {}", pName, line);
        else if (contains({"Debug:", "DEBUG:"}))
            spdlog::debug("[{}] {}", pName, line);
        else if (contains({"Trace:", "TRACE:"}))
            spdlog::trace("[{}] {}", pName, line);
        else
            spdlog::info("[{}] {}", pName, line);
    };

    if (!startProcess(desc.name, logosHostPath, arguments, callbacks))
        return false;

    out.name = desc.name;
    out.pid  = getProcessId(desc.name);
    return true;
}

bool SubprocessManager::sendToken(const std::string& name, const std::string& token)
{
    return sendTokenToProcess(name, token);
}

void SubprocessManager::terminate(const std::string& name)
{
    terminateProcess(name);
}

void SubprocessManager::terminateAll()
{
    terminateAllProcesses();
}

bool SubprocessManager::hasModule(const std::string& name) const
{
    return hasProcess(name);
}

std::optional<int64_t> SubprocessManager::pid(const std::string& name) const
{
    int64_t p = getProcessId(name);
    if (p < 0) return std::nullopt;
    return p;
}

std::unordered_map<std::string, int64_t> SubprocessManager::getAllPids() const
{
    return getAllProcessIds();
}

// ===========================================================================
// Static process management API
// ===========================================================================

bool SubprocessManager::startProcess(const std::string& name, const std::string& executable,
                                      const std::vector<std::string>& arguments,
                                      const ProcessCallbacks& callbacks)
{
    IoRuntime& rt = ioRuntime();

    boost::system::error_code ec;

    // Separate pipes for stdout and stderr so the reader can distinguish
    // them (child's stdout → out_rpipe, child's stderr → err_rpipe).
    asio::readable_pipe out_rpipe(rt.ctx), err_rpipe(rt.ctx);
    asio::writable_pipe out_wpipe(rt.ctx), err_wpipe(rt.ctx);

    asio::connect_pipe(out_rpipe, out_wpipe, ec);
    if (ec) {
        fprintf(stderr, "[SubprocessManager] Failed to create stdout pipe for %s: %s\n",
                name.c_str(), ec.message().c_str());
        return false;
    }
    asio::connect_pipe(err_rpipe, err_wpipe, ec);
    if (ec) {
        fprintf(stderr, "[SubprocessManager] Failed to create stderr pipe for %s: %s\n",
                name.c_str(), ec.message().c_str());
        return false;
    }

    bp2::process_stdio pstdio;
    pstdio.out = out_wpipe;
    pstdio.err = err_wpipe;

    bp2::process proc = bp2::default_process_launcher()(rt.ctx, ec, executable, arguments, pstdio);

    // Close write ends in parent once child has inherited them
    out_wpipe.close();
    err_wpipe.close();

    if (ec) {
        fprintf(stderr, "[SubprocessManager] Failed to start process for %s: %s\n",
                name.c_str(), ec.message().c_str());
        return false;
    }

    auto entry = std::make_shared<ProcessEntry>(
        std::move(proc), std::move(out_rpipe), std::move(err_rpipe), name, callbacks);

    {
        std::lock_guard<std::mutex> lock(s_processesMutex);
        s_processes[name] = entry;
    }

    asio::post(rt.ctx, [entry]() {
        scheduleRead(entry, /*isStderr=*/false);
        scheduleRead(entry, /*isStderr=*/true);
        scheduleWait(entry);
    });

    return true;
}

bool SubprocessManager::sendTokenToProcess(const std::string& name,
                                            const std::string& token,
                                            int max_wait_ms)
{
    // Socket name is scoped by LOGOS_INSTANCE_ID so parallel Logos instances
    // (multiple daemons or Basecamp profiles) don't clash when loading the same
    // module. Matches the scheme in QtTokenReceiver on the receiver side.
    const char* instanceId = std::getenv("LOGOS_INSTANCE_ID");
    std::string socketName = "logos_token_" + name;
    if (instanceId && *instanceId) {
        socketName += "_";
        socketName += instanceId;
    }
    std::string path = unixSocketPath(socketName);

    // Validate up front: sockaddr_un::sun_path is fixed-size
    // (~104 bytes on macOS, ~108 on Linux). A long TMPDIR + module
    // name + LOGOS_INSTANCE_ID combination overflows that buffer,
    // and `strncpy(...sizeof(...) - 1)` below would silently
    // truncate — leaving us connecting to a *different* (or
    // nonexistent) socket while the child is bound to the full
    // path. Fail loudly instead.
    {
        struct sockaddr_un sample{};
        if (path.size() >= sizeof(sample.sun_path)) {
            fprintf(stderr,
                "[SubprocessManager] Unix socket path too long (%zu >= %zu): %s\n",
                path.size(), sizeof(sample.sun_path), path.c_str());
            std::shared_ptr<ProcessEntry> entry;
            {
                std::lock_guard<std::mutex> lock(s_processesMutex);
                auto it = s_processes.find(name);
                if (it != s_processes.end()) {
                    entry = it->second;
                    s_processes.erase(it);
                }
            }
            syncKill(entry);
            return false;
        }
    }

    // Deadline-driven retry loop. Sleep 50ms between attempts — fine
    // grained enough that a hot child (socket up before we finish our
    // own setup) returns within a couple of polls, but the total
    // budget is generous: previously a hard-coded 900ms (10 × 100ms),
    // which was tight enough that we'd lose to cold-start child Qt
    // initialisation under load and end up with "Failed to connect to
    // token socket" + a half-loaded module. The 5s default covers
    // realistic worst case (cold dylib load + Qt platform bring-up
    // + plugin parse) with margin.
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(max_wait_ms);

    int sock = -1;
    for (;;) {
        sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0) {
            fprintf(stderr, "[SubprocessManager] socket() failed: %s\n", strerror(errno));
            break;
        }

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        // Length already validated above — strncpy is safe here.
        strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0)
            break;

        ::close(sock);
        sock = -1;
        if (clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (sock < 0) {
        fprintf(stderr, "[SubprocessManager] Failed to connect to token socket for: %s\n",
                name.c_str());

        std::shared_ptr<ProcessEntry> entry;
        {
            std::lock_guard<std::mutex> lock(s_processesMutex);
            auto it = s_processes.find(name);
            if (it != s_processes.end()) {
                entry = it->second;
                s_processes.erase(it);
            }
        }
        syncKill(entry);
        return false;
    }

    const char* data = token.c_str();
    std::size_t total = token.size();
    std::size_t written = 0;
    while (written < total) {
        ssize_t n = ::write(sock, data + written, total - written);
        if (n <= 0) break;
        written += static_cast<std::size_t>(n);
    }

    ::close(sock);
    return written == total;
}

void SubprocessManager::terminateProcess(const std::string& name)
{
    std::shared_ptr<ProcessEntry> entry;
    {
        std::lock_guard<std::mutex> lock(s_processesMutex);
        auto it = s_processes.find(name);
        if (it == s_processes.end()) return;
        entry = it->second;
        s_processes.erase(it);
    }
    syncKill(entry);
}

void SubprocessManager::terminateAllProcesses()
{
    std::unordered_map<std::string, std::shared_ptr<ProcessEntry>> snapshot;
    {
        std::lock_guard<std::mutex> lock(s_processesMutex);
        if (s_processes.empty()) return;
        snapshot.swap(s_processes);
    }
    for (auto& [n, entry] : snapshot)
        syncKill(entry);
}

bool SubprocessManager::hasProcess(const std::string& name)
{
    std::lock_guard<std::mutex> lock(s_processesMutex);
    return s_processes.count(name) > 0;
}

int64_t SubprocessManager::getProcessId(const std::string& name)
{
    std::lock_guard<std::mutex> lock(s_processesMutex);
    auto it = s_processes.find(name);
    if (it == s_processes.end()) return -1;
    if (!it->second)              return -1;
    return static_cast<int64_t>(it->second->process.id());
}

std::unordered_map<std::string, int64_t> SubprocessManager::getAllProcessIds()
{
    std::lock_guard<std::mutex> lock(s_processesMutex);
    std::unordered_map<std::string, int64_t> result;
    for (auto& [n, entry] : s_processes)
        if (entry)
            result[n] = static_cast<int64_t>(entry->process.id());
    return result;
}

void SubprocessManager::clearAll()
{
    std::unordered_map<std::string, std::shared_ptr<ProcessEntry>> snapshot;
    {
        std::lock_guard<std::mutex> lock(s_processesMutex);
        snapshot.swap(s_processes);
    }
    for (auto& [n, entry] : snapshot)
        syncKill(entry);
}

void SubprocessManager::registerProcess(const std::string& name)
{
    std::lock_guard<std::mutex> lock(s_processesMutex);
    if (!s_processes.count(name))
        s_processes[name] = nullptr;
}
