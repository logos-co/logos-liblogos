#ifndef COMMAND_LINE_PARSER_H
#define COMMAND_LINE_PARSER_H

#include <QString>

class QCoreApplication;

struct CoreArgs {
    bool valid;
    QString modulesDir;  // Optional: custom modules directory
};

CoreArgs parseCommandLineArgs(QCoreApplication& app);

#endif // COMMAND_LINE_PARSER_H
