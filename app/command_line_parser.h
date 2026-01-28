#ifndef COMMAND_LINE_PARSER_H
#define COMMAND_LINE_PARSER_H

#include <QString>
#include <QStringList>
#include <QList>

class QCoreApplication;

struct ModuleCall {
    QString moduleName;
    QString methodName;
    QStringList params;  // Raw param strings (including @file refs)
};

struct CoreArgs {
    bool valid;
    QString modulesDir;           // Optional: custom modules directory
    QStringList loadModules;      // Optional: modules to load in order
    QList<ModuleCall> calls;      // Optional: module method calls to execute
};

CoreArgs parseCommandLineArgs(QCoreApplication& app);

#endif // COMMAND_LINE_PARSER_H
