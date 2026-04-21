#ifndef COMMAND_LINE_PARSER_H
#define COMMAND_LINE_PARSER_H

#include <string>

struct PluginArgs {
    std::string name;
    std::string path;
    std::string instancePersistencePath;
    bool valid;
};

PluginArgs parseCommandLineArgs(int argc, char *argv[]);

#endif // COMMAND_LINE_PARSER_H
