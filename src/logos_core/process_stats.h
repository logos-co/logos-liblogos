#ifndef PROCESS_STATS_H
#define PROCESS_STATS_H

#include "logos_core_internal.h"

namespace ProcessStats {
    // Get process statistics (CPU and memory usage) for a given process ID
    // Returns ProcessStatsData structure with CPU percentage, CPU time, and memory usage
    ProcessStatsData getProcessStats(qint64 pid);
    
    // Get module statistics for all loaded modules as JSON
    // Returns a JSON string containing array of module stats, or nullptr on error
    // The returned string must be freed by the caller
    char* getModuleStats();
}

#endif // PROCESS_STATS_H
