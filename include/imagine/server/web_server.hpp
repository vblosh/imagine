#pragma once

#include <string>
#include <memory>
#include <thread>
#include <mutex>
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

    void setApiToken(std::string token);
    const std::string& apiToken() const { return apiToken_; }

    void setAllowedOrigin(std::string origin);
    const std::string& allowedOrigin() const { return allowedOrigin_; }

    httplib::Server& server();
    ApiRouter* router() const { return router_.get(); }

private:
    void setupStaticFileServing();

    mutable std::mutex lifecycleMutex_;
    core::Catalog& catalog_;
    std::string webRoot_;
    std::string host_{"0.0.0.0"};
    int port_{8080};
    std::string apiToken_;
    std::string allowedOrigin_{"*"};
    std::unique_ptr<httplib::Server> server_;
    std::unique_ptr<ApiRouter> router_;
    std::shared_ptr<std::thread> thread_;
    std::atomic<bool> isRunning_{false};
};

} // namespace imagine::server
