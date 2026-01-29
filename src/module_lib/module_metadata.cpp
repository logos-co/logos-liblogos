#include "module_metadata.h"
#include <QPluginLoader>
#include <QJsonArray>
#include <QDebug>

namespace ModuleLib {

std::optional<ModuleMetadata> ModuleMetadata::fromPath(const QString& pluginPath) {
    QPluginLoader loader(pluginPath);
    
    QJsonObject metadata = loader.metaData();
    if (metadata.isEmpty()) {
        qWarning() << "ModuleMetadata: No metadata found for plugin:" << pluginPath;
        return std::nullopt;
    }
    
    return fromJson(metadata);
}

std::optional<ModuleMetadata> ModuleMetadata::fromJson(const QJsonObject& json) {
    QJsonObject customMetadata = json.value("MetaData").toObject();
    if (customMetadata.isEmpty()) {
        qWarning() << "ModuleMetadata: No custom metadata (MetaData section) found";
        return std::nullopt;
    }
    
    ModuleMetadata result = fromCustomMetadata(customMetadata);
    if (!result.isValid()) {
        return std::nullopt;
    }
    
    return result;
}

ModuleMetadata ModuleMetadata::fromCustomMetadata(const QJsonObject& customMetadata) {
    ModuleMetadata result;
    
    result.name = customMetadata.value("name").toString();
    result.version = customMetadata.value("version").toString();
    result.description = customMetadata.value("description").toString();
    result.author = customMetadata.value("author").toString();
    result.type = customMetadata.value("type").toString();
    result.rawMetadata = customMetadata;
    
    // Parse dependencies array
    QJsonArray depsArray = customMetadata.value("dependencies").toArray();
    for (const QJsonValue& dep : depsArray) {
        QString depName = dep.toString();
        if (!depName.isEmpty()) {
            result.dependencies.append(depName);
        }
    }
    
    return result;
}

} // namespace ModuleLib
