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

    parser.process(app);

    args.valid = true;
    return args;
}
