#include "command_line_parser.h"
#include <QCoreApplication>
#include <QCommandLineParser>

CoreArgs parseCommandLineArgs(QCoreApplication& app) {
    CoreArgs args;
    args.valid = false;

    QCommandLineParser parser;
    parser.setApplicationDescription("Logos Core - Plugin-based application framework");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption modulesDirOption(
        QStringList() << "modules-dir" << "m",
        "Directory to scan for modules",
        "path"
    );
    parser.addOption(modulesDirOption);

    QCommandLineOption loadModulesOption(
        QStringList() << "load-modules" << "l",
        "Comma-separated list of modules to load in order",
        "modules"
    );
    parser.addOption(loadModulesOption);

    parser.process(app);

    if (parser.isSet(modulesDirOption)) {
        args.modulesDir = parser.value(modulesDirOption);
    }

    if (parser.isSet(loadModulesOption)) {
        QString modulesList = parser.value(loadModulesOption);
        args.loadModules = modulesList.split(',', Qt::SkipEmptyParts);
    }

    args.valid = true;
    return args;
}
