#ifndef APP_LIFECYCLE_H
#define APP_LIFECYCLE_H

namespace AppLifecycle {
    void init(int argc, char* argv[]);
    void start();
    int exec();
    void cleanup();
    void processEvents();
}

#endif // APP_LIFECYCLE_H
