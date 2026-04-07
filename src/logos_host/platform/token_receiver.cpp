#include "token_receiver.h"
#include "ipc_paths.h"
#include "logos_logging.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace TokenReceiver {

    std::string receiveAuthToken(const std::string& pluginName)
    {
        const auto path = logos_token_socket_path(pluginName);
        const std::string pathStr = path.string();
        if (pathStr.size() >= sizeof(sockaddr_un::sun_path)) {
            logos_log_critical("Token socket path too long for {}", pluginName);
            return {};
        }

        ::unlink(pathStr.c_str());

        const int serverFd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (serverFd < 0) {
            logos_log_critical("socket() failed: {}", strerror(errno));
            return {};
        }

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, pathStr.c_str(), sizeof(addr.sun_path) - 1);

        if (bind(serverFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            logos_log_critical("Failed to bind token server: {}", strerror(errno));
            close(serverFd);
            return {};
        }

        if (listen(serverFd, 1) != 0) {
            logos_log_critical("listen() failed: {}", strerror(errno));
            close(serverFd);
            ::unlink(pathStr.c_str());
            return {};
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(serverFd, &rfds);
        timeval tv;
        tv.tv_sec = 10;
        tv.tv_usec = 0;

        if (select(serverFd + 1, &rfds, nullptr, nullptr, &tv) <= 0) {
            logos_log_critical("Timeout waiting for auth token");
            close(serverFd);
            ::unlink(pathStr.c_str());
            return {};
        }

        const int clientFd = accept(serverFd, nullptr, nullptr);
        close(serverFd);
        ::unlink(pathStr.c_str());

        if (clientFd < 0) {
            logos_log_critical("accept() failed: {}", strerror(errno));
            return {};
        }

        std::vector<char> buf(4096);
        std::string authToken;
        for (;;) {
            const ssize_t n = read(clientFd, buf.data(), buf.size());
            if (n <= 0)
                break;
            authToken.append(buf.data(), static_cast<size_t>(n));
        }
        close(clientFd);

        if (authToken.empty()) {
            logos_log_critical("No auth token received");
            return {};
        }

        return authToken;
    }

}
