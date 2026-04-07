#include <gtest/gtest.h>
#include "process_stats.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <csignal>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>

class ProcessStatsTest : public ::testing::Test {
protected:
    std::vector<pid_t> m_childPids;

    void SetUp() override
    {
        ProcessStats::clearHistory();
    }

    void TearDown() override
    {
        ProcessStats::clearHistory();
        for (pid_t pid : m_childPids) {
            if (pid > 0) {
                kill(pid, SIGTERM);
                int st = 0;
                waitpid(pid, &st, 0);
            }
        }
        m_childPids.clear();
    }
};

TEST_F(ProcessStatsTest, GetProcessStats_ReturnsZeroedStatsForNegativePid)
{
    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(-1);

    EXPECT_EQ(stats.cpuPercent, 0.0);
    EXPECT_EQ(stats.cpuTimeSeconds, 0.0);
    EXPECT_EQ(stats.memoryMB, 0.0);
}

TEST_F(ProcessStatsTest, GetProcessStats_ReturnsZeroedStatsForZeroPid)
{
    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(0);

    EXPECT_EQ(stats.cpuPercent, 0.0);
    EXPECT_EQ(stats.cpuTimeSeconds, 0.0);
    EXPECT_EQ(stats.memoryMB, 0.0);
}

TEST_F(ProcessStatsTest, GetProcessStats_ReturnsValidStatsForCurrentProcess)
{
    int64_t currentPid = static_cast<int64_t>(getpid());

    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);

    EXPECT_GT(stats.memoryMB, 0.0);
    EXPECT_GE(stats.cpuTimeSeconds, 0.0);
}

TEST_F(ProcessStatsTest, GetProcessStats_MemoryIsNonNegative)
{
    int64_t currentPid = static_cast<int64_t>(getpid());

    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);

    EXPECT_GE(stats.memoryMB, 0.0);
}

TEST_F(ProcessStatsTest, GetProcessStats_CpuTimeIsNonNegative)
{
    int64_t currentPid = static_cast<int64_t>(getpid());

    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);

    EXPECT_GE(stats.cpuTimeSeconds, 0.0);
}

TEST_F(ProcessStatsTest, GetProcessStats_CpuPercentIsZeroOnFirstCall)
{
    int64_t currentPid = static_cast<int64_t>(getpid());

    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);

    EXPECT_EQ(stats.cpuPercent, 0.0);
}

TEST_F(ProcessStatsTest, GetProcessStats_CpuPercentUpdatesOnSecondCall)
{
    int64_t currentPid = static_cast<int64_t>(getpid());

    ProcessStats::getProcessStats(currentPid);

    volatile double sum = 0.0;
    for (int i = 0; i < 1000000; ++i) {
        sum += i * 0.1;
    }

    usleep(10000);

    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);

    EXPECT_GE(stats.cpuPercent, 0.0);
}

TEST_F(ProcessStatsTest, GetModuleStats_ReturnsEmptyArrayWhenNoPlugins)
{
    std::unordered_map<std::string, int64_t> processes;
    char* result = ProcessStats::getModuleStats(processes);

    ASSERT_NE(result, nullptr);

    nlohmann::json doc = nlohmann::json::parse(result);

    EXPECT_TRUE(doc.is_array());
    EXPECT_EQ(doc.size(), 0u);

    delete[] result;
}

TEST_F(ProcessStatsTest, GetModuleStats_ReturnsNonNullPointer)
{
    std::unordered_map<std::string, int64_t> processes;
    char* result = ProcessStats::getModuleStats(processes);

    ASSERT_NE(result, nullptr);

    delete[] result;
}

TEST_F(ProcessStatsTest, GetModuleStats_ReturnsValidJsonStructure)
{
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        execlp("sleep", "sleep", "2", nullptr);
        _exit(127);
    }
    m_childPids.push_back(pid);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::unordered_map<std::string, int64_t> processes;
    processes["test_plugin"] = static_cast<int64_t>(pid);

    char* result = ProcessStats::getModuleStats(processes);

    ASSERT_NE(result, nullptr);

    nlohmann::json doc = nlohmann::json::parse(result);

    EXPECT_TRUE(doc.is_array());
    ASSERT_EQ(doc.size(), 1u);

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
