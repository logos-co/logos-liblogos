#ifndef IPC_PATHS_H
#define IPC_PATHS_H

#include <filesystem>
#include <string>

inline std::filesystem::path logos_token_socket_path(const std::string& plugin_name)
{
    return std::filesystem::temp_directory_path() / ("logos_token_" + plugin_name);
}

#endif
