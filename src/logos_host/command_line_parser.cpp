#include "command_line_parser.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QStringList>
#include <QDebug>

PluginArgs parseCommandLineArgs(QCoreApplication& app)
{
    PluginArgs args;
    args.valid = false;

    // Setup command line parser
    QCommandLineParser parser;
    parser.setApplicationDescription("Logos host for loading plugins in separate processes");
    parser.addHelpOption();
    parser.addVersionOption();

    // Add plugin name option
    QCommandLineOption pluginNameOption(QStringList() << "n" << "name",
                                       "Name of the plugin to load",
                                       "plugin_name");
    parser.addOption(pluginNameOption);

    // Add plugin path option
    QCommandLineOption pluginPathOption(QStringList() << "p" << "path",
                                       "Path to the plugin file",
                                       "plugin_path");
    parser.addOption(pluginPathOption);

    // Process the command line arguments
    parser.process(app);

    // Get plugin name and path
    args.name = parser.value(pluginNameOption);
    args.path = parser.value(pluginPathOption);

    if (args.name.isEmpty() || args.path.isEmpty()) {
        qCritical() << "Both plugin name and path must be specified";
        qCritical() << "Usage:" << app.arguments()[0] << "--name <plugin_name> --path <plugin_path>";
        return args;
    }

    args.valid = true;
    return args;
}
