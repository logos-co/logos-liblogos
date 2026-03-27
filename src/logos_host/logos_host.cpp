#include <QCoreApplication>
#include "command_line_parser.h"
#include "plugin_initializer.h"
#include "logos_api.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName("logos_host");
    app.setApplicationVersion("1.0");

    PluginArgs args = parseCommandLineArgs(app);
    if (!args.valid) {
        return 1;
    }

    LogosAPI* logos_api = setupPlugin(args.name, args.path);
    if (!logos_api) {
        return 1;
    }

    int result = app.exec();
    delete logos_api;

    return result;
} 
