#ifndef COMMAND_LINE_PARSER_H
#define COMMAND_LINE_PARSER_H

#include <QString>
#include <QStringList>

class QCoreApplication;

struct CoreArgs {
    bool valid;
    QString modulesDir;           // Optional: custom modules directory
    QStringList loadModules;      // Optional: modules to load in order
};

CoreArgs parseCommandLineArgs(QCoreApplication& app);

#endif // COMMAND_LINE_PARSER_H
