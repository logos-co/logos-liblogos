#ifndef COMMAND_LINE_PARSER_H
#define COMMAND_LINE_PARSER_H

class QCoreApplication;

struct CoreArgs {
    bool valid;
};

CoreArgs parseCommandLineArgs(QCoreApplication& app);

#endif // COMMAND_LINE_PARSER_H
