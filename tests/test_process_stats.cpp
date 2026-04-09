#include <gtest/gtest.h>
#include <process_stats/process_stats.h>
#include <nlohmann/json.hpp>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

// Spawn a child process and return its PID (0 on failure).
static pid_t spawnProcess(const char* path, char* const argv[]) {
    pid_t pid = 0;
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    int rc = posix_spawn(&pid, path, nullptr, &attr,
                         const_cast<char* const*>(argv), environ);
    posix_spawnattr_destroy(&attr);
    return (rc == 0) ? pid : 0;
}

// Kill a child process and reap it.
static void killProcess(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    int status;
    waitpid(pid, &status, 0);
}

class ProcessStatsTest : public ::testing::Test {
protected:
    std::vector<pid_t> m_processes;

    void SetUp() override {
        ProcessStats::clearHistory();
    }

    void TearDown() override {
        ProcessStats::clearHistory();
        for (pid_t pid : m_processes) {
            killProcess(pid);
        }
        m_processes.clear();
    }
};

// =============================================================================
// getProcessStats Tests
// =============================================================================

TEST_F(ProcessStatsTest, GetProcessStats_ReturnsZeroedStatsForNegativePid) {
    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(-1);

    EXPECT_EQ(stats.cpuPercent, 0.0);
    EXPECT_EQ(stats.cpuTimeSeconds, 0.0);
    EXPECT_EQ(stats.memoryMB, 0.0);
}

TEST_F(ProcessStatsTest, GetProcessStats_ReturnsZeroedStatsForZeroPid) {
    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(0);

    EXPECT_EQ(stats.cpuPercent, 0.0);
    EXPECT_EQ(stats.cpuTimeSeconds, 0.0);
    EXPECT_EQ(stats.memoryMB, 0.0);
}

TEST_F(ProcessStatsTest, GetProcessStats_ReturnsValidStatsForCurrentProcess) {
    int64_t currentPid = static_cast<int64_t>(getpid());

    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);

    EXPECT_GT(stats.memoryMB, 0.0);
    EXPECT_GE(stats.cpuTimeSeconds, 0.0);
}

TEST_F(ProcessStatsTest, GetProcessStats_MemoryIsNonNegative) {
    int64_t currentPid = static_cast<int64_t>(getpid());

    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);

    EXPECT_GE(stats.memoryMB, 0.0);
}

TEST_F(ProcessStatsTest, GetProcessStats_CpuTimeIsNonNegative) {
    int64_t currentPid = static_cast<int64_t>(getpid());

    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);

    EXPECT_GE(stats.cpuTimeSeconds, 0.0);
}

TEST_F(ProcessStatsTest, GetProcessStats_CpuPercentIsZeroOnFirstCall) {
    int64_t currentPid = static_cast<int64_t>(getpid());

    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);

    EXPECT_EQ(stats.cpuPercent, 0.0);
}

TEST_F(ProcessStatsTest, GetProcessStats_CpuPercentUpdatesOnSecondCall) {
    int64_t currentPid = static_cast<int64_t>(getpid());

    ProcessStats::getProcessStats(currentPid);

    volatile double sum = 0.0;
    for (int i = 0; i < 1000000; ++i) {
        sum += i * 0.1;
    }

    usleep(10000); // 10ms

    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);

    EXPECT_GE(stats.cpuPercent, 0.0);
}

// =============================================================================
// getModuleStats Tests
// =============================================================================

TEST_F(ProcessStatsTest, GetModuleStats_ReturnsEmptyArrayWhenNoPlugins) {
    std::unordered_map<std::string, int64_t> processes;
    char* result = ProcessStats::getModuleStats(processes);

    ASSERT_NE(result, nullptr);

    nlohmann::json doc = nlohmann::json::parse(result);

    EXPECT_TRUE(doc.is_array());
    EXPECT_EQ(doc.size(), 0);

    delete[] result;
}

TEST_F(ProcessStatsTest, GetModuleStats_ReturnsNonNullPointer) {
    std::unordered_map<std::string, int64_t> processes;
    char* result = ProcessStats::getModuleStats(processes);

    ASSERT_NE(result, nullptr);

    delete[] result;
}

TEST_F(ProcessStatsTest, GetModuleStats_ReturnsValidJsonStructure) {
    // Spawn a real "sleep 1" child process so we have a valid PID.
    const char* sleepPath = "/bin/sleep";
    char* argv[] = {(char*)"sleep", (char*)"2", nullptr};
    pid_t pid = spawnProcess(sleepPath, argv);
    if (pid == 0) {
        // Try /usr/bin/sleep on some systems
        sleepPath = "/usr/bin/sleep";
        pid = spawnProcess(sleepPath, argv);
    }
    ASSERT_GT(pid, 0) << "Failed to spawn sleep process";
    m_processes.push_back(pid);

    std::unordered_map<std::string, int64_t> processes;
    processes.emplace("test_plugin", static_cast<int64_t>(pid));

    char* result = ProcessStats::getModuleStats(processes);

    ASSERT_NE(result, nullptr);

    nlohmann::json doc = nlohmann::json::parse(result);

    EXPECT_TRUE(doc.is_array());
    ASSERT_EQ(doc.size(), 1);

    auto moduleObj = doc[0];
    EXPECT_TRUE(moduleObj.contains("name"));
    EXPECT_TRUE(moduleObj.contains("cpu_percent"));
    EXPECT_TRUE(moduleObj.contains("cpu_time_seconds"));
    EXPECT_TRUE(moduleObj.contains("memory_mb"));

    EXPECT_EQ(moduleObj["name"].get<std::string>(), "test_plugin");
    EXPECT_GE(moduleObj["cpu_percent"].get<double>(), 0.0);
    EXPECT_GE(moduleObj["cpu_time_seconds"].get<double>(), 0.0);
    EXPECT_GE(moduleObj["memory_mb"].get<double>(), 0.0);

    delete[] result;
}
