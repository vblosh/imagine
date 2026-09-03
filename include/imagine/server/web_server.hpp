#pragma once

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include "imagine/common/error.hpp"

namespace httplib {
class Server;
}

namespace imagine::core {
class Catalog;
}

namespace imagine::server {

class ApiRouter;

class WebServer {
public:
    explicit WebServer(core::Catalog& catalog, std::string webRoot = "web");
    ~WebServer();

    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    Status start(const std::string& host = "0.0.0.0", int port = 8080, const std::string& webRoot = "");
    void stop();
    void wait();
    Status run(const std::string& host = "0.0.0.0", int port = 8080, const std::string& webRoot = "");

    bool isRunning() const;
    int port() const { return port_; }
    const std::string& host() const { return host_; }
    const std::string& webRoot() const { return webRoot_; }

    httplib::Server& server();

private:
    void setupStaticFileServing();

    core::Catalog& catalog_;
    std::string webRoot_;
    std::string host_{"0.0.0.0"};
    int port_{8080};
    std::unique_ptr<httplib::Server> server_;
    std::unique_ptr<ApiRouter> router_;
    std::unique_ptr<std::thread> thread_;
    std::atomic<bool> isRunning_{false};
};

} // namespace imagine::server
