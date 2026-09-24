// Stand-in for logos_host_qt, the same executable on every platform, so the
// load path is tested the same way on Windows as on Unix.
//
// With --path, the first line of that file picks the behaviour (see
// fake_module_host.h), and `enter` and `report` marks go to host_events beside
// it. With LOGOS_TEST_PID_DIR set, it writes its pid to <dir>/<name>.pid,
// reports ok and stays up. It predates --inspect, as an old host does, unless
// LOGOS_TEST_INSPECT_LOG names a file to count inspections in.

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#ifdef __linux__
#include <csignal>
#include <sys/prctl.h>
#endif

namespace {

long long currentPid()
{
#ifdef _WIN32
    return static_cast<long long>(GetCurrentProcessId());
#else
    return static_cast<long long>(::getpid());
#endif
}

std::string dirOf(const std::string& path)
{
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

std::string moduleOf(const std::string& path)
{
    const auto slash = path.find_last_of("/\\");
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::string suffix = "_plugin.so";
    if (base.size() > suffix.size() && base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0)
        base.resize(base.size() - suffix.size());
    return base;
}

// One write to an append-only handle, so marks from concurrent hosts never
// interleave or overwrite each other.
void mark(const std::string& path, const char* what)
{
    if (path.empty()) return;
    const std::string line = std::string(what) + ' ' + moduleOf(path) + '\n';
    const std::string log = dirOf(path) + "/host_events";
#ifdef _WIN32
    HANDLE file = CreateFileA(log.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
#else
    const int fd = ::open(log.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) return;
    if (::write(fd, line.data(), line.size()) < 0) {}
    ::close(fd);
#endif
}

void report(const char* line)
{
    std::cout << line << std::endl;
}

// Until killed or `seconds` pass. The container asks a host to stop with
// SIGTERM on Unix, which ends this process, and with WM_QUIT to its main thread
// on Windows, which the real hosts' event loops end on; so does this one.
[[noreturn]] void stayUp(int seconds)
{
#ifdef _WIN32
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    for (auto now = std::chrono::steady_clock::now(); now < deadline;
         now = std::chrono::steady_clock::now()) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(left.count()),
                                    QS_ALLPOSTMESSAGE);
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            if (msg.message == WM_QUIT) std::exit(0);
    }
#else
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
#endif
    std::exit(0);
}

} // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    // A thread gets a message queue only once it asks for one; without it the
    // container's WM_QUIT cannot be posted at all.
    MSG queue;
    ::PeekMessageW(&queue, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
#endif
    const char* inspectLog = std::getenv("LOGOS_TEST_INSPECT_LOG");
    if (inspectLog && !*inspectLog) inspectLog = nullptr;
    std::string path, name, instance;
    int seconds = 300;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "-p" || arg == "--path") && i + 1 < argc) path = argv[++i];
        else if ((arg == "-n" || arg == "--name") && i + 1 < argc) name = argv[++i];
        else if (arg == "--instance-persistence-path" && i + 1 < argc) instance = argv[++i];
        else if (arg == "--help") {
            // Discovery reads a host's usage to learn whether it can inspect.
            std::cout << (inspectLog ? "usage: --inspect <plugin>"
                                     : "usage: logos_fake_module_host --name NAME --path PATH")
                      << std::endl;
            return 0;
        } else if (arg == "--inspect") {
            if (!inspectLog) {
                std::cerr << "The following argument was not expected: --inspect" << std::endl;
                return 109;
            }
            std::ofstream(inspectLog, std::ios::app) << "x\n";
            std::cout << R"({"name":")" << moduleOf(i + 1 < argc ? argv[i + 1] : "")
                      << R"(","version":"1.0.0"})" << std::endl;
            return 0;
        } else if (arg == "--dualstream") {
            std::cout << "out-line" << std::endl;
            std::cerr << "err-line" << std::endl;
            return 0;
        } else if (!arg.empty() && arg.find_first_not_of("0123456789") == std::string::npos) {
            seconds = std::atoi(arg.c_str());  // `logos_fake_module_host 5` is `sleep 5`
        }
    }

    if (const char* given = std::getenv("LOGOS_TEST_GIVEN_INSTANCE"); given && *given)
        std::ofstream(given) << instance << '\n';

    if (const char* pidDir = std::getenv("LOGOS_TEST_PID_DIR"); pidDir && *pidDir) {
        std::ofstream(std::string(pidDir) + "/" + name + ".pid") << currentPid() << '\n';
        report("@logos-load-status ok");
        stayUp(seconds);
    }

    mark(path, "enter");
    std::string first;
    if (!path.empty()) {
        std::ifstream module(path);
        std::getline(module, first);
        if (!first.empty() && first.back() == '\r') first.pop_back();
    }
    if (first == "die") return 3;
    if (first == "report-fail") {
        mark(path, "report");
        report("@logos-load-status failed undefined symbol: logos_module_install");
        return 1;
    }
    if (first == "report-ok-then-die") {
        mark(path, "report");
        report("@logos-load-status ok");
        return 0;
    }
    if (first == "slow-ok") std::this_thread::sleep_for(std::chrono::seconds(1));
#ifdef __linux__
    // As logos_host_qt does: die with the thread that started this process.
    if (first == "pdeathsig-ok") ::prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
    if (first == "report-ok" || first == "slow-ok" || first == "pdeathsig-ok") {
        mark(path, "report");
        report("@logos-load-status ok");
    }
    stayUp(seconds);
}
