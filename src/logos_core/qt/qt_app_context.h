#ifndef QT_APP_CONTEXT_H
#define QT_APP_CONTEXT_H

namespace QtAppContext {
    void init(int argc, char* argv[]);
    int exec();
    void cleanup();
    void processEvents();
}

#endif // QT_APP_CONTEXT_H
