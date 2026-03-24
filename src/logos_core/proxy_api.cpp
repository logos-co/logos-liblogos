#include "proxy_api.h"
#include "logos_core_internal.h"
#include "plugin_manager.h"
#include <QDebug>
#include <QTimer>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include "logos_json_utils.h"
#include "logos_sdk_c.h"
#include "logos_mode.h"
#include <cassert>

namespace ProxyAPI {

    QVariant jsonParamToQVariant(const QJsonObject& param) {
        return LogosJsonUtils::jsonParamToVariant(param);
    }

    void asyncOperation(const char* data, AsyncCallback callback, void* user_data) {
        if (!callback) {
            qWarning() << "asyncOperation: callback is null, returning early";
            return;
        }
        
        QString inputData = data ? QString::fromUtf8(data) : QString("no data");
        qDebug() << "Starting async operation with data:" << inputData;
        
        QTimer* timer = new QTimer();
        timer->setSingleShot(true);
        timer->setInterval(2000);
        
        QObject::connect(timer, &QTimer::timeout, [=]() {
            qDebug() << "Async operation completed for data:" << inputData;
            QString resultMessage = QString("Async operation completed successfully for: %1").arg(inputData);
            QByteArray messageBytes = resultMessage.toUtf8();
            callback(1, messageBytes.constData(), user_data);
            timer->deleteLater();
        });
        
        timer->start();
        qDebug() << "Async operation timer started, will complete in 2 seconds";
    }

    void loadPluginAsync(const char* plugin_name, AsyncCallback callback, void* user_data) {
        if (!callback) {
            qWarning() << "loadPluginAsync: callback is null, returning early";
            return;
        }
        
        if (!plugin_name) {
            qWarning() << "loadPluginAsync: plugin_name is null";
            callback(0, "Plugin name is null", user_data);
            return;
        }
        
        QString name = QString::fromUtf8(plugin_name);
        qDebug() << "Starting async plugin load for:" << name;
        
        if (!g_known_plugins.contains(name)) {
            QString errorMsg = QString("Plugin not found among known plugins: %1").arg(name);
            QByteArray errorBytes = errorMsg.toUtf8();
            callback(0, errorBytes.constData(), user_data);
            return;
        }
        
        QTimer* timer = new QTimer();
        timer->setSingleShot(true);
        timer->setInterval(1000);
        
        QObject::connect(timer, &QTimer::timeout, [=]() {
            qDebug() << "Executing async plugin load for:" << name;
            bool success = PluginManager::loadPlugin(name);
            
            QString resultMessage;
            if (success) {
                resultMessage = QString("Plugin '%1' loaded successfully").arg(name);
            } else {
                resultMessage = QString("Failed to load plugin '%1'").arg(name);
            }
            
            QByteArray messageBytes = resultMessage.toUtf8();
            callback(success ? 1 : 0, messageBytes.constData(), user_data);
            timer->deleteLater();
        });
        
        timer->start();
        qDebug() << "Async plugin load timer started for:" << name;
    }

    void callPluginMethodAsync(const char* plugin_name, const char* method_name, const char* params_json, AsyncCallback callback, void* user_data) {
        if (!callback) {
            qWarning() << "callPluginMethodAsync: callback is null, returning early";
            return;
        }
        
        if (!plugin_name || !method_name) {
            qWarning() << "callPluginMethodAsync: plugin_name or method_name is null";
            callback(0, "Plugin name or method name is null", user_data);
            return;
        }
        
        QString pluginNameStr = QString::fromUtf8(plugin_name);
        
        if (!g_loaded_plugins.contains(pluginNameStr)) {
            QString errorMsg = QString("Plugin not loaded: %1").arg(pluginNameStr);
            QByteArray errorBytes = errorMsg.toUtf8();
            callback(0, errorBytes.constData(), user_data);
            return;
        }
        
        logos_sdk_call_method_async(plugin_name, method_name, params_json, callback, user_data);
    }

    void registerEventListener(const char* plugin_name, const char* event_name, AsyncCallback callback, void* user_data) {
        if (!plugin_name || !event_name || !callback) {
            qWarning() << "registerEventListener: null parameter, returning early";
            return;
        }
        
        QString pluginNameStr = QString::fromUtf8(plugin_name);
        QString eventNameStr = QString::fromUtf8(event_name);
        
        qDebug() << "Registering event listener for plugin:" << pluginNameStr << "event:" << eventNameStr;
        
        if (!g_loaded_plugins.contains(pluginNameStr)) {
            qWarning() << "Cannot register event listener: Plugin not loaded:" << pluginNameStr;
            return;
        }
        
        EventListener listener;
        listener.pluginName = pluginNameStr;
        listener.eventName = eventNameStr;
        listener.callback = callback;
        listener.userData = user_data;
        g_event_listeners.append(listener);
        
        logos_sdk_register_event(plugin_name, event_name, callback, user_data);
    }

}
