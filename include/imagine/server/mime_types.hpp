#pragma once

#include <string>
#include <string_view>
#include <cctype>

namespace imagine::server {

inline std::string getMimeType(std::string_view path) {
    auto dot = path.rfind('.');
    if (dot == std::string_view::npos) {
        return "application/octet-stream";
    }
    std::string ext(path.substr(dot));
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js" || ext == ".mjs") return "application/javascript";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".webp") return "image/webp";
    if (ext == ".gif") return "image/gif";
    if (ext == ".bmp") return "image/bmp";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".json") return "application/json";
    if (ext == ".woff") return "font/woff";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".ttf") return "font/ttf";
    if (ext == ".otf") return "font/otf";
    if (ext == ".txt") return "text/plain; charset=utf-8";
    if (ext == ".xml") return "application/xml";
    if (ext == ".map") return "application/json";
    return "application/octet-stream";
}

} // namespace imagine::server
