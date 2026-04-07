#include "app_context.h"
#include <boost/asio/io_context.hpp>
#include <condition_variable>
#include <mutex>

namespace {
    std::mutex s_mutex;
    std::condition_variable s_cv;
    bool s_stop = false;
    bool s_initialized = false;
    boost::asio::io_context s_io;
}

namespace AppContext {

    void init(int /*argc*/, char* /*argv*/[]) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_initialized = true;
        s_stop = false;
    }

    int exec() {
        std::unique_lock<std::mutex> lock(s_mutex);
        if (!s_initialized)
            return -1;
        s_cv.wait(lock, [] { return s_stop; });
        return 0;
    }

    void cleanup() {
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_stop = true;
            s_initialized = false;
        }
        s_cv.notify_all();
    }

    void processEvents() {
        s_io.poll();
    }

    void requestStop() {
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_stop = true;
        }
        s_cv.notify_all();
    }
}
