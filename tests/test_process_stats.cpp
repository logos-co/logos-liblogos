#include <gtest/gtest.h>
#include <process_stats/process_stats.h>
#include <nlohmann/json.hpp>
#include "test_platform.h"
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>

class ProcessStatsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ProcessStats::clearHistory();
    }

    void TearDown() override {
        ProcessStats::clearHistory();
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
    int64_t currentPid = logos_test::currentPid();

    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);

    EXPECT_GT(stats.memoryMB, 0.0);
    EXPECT_GE(stats.cpuTimeSeconds, 0.0);
}

TEST_F(ProcessStatsTest, GetProcessStats_MemoryIsNonNegative) {
    int64_t currentPid = logos_test::currentPid();

    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);

    EXPECT_GE(stats.memoryMB, 0.0);
}

TEST_F(ProcessStatsTest, GetProcessStats_CpuTimeIsNonNegative) {
    int64_t currentPid = logos_test::currentPid();

    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);

    EXPECT_GE(stats.cpuTimeSeconds, 0.0);
}

TEST_F(ProcessStatsTest, GetProcessStats_CpuPercentIsZeroOnFirstCall) {
    int64_t currentPid = logos_test::currentPid();

    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);

    EXPECT_EQ(stats.cpuPercent, 0.0);
}

TEST_F(ProcessStatsTest, GetProcessStats_CpuPercentUpdatesOnSecondCall) {
    int64_t currentPid = logos_test::currentPid();

    ProcessStats::getProcessStats(currentPid);

    volatile double sum = 0.0;
    for (int i = 0; i < 1000000; ++i) {
        sum += i * 0.1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);

    EXPECT_GE(stats.cpuPercent, 0.0);
}

// =============================================================================
// getModuleStats Tests
// =============================================================================

TEST_F(ProcessStatsTest, GetModuleStats_ReturnsEmptyArrayWhenNoModules) {
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
    // A real child that stays up, so there is a valid PID to measure.
    logos_test::Child child;
    ASSERT_TRUE(child.start(logos_test::fakeHostPath().string(), {"2"}))
        << "could not start " << logos_test::fakeHostPath();

    std::unordered_map<std::string, int64_t> processes;
    processes.emplace("test_module", child.pid());

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

    EXPECT_EQ(moduleObj["name"].get<std::string>(), "test_module");
    EXPECT_GE(moduleObj["cpu_percent"].get<double>(), 0.0);
    EXPECT_GE(moduleObj["cpu_time_seconds"].get<double>(), 0.0);
    EXPECT_GE(moduleObj["memory_mb"].get<double>(), 0.0);

    delete[] result;
}
