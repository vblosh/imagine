#include "imagine/server/web_server.hpp"
#include "imagine/server/api_router.hpp"
#include "imagine/core/catalog.hpp"
#include "imagine/common/logger.hpp"
#include <httplib.h>
#include <filesystem>
#include <fstream>

namespace imagine::server {

namespace {

std::string getStaticMimeType(const std::string& path) {
    std::string ext;
    auto dot = path.find_last_of('.');
    if (dot != std::string::npos) {
        ext = path.substr(dot);
        for (char& c : ext) c = static_cast<char>(std::tolower(c));
    }
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js") return "application/javascript";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".webp") return "image/webp";
    if (ext == ".gif") return "image/gif";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".json") return "application/json";
    if (ext == ".woff") return "font/woff";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".ttf") return "font/ttf";
    return "application/octet-stream";
}

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
        std::string mime = getStaticMimeType(path.string());

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
    : catalog_(catalog), webRoot_(std::move(webRoot)) {}

WebServer::~WebServer() {
    stop();
}

httplib::Server& WebServer::server() {
    if (!server_) {
        server_ = std::make_unique<httplib::Server>();
    }
    return *server_;
}

Status WebServer::start(const std::string& host, int port, const std::string& webRoot) {
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

    server_ = std::make_unique<httplib::Server>();
    router_ = std::make_unique<ApiRouter>(catalog_);

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
    thread_ = std::make_unique<std::thread>([this]() {
        IMAGINE_LOG_INFO("Web server started at http://" + host_ + ":" + std::to_string(port_));
        server_->listen_after_bind();
        isRunning_ = false;
    });

    return Status::ok();
}

void WebServer::stop() {
    if (!isRunning_ && !server_) {
        return;
    }
    if (server_) {
        server_->stop();
    }
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
    thread_.reset();
    server_.reset();
    router_.reset();
    isRunning_ = false;
    IMAGINE_LOG_INFO("Web server stopped");
}

void WebServer::wait() {
    if (thread_ && thread_->joinable()) {
        thread_->join();
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
    auto cache = std::make_shared<StaticFileCache>();

    server_->Get(R"(/(.*))", [root, cache](const httplib::Request& req, httplib::Response& res) {
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
            std::string filename = target.filename().string();
            if (filename == "index.html" || isSpaFallback) {
                res.set_header("Cache-Control", "no-cache");
            } else {
                res.set_header("Cache-Control", "public, max-age=86400");
            }

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

        std::filesystem::path target = root / relPath;
        if (serveCached(target, false)) {
            return;
        }

        // SPA routing fallback: serve index.html if file was not found
        std::filesystem::path indexPath = root / "index.html";
        if (serveCached(indexPath, true)) {
            return;
        }

        res.status = 404;
        res.set_content("File not found", "text/plain");
    });
}

} // namespace imagine::server
