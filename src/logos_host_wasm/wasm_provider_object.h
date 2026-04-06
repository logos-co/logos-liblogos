#ifndef WASM_PROVIDER_OBJECT_H
#define WASM_PROVIDER_OBJECT_H

#include "logos_provider_object.h"
#include <QString>
#include <QJsonArray>
#include <QHash>
#include <wasmtime.h>

// Implements LogosProviderObject for WebAssembly modules via Wasmtime.
// Each exported function in the .wasm becomes a callable method.
// Supports i32, i64, f32, f64 parameter and return types.
class WasmProviderObject : public LogosProviderObject {
public:
    explicit WasmProviderObject(const QString& wasmPath, const QString& moduleName,
                                const QString& moduleVersion);
    ~WasmProviderObject() override;

    bool isValid() const { return m_valid; }

    QVariant callMethod(const QString& methodName, const QVariantList& args) override;
    bool informModuleToken(const QString& moduleName, const QString& token) override;
    QJsonArray getMethods() override;
    void setEventListener(EventCallback callback) override;
    void init(void* apiInstance) override;
    QString providerName() const override { return m_name; }
    QString providerVersion() const override { return m_version; }

private:
    struct WasmFunc {
        wasmtime_func_t func;
        wasm_functype_t* type; // owned by module, don't free
        int paramCount;
        int resultCount;
    };

    bool loadModule(const QString& wasmPath);
    void discoverExports();

    wasm_engine_t* m_engine = nullptr;
    wasmtime_store_t* m_store = nullptr;
    wasmtime_module_t* m_module = nullptr;
    wasmtime_instance_t m_instance;
    bool m_valid = false;

    QString m_name;
    QString m_version;
    QHash<QString, WasmFunc> m_functions;
    EventCallback m_eventCallback;
    LogosAPI* m_logosAPI = nullptr;
};

#endif // WASM_PROVIDER_OBJECT_H
