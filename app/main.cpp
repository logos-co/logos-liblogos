#include <QCoreApplication>
#include <QDebug>
#include "logos_core.h"
#include "command_line_parser.h"

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("logoscore");
    app.setApplicationVersion("1.0");

    CoreArgs args = parseCommandLineArgs(app);
    if (!args.valid) {
        return 1;
    }

    logos_core_init(argc, argv);
    
    if (!args.modulesDir.isEmpty()) {
        logos_core_set_plugins_dir(args.modulesDir.toUtf8().constData());
    }
    
    logos_core_start();
    
    // Load modules specified via --load-modules in order
    for (const QString& moduleName : args.loadModules) {
        QString trimmed = moduleName.trimmed();
        if (!trimmed.isEmpty()) {
            if (!logos_core_load_plugin(trimmed.toUtf8().constData())) {
                qWarning() << "Failed to load module:" << trimmed;
            }
        }
    }
    
    return logos_core_exec();
} 
