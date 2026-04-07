#include "command_line_parser.h"
#include "host_app.h"
#include "plugin_initializer.h"
#include "logos_api.h"

int main(int argc, char* argv[])
{
    PluginArgs args = parseCommandLineArgs(argc, argv);
    if (!args.valid)
        return 1;

    HostApp::init(argc, argv);

    LogosAPI* logos_api = setupPlugin(args.name, args.path);
    if (!logos_api) {
        HostApp::cleanup();
        return 1;
    }

    int result = HostApp::exec();
    delete logos_api;
    HostApp::cleanup();

    return result;
}
