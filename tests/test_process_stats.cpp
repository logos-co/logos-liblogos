#include <gtest/gtest.h>
#include "process_stats.h"
#include "logos_core_internal.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <cstring>
#include <unistd.h>

// Test fixture for process stats tests
class ProcessStatsTest : public ::testing::Test {
protected:
    // Store test processes for cleanup
    QList<QProcess*> testProcesses;
    
    void SetUp() override {
        // Clear internal CPU time history to ensure test isolation
        ProcessStats::clearHistory();
    }
    
    void TearDown() override {
        // Clean up internal CPU time history
        ProcessStats::clearHistory();
        
        // Clean up test processes
        for (QProcess* process : testProcesses) {
            if (process) {
                process->terminate();
                process->waitForFinished(1000);
                delete process;
            }
        }
        testProcesses.clear();
    }
    
    // Helper to create a test process and track it for cleanup
    QProcess* createTestProcess() {
        QProcess* process = new QProcess();
        process->start("sleep", QStringList() << "10");
        process->waitForStarted();
        testProcesses.append(process);
        return process;
    }
};

// =============================================================================
// getProcessStats Tests
// =============================================================================

// Verifies that getProcessStats() returns zeroed stats for negative PID
TEST_F(ProcessStatsTest, GetProcessStats_ReturnsZeroedStatsForNegativePid) {
    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(-1);
    
    EXPECT_EQ(stats.cpuPercent, 0.0);
    EXPECT_EQ(stats.cpuTimeSeconds, 0.0);
    EXPECT_EQ(stats.memoryMB, 0.0);
}

// Verifies that getProcessStats() returns zeroed stats for zero PID
TEST_F(ProcessStatsTest, GetProcessStats_ReturnsZeroedStatsForZeroPid) {
    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(0);
    
    EXPECT_EQ(stats.cpuPercent, 0.0);
    EXPECT_EQ(stats.cpuTimeSeconds, 0.0);
    EXPECT_EQ(stats.memoryMB, 0.0);
}

// Verifies that getProcessStats() returns valid stats for the current process
TEST_F(ProcessStatsTest, GetProcessStats_ReturnsValidStatsForCurrentProcess) {
    qint64 currentPid = getpid();
    
    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);
    
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
    
    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);
    
    EXPECT_GE(stats.memoryMB, 0.0);
}

// Verifies that CPU time is non-negative for a valid process
TEST_F(ProcessStatsTest, GetProcessStats_CpuTimeIsNonNegative) {
    qint64 currentPid = getpid();
    
    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);
    
    EXPECT_GE(stats.cpuTimeSeconds, 0.0);
}

// Verifies that CPU percent is zero on first call (no previous data)
TEST_F(ProcessStatsTest, GetProcessStats_CpuPercentIsZeroOnFirstCall) {
    qint64 currentPid = getpid();
    
    // Ensure no previous data exists by clearing history
    ProcessStats::clearHistory();
    
    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);
    
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
    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);
    
    // CPU percent should be non-negative (might be 0 if work was too fast)
    EXPECT_GE(stats.cpuPercent, 0.0);
}

// =============================================================================
// getModuleStats Tests
// =============================================================================

// Verifies that getModuleStats() returns an empty JSON array when no processes are passed
TEST_F(ProcessStatsTest, GetModuleStats_ReturnsEmptyArrayWhenNoPlugins) {
    QHash<QString, qint64> emptyProcesses;
    char* result = ProcessStats::getModuleStats(emptyProcesses);
    
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
    QHash<QString, qint64> emptyProcesses;
    char* result = ProcessStats::getModuleStats(emptyProcesses);
    
    ASSERT_NE(result, nullptr);
    
    // Clean up
    delete[] result;
}

// Verifies that getModuleStats() returns valid JSON structure with correct fields
TEST_F(ProcessStatsTest, GetModuleStats_ReturnsValidJsonStructure) {
    // Create a test process
    QProcess* testProcess = createTestProcess();
    qint64 pid = testProcess->processId();
    ASSERT_GT(pid, 0);
    
    // Build PID map
    QHash<QString, qint64> processes;
    processes["test_plugin"] = pid;
    
    char* result = ProcessStats::getModuleStats(processes);
    
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
}

// Verifies that getModuleStats() includes all processes passed to it
TEST_F(ProcessStatsTest, GetModuleStats_IncludesAllPassedProcesses) {
    // Create test processes
    QProcess* process1 = createTestProcess();
    QProcess* process2 = createTestProcess();
    
    qint64 pid1 = process1->processId();
    qint64 pid2 = process2->processId();
    ASSERT_GT(pid1, 0);
    ASSERT_GT(pid2, 0);
    
    // Build PID map with multiple processes
    QHash<QString, qint64> processes;
    processes["plugin_one"] = pid1;
    processes["plugin_two"] = pid2;
    
    char* result = ProcessStats::getModuleStats(processes);
    
    ASSERT_NE(result, nullptr);
    
    // Parse the JSON
    QByteArray jsonData(result);
    QJsonDocument doc = QJsonDocument::fromJson(jsonData);
    
    EXPECT_TRUE(doc.isArray());
    
    QJsonArray modulesArray = doc.array();
    
    // Should contain both processes
    ASSERT_EQ(modulesArray.size(), 2);
    
    // Collect names
    QSet<QString> names;
    for (const QJsonValue& val : modulesArray) {
        names.insert(val.toObject()["name"].toString());
    }
    
    EXPECT_TRUE(names.contains("plugin_one"));
    EXPECT_TRUE(names.contains("plugin_two"));
    
    // Clean up
    delete[] result;
}

// Verifies that getModuleStats() skips invalid PIDs
TEST_F(ProcessStatsTest, GetModuleStats_SkipsInvalidPids) {
    // Create one valid process
    QProcess* validProcess = createTestProcess();
    qint64 validPid = validProcess->processId();
    ASSERT_GT(validPid, 0);
    
    // Build PID map with valid and invalid PIDs
    QHash<QString, qint64> processes;
    processes["valid_plugin"] = validPid;
    processes["invalid_plugin"] = -1;  // Invalid PID
    processes["zero_plugin"] = 0;      // Invalid PID
    
    char* result = ProcessStats::getModuleStats(processes);
    
    ASSERT_NE(result, nullptr);
    
    // Parse the JSON
    QByteArray jsonData(result);
    QJsonDocument doc = QJsonDocument::fromJson(jsonData);
    
    EXPECT_TRUE(doc.isArray());
    
    QJsonArray modulesArray = doc.array();
    
    // Should only contain the valid process
    ASSERT_EQ(modulesArray.size(), 1);
    
    QJsonObject moduleObj = modulesArray[0].toObject();
    EXPECT_EQ(moduleObj["name"].toString().toStdString(), "valid_plugin");
    
    // Clean up
    delete[] result;
}
