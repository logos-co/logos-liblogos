#ifndef PLUGIN_INITIALIZER_H
#define PLUGIN_INITIALIZER_H

#include <QString>
#include <module_lib/module_lib.h>

class PluginInterface;
class LogosAPI;
class QObject;

QString receiveAuthToken(const QString& pluginName);

ModuleLib::LogosModule loadPlugin(const QString& pluginPath, const QString& expectedName);

LogosAPI* initializeLogosAPI(const QString& pluginName, QObject* plugin, 
                              PluginInterface* basePlugin, const QString& authToken,
                              const QString& pluginPath);

LogosAPI* setupPlugin(const QString& pluginName, const QString& pluginPath);

#endif // PLUGIN_INITIALIZER_H
