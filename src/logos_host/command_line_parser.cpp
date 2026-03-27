#include "command_line_parser.h"
#include <CLI/CLI.hpp>

PluginArgs parseCommandLineArgs(int argc, char *argv[])
{
    PluginArgs result;
    result.valid = false;

    CLI::App app{"Logos host for loading plugins in separate processes"};
    app.set_version_flag("-v,--version", "1.0");

    app.add_option("-n,--name", result.name, "Name of the plugin to load")
        ->required();
    app.add_option("-p,--path", result.path, "Path to the plugin file")
        ->required();

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        app.exit(e);
        return result;
    }

    result.valid = true;
    return result;
}
