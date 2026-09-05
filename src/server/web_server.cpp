#include "imagine/server/web_server.hpp"
#include "imagine/server/api_router.hpp"
#include "imagine/server/mime_types.hpp"
#include "imagine/core/catalog.hpp"
#include "imagine/common/logger.hpp"
#include <httplib.h>
#include <filesystem>
#include <fstream>
#include <cstdlib>

namespace imagine::server {

namespace {

bool readTextOrBinaryFile(const std::filesystem::path& path, std::string& out) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) return false;
    auto size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    out.resize(size);
    if (!ifs.read(out.data(), size)) return false;
    return true;
}

struct CachedStaticFile {
    std::string content;
    std::string mime;
    std::string etag;
    std::filesystem::file_time_type lastWriteTime;
    uintmax_t fileSize{0};
};

class StaticFileCache {
public:
    bool get(const std::filesystem::path& path, std::string& outContent, std::string& outMime, std::string& outEtag) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || !std::filesystem::is_regular_file(path, ec)) {
            return false;
        }

        auto mtime = std::filesystem::last_write_time(path, ec);
        auto fsize = std::filesystem::file_size(path, ec);
        if (ec) return false;

        std::string key = path.string();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = cache_.find(key);
            if (it != cache_.end() && it->second.lastWriteTime == mtime && it->second.fileSize == fsize) {
                outContent = it->second.content;
                outMime = it->second.mime;
                outEtag = it->second.etag;
                return true;
            }
        }

        std::string content;
        if (!readTextOrBinaryFile(path, content)) {
            return false;
        }

        auto mtimeSec = std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count();
        std::string etag = "\"" + std::to_string(fsize) + "-" + std::to_string(mtimeSec) + "\"";
        std::string mime = getMimeType(path.string());

        {
            std::lock_guard<std::mutex> lock(mutex_);
            cache_[key] = CachedStaticFile{content, mime, etag, mtime, fsize};
        }

        outContent = std::move(content);
        outMime = std::move(mime);
        outEtag = std::move(etag);
        return true;
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::string, CachedStaticFile> cache_;
};

} // anonymous namespace

WebServer::WebServer(core::Catalog& catalog, std::string webRoot)
    : catalog_(catalog), webRoot_(std::move(webRoot)) {
    const char* envToken = std::getenv("IMAGINE_API_TOKEN");
    if (envToken && *envToken) {
        apiToken_ = envToken;
    }
    const char* envOrigin = std::getenv("IMAGINE_ALLOWED_ORIGIN");
    if (envOrigin && *envOrigin) {
        allowedOrigin_ = envOrigin;
    }
}

WebServer::~WebServer() {
    stop();
}

void WebServer::setApiToken(std::string token) {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    apiToken_ = std::move(token);
    if (router_) {
        router_->setApiToken(apiToken_);
    }
}

void WebServer::setAllowedOrigin(std::string origin) {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    allowedOrigin_ = std::move(origin);
    if (router_) {
        router_->setAllowedOrigin(allowedOrigin_);
    }
}

httplib::Server& WebServer::server() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (!server_) {
        server_ = std::make_unique<httplib::Server>();
    }
    return *server_;
}

Status WebServer::start(const std::string& host, int port, const std::string& webRoot) {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (isRunning_) {
        return Status::ok();
    }
    if (!catalog_.isOpen()) {
        return Status::internal("Catalog is not open");
    }

    host_ = host;
    port_ = port;
    if (!webRoot.empty()) {
        webRoot_ = webRoot;
    }

    if (host_ == "0.0.0.0" && apiToken_.empty()) {
        IMAGINE_LOG_WARN("WebServer bound to 0.0.0.0 without API token authentication configured");
    }

    server_ = std::make_unique<httplib::Server>();
    server_->set_payload_max_length(10 * 1024 * 1024); // 10MB payload limit

    router_ = std::make_unique<ApiRouter>(catalog_);
    if (!apiToken_.empty()) {
        router_->setApiToken(apiToken_);
    }
    if (!allowedOrigin_.empty()) {
        router_->setAllowedOrigin(allowedOrigin_);
    }

    // 1. Register API Routes first
    router_->registerRoutes(*server_);

    // 2. Register Static Files & SPA fallback route
    setupStaticFileServing();

    if (!server_->bind_to_port(host_.c_str(), port_)) {
        server_.reset();
        router_.reset();
        return Status::ioError("Failed to bind web server to " + host_ + ":" + std::to_string(port_));
    }

    isRunning_ = true;
    thread_ = std::make_shared<std::thread>([this, s = server_.get()]() {
        IMAGINE_LOG_INFO("Web server started at http://" + host_ + ":" + std::to_string(port_));
        s->listen_after_bind();
        isRunning_ = false;
    });

    return Status::ok();
}

void WebServer::stop() {
    std::shared_ptr<std::thread> t;
    std::unique_ptr<httplib::Server> s;
    std::unique_ptr<ApiRouter> r;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (!isRunning_ && !server_) {
            return;
        }
        r = std::move(router_);
        s = std::move(server_);
        t = std::move(thread_);
        isRunning_ = false;
    }
    if (r) {
        r->stopImport();
    }
    if (s) {
        s->stop();
    }
    if (t && t->joinable()) {
        try {
            t->join();
        } catch (...) {}
    }
    IMAGINE_LOG_INFO("Web server stopped");
}

void WebServer::wait() {
    std::shared_ptr<std::thread> t;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        t = thread_;
    }
    if (t && t->joinable()) {
        try {
            t->join();
        } catch (...) {}
    }
}

Status WebServer::run(const std::string& host, int port, const std::string& webRoot) {
    Status s = start(host, port, webRoot);
    if (!s.isOk()) {
        return s;
    }
    wait();
    return Status::ok();
}

bool WebServer::isRunning() const {
    return isRunning_.load();
}

void WebServer::setupStaticFileServing() {
    std::filesystem::path root(webRoot_);
    std::error_code ec;
    std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(root, ec);
    if (ec) {
        canonicalRoot = root.lexically_normal();
    }

    auto cache = std::make_shared<StaticFileCache>();

    server_->Get(R"(/(.*))", [canonicalRoot, cache](const httplib::Request& req, httplib::Response& res) {
        // Don't intercept API paths
        if (req.path.rfind("/api/", 0) == 0) {
            res.status = 404;
            res.set_content(R"({"error":"Not found"})", "application/json");
            return;
        }

        auto serveCached = [&](const std::filesystem::path& target, bool isSpaFallback) -> bool {
            std::string content, mime, etag;
            if (!cache->get(target, content, mime, etag)) {
                return false;
            }

            res.set_header("ETag", etag);
            res.set_header("Cache-Control", "no-cache");

            if (req.has_header("If-None-Match") && req.get_header_value("If-None-Match") == etag) {
                res.status = 304;
                return true;
            }

            res.status = 200;
            res.set_content(std::move(content), mime);
            return true;
        };

        std::string relPath = req.matches[1];
        if (relPath.empty() || relPath == "/") {
            relPath = "index.html";
        }

        // Sanitize leading slashes
        while (!relPath.empty() && (relPath.front() == '/' || relPath.front() == '\\')) {
            relPath.erase(0, 1);
        }

        std::error_code ec;
        std::filesystem::path target = std::filesystem::weakly_canonical(canonicalRoot / relPath, ec);
        if (ec) {
            res.status = 404;
            res.set_content("File not found", "text/plain");
            return;
        }

        std::string rootStr = canonicalRoot.string();
        if (!rootStr.empty() && rootStr.back() != std::filesystem::path::preferred_separator) {
            rootStr += std::filesystem::path::preferred_separator;
        }
        std::string targetStr = target.string();

        // Enforce boundary check to prevent path traversal and symlink escape
        if (targetStr != canonicalRoot.string() && targetStr.rfind(rootStr, 0) != 0) {
            res.status = 403;
            res.set_content("Access denied", "text/plain");
            return;
        }

        if (std::filesystem::is_regular_file(target, ec)) {
            if (serveCached(target, false)) {
                return;
            }
        }

        // SPA routing fallback: serve index.html if file was not found
        std::filesystem::path indexPath = std::filesystem::weakly_canonical(canonicalRoot / "index.html", ec);
        if (!ec && std::filesystem::is_regular_file(indexPath, ec)) {
            std::string indexStr = indexPath.string();
            if ((indexStr == canonicalRoot.string() || indexStr.rfind(rootStr, 0) == 0) &&
                serveCached(indexPath, true)) {
                return;
            }
        }

        res.status = 404;
        res.set_content("File not found", "text/plain");
    });
}

} // namespace imagine::server
