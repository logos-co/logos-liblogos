#include <QCoreApplication>
#include <QDebug>
#include "command_line_parser.h"
#include "plugin_initializer.h"
#include "logos_api.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName("logos_host");
    app.setApplicationVersion("1.0");

    // 1. Parse command line arguments
    PluginArgs args = parseCommandLineArgs(app);
    if (!args.valid) {
        return 1;
    }

    qDebug() << "Logos host starting for plugin:" << args.name;
    qDebug() << "Plugin path:" << args.path;

    // 2. Setup plugin (auth token, loading, and API initialization)
    LogosAPI* logos_api = setupPlugin(args.name, args.path);
    if (!logos_api) {
        return 1;
    }

    qDebug() << "Logos host ready, entering event loop...";
    
    // 5. Run event loop and cleanup
    int result = app.exec();
    delete logos_api;
    qDebug() << "Logos host shutting down";

    return result;
} 
