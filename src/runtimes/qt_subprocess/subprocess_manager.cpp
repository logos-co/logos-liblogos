#include "subprocess_manager.h"

#include <boost/asio/connect_pipe.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace bp2  = boost::process::v2;
namespace asio = boost::asio;

namespace {

// ---------------------------------------------------------------------------
// Background io_context: one thread, kept alive by a work guard.
// Lazily initialised on first startProcess() call via ioRuntime().
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
        if (thread.joinable()) thread.join();
    }
};

IoRuntime& ioRuntime() {
    static IoRuntime s_runtime;
    return s_runtime;
}

// ---------------------------------------------------------------------------
// ProcessEntry: owns one live child process and its read pipe.
// Lifetime is managed via shared_ptr so async callbacks can safely reference
// it even after the entry is removed from s_processes.
// ---------------------------------------------------------------------------

struct ProcessEntry {
    bp2::process                            process;
    asio::readable_pipe                     pipe;
    SubprocessManager::ProcessCallbacks     callbacks;
    std::string                             name;
    std::array<char, 4096>                  read_buf{};
    std::string                             line_buf;
    // Set by async_wait callback; used by syncKill to avoid double-waitpid.
    std::atomic<bool>                 exited{false};
    // Set by terminateProcess/terminateAll to suppress onFinished callback.
    std::atomic<bool>                 cancelled{false};

    ProcessEntry(bp2::process proc, asio::readable_pipe rp,
                 const std::string& n, const SubprocessManager::ProcessCallbacks& cb)
        : process(std::move(proc))
        , pipe(std::move(rp))
        , name(n)
        , callbacks(cb)
    {}
};

// ---------------------------------------------------------------------------
// Global process registry: name -> ProcessEntry (nullptr = placeholder)
// ---------------------------------------------------------------------------

std::unordered_map<std::string, std::shared_ptr<ProcessEntry>> s_processes;
std::mutex s_processesMutex;

// ---------------------------------------------------------------------------
// Unix domain socket path — matches Qt's QLocalSocket::connectToServer(),
// which resolves to $TMPDIR/<name> on macOS and /tmp/<name> on Linux.
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
// Async read loop: reads merged stdout+stderr from child, fires onOutput per line.
// Called from the io_context thread.
// ---------------------------------------------------------------------------

void scheduleRead(std::shared_ptr<ProcessEntry> entry);

void handleRead(std::shared_ptr<ProcessEntry> entry,
                const boost::system::error_code& ec, std::size_t n)
{
    if (n > 0) {
        entry->line_buf.append(entry->read_buf.data(), n);
        std::size_t pos = 0, nl;
        while ((nl = entry->line_buf.find('\n', pos)) != std::string::npos) {
            std::string line = entry->line_buf.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && entry->callbacks.onOutput)
                entry->callbacks.onOutput(entry->name, line, false);
            pos = nl + 1;
        }
        entry->line_buf.erase(0, pos);
    }

    if (!ec) {
        scheduleRead(std::move(entry));
    } else {
        // Pipe closed — flush any remaining partial line
        if (!entry->line_buf.empty() && entry->callbacks.onOutput)
            entry->callbacks.onOutput(entry->name, entry->line_buf, false);
        entry->line_buf.clear();
    }
}

void scheduleRead(std::shared_ptr<ProcessEntry> entry) {
    auto* e = entry.get();
    e->pipe.async_read_some(
        asio::buffer(e->read_buf),
        [entry = std::move(entry)](const boost::system::error_code& ec, std::size_t n) mutable {
            handleRead(std::move(entry), ec, n);
        });
}

// ---------------------------------------------------------------------------
// Async wait: fires when child process exits.
// Called from the io_context thread.
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
// Synchronous kill: send SIGTERM, wait up to 5 s, then SIGKILL.
// Waits for the exited flag set by the async_wait callback so we do not
// call waitpid a second time (which would race with Boost.Asio's reaping).
// ---------------------------------------------------------------------------

void syncKill(std::shared_ptr<ProcessEntry> entry) {
    if (!entry) return;

    entry->cancelled.store(true);

    boost::system::error_code ec;
    entry->pipe.close(ec); // cancel pending async_read

    entry->process.request_exit(ec); // SIGTERM

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
        entry->process.terminate(ec); // SIGKILL
        if (!wait(std::chrono::seconds(2))) {
            fprintf(stderr, "[SubprocessManager] Process did not respond to SIGKILL: %s\n",
                    entry->name.c_str());
        }
    }
}

} // anonymous namespace

// ===========================================================================
// SubprocessManager public API
// ===========================================================================

namespace SubprocessManager {

bool startProcess(const std::string& name, const std::string& executable,
                  const std::vector<std::string>& arguments,
                  const ProcessCallbacks& callbacks)
{
    IoRuntime& rt = ioRuntime();

    boost::system::error_code ec;

    // Create a pipe: parent reads from rpipe; child writes to wpipe (stdout+stderr)
    asio::readable_pipe rpipe(rt.ctx);
    asio::writable_pipe wpipe(rt.ctx);
    asio::connect_pipe(rpipe, wpipe, ec);
    if (ec) {
        fprintf(stderr, "[SubprocessManager] Failed to create pipe for %s: %s\n",
                name.c_str(), ec.message().c_str());
        return false;
    }

    // Redirect both stdout and stderr to wpipe (merged channels)
    bp2::process_stdio pstdio;
    pstdio.out = wpipe;
    pstdio.err = wpipe;

    bp2::process proc = bp2::default_process_launcher()(rt.ctx, ec, executable, arguments, pstdio);

    // Close write end in parent once child has inherited it
    wpipe.close();

    if (ec) {
        fprintf(stderr, "[SubprocessManager] Failed to start process for %s: %s\n",
                name.c_str(), ec.message().c_str());
        return false;
    }

    auto entry = std::make_shared<ProcessEntry>(
        std::move(proc), std::move(rpipe), name, callbacks);

    {
        std::lock_guard<std::mutex> lock(s_processesMutex);
        s_processes[name] = entry;
    }

    asio::post(rt.ctx, [entry]() {
        scheduleRead(entry);
        scheduleWait(entry);
    });

    return true;
}

bool sendToken(const std::string& name, const std::string& token)
{
    std::string socketName = "logos_token_" + name;
    std::string path = unixSocketPath(socketName);

    // Retry up to 10 times with 100 ms sleep (matching Qt behaviour)
    int sock = -1;
    for (int attempt = 0; attempt < 10; ++attempt) {
        sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0) {
            fprintf(stderr, "[SubprocessManager] socket() failed: %s\n", strerror(errno));
            break;
        }

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0)
            break; // connected

        ::close(sock);
        sock = -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (sock < 0) {
        fprintf(stderr, "[SubprocessManager] Failed to connect to token socket for: %s\n",
                name.c_str());

        // Remove and kill associated process
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

void terminateProcess(const std::string& name)
{
    std::shared_ptr<ProcessEntry> entry;
    {
        std::lock_guard<std::mutex> lock(s_processesMutex);
        auto it = s_processes.find(name);
        if (it == s_processes.end()) return;
        entry = it->second;
        s_processes.erase(it);
    }
    syncKill(entry); // blocks until process exits
}

void terminateAll()
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

bool hasProcess(const std::string& name)
{
    std::lock_guard<std::mutex> lock(s_processesMutex);
    return s_processes.count(name) > 0;
}

int64_t getProcessId(const std::string& name)
{
    std::lock_guard<std::mutex> lock(s_processesMutex);
    auto it = s_processes.find(name);
    if (it == s_processes.end()) return -1;
    if (!it->second)              return -1; // placeholder
    return static_cast<int64_t>(it->second->process.id());
}

std::unordered_map<std::string, int64_t> getAllProcessIds()
{
    std::lock_guard<std::mutex> lock(s_processesMutex);
    std::unordered_map<std::string, int64_t> result;
    for (auto& [n, entry] : s_processes)
        if (entry)
            result[n] = static_cast<int64_t>(entry->process.id());
    return result;
}

void clearAll()
{
    std::unordered_map<std::string, std::shared_ptr<ProcessEntry>> snapshot;
    {
        std::lock_guard<std::mutex> lock(s_processesMutex);
        snapshot.swap(s_processes);
    }
    for (auto& [n, entry] : snapshot)
        syncKill(entry);
}

void registerProcess(const std::string& name)
{
    std::lock_guard<std::mutex> lock(s_processesMutex);
    if (!s_processes.count(name))
        s_processes[name] = nullptr;
}

} // namespace SubprocessManager
