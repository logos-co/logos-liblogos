#include "command_line_parser.h"
#include "module_initializer.h"
#include "qt/qt_app.h"
#include "logos_api.h"

int main(int argc, char *argv[])
{
    ModuleArgs args = parseCommandLineArgs(argc, argv);
    if (!args.valid) {
        return 1;
    }

    QtApp::init(argc, argv);

    LogosAPI* logos_api = setupModule(args.name, args.path,
                                       args.instancePersistencePath,
                                       args.transportSetJson);
    if (!logos_api) {
        return 1;
    }

    int result = QtApp::exec();
    delete logos_api;
    QtApp::cleanup();

    return result;
}
