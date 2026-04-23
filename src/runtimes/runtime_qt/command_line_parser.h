#ifndef COMMAND_LINE_PARSER_H
#define COMMAND_LINE_PARSER_H

#include <string>

struct ModuleArgs {
    std::string name;
    std::string path;
    std::string instancePersistencePath;
    bool valid;
};

ModuleArgs parseCommandLineArgs(int argc, char *argv[]);

#endif // COMMAND_LINE_PARSER_H
