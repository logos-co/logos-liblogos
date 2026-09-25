// logos_runtime: the runtime in a process of its own. It holds the token
// authority and every module's credential; the app that spawned it
// (runtime_spawn.cpp) reaches it only through module calls, as its shell.

#include "runtime_host.h"

#include "logos_core.h"
#include "logging/logos_log.h"

#include <logos_protocol.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif

namespace {

using json = nlohmann::json;

// Presented tokens are untrusted text, and may not be UTF-8.
std::string dump(const json& value)
{
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string textOf(const json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

long long numberOf(const json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_number_integer() ? it->get<long long>() : 0;
}

json parsedOr(const char* text, json fallback)
{
    if (!text) return fallback;
    json value = json::parse(text, nullptr, false);
    return value.is_discarded() ? fallback : value;
}

long long currentPid()
{
#ifdef _WIN32
    return ::_getpid();
#else
    return ::getpid();
#endif
}

// ── stopping ──────────────────────────────────────────────────────────────────
// Every stop request (EOF from the app, a signal, WM_QUIT, the app's death)
// wakes the main thread once, and it then unloads the modules in order.

#ifdef _WIN32
DWORD g_mainThread = 0;

void prepareStop()
{
    // The queue first: the container's WM_QUIT cannot land before it exists.
    MSG message;
    ::PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    g_mainThread = ::GetCurrentThreadId();
}

void requestStop()
{
    ::PostThreadMessageW(g_mainThread, WM_QUIT, 0, 0);
}

void waitForStop()
{
    MSG message;
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
}
#else
int g_stopPipe[2] = {-1, -1};

void requestStop()
{
    const char byte = 1;
    (void)!::write(g_stopPipe[1], &byte, 1); // async-signal-safe
}

void onStopSignal(int)
{
    requestStop();
}

void prepareStop()
{
    if (::pipe(g_stopPipe) == 0) {
        for (int fd : g_stopPipe) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        ::fcntl(g_stopPipe[1], F_SETFL, O_NONBLOCK);
    }
    struct sigaction action {};
    action.sa_handler = &onStopSignal;
    sigemptyset(&action.sa_mask); // a macro on macOS
    for (int signal : {SIGTERM, SIGINT, SIGHUP}) ::sigaction(signal, &action, nullptr);
    // A pipe the app closed is an error to handle, not a fatal signal.
    ::signal(SIGPIPE, SIG_IGN);
}

void waitForStop()
{
    char byte = 0;
    while (::read(g_stopPipe[0], &byte, 1) < 0 && errno == EINTR) {
    }
}
#endif

// Stops with the app, so a crashed app leaves no runtime, and no module, behind.
void followParent()
{
#ifndef _WIN32
    // Its own session: the terminal's signals reach only the app, which then
    // stops this process in order.
    if (::setsid() == -1 && errno != EPERM) ::setpgid(0, 0);
    const pid_t parent = ::getppid();
#ifdef __linux__
    // Fires when the spawning thread exits; the container spawns from one that
    // lives as long as the app.
    ::prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
    if (::getppid() != parent) {
        requestStop();
        return;
    }
    std::thread([parent] {
        while (::getppid() == parent) ::sleep(1);
        requestStop();
    }).detach();
#endif
    // On Windows the app's container holds this process in a kill-on-close job.
}

// A teardown that hangs must not outlive the app: the module hosts follow this process.
void armStopDeadline()
{
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(60));
        std::fputs("logos_runtime: its modules took over 60 s to stop; exiting\n", stderr);
        std::_Exit(3);
    }).detach();
}

// ── the channel to the app ────────────────────────────────────────────────────

// Its stdin and stdout, taken over before anything else can use them: whatever
// else writes to stdout (module output relayed here, say) reaches stderr instead.
class Channel {
public:
    bool open()
    {
#ifdef _WIN32
        HANDLE self = ::GetCurrentProcess();
        if (!::DuplicateHandle(self, ::GetStdHandle(STD_INPUT_HANDLE), self, &m_in, 0, FALSE,
                               DUPLICATE_SAME_ACCESS)
            || !::DuplicateHandle(self, ::GetStdHandle(STD_OUTPUT_HANDLE), self, &m_out, 0, FALSE,
                                  DUPLICATE_SAME_ACCESS))
            return false;
        const int null = ::_open("NUL", _O_RDONLY);
        if (null >= 0) {
            ::_dup2(null, 0);
            ::_close(null);
        }
        ::_dup2(2, 1);
        ::SetStdHandle(STD_INPUT_HANDLE, reinterpret_cast<HANDLE>(::_get_osfhandle(0)));
        ::SetStdHandle(STD_OUTPUT_HANDLE, ::GetStdHandle(STD_ERROR_HANDLE));
        return true;
#else
        m_in = ::fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
        m_out = ::fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
        if (m_in < 0 || m_out < 0) return false;
        const int null = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (null >= 0) {
            ::dup2(null, STDIN_FILENO);
            ::close(null);
        }
        ::dup2(STDERR_FILENO, STDOUT_FILENO);
        return true;
#endif
    }

    // One reader at a time: the main thread for the configuration, then the reader.
    bool readLine(std::string& line)
    {
        for (;;) {
            if (const auto end = m_buffer.find('\n'); end != std::string::npos) {
                line = m_buffer.substr(0, end);
                m_buffer.erase(0, end + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return true;
            }
            char chunk[4096];
#ifdef _WIN32
            DWORD n = 0;
            if (!::ReadFile(m_in, chunk, sizeof chunk, &n, nullptr) || n == 0) return false;
#else
            const ssize_t n = ::read(m_in, chunk, sizeof chunk);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
#endif
            m_buffer.append(chunk, static_cast<std::size_t>(n));
        }
    }

    bool send(const json& message)
    {
        std::string text = dump(message);
        text.push_back('\n');
        std::lock_guard<std::mutex> lock(m_writeMutex);
        const char* data = text.data();
        std::size_t left = text.size();
        while (left > 0) {
#ifdef _WIN32
            DWORD n = 0;
            if (!::WriteFile(m_out, data, static_cast<DWORD>(left), &n, nullptr)) return false;
#else
            const ssize_t n = ::write(m_out, data, left);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
#endif
            data += n;
            left -= static_cast<std::size_t>(n);
        }
        return true;
    }

    // A hook call to the app, and its reply; nullopt once the channel closed or
    // `timeout` passed (none: until the app answers or goes).
    std::optional<json> request(json message, std::optional<std::chrono::milliseconds> timeout)
    {
        long long id = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_closed) return std::nullopt;
            id = m_next++;
            m_waiting.insert(id);
        }
        message["call"] = id;
        const bool sent = send(message);
        std::unique_lock<std::mutex> lock(m_mutex);
        const auto answered = [&] { return m_closed || m_replies.count(id) > 0; };
        if (sent) {
            if (timeout) m_changed.wait_for(lock, *timeout, answered);
            else m_changed.wait(lock, answered);
        }
        m_waiting.erase(id);
        const auto it = m_replies.find(id);
        if (it == m_replies.end()) return std::nullopt;
        json reply = std::move(it->second);
        m_replies.erase(it);
        return reply;
    }

    void deliver(const json& reply)
    {
        const long long id = numberOf(reply, "reply");
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_waiting.count(id)) return; // its caller gave up
        m_replies[id] = reply;
        m_changed.notify_all();
    }

    void close()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
        m_changed.notify_all();
    }

private:
#ifdef _WIN32
    HANDLE m_in = INVALID_HANDLE_VALUE;
    HANDLE m_out = INVALID_HANDLE_VALUE;
#else
    int m_in = -1;
    int m_out = -1;
#endif
    std::string m_buffer;
    std::mutex m_writeMutex;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::set<long long> m_waiting;
    std::map<long long, json> m_replies;
    long long m_next = 1;
    bool m_closed = false;
};

Channel* g_channel = nullptr;

// The app's own requests, one at a time and off the reader thread.
class Requests {
public:
    explicit Requests(Channel& channel) : m_channel(channel)
    {
        std::thread([this] { run(); }).detach();
    }

    void push(json request)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(std::move(request));
        m_changed.notify_one();
    }

    void stop()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopped = true;
        m_changed.notify_one();
    }

private:
    void run()
    {
        for (;;) {
            json request;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_changed.wait(lock, [&] { return m_stopped || !m_queue.empty(); });
                if (m_stopped) return;
                request = std::move(m_queue.front());
                m_queue.pop_front();
            }
            json reply = {{"reply", numberOf(request, "call")}, {"text", nullptr}};
            // The embedder's alone, as in-process: never a core_service method.
            if (textOf(request, "op") == "process_module") {
                const std::string path = textOf(request, "path");
                char* name = path.empty() ? nullptr : logos_core_process_module(path.c_str());
                if (name) reply["text"] = std::string(name);
                delete[] name;
            }
            m_channel.send(reply);
        }
    }

    Channel& m_channel;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<json> m_queue;
    bool m_stopped = false;
};

// ── the embedder's hooks, forwarded to the app ────────────────────────────────

char* unavailable(const std::string& message)
{
    return lp_string_copy(
        dump(json{{"status", "error"}, {"code", "UNAVAILABLE"}, {"message", message}}).c_str());
}

// Holds one of core_service's workers until the app answers.
char* forwardExtension(const char* callerJson, const char* method, const char* argsJson, void*)
{
    const auto reply = g_channel->request({{"hook", "extension"},
                                           {"caller", parsedOr(callerJson, json::object())},
                                           {"method", method ? method : ""},
                                           {"args", parsedOr(argsJson, json::array())}},
                                          std::nullopt);
    if (!reply) return unavailable("the shell that answers this method is gone");
    const auto text = reply->find("text");
    return text != reply->end() && text->is_string()
        ? lp_string_copy(text->get<std::string>().c_str())
        : nullptr;
}

char* forwardOperator(const char* token, const char* transport, void*)
{
    const auto reply = g_channel->request({{"hook", "operator"},
                                           {"token", token ? token : ""},
                                           {"transport", transport ? transport : ""}},
                                          std::chrono::seconds(10));
    const std::string name = reply ? textOf(*reply, "text") : std::string{};
    return name.empty() ? nullptr : lp_string_copy(name.c_str());
}

void forwardShutdown(void*)
{
    g_channel->send({{"hook", "shutdown"}});
}

// ── configuration ─────────────────────────────────────────────────────────────

// A document the setters take as text, given inline or as that text.
std::string documentText(const json& value)
{
    return value.is_string() ? value.get<std::string>() : value.dump();
}

bool paths(const json& config, const char* key, std::vector<std::string>& out)
{
    const auto it = config.find(key);
    if (it == config.end()) return true;
    if (!it->is_array()) return false;
    for (const auto& value : *it) {
        if (!value.is_string()) return false;
        out.push_back(value.get<std::string>());
    }
    return true;
}

// What the app would have passed to the setters itself.
bool configure(const json& config, std::string& error)
{
    if (!config.is_object()) {
        error = "its configuration is not a JSON object";
        return false;
    }
    const std::string shell = textOf(config, "shell");
    if (logos_core_set_shell_identity(shell.c_str()) != 0) {
        error = "'" + shell + "' is not a shell name";
        return false;
    }
    std::vector<std::string> dirs;
    std::vector<std::string> bundled;
    if (!paths(config, "modules_dirs", dirs) || !paths(config, "bundled_modules_dirs", bundled)) {
        error = "modules_dirs and bundled_modules_dirs are lists of paths";
        return false;
    }
    for (const auto& dir : dirs) logos_core_add_modules_dir(dir.c_str());
    if (config.contains("bundled_modules_dirs")) {
        std::vector<const char*> list;
        for (const auto& dir : bundled) list.push_back(dir.c_str());
        list.push_back(nullptr);
        if (logos_core_set_bundled_modules_dirs(list.data()) != 0) {
            error = "its bundled_modules_dirs were refused";
            return false;
        }
    }
    if (const auto it = config.find("persistence_base_path"); it != config.end()) {
        if (!it->is_string()) {
            error = "persistence_base_path is a path";
            return false;
        }
        logos_core_set_persistence_base_path(it->get_ref<const std::string&>().c_str());
    }
    if (const auto it = config.find("module_transports"); it != config.end()) {
        if (!it->is_object()) {
            error = "module_transports maps module names to transport sets";
            return false;
        }
        for (const auto& [name, set] : it->items())
            logos_core_set_module_transports(name.c_str(), documentText(set).c_str());
    }
    if (const auto it = config.find("access_policy"); it != config.end())
        logos_core_set_access_policy(documentText(*it).c_str());
    struct Setter {
        const char* key;
        int (*apply)(const char*);
    };
    for (const Setter& setter : {Setter{"placement_policy", &logos_core_set_placement_policy},
                                 Setter{"package_config", &logos_core_set_package_config},
                                 Setter{"core_service_transports",
                                        &logos_core_set_core_service_transports}}) {
        const auto it = config.find(setter.key);
        if (it != config.end() && setter.apply(documentText(*it).c_str()) != 0) {
            error = std::string("its ") + setter.key + " was refused";
            return false;
        }
    }
    return true;
}

// Only the hooks the app serves; without one, core_service refuses as it would in-process.
bool installHooks(const json& config, std::string& error)
{
    const auto hooks = config.find("hooks");
    if (hooks == config.end()) return true;
    if (!hooks->is_object()) {
        error = "hooks is an object";
        return false;
    }
    if (const auto methods = hooks->find("extension"); methods != hooks->end()) {
        if (!methods->is_array()
            || logos_core_set_core_service_extension(&forwardExtension, methods->dump().c_str(),
                                                     nullptr) != 0) {
            error = "hooks.extension lists methods as core_service.getMethods does";
            return false;
        }
    }
    if (const auto it = hooks->find("operator_resolver"); it != hooks->end() && it->is_boolean()
        && it->get<bool>())
        logos_core_set_operator_resolver(&forwardOperator, nullptr);
    if (const auto it = hooks->find("shutdown"); it != hooks->end() && it->is_boolean()
        && it->get<bool>())
        logos_core_set_shutdown_handler(&forwardShutdown, nullptr);
    return true;
}

int run()
{
    prepareStop();
    // Never freed: its reader may still be blocked on the pipe when this exits.
    auto* channel = new Channel;
    if (!channel->open()) {
        std::fputs("logos_runtime: stdin and stdout must be the app's pipes\n", stderr);
        return 1;
    }
    g_channel = channel;
    followParent();
    logos::initLogging();

    std::string line;
    if (!channel->readLine(line)) {
        spdlog::error("logos_runtime: its app sent no configuration");
        return 1;
    }
    std::string error;
    const json config = json::parse(line, nullptr, false);
    if (!configure(config, error) || !installHooks(config, error)) {
        spdlog::error("logos_runtime: {}", error);
        channel->send({{"error", error}});
        return 1;
    }

    auto* requests = new Requests(*channel);
    std::thread([channel, requests] {
        std::string text;
        while (channel->readLine(text)) {
            const json message = json::parse(text, nullptr, false);
            if (!message.is_object()) continue;
            if (message.contains("reply")) channel->deliver(message);
            else if (message.contains("op")) requests->push(message);
        }
        // The app closed it, or is gone.
        channel->close();
        requestStop();
    }).detach();

    logos_core_start();
    logos_consumer* shell = logos_core_take_shell_binding();
    if (!shell) {
        error = "there is no token authority, so it could not admit its shell (its log says why)";
        spdlog::error("logos_runtime: {}", error);
        channel->send({{"error", error}});
        logos_core_cleanup();
        return 1;
    }
    char* credential = logos_consumer_credential(shell);
    const char* instance = std::getenv("LOGOS_INSTANCE_ID");
    const bool ready = channel->send({{"ready",
                                       {{"instance", instance ? instance : ""},
                                        {"credential", credential ? credential : ""},
                                        {"pid", currentPid()}}}});
    logos_consumer_string_free(credential);
    if (ready)
        spdlog::info("logos_runtime is ready for its shell '{}'", logos_consumer_name(shell));
    else
        requestStop();

    waitForStop();
    armStopDeadline();
    spdlog::info("logos_runtime is stopping");
    requests->stop();
    logos_consumer_release(shell);
    logos_core_cleanup();
    return 0;
}

} // namespace

int logos_runtime_host_main(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::puts("logos_runtime: the Logos runtime in a process of its own.\n"
                      "An app starts it (logos_runtime_spawn) and hands it its configuration "
                      "on stdin.");
            return 0;
        }
    }
    const int code = run();
    std::fflush(nullptr);
    // Its reader may still be blocked on the pipe, and nothing is left to stop.
    std::_Exit(code);
}
