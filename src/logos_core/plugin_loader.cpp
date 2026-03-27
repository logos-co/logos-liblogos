#include "plugin_loader.h"
#include "plugin_registry.h"
#include "dependency_resolver.h"
#include "qt/qt_process_manager.h"
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QUuid>
#include <QCoreApplication>
#include "logos_api.h"
#include "logos_api_client.h"
#include "token_manager.h"

PluginLoader::PluginLoader(PluginRegistry& registry)
    : m_registry(registry)
{
}

QString PluginLoader::resolveLogosHostPath() {
    QString logosHostPath;

    QByteArray envPathBytes = qgetenv("LOGOS_HOST_PATH");
    if (!envPathBytes.isEmpty()) {
        logosHostPath = QString::fromUtf8(envPathBytes);
    }

    if (logosHostPath.isEmpty()) {
        logosHostPath = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/logos_host");
    }

    if (!QFile::exists(logosHostPath)) {
        QStringList dirs = m_registry.pluginsDirs();
        if (!dirs.isEmpty()) {
            QDir pluginsDirCandidate(dirs.first());
            QString candidate = QDir::cleanPath(pluginsDirCandidate.absoluteFilePath("../bin/logos_host"));
            if (QFile::exists(candidate)) {
                logosHostPath = candidate;
            }
        }
    }

    if (!QFile::exists(logosHostPath)) {
        qCritical() << "logos_host not found at:" << logosHostPath
                     << "- set LOGOS_HOST_PATH or place it next to the executable";
        return QString();
    }

    return logosHostPath;
}

void PluginLoader::notifyCapabilityModule(const QString& name, const QString& token) {
    if (!m_registry.isLoaded("capability_module"))
        return;

    TokenManager& tokenManager = TokenManager::instance();
    QString capabilityModuleToken = tokenManager.getToken("capability_module");

    static LogosAPI* s_coreApi = nullptr;
    if (!s_coreApi)
        s_coreApi = new LogosAPI("core");

    LogosAPIClient* client = s_coreApi->getClient("capability_module");
    if (!client->informModuleToken(capabilityModuleToken, name, token)) {
        qWarning() << "Failed to register token with capability module for:" << name;
    }
}

bool PluginLoader::loadPlugin(const QString& name) {
    if (!m_registry.isKnown(name)) {
        qWarning() << "Cannot load unknown plugin:" << name;
        return false;
    }

    if (m_registry.isLoaded(name)) {
        qWarning() << "Plugin already loaded:" << name;
        return false;
    }

    QString pluginPath = m_registry.pluginPath(name);

    QString logosHostPath = resolveLogosHostPath();
    if (logosHostPath.isEmpty())
        return false;

    std::vector<std::string> arguments = {
        "--name", name.toStdString(),
        "--path", pluginPath.toStdString()
    };

    PluginRegistry* registry = &m_registry;

    QtProcessManager::ProcessCallbacks callbacks;

    callbacks.onFinished = [registry](const std::string& pluginName, int exitCode, bool crashed) {
        Q_UNUSED(exitCode);
        QString qName = QString::fromStdString(pluginName);
        if (crashed) {
            qCritical() << "Plugin process crashed:" << qName;
            exit(1);
        }
        registry->markUnloaded(qName);
    };

    callbacks.onError = [](const std::string& pluginName, bool crashed) {
        if (crashed) {
            qCritical() << "Plugin process crashed:" << QString::fromStdString(pluginName);
            exit(1);
        }
    };

    callbacks.onOutput = [](const std::string& pluginName, const std::string& line, bool isStderr) {
        QString qName = QString::fromStdString(pluginName);
        QString qLine = QString::fromStdString(line);
        if (isStderr) {
            qCritical() << "[" << qName << "]" << qLine;
        } else if (qLine.contains("Warning:") || qLine.contains("WARNING:")) {
            qWarning() << "[" << qName << "]" << qLine;
        } else if (qLine.contains("Critical:") || qLine.contains("FAILED:") || qLine.contains("ERROR:")) {
            qCritical() << "[" << qName << "]" << qLine;
        }
    };

    if (!QtProcessManager::startProcess(name.toStdString(), logosHostPath.toStdString(), arguments, callbacks)) {
        return false;
    }

    QUuid authToken = QUuid::createUuid();
    QString authTokenString = authToken.toString(QUuid::WithoutBraces);

    if (!QtProcessManager::sendToken(name.toStdString(), authTokenString.toStdString())) {
        return false;
    }

    m_registry.markLoaded(name);

    TokenManager& tokenManager = TokenManager::instance();
    tokenManager.saveToken(name, authTokenString);

    notifyCapabilityModule(name, authTokenString);

    qInfo() << "Plugin loaded:" << name;

    return true;
}

bool PluginLoader::loadPluginWithDependencies(const QString& name) {
    QStringList requested;
    requested.append(name);

    QStringList resolved = DependencyResolver::resolve(
        requested,
        [this](const QString& n) { return m_registry.isKnown(n); },
        [this](const QString& n) { return m_registry.pluginDependencies(n); }
    );

    if (resolved.isEmpty() || !resolved.contains(name)) {
        qWarning() << "Cannot resolve dependencies for:" << name;
        return false;
    }

    bool allSucceeded = true;
    for (const QString& moduleName : resolved) {
        if (m_registry.isLoaded(moduleName))
            continue;
        if (!loadPlugin(moduleName)) {
            qWarning() << "Failed to load plugin:" << moduleName;
            allSucceeded = false;
        }
    }

    return allSucceeded;
}

bool PluginLoader::initializeCapabilityModule() {
    if (!m_registry.isKnown("capability_module"))
        return false;

    if (!loadPlugin("capability_module")) {
        qWarning() << "Failed to load capability module";
        return false;
    }

    return true;
}

bool PluginLoader::unloadPlugin(const QString& name) {
    if (!m_registry.isLoaded(name)) {
        qWarning() << "Cannot unload plugin (not loaded):" << name;
        return false;
    }

    if (!QtProcessManager::hasProcess(name.toStdString())) {
        qWarning() << "No process found for plugin:" << name;
        return false;
    }

    QtProcessManager::terminateProcess(name.toStdString());
    m_registry.markUnloaded(name);

    qInfo() << "Plugin unloaded:" << name;
    return true;
}

void PluginLoader::terminateAll() {
    QtProcessManager::terminateAll();
    m_registry.clearLoaded();
}

QHash<QString, qint64> PluginLoader::getPluginProcessIds() {
    auto stdMap = QtProcessManager::getAllProcessIds();
    QHash<QString, qint64> result;
    for (const auto& [name, pid] : stdMap) {
        result.insert(QString::fromStdString(name), pid);
    }
    return result;
}
