// The app's side of a runtime in its own process (runtime_host.cpp is the
// other): it starts bin/logos_runtime, hands it the configuration, adopts the
// shell's credential it answers with, and serves the hooks it forwards.

#include "runtime_host.h"

#include "logos_core.h"
#include "logging/logos_log.h"
#include "bootstrap_policy.h"
#include "instance_id.h"
#include "module_manager.h"
#include "module_registry.h"
#include "core_service/embedded_core_service.h"
#include "core_service/shell_binding.h"

#include <logos_container/channel_process.h>
#include <logos_protocol.h>
#include <nlohmann/json.hpp>
#include <boost/dll/runtime_symbol_info.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr auto kReadyTimeout = std::chrono::seconds(120);
constexpr auto kStopTimeout = std::chrono::seconds(30);
constexpr auto kRequestTimeout = std::chrono::seconds(60);
// As many as core_service answers at once.
constexpr std::size_t kHookWorkers = 8;

#ifdef _WIN32
constexpr const char* kExecutable = "logos_runtime.exe";
#else
constexpr const char* kExecutable = "logos_runtime";
#endif

std::string dump(const json& value)
{
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string textOf(const json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

spdlog::logger& runtimeLog()
{
    return logos::logger("runtime");
}

// LOGOS_RUNTIME_PATH, else beside this program, the module hosts or the modules.
std::string findRuntime(const json& config, std::string& error)
{
    if (const char* configured = std::getenv("LOGOS_RUNTIME_PATH"); configured && *configured) {
        std::error_code ec;
        if (fs::exists(configured, ec)) return configured;
        error = std::string("LOGOS_RUNTIME_PATH names no file: ") + configured;
        return {};
    }
    std::vector<fs::path> dirs;
    try {
        dirs.emplace_back(boost::dll::program_location().parent_path().string());
    } catch (...) {
    }
    for (const char* host : {"LOGOS_HOST_PLAIN_PATH", "LOGOS_HOST_PATH"})
        if (const char* path = std::getenv(host); path && *path)
            dirs.push_back(fs::path(path).parent_path());
    for (const char* key : {"bundled_modules_dirs", "modules_dirs"}) {
        const auto list = config.find(key);
        if (list != config.end() && list->is_array() && !list->empty() && list->front().is_string())
            dirs.push_back(
                fs::absolute(fs::path(list->front().get<std::string>()) / ".." / "bin").lexically_normal());
    }
    for (const auto& dir : dirs) {
        std::error_code ec;
        const fs::path candidate = dir / kExecutable;
        if (fs::exists(candidate, ec)) return candidate.string();
    }
    error = std::string(kExecutable) + " not found: set LOGOS_RUNTIME_PATH or ship it beside the app";
    return {};
}

// A runtime log line, "[time] [level] text", into this process's log at its level.
void relayLog(const std::string& line)
{
    auto level = spdlog::level::info;
    std::string text = line;
    if (const auto time = line.find("] ["); !line.empty() && line.front() == '[' && time != std::string::npos) {
        const auto end = line.find(']', time + 3);
        if (end != std::string::npos) {
            const std::string name = line.substr(time + 3, end - time - 3);
            const auto parsed = spdlog::level::from_str(name);
            // from_str says off for any name it does not know, such as "out".
            const bool known = parsed != spdlog::level::off || name == "off";
            if (known) level = parsed;
            text = line.substr(known ? end + 1 : time + 2);
            if (!text.empty() && text.front() == ' ') text.erase(0, 1);
        }
    }
    runtimeLog().log(level, "{}", text);
}

} // namespace

namespace logos::runtime_host {

struct Runtime {
    enum class Phase { Starting, Ready, Failed, Stopping };

    std::mutex mutex;
    std::condition_variable changed;
    Phase phase = Phase::Starting;
    std::string failure;
    std::string credential; // until the binding adopts it
    bool exited = false;
    std::string exitReason;
    logos_runtime_exit_cb onExit = nullptr;
    void* onExitData = nullptr;
    bool exitReported = false;
    std::shared_ptr<LogosCore::ChannelProcess> process;

    // This side's calls, and the replies to them.
    long long nextCall = 1;
    std::set<long long> waiting;
    std::map<long long, json> replies;

    // The hooks the runtime forwards, and the threads that answer them.
    core_service::Hooks hooks;
    std::deque<json> work;
    std::vector<std::thread> workers;
    bool stopping = false;
};

namespace {

std::atomic<bool> g_spawned{false};

bool writeLine(Runtime& rt, const std::string& line)
{
    std::shared_ptr<LogosCore::ChannelProcess> process;
    {
        std::lock_guard<std::mutex> lock(rt.mutex);
        process = rt.process;
    }
    return process && process->writeLine(line);
}

void serveHooks(const std::shared_ptr<Runtime>& rt)
{
    for (;;) {
        json request;
        {
            std::unique_lock<std::mutex> lock(rt->mutex);
            rt->changed.wait(lock, [&] { return rt->stopping || !rt->work.empty(); });
            if (rt->stopping) return;
            request = std::move(rt->work.front());
            rt->work.pop_front();
        }
        const core_service::Hooks& hooks = rt->hooks;
        const std::string hook = textOf(request, "hook");
        if (hook == "shutdown") {
            if (hooks.shutdown) hooks.shutdown(hooks.shutdownData);
            continue;
        }
        char* answer = nullptr;
        if (hook == "extension" && hooks.extension) {
            const auto caller = request.find("caller");
            const auto args = request.find("args");
            answer = hooks.extension(caller != request.end() ? dump(*caller).c_str() : "{}",
                                     textOf(request, "method").c_str(),
                                     args != request.end() ? dump(*args).c_str() : "[]",
                                     hooks.extensionData);
        } else if (hook == "operator" && hooks.operators) {
            answer = hooks.operators(textOf(request, "token").c_str(),
                                     textOf(request, "transport").c_str(), hooks.operatorData);
        }
        json reply = {{"reply", request.value("call", json(0))}, {"text", nullptr}};
        if (answer) reply["text"] = std::string(answer);
        lp_string_free(answer);
        writeLine(*rt, dump(reply));
    }
}

// On the container's thread: never blocks.
void onLine(const std::shared_ptr<Runtime>& rt, const std::string& line)
{
    const json message = json::parse(line, nullptr, false);
    if (!message.is_object()) {
        // Never its text: a line on this channel may carry a credential.
        runtimeLog().warn("an unreadable line on the runtime's channel");
        return;
    }
    std::lock_guard<std::mutex> lock(rt->mutex);
    if (const auto ready = message.find("ready"); ready != message.end() && ready->is_object()) {
        if (rt->phase == Runtime::Phase::Starting) {
            rt->credential = textOf(*ready, "credential");
            rt->phase = Runtime::Phase::Ready;
        }
    } else if (message.contains("error")) {
        if (rt->phase == Runtime::Phase::Starting) {
            rt->failure = textOf(message, "error");
            rt->phase = Runtime::Phase::Failed;
        }
    } else if (const auto reply = message.find("reply"); reply != message.end()) {
        if (reply->is_number_integer() && rt->waiting.count(reply->get<long long>()))
            rt->replies[reply->get<long long>()] = message;
    } else if (message.contains("hook") && !rt->stopping) {
        rt->work.push_back(message);
        if (rt->workers.size() < kHookWorkers) rt->workers.emplace_back([rt] { serveHooks(rt); });
    }
    rt->changed.notify_all();
}

std::string exitReason(int code, bool crashed)
{
    if (!crashed) return "the runtime exited with code " + std::to_string(code);
#ifdef _WIN32
    return "the runtime crashed (status " + std::to_string(static_cast<unsigned long>(code)) + ")";
#else
    return "the runtime died on signal " + std::to_string(code);
#endif
}

void onExit(const std::shared_ptr<Runtime>& rt, int code, bool crashed)
{
    logos_runtime_exit_cb callback = nullptr;
    void* data = nullptr;
    const std::string reason = exitReason(code, crashed);
    bool expected = false;
    {
        std::lock_guard<std::mutex> lock(rt->mutex);
        rt->exited = true;
        rt->exitReason = reason;
        expected = rt->phase == Runtime::Phase::Stopping;
        if (rt->phase == Runtime::Phase::Ready && rt->onExit && !rt->exitReported) {
            rt->exitReported = true;
            callback = rt->onExit;
            data = rt->onExitData;
        }
        rt->changed.notify_all();
    }
    if (!expected) runtimeLog().error("{}", reason);
    if (callback) callback(reason.c_str(), data);
}

// EOF on its stdin, time to unload its modules in order, then force it.
void stopProcess(const std::shared_ptr<Runtime>& rt)
{
    std::shared_ptr<LogosCore::ChannelProcess> process;
    {
        std::lock_guard<std::mutex> lock(rt->mutex);
        rt->phase = Runtime::Phase::Stopping;
        process = std::move(rt->process);
        rt->changed.notify_all();
    }
    if (!process) return;
    process->closeInput();
    {
        std::unique_lock<std::mutex> lock(rt->mutex);
        if (!rt->changed.wait_for(lock, kStopTimeout, [&] { return rt->exited; }))
            runtimeLog().warn("the runtime did not stop within {} s; ending it", kStopTimeout.count());
    }
    process->terminate();
}

void stopWorkers(const std::shared_ptr<Runtime>& rt)
{
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(rt->mutex);
        rt->stopping = true;
        rt->work.clear();
        workers.swap(rt->workers);
        rt->changed.notify_all();
    }
    for (auto& worker : workers) {
        // A hook that stops the runtime itself runs on one of these.
        if (worker.get_id() == std::this_thread::get_id()) worker.detach();
        else worker.join();
    }
}

std::optional<json> call(const std::shared_ptr<Runtime>& rt, json request)
{
    long long id = 0;
    {
        std::lock_guard<std::mutex> lock(rt->mutex);
        if (rt->exited || rt->phase != Runtime::Phase::Ready) return std::nullopt;
        id = rt->nextCall++;
        rt->waiting.insert(id);
    }
    request["call"] = id;
    const bool sent = writeLine(*rt, dump(request));
    std::unique_lock<std::mutex> lock(rt->mutex);
    if (sent)
        rt->changed.wait_for(lock, kRequestTimeout,
                             [&] { return rt->exited || rt->replies.count(id) > 0; });
    rt->waiting.erase(id);
    const auto it = rt->replies.find(id);
    if (it == rt->replies.end()) return std::nullopt;
    json reply = std::move(it->second);
    rt->replies.erase(it);
    return reply;
}

} // namespace

bool spawned()
{
    return g_spawned.load();
}

} // namespace logos::runtime_host

struct logos_runtime {
    std::shared_ptr<logos::runtime_host::Runtime> state;
    logos_consumer* binding = nullptr;
};

extern "C" {

logos_runtime* logos_runtime_spawn(const char* config_json, char** out_error)
{
    using logos::runtime_host::Runtime;
    if (out_error) *out_error = nullptr;
    logos::initLogging();
    auto fail = [&](const std::string& why) -> logos_runtime* {
        runtimeLog().error("logos_runtime_spawn: {}", why);
        if (out_error) *out_error = lp_string_copy(why.c_str());
        return nullptr;
    };
    json config = json::parse(config_json ? config_json : "", nullptr, false);
    if (!config.is_object()) return fail("the configuration is not a JSON object");
    const std::string shell = textOf(config, "shell");
    if (!logos::isValidModuleName(shell) || logos::bootstrap::rowFor(shell))
        return fail("'" + shell + "' is not a shell name");
    if (ModuleManager::started()) return fail("this process already runs a runtime itself");
    if (logos::runtime_host::g_spawned.exchange(true))
        return fail("this process already spawned a runtime");
    auto failSpawn = [&](const std::string& why) {
        logos::runtime_host::g_spawned = false;
        return fail(why);
    };
    std::string missing;
    const std::string executable = findRuntime(config, missing);
    if (executable.empty()) return failSpawn(missing);

    // The runtime inherits it, so both sides name the same sockets.
    logos::ensureInstanceId();
    auto rt = std::make_shared<Runtime>();
    rt->hooks = logos::core_service::hooks();
    json hooks = json::object();
    if (rt->hooks.extension)
        hooks["extension"] = json::parse(rt->hooks.extensionMethods, nullptr, false);
    if (rt->hooks.operators) hooks["operator_resolver"] = true;
    if (rt->hooks.shutdown) hooks["shutdown"] = true;
    if (!hooks.empty()) config["hooks"] = hooks;

    std::weak_ptr<Runtime> weak = rt;
    LogosCore::ChannelCallbacks callbacks;
    callbacks.onLine = [weak](const std::string& line) {
        if (auto state = weak.lock()) logos::runtime_host::onLine(state, line);
    };
    callbacks.onLog = &relayLog;
    callbacks.onExit = [weak](int code, bool crashed) {
        if (auto state = weak.lock()) logos::runtime_host::onExit(state, code, crashed);
    };
    std::shared_ptr<LogosCore::ChannelProcess> process =
        LogosCore::startChannelProcess(executable, {}, callbacks);
    if (!process) return failSpawn("could not start " + executable);
    const int64_t pid = process->pid();
    {
        std::lock_guard<std::mutex> lock(rt->mutex);
        rt->process = process;
    }

    std::string why;
    if (!logos::runtime_host::writeLine(*rt, dump(config))) {
        why = "could not hand the runtime its configuration";
    } else {
        std::unique_lock<std::mutex> lock(rt->mutex);
        rt->changed.wait_for(lock, kReadyTimeout,
                             [&] { return rt->phase != Runtime::Phase::Starting || rt->exited; });
        // Its last words may still be on the pipe when its exit is seen.
        if (rt->phase == Runtime::Phase::Starting && rt->exited)
            rt->changed.wait_for(lock, std::chrono::seconds(1),
                                 [&] { return rt->phase != Runtime::Phase::Starting; });
        if (rt->phase == Runtime::Phase::Failed) why = rt->failure;
        else if (rt->phase == Runtime::Phase::Starting)
            why = rt->exited ? rt->exitReason
                             : "the runtime was not ready within "
                                   + std::to_string(kReadyTimeout.count()) + " s";
    }
    logos_consumer* binding = nullptr;
    if (why.empty()) {
        std::string credential;
        {
            std::lock_guard<std::mutex> lock(rt->mutex);
            credential.swap(rt->credential);
        }
        binding = logos::shell_binding::adopt(shell, credential);
        if (!binding) why = "could not adopt the credential of its shell '" + shell + "'";
    }
    if (!why.empty()) {
        logos::runtime_host::stopProcess(rt);
        logos::runtime_host::stopWorkers(rt);
        return failSpawn(why);
    }
    runtimeLog().info("{} runs the runtime for '{}' (pid {})", executable, shell, pid);
    return new logos_runtime{rt, binding};
}

logos_consumer* logos_runtime_binding(logos_runtime* runtime)
{
    return runtime ? runtime->binding : nullptr;
}

char* logos_runtime_process_module(logos_runtime* runtime, const char* module_path)
{
    if (!runtime || !module_path || !*module_path) return nullptr;
    const auto reply = logos::runtime_host::call(
        runtime->state, {{"op", "process_module"}, {"path", module_path}});
    const std::string name = reply ? textOf(*reply, "text") : std::string{};
    return name.empty() ? nullptr : lp_string_copy(name.c_str());
}

void logos_runtime_on_exit(logos_runtime* runtime, logos_runtime_exit_cb cb, void* user_data)
{
    if (!runtime) return;
    auto& rt = *runtime->state;
    std::string reason;
    {
        std::lock_guard<std::mutex> lock(rt.mutex);
        rt.onExit = cb;
        rt.onExitData = user_data;
        // It may have gone before anyone asked.
        if (!cb || !rt.exited || rt.exitReported
            || rt.phase != logos::runtime_host::Runtime::Phase::Ready)
            return;
        rt.exitReported = true;
        reason = rt.exitReason;
    }
    cb(reason.c_str(), user_data);
}

void logos_runtime_stop(logos_runtime* runtime)
{
    if (!runtime) return;
    logos::shell_binding::release(runtime->binding);
    logos::runtime_host::stopProcess(runtime->state);
    logos::runtime_host::stopWorkers(runtime->state);
    delete runtime;
    logos::runtime_host::g_spawned = false;
}

} // extern "C"
