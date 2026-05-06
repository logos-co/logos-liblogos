#include "command_line_parser.h"
#include "module_initializer.h"
#include "qt/qt_app.h"
#include "token_receiver.h"
#include "logos_api.h"
#include "interface.h"

int main(int argc, char *argv[])
{
    ModuleArgs args = parseCommandLineArgs(argc, argv);
    if (!args.valid) {
        return 1;
    }

    QtApp::init(argc, argv);

    // Container concern: receive auth token via subprocess IPC (Unix socket).
    // A different container (Docker, in-process) would deliver the token
    // through its own channel; the runtime/loader doesn't care how it arrives.
    std::string authToken = SubprocessTokenReceiver::receive(args.name);
    if (authToken.empty()) {
        return 1;
    }

    // Runtime concern: load the Qt plugin and initialize LogosAPI.
    ModuleLib::LogosModule module = loadModule(args.path, args.name);
    if (!module.isValid()) {
        return 1;
    }

    PluginInterface* basePlugin = module.as<PluginInterface>();
    LogosAPI* logos_api = initializeLogosAPI(args.name, module.instance(),
                                             basePlugin, authToken, args.path,
                                             args.instancePersistencePath,
                                             args.transportSetJson);
    module.release();

    if (!logos_api) {
        return 1;
    }

    int result = QtApp::exec();
    delete logos_api;
    QtApp::cleanup();

    return result;
}
