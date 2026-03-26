#include "qt_app_context.h"
#include <QCoreApplication>
#include <QMetaType>

Q_DECLARE_METATYPE(QObject*)

namespace {
    QCoreApplication* s_app = nullptr;
    QCoreApplication* s_owned_app = nullptr;
}

namespace QtAppContext {

    void init(int argc, char* argv[]) {
        if (QCoreApplication::instance()) {
            s_app = QCoreApplication::instance();
        } else {
            s_app = new QCoreApplication(argc, argv);
            s_owned_app = s_app;
        }

        qRegisterMetaType<QObject*>("QObject*");
    }

    int exec() {
        if (!s_app) return -1;
        return s_app->exec();
    }

    void cleanup() {
        delete s_owned_app;
        s_owned_app = nullptr;
        s_app = nullptr;
    }

    void processEvents() {
        if (!s_app) return;
        s_app->processEvents();
    }

}
