#include "subprocess_container.h"

#include <boost/asio/connect_pipe.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>

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
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace bp2  = boost::process::v2;
namespace asio = boost::asio;

namespace {

std::shared_ptr<spdlog::logger>& moduleStdoutLogger() {
    static std::shared_ptr<spdlog::logger> logger = []() {
        auto l = spdlog::stdout_color_mt("logos_module_stdout");
        l->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [out] %v");
        return l;
    }();
    return logger;
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
            // See subprocess_manager.cpp comment on self-join deadlock during
            // exit()-triggered static destruction.
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
    bp2::process                              process;
    asio::readable_pipe                       out_pipe;
    asio::readable_pipe                       err_pipe;
    SubprocessContainer::ProcessCallbacks     callbacks;
    std::string                               name;
    std::array<char, 4096>                    out_read_buf{};
    std::array<char, 4096>                    err_read_buf{};
    std::string                               out_line_buf;
    std::string                               err_line_buf;
    std::atomic<bool>                         exited{false};
    std::atomic<bool>                         cancelled{false};

    ProcessEntry(bp2::process proc,
                 asio::readable_pipe out_rp, asio::readable_pipe err_rp,
                 const std::string& n, const SubprocessContainer::ProcessCallbacks& cb)
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
// Async read loop
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
    entry->out_pipe.close(ec);
    entry->err_pipe.close(ec);

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
        fprintf(stderr, "[SubprocessContainer] Process did not terminate gracefully, killing: %s\n",
                entry->name.c_str());
        entry->process.terminate(ec);
        if (!wait(std::chrono::seconds(2))) {
            fprintf(stderr, "[SubprocessContainer] Process did not respond to SIGKILL: %s\n",
                    entry->name.c_str());
        }
    }
}

} // anonymous namespace

// ===========================================================================
// ModuleContainer interface
// ===========================================================================

bool SubprocessContainer::canHandle(const LogosCore::ModuleDescriptor& /*desc*/) const
{
    return true;
}

bool SubprocessContainer::launch(const LogosCore::ModuleDescriptor& desc,
                                  const std::string& hostBinary,
                                  const std::vector<std::string>& args,
                                  std::function<void(const std::string&)> onTerminated,
                                  LogosCore::LoadedModuleHandle& out)
{
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

    if (!startProcess(desc.name, hostBinary, args, callbacks))
        return false;

    out.name = desc.name;
    out.pid  = getProcessId(desc.name);
    return true;
}

bool SubprocessContainer::sendToken(const std::string& name, const std::string& token)
{
    return sendTokenToProcess(name, token);
}

void SubprocessContainer::terminate(const std::string& name)
{
    terminateProcess(name);
}

void SubprocessContainer::terminateAll()
{
    terminateAllProcesses();
}

bool SubprocessContainer::hasModule(const std::string& name) const
{
    return hasProcess(name);
}

std::optional<int64_t> SubprocessContainer::pid(const std::string& name) const
{
    int64_t p = getProcessId(name);
    if (p < 0) return std::nullopt;
    return p;
}

std::unordered_map<std::string, int64_t> SubprocessContainer::getAllPids() const
{
    return getAllProcessIds();
}

// ===========================================================================
// Static process management API
// ===========================================================================

bool SubprocessContainer::startProcess(const std::string& name, const std::string& executable,
                                        const std::vector<std::string>& arguments,
                                        const ProcessCallbacks& callbacks)
{
    IoRuntime& rt = ioRuntime();

    boost::system::error_code ec;

    asio::readable_pipe out_rpipe(rt.ctx), err_rpipe(rt.ctx);
    asio::writable_pipe out_wpipe(rt.ctx), err_wpipe(rt.ctx);

    asio::connect_pipe(out_rpipe, out_wpipe, ec);
    if (ec) {
        fprintf(stderr, "[SubprocessContainer] Failed to create stdout pipe for %s: %s\n",
                name.c_str(), ec.message().c_str());
        return false;
    }
    asio::connect_pipe(err_rpipe, err_wpipe, ec);
    if (ec) {
        fprintf(stderr, "[SubprocessContainer] Failed to create stderr pipe for %s: %s\n",
                name.c_str(), ec.message().c_str());
        return false;
    }

    bp2::process_stdio pstdio;
    pstdio.out = out_wpipe;
    pstdio.err = err_wpipe;

    bp2::process proc = bp2::default_process_launcher()(rt.ctx, ec, executable, arguments, pstdio);

    out_wpipe.close();
    err_wpipe.close();

    if (ec) {
        fprintf(stderr, "[SubprocessContainer] Failed to start process for %s: %s\n",
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

bool SubprocessContainer::sendTokenToProcess(const std::string& name,
                                              const std::string& token,
                                              int max_wait_ms)
{
    const char* instanceId = std::getenv("LOGOS_INSTANCE_ID");
    std::string socketName = "logos_token_" + name;
    if (instanceId && *instanceId) {
        socketName += "_";
        socketName += instanceId;
    }
    std::string path = unixSocketPath(socketName);

    {
        struct sockaddr_un sample{};
        if (path.size() >= sizeof(sample.sun_path)) {
            fprintf(stderr,
                "[SubprocessContainer] Unix socket path too long (%zu >= %zu): %s\n",
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

    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(max_wait_ms);

    int sock = -1;
    for (;;) {
        sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0) {
            fprintf(stderr, "[SubprocessContainer] socket() failed: %s\n", strerror(errno));
            break;
        }

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0)
            break;

        ::close(sock);
        sock = -1;
        if (clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (sock < 0) {
        fprintf(stderr, "[SubprocessContainer] Failed to connect to token socket for: %s\n",
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

void SubprocessContainer::terminateProcess(const std::string& name)
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

void SubprocessContainer::terminateAllProcesses()
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

bool SubprocessContainer::hasProcess(const std::string& name)
{
    std::lock_guard<std::mutex> lock(s_processesMutex);
    return s_processes.count(name) > 0;
}

int64_t SubprocessContainer::getProcessId(const std::string& name)
{
    std::lock_guard<std::mutex> lock(s_processesMutex);
    auto it = s_processes.find(name);
    if (it == s_processes.end()) return -1;
    if (!it->second)              return -1;
    return static_cast<int64_t>(it->second->process.id());
}

std::unordered_map<std::string, int64_t> SubprocessContainer::getAllProcessIds()
{
    std::lock_guard<std::mutex> lock(s_processesMutex);
    std::unordered_map<std::string, int64_t> result;
    for (auto& [n, entry] : s_processes)
        if (entry)
            result[n] = static_cast<int64_t>(entry->process.id());
    return result;
}

void SubprocessContainer::clearAll()
{
    std::unordered_map<std::string, std::shared_ptr<ProcessEntry>> snapshot;
    {
        std::lock_guard<std::mutex> lock(s_processesMutex);
        snapshot.swap(s_processes);
    }
    for (auto& [n, entry] : snapshot)
        syncKill(entry);
}

void SubprocessContainer::registerProcess(const std::string& name)
{
    std::lock_guard<std::mutex> lock(s_processesMutex);
    if (!s_processes.count(name))
        s_processes[name] = nullptr;
}
