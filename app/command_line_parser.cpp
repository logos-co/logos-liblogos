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

    parser.process(app);

    if (parser.isSet(modulesDirOption)) {
        args.modulesDir = parser.value(modulesDirOption);
    }

    args.valid = true;
    return args;
}
