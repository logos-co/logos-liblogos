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

    qDebug() << "Logos host path (resolved):" << logosHostPath;

    if (!QFile::exists(logosHostPath)) {
        qCritical() << "logos_host executable not found at:" << logosHostPath;
        qCritical() << "Set environment variable LOGOS_HOST_PATH to the absolute path of logos_host or ensure it is next to the Electron executable or under ../bin from the plugins directory.";
        return QString();
    }

    return logosHostPath;
}

void PluginLoader::notifyCapabilityModule(const QString& name, const QString& token) {
    if (!m_registry.isLoaded("capability_module"))  {
        qDebug() << "Capability module not loaded, skipping token notification";
        return;
    }

    qDebug() << "Informing capability module about new module token for:" << name;

    TokenManager& tokenManager = TokenManager::instance();
    QString capabilityModuleToken = tokenManager.getToken("capability_module");
    qDebug() << "Capability module token:" << capabilityModuleToken;

    static LogosAPI* s_coreApi = nullptr;
    if (!s_coreApi)
        s_coreApi = new LogosAPI("core");

    LogosAPIClient* client = s_coreApi->getClient("capability_module");
    bool success = client->informModuleToken(capabilityModuleToken, name, token);
    if (success) {
        qDebug() << "Successfully informed capability module about token for:" << name;
    } else {
        qWarning() << "Failed to inform capability module about token for:" << name;
    }
}

bool PluginLoader::loadPlugin(const QString& name) {
    qDebug() << "Attempting to load plugin by name:" << name;

    if (!m_registry.isKnown(name)) {
        qWarning() << "Plugin not found among known plugins:" << name;
        return false;
    }

    QString pluginPath = m_registry.pluginPath(name);
    qDebug() << "Loading plugin:" << name << "from path:" << pluginPath << "in separate process";

    if (m_registry.isLoaded(name)) {
        qWarning() << "Plugin already loaded:" << name;
        return false;
    }

    QString logosHostPath = resolveLogosHostPath();
    if (logosHostPath.isEmpty())
        return false;

    std::vector<std::string> arguments = {
        "--name", name.toStdString(),
        "--path", pluginPath.toStdString()
    };

    qDebug() << "Starting logos_host with arguments:" << name << pluginPath;

    PluginRegistry* registry = &m_registry;

    QtProcessManager::ProcessCallbacks callbacks;

    callbacks.onFinished = [registry](const std::string& pluginName, int exitCode, bool crashed) {
        Q_UNUSED(exitCode);
        QString qName = QString::fromStdString(pluginName);
        if (crashed) {
            qCritical() << "Plugin process crashed:" << qName << "- terminating core with error";
            exit(1);
        }
        registry->markUnloaded(qName);
    };

    callbacks.onError = [](const std::string& pluginName, bool crashed) {
        if (crashed) {
            qCritical() << "Plugin process crashed:" << QString::fromStdString(pluginName) << "- terminating core with error";
            exit(1);
        }
    };

    callbacks.onOutput = [](const std::string& pluginName, const std::string& line, bool isStderr) {
        QString qName = QString::fromStdString(pluginName);
        QString qLine = QString::fromStdString(line);
        if (isStderr) {
            qCritical() << "[LOGOS_HOST" << qName << "] STDERR:" << qLine;
        } else if (qLine.contains("qrc:") || qLine.contains("Warning:") || qLine.contains("WARNING:")) {
            qWarning() << "[LOGOS_HOST" << qName << "]:" << qLine;
        } else if (qLine.contains("Critical:") || qLine.contains("FAILED:") || qLine.contains("ERROR:")) {
            qCritical() << "[LOGOS_HOST" << qName << "]:" << qLine;
        } else {
            qDebug() << "[LOGOS_HOST" << qName << "]:" << qLine;
        }
    };

    if (!QtProcessManager::startProcess(name.toStdString(), logosHostPath.toStdString(), arguments, callbacks)) {
        return false;
    }

    QUuid authToken = QUuid::createUuid();
    QString authTokenString = authToken.toString(QUuid::WithoutBraces);
    qDebug() << "Generated auth token:" << authTokenString;

    if (!QtProcessManager::sendToken(name.toStdString(), authTokenString.toStdString())) {
        return false;
    }

    qDebug() << "Auth token sent securely to plugin:" << name;

    m_registry.markLoaded(name);

    TokenManager& tokenManager = TokenManager::instance();
    tokenManager.saveToken(name, authTokenString);

    notifyCapabilityModule(name, authTokenString);

    qDebug() << "Plugin" << name << "is now running in separate process";
    qDebug() << "Remote registry URL for this plugin: local:logos_" << name;

    return true;
}

bool PluginLoader::loadPluginWithDependencies(const QString& name) {
    QStringList requested;
    requested.append(name);

    QStringList resolved = DependencyResolver::resolve(
        requested,
        [this](const QString& n) { return m_registry.isKnown(n); },
        [this](const QString& n) { return m_registry.pluginMetadata(n); }
    );

    if (resolved.isEmpty() || !resolved.contains(name)) {
        qWarning() << "Cannot load plugin: plugin not found:" << name;
        return false;
    }

    bool allSucceeded = true;
    for (const QString& moduleName : resolved) {
        if (m_registry.isLoaded(moduleName)) {
            qDebug() << "Plugin already loaded, skipping:" << moduleName;
            continue;
        }
        if (!loadPlugin(moduleName)) {
            qWarning() << "Failed to load module:" << moduleName;
            allSucceeded = false;
        }
    }

    return allSucceeded;
}

bool PluginLoader::initializeCapabilityModule() {
    qDebug() << "\n=== Initializing Capability Module ===";

    if (!m_registry.isKnown("capability_module")) {
        qDebug() << "Capability module not found in known plugins, skipping initialization";
        return false;
    }

    qDebug() << "Capability module found, attempting to load...";

    bool success = loadPlugin("capability_module");

    if (!success) {
        qDebug() << "Failed to load capability module";
        return false;
    }

    qDebug() << "Capability module loaded successfully";
    return true;
}

bool PluginLoader::unloadPlugin(const QString& name) {
    qDebug() << "Attempting to unload plugin by name:" << name;

    if (!m_registry.isLoaded(name)) {
        qWarning() << "Plugin not loaded, cannot unload:" << name;
        qDebug() << "Loaded plugins:" << m_registry.loadedPluginNames();
        return false;
    }

    if (!QtProcessManager::hasProcess(name.toStdString())) {
        qWarning() << "No process found for plugin:" << name;
        return false;
    }

    QtProcessManager::terminateProcess(name.toStdString());

    m_registry.markUnloaded(name);

    qDebug() << "Successfully unloaded plugin:" << name;
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
