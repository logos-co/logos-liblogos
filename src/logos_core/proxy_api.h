#ifndef PROXY_API_H
#define PROXY_API_H

#include <QString>
#include <QVariant>
#include "logos_core.h"

namespace ProxyAPI {
    // Convert JSON parameter to QVariant
    QVariant jsonParamToQVariant(const QJsonObject& param);
    
    // Simple async operation example that uses a callback
    void asyncOperation(const char* data, AsyncCallback callback, void* user_data);
    
    // Async plugin loading with callback
    void loadPluginAsync(const char* plugin_name, AsyncCallback callback, void* user_data);
    
    // Proxy method to call plugin methods remotely with async callback
    void callPluginMethodAsync(
        const char* plugin_name, 
        const char* method_name, 
        const char* params_json, 
        AsyncCallback callback, 
        void* user_data
    );
    
    // Register an event listener for a specific event from a specific plugin
    void registerEventListener(
        const char* plugin_name,
        const char* event_name, 
        AsyncCallback callback,
        void* user_data
    );
}

#endif // PROXY_API_H
