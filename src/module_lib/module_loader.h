#ifndef MODULE_LOADER_H
#define MODULE_LOADER_H

#include "module_metadata.h"
#include <QString>
#include <QObject>
#include <QPluginLoader>
#include <memory>
#include <vector>

namespace ModuleLib {

/**
 * @brief ModuleHandle is an RAII wrapper for a loaded plugin.
 * 
 * It manages the lifecycle of a loaded plugin and provides access to
 * the plugin instance, metadata, and type-safe casting.
 */
class ModuleHandle {
public:
    ModuleHandle();
    ~ModuleHandle();
    
    // Move semantics (no copying)
    ModuleHandle(ModuleHandle&& other) noexcept;
    ModuleHandle& operator=(ModuleHandle&& other) noexcept;
    ModuleHandle(const ModuleHandle&) = delete;
    ModuleHandle& operator=(const ModuleHandle&) = delete;
    
    /**
     * @brief Check if the handle contains a valid loaded plugin
     */
    bool isValid() const;
    
    /**
     * @brief Get the raw QObject instance of the plugin
     */
    QObject* instance() const;
    
    /**
     * @brief Get the metadata for this plugin
     */
    const ModuleMetadata& metadata() const;
    
    /**
     * @brief Get the last error message if loading failed
     */
    QString errorString() const;
    
    /**
     * @brief Cast the plugin instance to a specific interface type.
     * 
     * This wraps qobject_cast to provide type-safe casting.
     * 
     * @tparam T The interface type to cast to
     * @return T* Pointer to the interface, or nullptr if cast fails
     */
    template<typename T>
    T* as() const {
        if (!m_instance) {
            return nullptr;
        }
        return qobject_cast<T*>(m_instance);
    }
    
    /**
     * @brief Unload the plugin and release resources
     */
    void unload();
    
    /**
     * @brief Release ownership of the plugin instance without unloading.
     * 
     * After calling this, the ModuleHandle no longer manages the plugin lifecycle.
     * The caller is responsible for ensuring proper cleanup.
     * 
     * @return QObject* The plugin instance (ownership transferred to caller)
     */
    QObject* release();

private:
    friend class ModuleLoader;
    
    QPluginLoader* m_loader = nullptr;
    QObject* m_instance = nullptr;
    ModuleMetadata m_metadata;
    QString m_errorString;
    bool m_isStatic = false;  // Static plugins shouldn't be unloaded
};

/**
 * @brief ModuleLoader provides static methods for loading plugins.
 * 
 * This class abstracts QPluginLoader operations, allowing for potential
 * future replacement with a different plugin loading mechanism.
 */
class ModuleLoader {
public:
    /**
     * @brief Load a plugin from a file path.
     * 
     * @param pluginPath Path to the plugin file (.so, .dylib, .dll)
     * @param errorString Optional pointer to receive error message on failure
     * @return ModuleHandle Handle to the loaded plugin (check isValid())
     */
    static ModuleHandle loadFromPath(const QString& pluginPath, QString* errorString = nullptr);
    
    /**
     * @brief Load a plugin and set the file path separately.
     * 
     * This is useful when you need to configure the loader before loading.
     * 
     * @param pluginPath Path to the plugin file
     * @param errorString Optional pointer to receive error message on failure
     * @return ModuleHandle Handle to the loaded plugin (check isValid())
     */
    static ModuleHandle load(const QString& pluginPath, QString* errorString = nullptr);
    
    /**
     * @brief Get all statically linked plugins.
     * 
     * Returns handles to plugins that were linked statically via Q_IMPORT_PLUGIN().
     * 
     * @return std::vector<ModuleHandle> Vector of handles to static plugins
     */
    static std::vector<ModuleHandle> getStaticModules();
    
    /**
     * @brief Create a ModuleHandle from an existing QObject (for static plugins).
     * 
     * @param pluginObject The plugin QObject instance
     * @param metadata Pre-extracted metadata for the plugin
     * @return ModuleHandle Handle wrapping the existing plugin
     */
    static ModuleHandle wrapExisting(QObject* pluginObject, const ModuleMetadata& metadata = ModuleMetadata());
    
    /**
     * @brief Extract metadata from a plugin file without loading it.
     * 
     * @param pluginPath Path to the plugin file
     * @return std::optional<ModuleMetadata> The metadata if extraction succeeded
     */
    static std::optional<ModuleMetadata> extractMetadata(const QString& pluginPath);

private:
    ModuleLoader() = default;  // Static class, no instantiation
};

} // namespace ModuleLib

#endif // MODULE_LOADER_H
