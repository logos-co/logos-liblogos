#ifndef COMMAND_LINE_PARSER_H
#define COMMAND_LINE_PARSER_H

#include <string>

struct ModuleArgs {
    std::string name;
    std::string path;
    std::string instancePersistencePath;
    // Per-module transport set, serialized as JSON (the format defined
    // by logos-cpp-sdk's logos_transport_config_json.h). Empty means
    // "use the global default (LocalSocket only)" — the back-compat
    // behaviour for modules the daemon hasn't configured.
    std::string transportSetJson;
    bool valid;
};

ModuleArgs parseCommandLineArgs(int argc, char *argv[]);

#endif // COMMAND_LINE_PARSER_H
