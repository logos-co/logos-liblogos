#ifndef PROCESS_STATS_H
#define PROCESS_STATS_H

#include <QHash>
#include <QString>
#include <QtGlobal>

namespace ProcessStats {
    // Structure for process statistics
    struct ProcessStatsData {
        double cpuPercent;
        double cpuTimeSeconds;
        double memoryMB;
    };

    // Get process statistics (CPU and memory usage) for a given process ID
    // Returns ProcessStatsData structure with CPU percentage, CPU time, and memory usage
    ProcessStatsData getProcessStats(qint64 pid);
    
    // Get module statistics for the provided processes as JSON
    // @param processes: map of module name -> process ID
    // Returns a JSON string containing array of module stats, or nullptr on error
    // The returned string must be freed by the caller
    char* getModuleStats(const QHash<QString, qint64>& processes);

    // Clear internal CPU time history cache
    // Useful for test isolation and when resetting state
    void clearHistory();
}

#endif // PROCESS_STATS_H
