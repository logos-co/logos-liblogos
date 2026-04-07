#include "process_manager.h"
#include "ipc_paths.h"
#include "logos_logging.h"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/write.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>
#include <boost/system/error_code.hpp>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

    namespace bp = boost::process::v2;

    struct ManagedProcess {
        boost::asio::io_context io;
        boost::asio::readable_pipe pipe;
        std::unique_ptr<bp::process> proc;
        std::thread worker;

        ManagedProcess()
            : pipe(io)
        {
        }
    };

    std::mutex s_mutex;
    std::unordered_map<std::string, ManagedProcess*> s_processes;

    static void trim_inplace(std::string& s)
    {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
            s.pop_back();
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
            ++i;
        if (i > 0)
            s.erase(0, i);
    }

    static void flush_lines(std::string& buf, const std::string& name,
                            const ProcessManager::ProcessCallbacks& callbacks, bool isStderr)
    {
        for (;;) {
            const auto p = buf.find('\n');
            if (p == std::string::npos)
                break;
            std::string line = buf.substr(0, p);
            buf.erase(0, p + 1);
            trim_inplace(line);
            if (!line.empty() && callbacks.onOutput)
                callbacks.onOutput(name, line, isStderr);
        }
    }

    static void destroy_process(ManagedProcess* mp, const std::string& name)
    {
        if (!mp)
            return;
        if (mp->proc && mp->proc->is_open()) {
            boost::system::error_code ec;
            mp->proc->request_exit(ec);
            for (int i = 0; i < 50; ++i) {
                if (!mp->proc->running(ec))
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (mp->proc->running(ec)) {
                logos_log_warn("Process did not terminate gracefully, killing it: {}", name);
                mp->proc->terminate(ec);
            }
        }
        if (mp->worker.joinable())
            mp->worker.join();
        delete mp;
    }

    static void worker_main(std::string name, boost::asio::readable_pipe* outPipe, bp::process* proc,
                            ProcessManager::ProcessCallbacks callbacks)
    {
        std::string buf;
        char tmp[4096];
        boost::system::error_code ec;

        for (;;) {
            std::size_t n = outPipe->read_some(boost::asio::buffer(tmp), ec);
            if (ec == boost::asio::error::eof)
                break;
            if (ec)
                break;
            if (n == 0)
                continue;
            buf.append(tmp, n);
            flush_lines(buf, name, callbacks, false);
        }
        trim_inplace(buf);
        if (!buf.empty() && callbacks.onOutput)
            callbacks.onOutput(name, buf, false);

        proc->wait(ec);
        const int native = proc->native_exit_code();
        const bool crashed = WIFSIGNALED(native) != 0;
        const int exitCode = WIFEXITED(native) ? WEXITSTATUS(native) : -1;

        boost::system::error_code closeEc;
        outPipe->close(closeEc);

        if (callbacks.onFinished)
            callbacks.onFinished(name, exitCode, crashed);
    }
}

namespace ProcessManager {

    bool startProcess(const std::string& name, const std::string& executable,
                      const std::vector<std::string>& arguments, const ProcessCallbacks& callbacks)
    {
        int fds[2];
        if (::pipe(fds) != 0) {
            logos_log_critical("pipe() failed for process {}: {}", name, std::strerror(errno));
            return false;
        }

        auto* mp = new ManagedProcess();
        boost::system::error_code pec;
        mp->pipe.assign(fds[0], pec);
        if (pec) {
            logos_log_critical("readable_pipe::assign failed for {}: {}", name, pec.message());
            ::close(fds[0]);
            ::close(fds[1]);
            delete mp;
            return false;
        }

        bp::process_stdio stdio{};
        stdio.out = bp::detail::process_output_binding(fds[1]);
        stdio.err = bp::detail::process_error_binding(fds[1]);

        try {
            const boost::asio::any_io_executor exec = mp->io.get_executor();
            mp->proc = std::make_unique<bp::process>(exec, boost::filesystem::path(executable), arguments,
                                                     std::move(stdio));
        } catch (const std::exception& ex) {
            logos_log_critical("Failed to start process for {}: {}", name, ex.what());
            boost::system::error_code cec;
            mp->pipe.close(cec);
            ::close(fds[1]);
            delete mp;
            return false;
        }

        ::close(fds[1]);

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        boost::system::error_code rec;
        if (!mp->proc->running(rec)) {
            if (!rec) {
                const int code = mp->proc->exit_code();
                boost::system::error_code cec;
                mp->pipe.close(cec);
                logos_log_critical("Failed to start process for {}: child exited immediately (code {})", name, code);
                delete mp;
                return false;
            }
        }

        mp->worker = std::thread(worker_main, name, &mp->pipe, mp->proc.get(), callbacks);

        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_processes[name] = mp;
        }

        return true;
    }

    bool sendToken(const std::string& name, const std::string& token)
    {
        const auto path = logos_token_socket_path(name).string();
        if (path.size() >= sizeof(sockaddr_un::sun_path)) {
            logos_log_critical("Token socket path too long for {}", name);
            return false;
        }

        using local = boost::asio::local::stream_protocol;
        bool connected = false;
        for (int attempt = 0; attempt < 10; ++attempt) {
            boost::asio::io_context io;
            local::endpoint ep(path);
            local::socket sock(io);
            boost::system::error_code ec;
            sock.connect(ep, ec);
            if (!ec) {
                boost::asio::write(sock, boost::asio::buffer(token), ec);
                if (!ec) {
                    connected = true;
                    boost::system::error_code sec;
                    sock.shutdown(local::socket::shutdown_send, sec);
                }
                break;
            }
            if (attempt + 1 < 10)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (!connected) {
            logos_log_critical("Failed to connect to token socket for: {}", name);
            ManagedProcess* p = nullptr;
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                auto it = s_processes.find(name);
                if (it != s_processes.end()) {
                    p = it->second;
                    s_processes.erase(it);
                }
            }
            if (p)
                destroy_process(p, name);
            return false;
        }

        return true;
    }

    void terminateProcess(const std::string& name)
    {
        ManagedProcess* p = nullptr;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            auto it = s_processes.find(name);
            if (it != s_processes.end()) {
                p = it->second;
                s_processes.erase(it);
            }
        }
        if (p)
            destroy_process(p, name);
    }

    void terminateAll()
    {
        std::unordered_map<std::string, ManagedProcess*> snapshot;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_processes.empty())
                return;
            snapshot.swap(s_processes);
        }
        for (auto& e : snapshot) {
            if (e.second)
                destroy_process(e.second, e.first);
        }
    }

    bool hasProcess(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_processes.find(name) != s_processes.end();
    }

    int64_t getProcessId(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_processes.find(name);
        if (it == s_processes.end() || !it->second || !it->second->proc || !it->second->proc->is_open())
            return -1;
        return static_cast<int64_t>(it->second->proc->id());
    }

    std::unordered_map<std::string, int64_t> getAllProcessIds()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        std::unordered_map<std::string, int64_t> result;
        for (const auto& e : s_processes) {
            if (e.second && e.second->proc && e.second->proc->is_open())
                result[e.first] = static_cast<int64_t>(e.second->proc->id());
        }
        return result;
    }

    void clearAll()
    {
        std::unordered_map<std::string, ManagedProcess*> snapshot;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            snapshot.swap(s_processes);
        }
        for (auto& e : snapshot) {
            if (!e.second)
                continue;
            ManagedProcess* mp = e.second;
            boost::system::error_code ec;
            mp->pipe.close(ec);
            if (mp->proc && mp->proc->is_open()) {
                mp->proc->request_exit(ec);
                for (int i = 0; i < 50; ++i) {
                    if (!mp->proc->running(ec))
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (mp->proc->running(ec))
                    mp->proc->terminate(ec);
            }
            if (mp->worker.joinable())
                mp->worker.join();
            delete mp;
        }
    }

    void registerProcess(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_processes.find(name) == s_processes.end())
            s_processes[name] = nullptr;
    }
}
