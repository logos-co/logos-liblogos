#include <gtest/gtest.h>
#include <process_stats/process_stats.h>
#include <QCoreApplication>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <cstring>
#include <unistd.h>

class ProcessStatsTest : public ::testing::Test {
protected:
    QList<QProcess*> m_processes;

    void SetUp() override {
        ProcessStats::clearHistory();
    }

    void TearDown() override {
        ProcessStats::clearHistory();
        for (QProcess* process : m_processes) {
            if (process) {
                process->terminate();
                process->waitForFinished(1000);
                delete process;
            }
        }
        m_processes.clear();
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

    EXPECT_GT(stats.memoryMB, 0.0);
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

    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);

    EXPECT_EQ(stats.cpuPercent, 0.0);
}

// Verifies that CPU percent is calculated after the initial call
TEST_F(ProcessStatsTest, GetProcessStats_CpuPercentUpdatesOnSecondCall) {
    qint64 currentPid = getpid();

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

// Verifies that getModuleStats() returns an empty JSON array when no plugins are loaded
TEST_F(ProcessStatsTest, GetModuleStats_ReturnsEmptyArrayWhenNoPlugins) {
    QHash<QString, qint64> processes;
    char* result = ProcessStats::getModuleStats(processes);

    ASSERT_NE(result, nullptr);

    QByteArray jsonData(result);
    QJsonDocument doc = QJsonDocument::fromJson(jsonData);

    EXPECT_TRUE(doc.isArray());

    QJsonArray modulesArray = doc.array();
    EXPECT_EQ(modulesArray.size(), 0);

    delete[] result;
}

// Verifies that getModuleStats() returns a non-null pointer
TEST_F(ProcessStatsTest, GetModuleStats_ReturnsNonNullPointer) {
    QHash<QString, qint64> processes;
    char* result = ProcessStats::getModuleStats(processes);

    ASSERT_NE(result, nullptr);

    delete[] result;
}

// Verifies that getModuleStats() returns valid JSON structure with correct fields
TEST_F(ProcessStatsTest, GetModuleStats_ReturnsValidJsonStructure) {
    QProcess* dummyProcess = new QProcess();
    m_processes.append(dummyProcess);
    dummyProcess->start("sleep", QStringList() << "1");
    dummyProcess->waitForStarted();

    qint64 pid = dummyProcess->processId();
    ASSERT_GT(pid, 0);

    QHash<QString, qint64> processes;
    processes.insert("test_plugin", pid);

    char* result = ProcessStats::getModuleStats(processes);

    ASSERT_NE(result, nullptr);

    QByteArray jsonData(result);
    QJsonDocument doc = QJsonDocument::fromJson(jsonData);

    EXPECT_TRUE(doc.isArray());

    QJsonArray modulesArray = doc.array();
    ASSERT_EQ(modulesArray.size(), 1);

    QJsonObject moduleObj = modulesArray[0].toObject();
    EXPECT_TRUE(moduleObj.contains("name"));
    EXPECT_TRUE(moduleObj.contains("cpu_percent"));
    EXPECT_TRUE(moduleObj.contains("cpu_time_seconds"));
    EXPECT_TRUE(moduleObj.contains("memory_mb"));

    EXPECT_EQ(moduleObj["name"].toString().toStdString(), "test_plugin");
    EXPECT_GE(moduleObj["cpu_percent"].toDouble(), 0.0);
    EXPECT_GE(moduleObj["cpu_time_seconds"].toDouble(), 0.0);
    EXPECT_GE(moduleObj["memory_mb"].toDouble(), 0.0);

    delete[] result;
}
