#include "module_introspection.h"
#include <QMetaObject>
#include <QMetaMethod>
#include <QDebug>

namespace ModuleLib {

QJsonObject ParameterInfo::toJson() const {
    QJsonObject obj;
    obj["name"] = name;
    obj["type"] = type;
    return obj;
}

QJsonObject MethodInfo::toJson() const {
    QJsonObject obj;
    obj["name"] = name;
    obj["signature"] = signature;
    obj["returnType"] = returnType;
    obj["isInvokable"] = isInvokable;
    
    if (!parameters.empty()) {
        QJsonArray paramsArray;
        for (const auto& param : parameters) {
            paramsArray.append(param.toJson());
        }
        obj["parameters"] = paramsArray;
    }
    
    return obj;
}

std::vector<MethodInfo> ModuleIntrospection::getMethods(QObject* plugin, bool excludeBaseClass) {
    std::vector<MethodInfo> methods;
    
    if (!plugin) {
        qWarning() << "ModuleIntrospection: Null plugin object";
        return methods;
    }
    
    const QMetaObject* metaObject = plugin->metaObject();
    
    for (int i = 0; i < metaObject->methodCount(); ++i) {
        QMetaMethod method = metaObject->method(i);
        
        // Skip methods from QObject and other base classes if requested
        if (excludeBaseClass && method.enclosingMetaObject() != metaObject) {
            continue;
        }
        
        MethodInfo info;
        info.signature = QString::fromUtf8(method.methodSignature());
        info.name = QString::fromUtf8(method.name());
        info.returnType = QString::fromUtf8(method.typeName());
        
        // Check if the method is invokable via QMetaObject::invokeMethod
        info.isInvokable = method.isValid() && 
                          (method.methodType() == QMetaMethod::Method || 
                           method.methodType() == QMetaMethod::Slot);
        
        // Extract parameter information
        if (method.parameterCount() > 0) {
            QByteArrayList paramNames = method.parameterNames();
            
            for (int p = 0; p < method.parameterCount(); ++p) {
                ParameterInfo param;
                param.type = QString::fromUtf8(method.parameterTypeName(p));
                
                // Try to get parameter name if available
                if (p < paramNames.size() && !paramNames.at(p).isEmpty()) {
                    param.name = QString::fromUtf8(paramNames.at(p));
                } else {
                    param.name = "param" + QString::number(p);
                }
                
                info.parameters.push_back(param);
            }
        }
        
        methods.push_back(info);
    }
    
    return methods;
}

QJsonArray ModuleIntrospection::getMethodsAsJson(QObject* plugin, bool excludeBaseClass) {
    QJsonArray methodsArray;
    
    auto methods = getMethods(plugin, excludeBaseClass);
    for (const auto& method : methods) {
        methodsArray.append(method.toJson());
    }
    
    return methodsArray;
}

QString ModuleIntrospection::getClassName(QObject* plugin) {
    if (!plugin) {
        return QString();
    }
    return QString::fromUtf8(plugin->metaObject()->className());
}

bool ModuleIntrospection::hasMethod(QObject* plugin, const QString& methodName) {
    if (!plugin) {
        return false;
    }
    
    auto methods = getMethods(plugin, false);
    for (const auto& method : methods) {
        if (method.name == methodName) {
            return true;
        }
    }
    
    return false;
}

} // namespace ModuleLib
