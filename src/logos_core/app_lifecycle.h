#ifndef APP_LIFECYCLE_H
#define APP_LIFECYCLE_H

class QCoreApplication;

namespace AppLifecycle {
    void init(int argc, char* argv[]);
    void start();
    int exec();
    void cleanup();
    void processEvents();

    QCoreApplication* app();
    bool isAppOwnedByUs();
}

#endif // APP_LIFECYCLE_H
