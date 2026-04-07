#include "host_app.h"
#include <QCoreApplication>

namespace {
    QCoreApplication* s_app = nullptr;
}

namespace HostApp {

    void init(int argc, char* argv[])
    {
        s_app = new QCoreApplication(argc, argv);
    }

    int exec()
    {
        if (!s_app)
            return -1;
        return s_app->exec();
    }

    void cleanup()
    {
        delete s_app;
        s_app = nullptr;
    }

}
