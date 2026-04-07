#include "token_receiver.h"
#include "ipc_paths.h"
#include "logos_logging.h"
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <sys/un.h>
#include <unistd.h>

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

        boost::asio::io_context io;
        using local = boost::asio::local::stream_protocol;
        local::endpoint ep(pathStr);

        try {
            local::acceptor acceptor(io, ep);

            local::socket sock(io);
            bool acceptedOk = false;
            boost::asio::steady_timer timer(io);
            timer.expires_after(std::chrono::seconds(10));

            acceptor.async_accept(sock, [&](boost::system::error_code ec) {
                if (!ec)
                    acceptedOk = true;
                timer.cancel();
            });

            timer.async_wait([&](boost::system::error_code ec) {
                if (ec == boost::asio::error::operation_aborted)
                    return;
                if (!ec) {
                    boost::system::error_code closeEc;
                    acceptor.close(closeEc);
                }
            });

            io.run();

            boost::system::error_code closeEc;
            acceptor.close(closeEc);
            ::unlink(pathStr.c_str());

            if (!acceptedOk) {
                logos_log_critical("Timeout waiting for auth token");
                return {};
            }

            std::string authToken;
            boost::system::error_code readEc;
            boost::asio::read(sock, boost::asio::dynamic_buffer(authToken), readEc);
            if (readEc && readEc != boost::asio::error::eof) {
                logos_log_critical("read token failed: {}", readEc.message());
                return {};
            }

            if (authToken.empty()) {
                logos_log_critical("No auth token received");
                return {};
            }

            return authToken;
        } catch (const std::exception& ex) {
            logos_log_critical("Failed to bind token server: {}", ex.what());
            ::unlink(pathStr.c_str());
            return {};
        }
    }

}
