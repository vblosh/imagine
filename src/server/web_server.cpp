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
    std::shared_ptr<const CachedStaticFile> get(const std::filesystem::path& path) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || !std::filesystem::is_regular_file(path, ec)) {
            return nullptr;
        }

        auto mtime = std::filesystem::last_write_time(path, ec);
        auto fsize = std::filesystem::file_size(path, ec);
        if (ec) return nullptr;

        std::string key = path.string();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = cache_.find(key);
            if (it != cache_.end() && it->second->lastWriteTime == mtime && it->second->fileSize == fsize) {
                return it->second;
            }
        }

        std::string content;
        if (!readTextOrBinaryFile(path, content)) {
            return nullptr;
        }

        auto mtimeSec = std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count();
        std::string etag = "\"" + std::to_string(fsize) + "-" + std::to_string(mtimeSec) + "\"";
        std::string mime = getMimeType(path.string());

        auto entry = std::make_shared<CachedStaticFile>(
            CachedStaticFile{std::move(content), std::move(mime), std::move(etag), mtime, fsize});

        {
            std::lock_guard<std::mutex> lock(mutex_);
            cache_[key] = entry;
        }

        return entry;
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<const CachedStaticFile>> cache_;
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
    const char* envPayload = std::getenv("IMAGINE_MAX_PAYLOAD_MB");
    if (envPayload && *envPayload) {
        try {
            size_t mb = std::stoul(envPayload);
            if (mb > 0) {
                payloadMaxLength_ = mb * 1024 * 1024;
            }
        } catch (const std::exception& ex) {
            IMAGINE_LOG_WARN("Failed to parse IMAGINE_MAX_PAYLOAD_MB: " + std::string(ex.what()));
        } catch (...) {
            IMAGINE_LOG_WARN("Failed to parse IMAGINE_MAX_PAYLOAD_MB: unknown exception");
        }
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

void WebServer::setPayloadMaxLength(size_t bytes) {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    payloadMaxLength_ = bytes;
    if (server_) {
        server_->set_payload_max_length(payloadMaxLength_);
    }
}

size_t WebServer::payloadMaxLength() const {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    return payloadMaxLength_;
}

httplib::Server& WebServer::server() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (!server_) {
        server_ = std::make_unique<httplib::Server>();
        server_->set_payload_max_length(payloadMaxLength_);
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
    server_->set_payload_max_length(payloadMaxLength_);

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
        IMAGINE_LOG_ERROR("Failed to bind to " + host_ + ":" + std::to_string(port_));
        server_.reset();
        router_.reset();
        return Status::ioError("Failed to bind web server to " + host_ + ":" + std::to_string(port_));
    }

    isRunning_ = true;
    stopping_ = false;
    thread_ = std::make_unique<std::thread>([this, s = server_.get()]() {
        IMAGINE_LOG_INFO("Web server started at http://" + host_ + ":" + std::to_string(port_));
        s->listen_after_bind();
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex_);
            isRunning_ = false;
        }
        cv_.notify_all();
    });

    return Status::ok();
}

void WebServer::stop() {
    IMAGINE_LOG_INFO("Web server shutting down...");
    std::unique_ptr<std::thread> t;
    std::unique_ptr<httplib::Server> s;
    std::unique_ptr<ApiRouter> r;
    {
        std::unique_lock<std::mutex> lock(lifecycleMutex_);
        if (stopping_) {
            cv_.wait(lock, [this]() { return !stopping_; });
            return;
        }
        if (!isRunning_ && !server_ && !thread_) {
            return;
        }
        stopping_ = true;
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
            if (t->get_id() != std::this_thread::get_id()) {
                t->join();
            } else {
                t->detach();
            }
        } catch (const std::exception& ex) {
            IMAGINE_LOG_WARN("Exception during web server thread shutdown: " + std::string(ex.what()));
        } catch (...) {
            IMAGINE_LOG_WARN("Unknown exception during web server thread shutdown");
        }
    }
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        stopping_ = false;
    }
    cv_.notify_all();
    IMAGINE_LOG_INFO("Web server stopped");
}

void WebServer::wait() {
    std::unique_lock<std::mutex> lock(lifecycleMutex_);
    cv_.wait(lock, [this]() {
        return !isRunning_ && !stopping_;
    });
    if (thread_) {
        lock.unlock();
        stop();
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
            auto entry = cache->get(target);
            if (!entry) {
                return false;
            }

            res.set_header("ETag", entry->etag);
            res.set_header("Cache-Control", "no-cache");

            if (req.has_header("If-None-Match") && req.get_header_value("If-None-Match") == entry->etag) {
                res.status = 304;
                return true;
            }

            res.status = 200;
            res.set_content(entry->content.data(), entry->content.size(), entry->mime);
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
