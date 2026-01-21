#include "process_stats.h"
#include <QDebug>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <cmath>
#include <cstring>

// Platform-specific includes for process monitoring
#if (defined(Q_OS_MACOS) || defined(Q_OS_MAC)) && !defined(Q_OS_IOS)
#include <libproc.h>
#include <mach/mach.h>
#include <mach/task_info.h>
#include <sys/sysctl.h>
#elif defined(Q_OS_LINUX)
#include <sys/resource.h>
#include <sys/times.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#endif

namespace ProcessStats {

ProcessStatsData getProcessStats(qint64 pid)
{
    ProcessStatsData stats = {0.0, 0.0, 0.0};
    
    if (pid <= 0) {
        return stats;
    }
    
#if (defined(Q_OS_MACOS) || defined(Q_OS_MAC)) && !defined(Q_OS_IOS)
    // macOS implementation using libproc
    struct proc_taskinfo taskInfo;
    int ret = proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &taskInfo, sizeof(taskInfo));
    
    if (ret == sizeof(taskInfo)) {
        // Get CPU time (user + system time) in microseconds, convert to seconds
        uint64_t totalTime = taskInfo.pti_total_user + taskInfo.pti_total_system;
        stats.cpuTimeSeconds = totalTime / 1e6;
        
        // Get memory footprint (resident size) in bytes, convert to megabytes
        stats.memoryMB = taskInfo.pti_resident_size / (1024.0 * 1024.0);
        
        // Calculate CPU percentage
        qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
        if (g_previous_cpu_times.contains(pid)) {
            QPair<double, qint64> previous = g_previous_cpu_times[pid];
            double timeDelta = (currentTime - previous.second) / 1000.0; // Convert to seconds
            double cpuDelta = stats.cpuTimeSeconds - previous.first;
            
            if (timeDelta > 0) {
                stats.cpuPercent = (cpuDelta / timeDelta) * 100.0;
            }
        }
        
        // Update previous values
        g_previous_cpu_times[pid] = QPair<double, qint64>(stats.cpuTimeSeconds, currentTime);
    }
    
#elif defined(Q_OS_LINUX)
    // Linux implementation using /proc filesystem
    QString statPath = QString("/proc/%1/stat").arg(pid);
    QString statusPath = QString("/proc/%1/status").arg(pid);
    
    // Read CPU time from /proc/[pid]/stat
    std::ifstream statFile(statPath.toStdString());
    if (statFile.is_open()) {
        std::string line;
        std::getline(statFile, line);
        std::istringstream iss(line);
        std::string token;
        
        // Skip first 13 tokens (pid, comm, state, ppid, etc.)
        // utime is token 14 (index 13), stime is token 15 (index 14)
        for (int i = 0; i < 14 && iss >> token; ++i) {}
        
        unsigned long utime = 0, stime = 0;
        if (iss >> utime && iss >> stime) {
            // CPU time is in clock ticks, convert to seconds
            long clockTicks = sysconf(_SC_CLK_TCK);
            if (clockTicks > 0) {
                stats.cpuTimeSeconds = (utime + stime) / static_cast<double>(clockTicks);
            }
        }
        statFile.close();
    }
    
    // Read memory from /proc/[pid]/status
    std::ifstream statusFile(statusPath.toStdString());
    if (statusFile.is_open()) {
        std::string line;
        while (std::getline(statusFile, line)) {
            if (line.find("VmRSS:") == 0) {
                // Extract memory value (in KB)
                std::istringstream iss(line);
                std::string label, value, unit;
                iss >> label >> value >> unit;
                if (!value.empty()) {
                    double memoryKB = std::stod(value);
                    stats.memoryMB = memoryKB / 1024.0;
                }
                break;
            }
        }
        statusFile.close();
    }
    
    // Calculate CPU percentage
    qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
    if (g_previous_cpu_times.contains(pid)) {
        QPair<double, qint64> previous = g_previous_cpu_times[pid];
        double timeDelta = (currentTime - previous.second) / 1000.0; // Convert to seconds
        double cpuDelta = stats.cpuTimeSeconds - previous.first;
        
        if (timeDelta > 0) {
            stats.cpuPercent = (cpuDelta / timeDelta) * 100.0;
        }
    }
    
    // Update previous values
    g_previous_cpu_times[pid] = QPair<double, qint64>(stats.cpuTimeSeconds, currentTime);
    
#else
    // Unsupported platform
    qWarning() << "Process monitoring not supported on this platform";
#endif
    
    return stats;
}

char* getModuleStats()
{
    qDebug() << "getModuleStats() called";
    
    QJsonArray modulesArray;
    
#ifndef Q_OS_IOS
    // Iterate through plugin processes
    for (auto it = g_plugin_processes.begin(); it != g_plugin_processes.end(); ++it) {
        QString pluginName = it.key();
        QProcess* process = it.value();
        
        // Skip core_manager as it runs in-process
        if (pluginName == "core_manager") {
            continue;
        }
        
        // Get process ID
        qint64 pid = process->processId();
        if (pid <= 0) {
            qWarning() << "Invalid PID for plugin:" << pluginName;
            continue;
        }
        
        // Get process statistics
        ProcessStatsData stats = getProcessStats(pid);
        
        // Create JSON object for this module
        QJsonObject moduleObj;
        moduleObj["name"] = pluginName;
        moduleObj["cpu_percent"] = stats.cpuPercent;
        moduleObj["cpu_time_seconds"] = stats.cpuTimeSeconds;
        moduleObj["memory_mb"] = stats.memoryMB;
        
        modulesArray.append(moduleObj);
        
        qDebug() << "Module stats for" << pluginName 
                 << "- CPU:" << stats.cpuPercent << "%" 
                 << "(" << stats.cpuTimeSeconds << "s),"
                 << "Memory:" << stats.memoryMB << "MB";
    }
#endif // Q_OS_IOS
    
    // Convert to JSON string
    QJsonDocument doc(modulesArray);
    QByteArray jsonData = doc.toJson(QJsonDocument::Compact);
    
    // Allocate memory for the result string
    char* result = new char[jsonData.size() + 1];
    strcpy(result, jsonData.constData());
    
    qDebug() << "Returning module stats JSON for" << modulesArray.size() << "modules";
    
    return result;
}

} // namespace ProcessStats
