#include <QCoreApplication>
#include <QDebug>
#include "logos_core.h"
#include "command_line_parser.h"
#include "plugin_manager.h"

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
    
    if (!args.loadModules.isEmpty()) {
        QStringList trimmedModules;
        for (const QString& moduleName : args.loadModules) {
            QString trimmed = moduleName.trimmed();
            if (!trimmed.isEmpty()) {
                trimmedModules.append(trimmed);
            }
        }
        
        QStringList resolvedModules = PluginManager::resolveDependencies(trimmedModules);
        
        qDebug() << "Loading modules with resolved dependencies:" << resolvedModules;
        
        for (const QString& moduleName : resolvedModules) {
            if (!logos_core_load_plugin(moduleName.toUtf8().constData())) {
                qWarning() << "Failed to load module:" << moduleName;
            }
        }
    }
    
    return logos_core_exec();
} 
