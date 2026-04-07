#ifndef APP_CONTEXT_H
#define APP_CONTEXT_H

namespace AppContext {
    void init(int argc, char* argv[]);
    int exec();
    void cleanup();
    void processEvents();
    void requestStop();
}

#endif
