#include <QCoreApplication>
#include <QDebug>
#include <QTimer>
#include <csignal>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include "command_line_parser.h"
#include "plugin_initializer.h"
#include "logos_api.h"

// Global pointer to the application for signal handler
static QCoreApplication* g_app = nullptr;

// Signal handler for graceful shutdown
static void hostSignalHandler(int signum) {
    qDebug() << "logos_host received signal:" << signum;
    if (g_app) {
        g_app->quit();
    }
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    g_app = &app;
    app.setApplicationName("logos_host");
    app.setApplicationVersion("1.0");

    // Register signal handlers for graceful shutdown
    std::signal(SIGTERM, hostSignalHandler);
    std::signal(SIGHUP, hostSignalHandler);
    std::signal(SIGINT, hostSignalHandler);

#ifdef __linux__
    // Linux: Request SIGHUP when parent process dies
    prctl(PR_SET_PDEATHSIG, SIGHUP);
    // Check if parent already died (race condition protection)
    if (getppid() == 1) {
        qDebug() << "Parent already dead (ppid=1), exiting";
        return 1;
    }
    qDebug() << "Parent death signal configured (SIGHUP on parent exit)";
#else
    // macOS/BSD: Poll parent PID to detect parent death
    pid_t originalParent = getppid();
    QTimer* parentChecker = new QTimer(&app);
    QObject::connect(parentChecker, &QTimer::timeout, [&app, originalParent]() {
        if (getppid() != originalParent) {
            qDebug() << "Parent process died (ppid changed), shutting down";
            app.quit();
        }
    });
    parentChecker->start(1000); // Check every second
    qDebug() << "Parent death detection configured (polling ppid)";
#endif

    // 1. Parse command line arguments
    PluginArgs args = parseCommandLineArgs(app);
    if (!args.valid) {
        return 1;
    }

    qDebug() << "Logos host starting for plugin:" << args.name;
    qDebug() << "Plugin path:" << args.path;

    // 2. Setup plugin (auth token, loading, and API initialization)
    LogosAPI* logos_api = setupPlugin(args.name, args.path);
    if (!logos_api) {
        return 1;
    }

    qDebug() << "Logos host ready, entering event loop...";
    
    // 3. Run event loop and cleanup
    int result = app.exec();
    delete logos_api;
    g_app = nullptr;
    qDebug() << "Logos host shutting down";

    return result;
} 
