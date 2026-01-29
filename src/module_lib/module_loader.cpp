#include "module_loader.h"
#include <QDebug>

namespace ModuleLib {

// ModuleHandle implementation

ModuleHandle::ModuleHandle() = default;

ModuleHandle::~ModuleHandle() {
    unload();
}

ModuleHandle::ModuleHandle(ModuleHandle&& other) noexcept
    : m_loader(other.m_loader)
    , m_instance(other.m_instance)
    , m_metadata(std::move(other.m_metadata))
    , m_errorString(std::move(other.m_errorString))
    , m_isStatic(other.m_isStatic)
{
    other.m_loader = nullptr;
    other.m_instance = nullptr;
    other.m_isStatic = false;
}

ModuleHandle& ModuleHandle::operator=(ModuleHandle&& other) noexcept {
    if (this != &other) {
        unload();
        
        m_loader = other.m_loader;
        m_instance = other.m_instance;
        m_metadata = std::move(other.m_metadata);
        m_errorString = std::move(other.m_errorString);
        m_isStatic = other.m_isStatic;
        
        other.m_loader = nullptr;
        other.m_instance = nullptr;
        other.m_isStatic = false;
    }
    return *this;
}

bool ModuleHandle::isValid() const {
    return m_instance != nullptr;
}

QObject* ModuleHandle::instance() const {
    return m_instance;
}

const ModuleMetadata& ModuleHandle::metadata() const {
    return m_metadata;
}

QString ModuleHandle::errorString() const {
    return m_errorString;
}

void ModuleHandle::unload() {
    if (m_loader && !m_isStatic) {
        m_loader->unload();
        delete m_loader;
    }
    m_loader = nullptr;
    m_instance = nullptr;
}

QObject* ModuleHandle::release() {
    QObject* instance = m_instance;
    
    // Don't unload - just release ownership
    // The loader is intentionally leaked here as the plugin needs to stay loaded
    m_loader = nullptr;
    m_instance = nullptr;
    m_isStatic = true;  // Prevent any accidental cleanup
    
    return instance;
}

// ModuleLoader implementation

ModuleHandle ModuleLoader::loadFromPath(const QString& pluginPath, QString* errorString) {
    return load(pluginPath, errorString);
}

ModuleHandle ModuleLoader::load(const QString& pluginPath, QString* errorString) {
    ModuleHandle handle;
    
    // Create the loader
    handle.m_loader = new QPluginLoader(pluginPath);
    
    // Extract metadata before loading
    QJsonObject rawMetadata = handle.m_loader->metaData();
    if (!rawMetadata.isEmpty()) {
        auto metadata = ModuleMetadata::fromJson(rawMetadata);
        if (metadata) {
            handle.m_metadata = *metadata;
        }
    }
    
    // Load the plugin instance
    handle.m_instance = handle.m_loader->instance();
    
    if (!handle.m_instance) {
        handle.m_errorString = handle.m_loader->errorString();
        if (errorString) {
            *errorString = handle.m_errorString;
        }
        qWarning() << "ModuleLoader: Failed to load plugin:" << pluginPath 
                   << "Error:" << handle.m_errorString;
        
        // Clean up the loader since we failed
        delete handle.m_loader;
        handle.m_loader = nullptr;
        return handle;
    }
    
    qDebug() << "ModuleLoader: Plugin loaded successfully:" << pluginPath;
    
    return handle;
}

std::vector<ModuleHandle> ModuleLoader::getStaticModules() {
    std::vector<ModuleHandle> handles;
    
    const QObjectList staticPlugins = QPluginLoader::staticInstances();
    qDebug() << "ModuleLoader: Found" << staticPlugins.size() << "static plugin instances";
    
    for (QObject* pluginObject : staticPlugins) {
        if (!pluginObject) {
            continue;
        }
        
        ModuleHandle handle = wrapExisting(pluginObject);
        handle.m_isStatic = true;
        
        if (handle.isValid()) {
            handles.push_back(std::move(handle));
        }
    }
    
    return handles;
}

ModuleHandle ModuleLoader::wrapExisting(QObject* pluginObject, const ModuleMetadata& metadata) {
    ModuleHandle handle;
    
    if (!pluginObject) {
        handle.m_errorString = "Null plugin object";
        return handle;
    }
    
    handle.m_instance = pluginObject;
    handle.m_metadata = metadata;
    handle.m_isStatic = true;  // Wrapped objects shouldn't be unloaded by us
    handle.m_loader = nullptr;
    
    return handle;
}

std::optional<ModuleMetadata> ModuleLoader::extractMetadata(const QString& pluginPath) {
    return ModuleMetadata::fromPath(pluginPath);
}

} // namespace ModuleLib
