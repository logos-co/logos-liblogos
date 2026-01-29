#ifndef MODULE_LIB_H
#define MODULE_LIB_H

/**
 * @file module_lib.h
 * @brief Convenience header that includes all module_lib components.
 * 
 * The module_lib library provides an abstraction layer over Qt's plugin system,
 * allowing for potential future replacement with a different backend while
 * keeping consumer code unchanged.
 * 
 * Main components:
 * - ModuleMetadata: Plugin metadata extraction and storage
 * - ModuleLoader/ModuleHandle: Plugin loading and lifecycle management
 * - ModuleIntrospection: Runtime method introspection
 * 
 * Example usage:
 * @code
 * #include <module_lib/module_lib.h>
 * 
 * using namespace ModuleLib;
 * 
 * // Load a plugin
 * QString error;
 * auto handle = ModuleLoader::loadFromPath("/path/to/plugin.so", &error);
 * if (!handle.isValid()) {
 *     qCritical() << "Failed to load:" << error;
 *     return;
 * }
 * 
 * // Access metadata
 * qDebug() << "Plugin name:" << handle.metadata().name;
 * qDebug() << "Plugin version:" << handle.metadata().version;
 * 
 * // Cast to interface
 * auto* plugin = handle.as<PluginInterface>();
 * if (plugin) {
 *     // Use plugin...
 * }
 * 
 * // Introspect methods
 * auto methods = ModuleIntrospection::getMethods(handle.instance());
 * for (const auto& method : methods) {
 *     qDebug() << "Method:" << method.name << method.signature;
 * }
 * @endcode
 */

#include "module_metadata.h"
#include "module_loader.h"
#include "module_introspection.h"

#endif // MODULE_LIB_H
