#ifndef PLUGIN_INITIALIZER_H
#define PLUGIN_INITIALIZER_H

#include <string>
#include "module_lib.h"

class PluginInterface;
class LogosAPI;
class QObject;

ModuleLib::LogosModule loadPlugin(const std::string& pluginPath, const std::string& expectedName);

LogosAPI* initializeLogosAPI(const std::string& pluginName, QObject* plugin,
                              PluginInterface* basePlugin, const std::string& authToken,
                              const std::string& pluginPath,
                              const std::string& instancePersistencePath = {});

LogosAPI* setupPlugin(const std::string& pluginName, const std::string& pluginPath,
                      const std::string& instancePersistencePath = {});

#endif // PLUGIN_INITIALIZER_H
