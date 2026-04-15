#include "command_line_parser.h"
#include "plugin_initializer.h"
#include "qt/qt_app.h"
#include "logos_api.h"

int main(int argc, char *argv[])
{
    PluginArgs args = parseCommandLineArgs(argc, argv);
    if (!args.valid) {
        return 1;
    }

    QtApp::init(argc, argv);

    LogosAPI* logos_api = setupPlugin(args.name, args.path, args.instancePersistencePath);
    if (!logos_api) {
        return 1;
    }

    int result = QtApp::exec();
    delete logos_api;
    QtApp::cleanup();

    return result;
}
