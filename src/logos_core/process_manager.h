#ifndef PROCESS_MANAGER_H
#define PROCESS_MANAGER_H

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <cstdint>

namespace QtProcessManager {

    struct ProcessCallbacks {
        std::function<void(const std::string& name, int exitCode, bool crashed)> onFinished;
        std::function<void(const std::string& name, bool crashed)> onError;
        std::function<void(const std::string& name, const std::string& line, bool isStderr)> onOutput;
    };

    bool startProcess(const std::string& name, const std::string& executable,
                      const std::vector<std::string>& arguments, const ProcessCallbacks& callbacks);
    bool sendToken(const std::string& name, const std::string& token);
    void terminateProcess(const std::string& name);
    void terminateAll();
    bool hasProcess(const std::string& name);
    int64_t getProcessId(const std::string& name);
    std::unordered_map<std::string, int64_t> getAllProcessIds();
    void clearAll();
    void registerProcess(const std::string& name);
}

#endif // PROCESS_MANAGER_H
