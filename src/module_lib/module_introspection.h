#ifndef MODULE_INTROSPECTION_H
#define MODULE_INTROSPECTION_H

#include <QString>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <vector>

namespace ModuleLib {

/**
 * @brief ParameterInfo represents information about a method parameter.
 */
struct ParameterInfo {
    QString name;
    QString type;
    
    QJsonObject toJson() const;
};

/**
 * @brief MethodInfo represents information about a single method.
 */
struct MethodInfo {
    QString name;
    QString signature;
    QString returnType;
    bool isInvokable = false;
    std::vector<ParameterInfo> parameters;
    
    QJsonObject toJson() const;
};

/**
 * @brief ModuleIntrospection provides runtime introspection of plugin methods.
 * 
 * This class wraps Qt's QMetaObject system to extract method information
 * from loaded plugins.
 */
class ModuleIntrospection {
public:
    /**
     * @brief Get all methods defined by a plugin object.
     * 
     * @param plugin The QObject to introspect
     * @param excludeBaseClass If true, excludes methods inherited from QObject
     * @return std::vector<MethodInfo> List of methods defined by the plugin
     */
    static std::vector<MethodInfo> getMethods(QObject* plugin, bool excludeBaseClass = true);
    
    /**
     * @brief Get all methods as a QJsonArray (for compatibility with existing code).
     * 
     * This produces the same JSON structure as the original getPluginMethods()
     * implementation in CoreManagerPlugin.
     * 
     * @param plugin The QObject to introspect
     * @param excludeBaseClass If true, excludes methods inherited from QObject
     * @return QJsonArray Array of method information objects
     */
    static QJsonArray getMethodsAsJson(QObject* plugin, bool excludeBaseClass = true);
    
    /**
     * @brief Get the class name of the plugin's meta-object.
     * 
     * @param plugin The QObject to introspect
     * @return QString The class name
     */
    static QString getClassName(QObject* plugin);
    
    /**
     * @brief Check if the plugin has a method with the given name.
     * 
     * @param plugin The QObject to check
     * @param methodName The method name to look for
     * @return bool True if the method exists
     */
    static bool hasMethod(QObject* plugin, const QString& methodName);

private:
    ModuleIntrospection() = default;  // Static class, no instantiation
};

} // namespace ModuleLib

#endif // MODULE_INTROSPECTION_H
