#include "wasm_provider_object.h"
#include <QDebug>
#include <QFile>
#include <QJsonObject>
#include <wasi.h>

WasmProviderObject::WasmProviderObject(const QString& wasmPath, const QString& moduleName,
                                       const QString& moduleVersion)
    : m_name(moduleName)
    , m_version(moduleVersion)
{
    m_valid = loadModule(wasmPath);
    if (m_valid) {
        discoverExports();
        qDebug() << "WasmProviderObject: loaded" << m_name << "with"
                 << m_functions.size() << "exported functions";
    }
}

WasmProviderObject::~WasmProviderObject()
{
    if (m_module) wasmtime_module_delete(m_module);
    if (m_store) wasmtime_store_delete(m_store);
    if (m_engine) wasm_engine_delete(m_engine);
}

bool WasmProviderObject::loadModule(const QString& wasmPath)
{
    // Read .wasm file
    QFile file(wasmPath);
    if (!file.open(QIODevice::ReadOnly)) {
        qCritical() << "WasmProviderObject: cannot open" << wasmPath;
        return false;
    }
    QByteArray wasmBytes = file.readAll();
    file.close();

    // Create engine
    m_engine = wasm_engine_new();
    if (!m_engine) {
        qCritical() << "WasmProviderObject: failed to create wasm engine";
        return false;
    }

    // Compile module
    wasmtime_error_t* error = nullptr;
    error = wasmtime_module_new(m_engine,
                                reinterpret_cast<const uint8_t*>(wasmBytes.constData()),
                                wasmBytes.size(), &m_module);
    if (error) {
        wasm_message_t msg;
        wasmtime_error_message(error, &msg);
        qCritical() << "WasmProviderObject: compile error:" << QByteArray(msg.data, msg.size);
        wasm_byte_vec_delete(&msg);
        wasmtime_error_delete(error);
        return false;
    }

    // Create store with WASI config
    wasi_config_t* wasi_config = wasi_config_new();
    wasi_config_inherit_stdout(wasi_config);
    wasi_config_inherit_stderr(wasi_config);

    m_store = wasmtime_store_new(m_engine, nullptr, nullptr);
    wasmtime_context_t* context = wasmtime_store_context(m_store);

    error = wasmtime_context_set_wasi(context, wasi_config);
    if (error) {
        wasm_message_t msg;
        wasmtime_error_message(error, &msg);
        qCritical() << "WasmProviderObject: WASI setup error:" << QByteArray(msg.data, msg.size);
        wasm_byte_vec_delete(&msg);
        wasmtime_error_delete(error);
        return false;
    }

    // Create linker and define WASI
    wasmtime_linker_t* linker = wasmtime_linker_new(m_engine);
    error = wasmtime_linker_define_wasi(linker);
    if (error) {
        wasmtime_linker_delete(linker);
        wasm_message_t msg;
        wasmtime_error_message(error, &msg);
        qCritical() << "WasmProviderObject: linker WASI error:" << QByteArray(msg.data, msg.size);
        wasm_byte_vec_delete(&msg);
        wasmtime_error_delete(error);
        return false;
    }

    // Instantiate
    wasm_trap_t* trap = nullptr;
    error = wasmtime_linker_instantiate(linker, context, m_module, &m_instance, &trap);
    wasmtime_linker_delete(linker);

    if (error || trap) {
        if (error) {
            wasm_message_t msg;
            wasmtime_error_message(error, &msg);
            qCritical() << "WasmProviderObject: instantiation error:" << QByteArray(msg.data, msg.size);
            wasm_byte_vec_delete(&msg);
            wasmtime_error_delete(error);
        }
        if (trap) {
            wasm_message_t msg;
            wasm_trap_message(trap, &msg);
            qCritical() << "WasmProviderObject: instantiation trap:" << QByteArray(msg.data, msg.size);
            wasm_byte_vec_delete(&msg);
            wasm_trap_delete(trap);
        }
        return false;
    }

    qDebug() << "WasmProviderObject: module instantiated successfully";
    return true;
}

void WasmProviderObject::discoverExports()
{
    wasmtime_context_t* context = wasmtime_store_context(m_store);

    // Skip internal WASI exports (start with _ or are "memory")
    QSet<QString> skipNames = {"memory", "_start", "_initialize", "__data_end",
                               "__heap_base", "__indirect_function_table"};

    size_t i = 0;
    char* name = nullptr;
    size_t nameLen = 0;
    wasmtime_extern_t ext;

    while (wasmtime_instance_export_nth(context, &m_instance, i, &name, &nameLen, &ext)) {
        QString funcName = QString::fromUtf8(name, nameLen);

        if (ext.kind == WASMTIME_EXTERN_FUNC && !skipNames.contains(funcName)
            && !funcName.startsWith("__")) {
            WasmFunc wf;
            wf.func = ext.of.func;

            // Get function type
            wf.type = wasmtime_func_type(context, &ext.of.func);
            const wasm_valtype_vec_t* params = wasm_functype_params(wf.type);
            const wasm_valtype_vec_t* results = wasm_functype_results(wf.type);
            wf.paramCount = params ? params->size : 0;
            wf.resultCount = results ? results->size : 0;

            m_functions.insert(funcName, wf);
            qDebug() << "WasmProviderObject: discovered export:" << funcName
                     << "params:" << wf.paramCount << "results:" << wf.resultCount;
        }
        ++i;
    }
}

QVariant WasmProviderObject::callMethod(const QString& methodName, const QVariantList& args)
{
    auto it = m_functions.find(methodName);
    if (it == m_functions.end()) {
        qWarning() << "WasmProviderObject: unknown method:" << methodName;
        return QVariant();
    }

    const WasmFunc& wf = it.value();
    wasmtime_context_t* context = wasmtime_store_context(m_store);

    // Convert QVariant args to wasmtime vals
    std::vector<wasmtime_val_t> params(wf.paramCount);
    const wasm_valtype_vec_t* paramTypes = wasm_functype_params(wf.type);

    for (int i = 0; i < wf.paramCount && i < args.size(); ++i) {
        wasm_valkind_t kind = wasm_valtype_kind(paramTypes->data[i]);
        switch (kind) {
        case WASM_I32:
            params[i].kind = WASMTIME_I32;
            params[i].of.i32 = args[i].toInt();
            break;
        case WASM_I64:
            params[i].kind = WASMTIME_I64;
            params[i].of.i64 = args[i].toLongLong();
            break;
        case WASM_F32:
            params[i].kind = WASMTIME_F32;
            params[i].of.f32 = args[i].toFloat();
            break;
        case WASM_F64:
            params[i].kind = WASMTIME_F64;
            params[i].of.f64 = args[i].toDouble();
            break;
        default:
            params[i].kind = WASMTIME_I64;
            params[i].of.i64 = args[i].toLongLong();
            break;
        }
    }

    // Call the function
    std::vector<wasmtime_val_t> results(wf.resultCount);
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* error = wasmtime_func_call(
        context, &wf.func,
        params.data(), params.size(),
        results.data(), results.size(),
        &trap);

    if (error || trap) {
        if (error) {
            wasm_message_t msg;
            wasmtime_error_message(error, &msg);
            qWarning() << "WasmProviderObject: call error:" << QByteArray(msg.data, msg.size);
            wasm_byte_vec_delete(&msg);
            wasmtime_error_delete(error);
        }
        if (trap) {
            wasm_message_t msg;
            wasm_trap_message(trap, &msg);
            qWarning() << "WasmProviderObject: call trap:" << QByteArray(msg.data, msg.size);
            wasm_byte_vec_delete(&msg);
            wasm_trap_delete(trap);
        }
        return QVariant();
    }

    // Convert result
    if (wf.resultCount == 0) {
        return QVariant(true); // void methods return true
    }

    switch (results[0].kind) {
    case WASMTIME_I32:
        return QVariant(results[0].of.i32);
    case WASMTIME_I64:
        return QVariant(static_cast<qlonglong>(results[0].of.i64));
    case WASMTIME_F32:
        return QVariant(static_cast<double>(results[0].of.f32));
    case WASMTIME_F64:
        return QVariant(results[0].of.f64);
    default:
        return QVariant(true);
    }
}

bool WasmProviderObject::informModuleToken(const QString& moduleName, const QString& token)
{
    Q_UNUSED(moduleName);
    Q_UNUSED(token);
    return true;
}

QJsonArray WasmProviderObject::getMethods()
{
    QJsonArray methods;

    auto wasmKindToQt = [](wasm_valkind_t kind) -> QString {
        switch (kind) {
        case WASM_I32: return "int";
        case WASM_I64: return "qlonglong";
        case WASM_F32: return "float";
        case WASM_F64: return "double";
        default: return "QVariant";
        }
    };

    for (auto it = m_functions.constBegin(); it != m_functions.constEnd(); ++it) {
        const WasmFunc& wf = it.value();
        QJsonObject obj;
        obj["name"] = it.key();
        obj["isInvokable"] = true;

        // Build return type
        const wasm_valtype_vec_t* results = wasm_functype_results(wf.type);
        if (wf.resultCount > 0) {
            obj["returnType"] = wasmKindToQt(wasm_valtype_kind(results->data[0]));
        } else {
            obj["returnType"] = "void";
        }

        // Build parameter list and signature
        const wasm_valtype_vec_t* params = wasm_functype_params(wf.type);
        QJsonArray paramArr;
        QStringList sigParts;
        for (int i = 0; i < wf.paramCount; ++i) {
            QString typeName = wasmKindToQt(wasm_valtype_kind(params->data[i]));
            QJsonObject p;
            p["type"] = typeName;
            p["name"] = QString("arg%1").arg(i);
            paramArr.append(p);
            sigParts.append(typeName);
        }
        obj["parameters"] = paramArr;
        obj["signature"] = it.key() + "(" + sigParts.join(",") + ")";
        methods.append(obj);
    }
    return methods;
}

void WasmProviderObject::setEventListener(EventCallback callback)
{
    m_eventCallback = callback;
}

void WasmProviderObject::init(void* apiInstance)
{
    m_logosAPI = static_cast<LogosAPI*>(apiInstance);
    qDebug() << "WasmProviderObject: init called, api=" << apiInstance;
}
