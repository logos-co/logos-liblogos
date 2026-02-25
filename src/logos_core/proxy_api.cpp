#include "proxy_api.h"
#include "logos_core_internal.h"
#include "plugin_manager.h"
#include <QDebug>
#include <QTimer>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include "logos_api.h"
#include "logos_api_client.h"
#include "logos_mode.h"
#include <cassert>

namespace ProxyAPI {

    QVariant jsonParamToQVariant(const QJsonObject& param) {
        QString name = param.value("name").toString();
        QString value = param.value("value").toString();
        QString type = param.value("type").toString();

        qDebug() << "Converting param:" << name << "type:" << type;

        if (type == "string" || type == "QString") {
            return QVariant(value);
        } else if (type == "int" || type == "integer") {
            bool ok;
            int intValue = value.toInt(&ok);
            return ok ? QVariant(intValue) : QVariant();
        } else if (type == "bool" || type == "boolean") {
            if (value.toLower() == "true" || value == "1") {
                return QVariant(true);
            } else if (value.toLower() == "false" || value == "0") {
                return QVariant(false);
            }
            return QVariant();
        } else if (type == "double" || type == "float") {
            bool ok;
            double doubleValue = value.toDouble(&ok);
            return ok ? QVariant(doubleValue) : QVariant();
        } else {
            // Default to string for unknown types
            qWarning() << "Unknown parameter type:" << type << "- treating as string";
            return QVariant(value);
        }
    }

    void asyncOperation(const char* data, AsyncCallback callback, void* user_data) {
        if (!callback) {
            qWarning() << "asyncOperation: callback is null, returning early";
            return;
        }

        QString inputData = data ? QString::fromUtf8(data) : QString("no data");
        qDebug() << "Starting async operation with data:" << inputData;

        // Create a timer to simulate async work
        QTimer* timer = new QTimer();
        timer->setSingleShot(true);
        timer->setInterval(2000); // 2 second delay

        // Connect the timer to execute the callback
        QObject::connect(timer, &QTimer::timeout, [=]() {
            qDebug() << "Async operation completed for data:" << inputData;

            // Create the result message
            QString resultMessage = QString("Async operation completed successfully for: %1").arg(inputData);
            QByteArray messageBytes = resultMessage.toUtf8();

            // Call the callback with success (1) and message
            callback(1, messageBytes.constData(), user_data);

            // Clean up the timer
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

        // Check if plugin exists in known plugins
        if (!g_known_plugins.contains(name)) {
            QString errorMsg = QString("Plugin not found among known plugins: %1").arg(name);
            QByteArray errorBytes = errorMsg.toUtf8();
            callback(0, errorBytes.constData(), user_data);
            return;
        }

        // Create a timer to simulate async plugin loading
        QTimer* timer = new QTimer();
        timer->setSingleShot(true);
        timer->setInterval(1000); // 1 second delay to simulate work

        // Connect the timer to execute the actual plugin loading
        QObject::connect(timer, &QTimer::timeout, [=]() {
            qDebug() << "Executing async plugin load for:" << name;

            // Use our existing synchronous loadPlugin function
            bool success = PluginManager::loadPlugin(name);

            QString resultMessage;
            if (success) {
                resultMessage = QString("Plugin '%1' loaded successfully").arg(name);
            } else {
                resultMessage = QString("Failed to load plugin '%1'").arg(name);
            }

            QByteArray messageBytes = resultMessage.toUtf8();

            // Call the callback with the result
            callback(success ? 1 : 0, messageBytes.constData(), user_data);

            // Clean up the timer
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
        QString methodNameStr = QString::fromUtf8(method_name);
        QString paramsJsonStr = params_json ? QString::fromUtf8(params_json) : QString("[]");

        qDebug() << "Starting async method call for plugin:" << pluginNameStr
                << "method:" << methodNameStr
                << "args_count:" << paramsJsonStr.size();

        // Check if plugin is loaded
        if (!g_loaded_plugins.contains(pluginNameStr)) {
            QString errorMsg = QString("Plugin not loaded: %1").arg(pluginNameStr);
            QByteArray errorBytes = errorMsg.toUtf8();
            callback(0, errorBytes.constData(), user_data);
            return;
        }

        // TODO: this delay might not longer be necessary, needs review
        int initialDelay = LogosModeConfig::isLocal() ? 0 : 500;
        int connectionDelay = LogosModeConfig::isLocal() ? 0 : 2000;

        // Create a timer to simulate async method call
        // Create timer with QCoreApplication as parent to ensure it's in the main thread
        QTimer* timer = new QTimer(QCoreApplication::instance());
        timer->setSingleShot(true);
        timer->setInterval(initialDelay);

        // Connect the timer to execute the actual method call
        QObject::connect(timer, &QTimer::timeout, [=]() {
            qDebug() << "Executing async method call for:" << pluginNameStr << "::" << methodNameStr;

            try {
                // Parse the JSON parameters
                QJsonParseError parseError;
                QJsonDocument jsonDoc = QJsonDocument::fromJson(paramsJsonStr.toUtf8(), &parseError);

                if (parseError.error != QJsonParseError::NoError) {
                    QString errorMsg = QString("JSON parse error: %1").arg(parseError.errorString());
                    QByteArray errorBytes = errorMsg.toUtf8();
                    callback(0, errorBytes.constData(), user_data);
                    timer->deleteLater();
                    return;
                }

                QJsonArray paramsArray = jsonDoc.array();
                QVariantList args;

                // Convert JSON parameters to QVariantList
                for (const QJsonValue& paramValue : paramsArray) {
                    if (paramValue.isObject()) {
                        QJsonObject paramObj = paramValue.toObject();
                        QVariant variant = jsonParamToQVariant(paramObj);
                        if (variant.isValid()) {
                            args.append(variant);
                        } else {
                            QString errorMsg = QString("Invalid parameter: %1").arg(paramObj.value("name").toString());
                            QByteArray errorBytes = errorMsg.toUtf8();
                            callback(0, errorBytes.constData(), user_data);
                            timer->deleteLater();
                            return;
                        }
                    }
                }

                qDebug() << "Converted parameters to QVariantList, count:" << args.size();

                // Create LogosAPI instance to make the remote call
                LogosAPI* logosAPI = new LogosAPI("core");

                QTimer* connectionTimer = new QTimer();
                connectionTimer->setSingleShot(true);
                connectionTimer->setInterval(connectionDelay);

                QObject::connect(connectionTimer, &QTimer::timeout, [=]() {
                    if (logosAPI->getClient(pluginNameStr)->isConnected()) {
                        qDebug() << "LogosAPI connected, making remote method call";

                        // Make the remote method call
                        QVariant result = logosAPI->getClient(pluginNameStr)->invokeRemoteMethod(pluginNameStr, methodNameStr, args);

                        QString resultMessage;
                        if (result.isValid()) {
                            // Convert result to string representation
                            QString resultStr;
                            if (result.canConvert<QString>()) {
                                resultStr = result.toString();
                            } else {
                                resultStr = QString("Result of type: %1").arg(result.typeName());
                            }
                            resultMessage = QString("Method call successful. Result: %1").arg(resultStr);

                            QByteArray messageBytes = resultMessage.toUtf8();
                            callback(1, messageBytes.constData(), user_data);
                        } else {
                            resultMessage = QString("Method call returned invalid result");
                            QByteArray messageBytes = resultMessage.toUtf8();
                            callback(0, messageBytes.constData(), user_data);
                        }
                    } else {
                        QString errorMsg = QString("Failed to connect to plugin: %1").arg(pluginNameStr);
                        QByteArray errorBytes = errorMsg.toUtf8();
                        callback(0, errorBytes.constData(), user_data);
                    }

                    // Clean up
                    logosAPI->deleteLater();
                    connectionTimer->deleteLater();
                });

                connectionTimer->start();

            } catch (const std::exception& e) {
                QString errorMsg = QString("Exception during method call: %1").arg(e.what());
                QByteArray errorBytes = errorMsg.toUtf8();
                callback(0, errorBytes.constData(), user_data);
            } catch (...) {
                QString errorMsg = QString("Unknown exception during method call");
                QByteArray errorBytes = errorMsg.toUtf8();
                callback(0, errorBytes.constData(), user_data);
            }

            // Clean up the main timer
            timer->deleteLater();
        });

        timer->start();
        qDebug() << "Async method call timer started for:" << pluginNameStr << "::" << methodNameStr;
    }

    void registerEventListener(const char* plugin_name, const char* event_name, AsyncCallback callback, void* user_data) {
        if (!plugin_name || !event_name || !callback) {
            qWarning() << "registerEventListener: null parameter, returning early";
            return;
        }

        QString pluginNameStr = QString::fromUtf8(plugin_name);
        QString eventNameStr = QString::fromUtf8(event_name);

        qDebug() << "Registering event listener for plugin:" << pluginNameStr << "event:" << eventNameStr;

        // Check if plugin is loaded
        if (!g_loaded_plugins.contains(pluginNameStr)) {
            qWarning() << "Cannot register event listener: Plugin not loaded:" << pluginNameStr;
            return;
        }

        // Create event listener structure
        EventListener listener;
        listener.pluginName = pluginNameStr;
        listener.eventName = eventNameStr;
        listener.callback = callback;
        listener.userData = user_data;

        // Add to global list
        g_event_listeners.append(listener);

        // Set up the actual Qt Remote Objects event listener
        // Create timer with QCoreApplication as parent to ensure it's in the main thread
        QTimer* setupTimer = new QTimer(QCoreApplication::instance());
        setupTimer->setSingleShot(true);
        setupTimer->setInterval(1000); // Give plugin time to be ready

        QObject::connect(setupTimer, &QTimer::timeout, [=]() {
            // Create LogosAPI instance to connect to the plugin
            LogosAPI* logosAPI = new LogosAPI("core");

            // Use a delay to ensure connection is established
            QTimer* connectionTimer = new QTimer();
            connectionTimer->setSingleShot(true);
            connectionTimer->setInterval(2000); // 2 second delay

            QObject::connect(connectionTimer, &QTimer::timeout, [=]() {
                if (logosAPI->getClient(pluginNameStr)->isConnected()) {
                    qDebug() << "LogosAPI connected for event listener, setting up event listener for" << eventNameStr;

                    // Get the replica object to set up event listener
                    QObject* replica = logosAPI->getClient(pluginNameStr)->requestObject(pluginNameStr);
                    if (replica) {
                        // Set up event listener for the specified event
                        logosAPI->getClient(pluginNameStr)->onEvent(replica, nullptr, eventNameStr, [=](const QString& eventName, const QVariantList& eventData) {
                            qDebug() << "Event listener captured event:" << eventName << "with data:" << eventData;

                            // Format the event data as JSON for the callback
                            QString eventResponse = QString("{\"event\":\"%1\",\"data\":[").arg(eventName);
                            for (int i = 0; i < eventData.size(); ++i) {
                                if (i > 0) eventResponse += ",";
                                eventResponse += QString("\"%1\"").arg(eventData[i].toString());
                            }
                            eventResponse += "]}";

                            QByteArray eventBytes = eventResponse.toUtf8();
                            callback(1, eventBytes.constData(), user_data);
                        });
                        qDebug() << "Event listener successfully registered for" << pluginNameStr << "::" << eventNameStr;
                    } else {
                        qWarning() << "Failed to get replica for event listener setup";
                    }
                } else {
                    qWarning() << "Failed to connect LogosAPI for event listener:" << pluginNameStr;
                }

                // Clean up timers but keep LogosAPI alive for the event listener
                connectionTimer->deleteLater();
            });

            connectionTimer->start();
            setupTimer->deleteLater();
        });

        setupTimer->start();
        qDebug() << "Event listener setup timer started for:" << pluginNameStr << "::" << eventNameStr;
    }

}
