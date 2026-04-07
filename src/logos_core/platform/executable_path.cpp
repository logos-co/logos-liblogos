#include "executable_path.h"
#include <climits>
#if defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

std::filesystem::path logos_executable_directory()
{
#if defined(__linux__)
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec)
        return p.parent_path();
#elif defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        std::error_code ec;
        return std::filesystem::weakly_canonical(std::filesystem::path(buf), ec).parent_path();
    }
#endif
    return std::filesystem::current_path();
}
