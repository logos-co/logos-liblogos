#ifndef COMMAND_LINE_PARSER_H
#define COMMAND_LINE_PARSER_H

#include <QString>

class QCoreApplication;

struct PluginArgs {
    QString name;
    QString path;
    bool valid;
};

PluginArgs parseCommandLineArgs(QCoreApplication& app);

#endif // COMMAND_LINE_PARSER_H
