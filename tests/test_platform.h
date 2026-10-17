#ifndef LOGOS_TEST_PLATFORM_H
#define LOGOS_TEST_PLATFORM_H

// What the tests need from the OS, the same way on Unix and Windows:
// environment edits, temporary directories, child processes, and the path of
// the stand-in module host (tests/fake_module_host_main.cpp).

#include <cstdint>
#include <filesystem>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdlib>
#else
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace logos_test {

namespace fs = std::filesystem;

inline void setEnv(const char* name, const std::string& value)
{
#ifdef _WIN32
    // The CRT's copy for getenv, the process block for children.
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    ::setenv(name, value.c_str(), 1);
#endif
}

inline void unsetEnv(const char* name)
{
#ifdef _WIN32
    _putenv_s(name, "");
    SetEnvironmentVariableA(name, nullptr);
#else
    ::unsetenv(name);
#endif
}

inline std::int64_t currentPid()
{
#ifdef _WIN32
    return static_cast<std::int64_t>(GetCurrentProcessId());
#else
    return static_cast<std::int64_t>(::getpid());
#endif
}

// A new, empty directory under the system temp directory.
inline fs::path makeTempDir(const std::string& prefix)
{
    std::random_device seed;
    std::mt19937_64 random(
        (static_cast<std::uint64_t>(seed()) << 32) ^ seed() ^
        static_cast<std::uint64_t>(currentPid()));
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::ostringstream name;
        name << prefix << std::hex << random();
        const fs::path candidate = fs::temp_directory_path() / name.str();
        std::error_code ec;
        if (fs::create_directory(candidate, ec)) return candidate;
    }
    throw std::runtime_error("could not create a temporary directory");
}

// Removed with everything in it when the test ends.
struct TmpDir {
    fs::path path;

    explicit TmpDir(const std::string& prefix = "logos_test_") : path(makeTempDir(prefix)) {}
    TmpDir(const TmpDir&) = delete;
    TmpDir& operator=(const TmpDir&) = delete;

    ~TmpDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Kills a process this test did not start, as a crash would.
inline bool killPid(std::int64_t pid)
{
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (!process) return false;
    const bool killed = TerminateProcess(process, 9) != 0;
    CloseHandle(process);
    return killed;
#else
    return ::kill(static_cast<pid_t>(pid), SIGKILL) == 0;
#endif
}

inline fs::path thisExecutable()
{
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) return {};
        if (n < buffer.size()) {
            buffer.resize(n);
            return fs::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    std::error_code ec;
    return fs::canonical(fs::path(buffer.c_str()), ec);
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec);
#endif
}

// The stand-in module host, built beside this test binary.
inline fs::path fakeHostPath()
{
#ifdef _WIN32
    return thisExecutable().parent_path() / "logos_fake_module_host.exe";
#else
    return thisExecutable().parent_path() / "logos_fake_module_host";
#endif
}

#ifdef _WIN32
inline std::wstring widen(const std::string& text)
{
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

// Quoted so CommandLineToArgvW gives the child `arg` back unchanged.
inline std::wstring quoteArg(const std::wstring& arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        out.push_back(*it);
    }
    out.push_back(L'"');
    return out;
}
#endif

// A child process, optionally with pipes to its stdin and from its stdout.
// Killed and reaped when it goes out of scope.
class Child {
public:
    Child() = default;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    ~Child() { kill(); }

    bool start(const std::string& exe, const std::vector<std::string>& args, bool pipes = false)
    {
#ifdef _WIN32
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE childIn = nullptr;
        HANDLE childOut = nullptr;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        if (pipes) {
            if (!CreatePipe(&childIn, &m_in, &inherit, 0)) return false;
            if (!CreatePipe(&m_out, &childOut, &inherit, 0)) {
                CloseHandle(childIn);
                closeInput();
                return false;
            }
            SetHandleInformation(m_in, HANDLE_FLAG_INHERIT, 0);
            SetHandleInformation(m_out, HANDLE_FLAG_INHERIT, 0);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = childIn;
            startup.hStdOutput = childOut;
            startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        }
        std::wstring command = quoteArg(widen(exe));
        for (const auto& arg : args) command += L" " + quoteArg(widen(arg));
        PROCESS_INFORMATION info{};
        const BOOL started = CreateProcessW(widen(exe).c_str(), command.data(), nullptr, nullptr,
                                            pipes ? TRUE : FALSE, 0, nullptr, nullptr, &startup, &info);
        if (childIn) CloseHandle(childIn);
        if (childOut) CloseHandle(childOut);
        if (!started) {
            closeInput();
            closeOutput();
            return false;
        }
        CloseHandle(info.hThread);
        m_process = info.hProcess;
        m_pid = static_cast<std::int64_t>(info.dwProcessId);
        return true;
#else
        int in[2] = {-1, -1};
        int out[2] = {-1, -1};
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        if (pipes) {
            if (::pipe(in) != 0 || ::pipe(out) != 0) {
                posix_spawn_file_actions_destroy(&actions);
                for (int fd : {in[0], in[1], out[0], out[1]})
                    if (fd >= 0) ::close(fd);
                return false;
            }
            for (int fd : {in[0], in[1], out[0], out[1]}) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
            posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
            posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
        }
        std::vector<std::string> argvStore{exe};
        argvStore.insert(argvStore.end(), args.begin(), args.end());
        std::vector<char*> argv;
        for (auto& arg : argvStore) argv.push_back(arg.data());
        argv.push_back(nullptr);
        pid_t pid = 0;
        const int rc = posix_spawn(&pid, exe.c_str(), &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (pipes) {
            ::close(in[0]);
            ::close(out[1]);
            m_in = in[1];
            m_out = out[0];
        }
        if (rc != 0) {
            closeInput();
            closeOutput();
            return false;
        }
        m_pid = pid;
        return true;
#endif
    }

    std::int64_t pid() const { return m_pid; }

    bool write(const std::string& data)
    {
#ifdef _WIN32
        DWORD written = 0;
        return m_in && WriteFile(m_in, data.data(), static_cast<DWORD>(data.size()), &written, nullptr)
            && written == data.size();
#else
        return m_in >= 0 && ::write(m_in, data.data(), data.size()) == static_cast<ssize_t>(data.size());
#endif
    }

    // What the child wrote to stdout, up to `size` bytes; 0 once it is closed.
    size_t read(char* buffer, size_t size)
    {
#ifdef _WIN32
        DWORD n = 0;
        if (!m_out || !ReadFile(m_out, buffer, static_cast<DWORD>(size), &n, nullptr)) return 0;
        return n;
#else
        if (m_out < 0) return 0;
        ssize_t n;
        do n = ::read(m_out, buffer, size);
        while (n < 0 && errno == EINTR);
        return n > 0 ? static_cast<size_t>(n) : 0;
#endif
    }

    void closeInput()
    {
#ifdef _WIN32
        if (m_in) CloseHandle(m_in);
        m_in = nullptr;
#else
        if (m_in >= 0) ::close(m_in);
        m_in = -1;
#endif
    }

    void kill()
    {
        closeInput();
#ifdef _WIN32
        if (m_process) {
            TerminateProcess(m_process, 1);
            WaitForSingleObject(m_process, 10000);
            CloseHandle(m_process);
            m_process = nullptr;
        }
#else
        if (m_pid > 0) {
            ::kill(static_cast<pid_t>(m_pid), SIGKILL);
            int status = 0;
            while (::waitpid(static_cast<pid_t>(m_pid), &status, 0) < 0 && errno == EINTR) {}
        }
#endif
        m_pid = 0;
        closeOutput();
    }

private:
    void closeOutput()
    {
#ifdef _WIN32
        if (m_out) CloseHandle(m_out);
        m_out = nullptr;
#else
        if (m_out >= 0) ::close(m_out);
        m_out = -1;
#endif
    }

    std::int64_t m_pid = 0;
#ifdef _WIN32
    HANDLE m_process = nullptr;
    HANDLE m_in = nullptr;
    HANDLE m_out = nullptr;
#else
    int m_in = -1;
    int m_out = -1;
#endif
};

} // namespace logos_test

#endif // LOGOS_TEST_PLATFORM_H
