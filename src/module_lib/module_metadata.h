#ifndef MODULE_METADATA_H
#define MODULE_METADATA_H

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <optional>

namespace ModuleLib {

/**
 * @brief ModuleMetadata represents the metadata associated with a plugin/module.
 * 
 * This struct abstracts the metadata extraction from Qt's plugin system,
 * allowing for potential future replacement with a different backend.
 */
struct ModuleMetadata {
    QString name;
    QString version;
    QString description;
    QString author;
    QString type;
    QStringList dependencies;
    
    // Raw JSON metadata for any additional fields
    QJsonObject rawMetadata;
    
    /**
     * @brief Check if the metadata is valid (has at least a name)
     */
    bool isValid() const { return !name.isEmpty(); }
    
    /**
     * @brief Extract metadata from a plugin file without fully loading it.
     * 
     * This uses QPluginLoader::metaData() to read the embedded JSON metadata
     * from the plugin binary.
     * 
     * @param pluginPath Path to the plugin file
     * @return std::optional<ModuleMetadata> The metadata if extraction succeeded, std::nullopt otherwise
     */
    static std::optional<ModuleMetadata> fromPath(const QString& pluginPath);
    
    /**
     * @brief Create ModuleMetadata from a QJsonObject.
     * 
     * Expects the JSON object to have the structure used by Qt plugins:
     * { "MetaData": { "name": "...", "version": "...", ... } }
     * 
     * @param json The JSON object containing plugin metadata
     * @return std::optional<ModuleMetadata> The metadata if parsing succeeded, std::nullopt otherwise
     */
    static std::optional<ModuleMetadata> fromJson(const QJsonObject& json);
    
    /**
     * @brief Create ModuleMetadata from the custom metadata section.
     * 
     * This expects the inner "MetaData" object directly, not the full plugin metadata.
     * 
     * @param customMetadata The custom metadata JSON object
     * @return ModuleMetadata The parsed metadata (may be invalid if required fields missing)
     */
    static ModuleMetadata fromCustomMetadata(const QJsonObject& customMetadata);
};

} // namespace ModuleLib

#endif // MODULE_METADATA_H
