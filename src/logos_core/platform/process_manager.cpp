#include "process_manager.h"
#include "ipc_paths.h"
#include "logos_logging.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <csignal>
#include <mutex>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

    struct ManagedProcess {
        pid_t pid = -1;
        int readFd = -1;
        std::thread worker;
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
        if (mp->pid > 0) {
            kill(mp->pid, SIGTERM);
            int status = 0;
            for (int i = 0; i < 50; ++i) {
                pid_t w = waitpid(mp->pid, &status, WNOHANG);
                if (w == mp->pid)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (waitpid(mp->pid, &status, WNOHANG) == 0) {
                logos_log_warn("Process did not terminate gracefully, killing it: {}", name);
                kill(mp->pid, SIGKILL);
                waitpid(mp->pid, &status, 0);
            }
            mp->pid = -1;
        }
        if (mp->worker.joinable())
            mp->worker.join();
        delete mp;
    }

    static void worker_main(std::string name, pid_t pid, int readFd, ProcessManager::ProcessCallbacks callbacks)
    {
        std::string buf;
        char tmp[4096];
        for (;;) {
            const ssize_t n = read(readFd, tmp, sizeof tmp);
            if (n <= 0)
                break;
            buf.append(tmp, static_cast<size_t>(n));
            flush_lines(buf, name, callbacks, false);
        }
        trim_inplace(buf);
        if (!buf.empty() && callbacks.onOutput)
            callbacks.onOutput(name, buf, false);

        int status = 0;
        waitpid(pid, &status, 0);
        const bool crashed = WIFSIGNALED(status) != 0;
        const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

        close(readFd);

        if (callbacks.onFinished)
            callbacks.onFinished(name, exitCode, crashed);
    }
}

namespace ProcessManager {

    bool startProcess(const std::string& name, const std::string& executable,
                      const std::vector<std::string>& arguments, const ProcessCallbacks& callbacks)
    {
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            logos_log_critical("pipe() failed for process {}: {}", name, strerror(errno));
            return false;
        }

        pid_t pid = fork();
        if (pid < 0) {
            logos_log_critical("fork() failed for {}: {}", name, strerror(errno));
            close(pipefd[0]);
            close(pipefd[1]);
            return false;
        }

        if (pid == 0) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            if (pipefd[1] != STDOUT_FILENO && pipefd[1] != STDERR_FILENO)
                close(pipefd[1]);

            std::vector<char*> argv;
            argv.reserve(arguments.size() + 2);
            argv.push_back(const_cast<char*>(executable.c_str()));
            for (const auto& a : arguments)
                argv.push_back(const_cast<char*>(const_cast<std::string&>(a).data()));
            argv.push_back(nullptr);

            execvp(executable.c_str(), argv.data());
            _exit(127);
        }

        close(pipefd[1]);
        const int readFd = pipefd[0];

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        int st = 0;
        const pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) {
            close(readFd);
            logos_log_critical("Failed to start process for {}: child exited immediately (code {})", name,
                            WIFEXITED(st) ? WEXITSTATUS(st) : -1);
            return false;
        }

        auto* mp = new ManagedProcess();
        mp->pid = pid;
        mp->readFd = readFd;
        mp->worker = std::thread(worker_main, name, pid, readFd, callbacks);

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

        bool connected = false;
        int fd = -1;
        for (int attempt = 0; attempt < 10; ++attempt) {
            fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
            if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                connected = true;
                break;
            }
            close(fd);
            fd = -1;
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

        const ssize_t w = write(fd, token.data(), token.size());
        (void)w;
        close(fd);
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
        if (it == s_processes.end() || !it->second)
            return -1;
        return static_cast<int64_t>(it->second->pid);
    }

    std::unordered_map<std::string, int64_t> getAllProcessIds()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        std::unordered_map<std::string, int64_t> result;
        for (const auto& e : s_processes) {
            if (e.second && e.second->pid > 0)
                result[e.first] = static_cast<int64_t>(e.second->pid);
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
            if (mp->readFd >= 0) {
                close(mp->readFd);
                mp->readFd = -1;
            }
            if (mp->pid > 0) {
                kill(mp->pid, SIGTERM);
                int status = 0;
                waitpid(mp->pid, &status, 0);
                mp->pid = -1;
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
