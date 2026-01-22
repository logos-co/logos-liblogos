#include <gtest/gtest.h>
#include "process_stats.h"
#include "logos_core_internal.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <cstring>
#include <unistd.h>

// Test fixture for process stats tests
class ProcessStatsTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Clear previous CPU times to ensure test isolation
        g_previous_cpu_times.clear();
        
        // Clear plugin processes
        g_plugin_processes.clear();
    }
    
    void TearDown() override {
        // Clean up previous CPU times
        g_previous_cpu_times.clear();
        
        // Clean up plugin processes
        for (auto it = g_plugin_processes.begin(); it != g_plugin_processes.end(); ++it) {
            QProcess* process = it.value();
            if (process) {
                process->terminate();
                process->waitForFinished(1000);
                delete process;
            }
        }
        g_plugin_processes.clear();
    }
};

// =============================================================================
// getProcessStats Tests
// =============================================================================

// Verifies that getProcessStats() returns zeroed stats for negative PID
TEST_F(ProcessStatsTest, GetProcessStats_ReturnsZeroedStatsForNegativePid) {
    ProcessStatsData stats = ProcessStats::getProcessStats(-1);
    
    EXPECT_EQ(stats.cpuPercent, 0.0);
    EXPECT_EQ(stats.cpuTimeSeconds, 0.0);
    EXPECT_EQ(stats.memoryMB, 0.0);
}

// Verifies that getProcessStats() returns zeroed stats for zero PID
TEST_F(ProcessStatsTest, GetProcessStats_ReturnsZeroedStatsForZeroPid) {
    ProcessStatsData stats = ProcessStats::getProcessStats(0);
    
    EXPECT_EQ(stats.cpuPercent, 0.0);
    EXPECT_EQ(stats.cpuTimeSeconds, 0.0);
    EXPECT_EQ(stats.memoryMB, 0.0);
}

// Verifies that getProcessStats() returns valid stats for the current process
TEST_F(ProcessStatsTest, GetProcessStats_ReturnsValidStatsForCurrentProcess) {
    qint64 currentPid = getpid();
    
    ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);
    
    // We can't predict exact values, but we can verify the structure is populated
    // On supported platforms (macOS, Linux), at least some stats should be non-zero
    // Memory should be greater than 0 for a running process
    EXPECT_GT(stats.memoryMB, 0.0);
    // CPU time should be non-negative
    EXPECT_GE(stats.cpuTimeSeconds, 0.0);
}

// Verifies that memory usage is non-negative for a valid process
TEST_F(ProcessStatsTest, GetProcessStats_MemoryIsNonNegative) {
    qint64 currentPid = getpid();
    
    ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);
    
    EXPECT_GE(stats.memoryMB, 0.0);
}

// Verifies that CPU time is non-negative for a valid process
TEST_F(ProcessStatsTest, GetProcessStats_CpuTimeIsNonNegative) {
    qint64 currentPid = getpid();
    
    ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);
    
    EXPECT_GE(stats.cpuTimeSeconds, 0.0);
}

// Verifies that CPU percent is zero on first call (no previous data)
TEST_F(ProcessStatsTest, GetProcessStats_CpuPercentIsZeroOnFirstCall) {
    qint64 currentPid = getpid();
    
    // Ensure no previous data exists
    g_previous_cpu_times.remove(currentPid);
    
    ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);
    
    // First call should have 0% CPU since there's no previous measurement
    EXPECT_EQ(stats.cpuPercent, 0.0);
}

// Verifies that CPU percent is calculated after the initial call
TEST_F(ProcessStatsTest, GetProcessStats_CpuPercentUpdatesOnSecondCall) {
    qint64 currentPid = getpid();
    
    // First call to establish baseline
    ProcessStats::getProcessStats(currentPid);
    
    // Do some work to use CPU time
    volatile double sum = 0.0;
    for (int i = 0; i < 1000000; ++i) {
        sum += i * 0.1;
    }
    
    // Small delay to ensure time passes
    usleep(10000); // 10ms
    
    // Second call should potentially have non-zero CPU percent
    ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);
    
    // CPU percent should be non-negative (might be 0 if work was too fast)
    EXPECT_GE(stats.cpuPercent, 0.0);
    
    // Verify that previous CPU times are being tracked
    EXPECT_TRUE(g_previous_cpu_times.contains(currentPid));
}

// =============================================================================
// getModuleStats Tests
// =============================================================================

// Verifies that getModuleStats() returns an empty JSON array when no plugins are loaded
TEST_F(ProcessStatsTest, GetModuleStats_ReturnsEmptyArrayWhenNoPlugins) {
    char* result = ProcessStats::getModuleStats();
    
    ASSERT_NE(result, nullptr);
    
    // Parse the JSON
    QByteArray jsonData(result);
    QJsonDocument doc = QJsonDocument::fromJson(jsonData);
    
    EXPECT_TRUE(doc.isArray());
    
    QJsonArray modulesArray = doc.array();
    EXPECT_EQ(modulesArray.size(), 0);
    
    // Clean up
    delete[] result;
}

// Verifies that getModuleStats() returns a non-null pointer
TEST_F(ProcessStatsTest, GetModuleStats_ReturnsNonNullPointer) {
    char* result = ProcessStats::getModuleStats();
    
    ASSERT_NE(result, nullptr);
    
    // Clean up
    delete[] result;
}

// Verifies that getModuleStats() returns valid JSON structure with correct fields
TEST_F(ProcessStatsTest, GetModuleStats_ReturnsValidJsonStructure) {
    // Create a dummy plugin process
    QProcess* dummyProcess = new QProcess();
    dummyProcess->start("sleep", QStringList() << "1");
    dummyProcess->waitForStarted();
    
    qint64 pid = dummyProcess->processId();
    ASSERT_GT(pid, 0);
    
    g_plugin_processes.insert("test_plugin", dummyProcess);
    
    char* result = ProcessStats::getModuleStats();
    
    ASSERT_NE(result, nullptr);
    
    // Parse the JSON
    QByteArray jsonData(result);
    QJsonDocument doc = QJsonDocument::fromJson(jsonData);
    
    EXPECT_TRUE(doc.isArray());
    
    QJsonArray modulesArray = doc.array();
    ASSERT_EQ(modulesArray.size(), 1);
    
    // Check the structure of the first module
    QJsonObject moduleObj = modulesArray[0].toObject();
    EXPECT_TRUE(moduleObj.contains("name"));
    EXPECT_TRUE(moduleObj.contains("cpu_percent"));
    EXPECT_TRUE(moduleObj.contains("cpu_time_seconds"));
    EXPECT_TRUE(moduleObj.contains("memory_mb"));
    
    EXPECT_EQ(moduleObj["name"].toString().toStdString(), "test_plugin");
    EXPECT_GE(moduleObj["cpu_percent"].toDouble(), 0.0);
    EXPECT_GE(moduleObj["cpu_time_seconds"].toDouble(), 0.0);
    EXPECT_GE(moduleObj["memory_mb"].toDouble(), 0.0);
    
    // Clean up
    delete[] result;
    // Note: Process cleanup is handled by TearDown()
}

// Verifies that getModuleStats() skips core_manager plugin
TEST_F(ProcessStatsTest, GetModuleStats_SkipsCoreManager) {
    // Create a dummy process for core_manager
    QProcess* coreManagerProcess = new QProcess();
    coreManagerProcess->start("sleep", QStringList() << "1");
    coreManagerProcess->waitForStarted();
    g_plugin_processes.insert("core_manager", coreManagerProcess);
    
    // Create another plugin process
    QProcess* otherProcess = new QProcess();
    otherProcess->start("sleep", QStringList() << "1");
    otherProcess->waitForStarted();
    g_plugin_processes.insert("other_plugin", otherProcess);
    
    char* result = ProcessStats::getModuleStats();
    
    ASSERT_NE(result, nullptr);
    
    // Parse the JSON
    QByteArray jsonData(result);
    QJsonDocument doc = QJsonDocument::fromJson(jsonData);
    
    EXPECT_TRUE(doc.isArray());
    
    QJsonArray modulesArray = doc.array();
    
    // Should only contain other_plugin, not core_manager
    ASSERT_EQ(modulesArray.size(), 1);
    
    QJsonObject moduleObj = modulesArray[0].toObject();
    EXPECT_EQ(moduleObj["name"].toString().toStdString(), "other_plugin");
    
    // Clean up
    delete[] result;
    // Note: Process cleanup is handled by TearDown()
}
