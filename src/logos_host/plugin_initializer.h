#ifndef PLUGIN_INITIALIZER_H
#define PLUGIN_INITIALIZER_H

#include <QString>

class QPluginLoader;
class PluginInterface;
class LogosAPI;
class QObject;

QString receiveAuthToken(const QString& pluginName);

PluginInterface* loadPlugin(const QString& pluginPath, const QString& expectedName, QPluginLoader& loader);

LogosAPI* initializeLogosAPI(const QString& pluginName, QObject* plugin, 
                              PluginInterface* basePlugin, const QString& authToken);

LogosAPI* setupPlugin(const QString& pluginName, const QString& pluginPath, QPluginLoader& loader);

#endif // PLUGIN_INITIALIZER_H
