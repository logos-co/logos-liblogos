#include "command_line_parser.h"
#include <CLI/CLI.hpp>

ModuleArgs parseCommandLineArgs(int argc, char *argv[])
{
    ModuleArgs result;
    result.valid = false;

    CLI::App app{"Logos host for loading modules in separate processes"};
    app.set_version_flag("-v,--version", "1.0");

    app.add_option("-n,--name", result.name, "Name of the module to load")
        ->required();
    app.add_option("-p,--path", result.path, "Path to the module file")
        ->required();
    app.add_option("--instance-persistence-path", result.instancePersistencePath,
        "Instance persistence directory for the module");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        app.exit(e);
        return result;
    }

    result.valid = true;
    return result;
}
