#pragma once

#include <string>
#include <utility>
#include <mutex>
#include "imagine/common/error.hpp"

namespace imagine::thumbnail {

class Cache {
public:
    static constexpr int SmallSize = 256;
    static constexpr int LargeSize = 1024;

    explicit Cache(std::string cacheDir = "");

    void setCacheDir(const std::string& cacheDir);
    const std::string& cacheDir() const { return cacheDir_; }

    std::string getThumbnailPath(const std::string& hash, int size) const;
    static std::string getRelativeThumbnailPath(const std::string& hash, int size);
    bool hasThumbnail(const std::string& hash, int size) const;

    Result<std::string> ensureThumbnail(
        const std::string& sourceImagePath,
        const std::string& hash,
        int size,
        int orientation = 1
    );

    Result<std::pair<std::string, std::string>> ensureDualThumbnails(
        const std::string& sourceImagePath,
        const std::string& hash,
        int orientation = 1
    );

    Result<std::pair<std::string, std::string>> ensureDualThumbnailsFromMemory(
        const uint8_t* data,
        size_t size,
        const std::string& hash,
        int orientation = 1,
        int* outWidth = nullptr,
        int* outHeight = nullptr
    );

    static std::string defaultCacheDir();

private:
    std::string cacheDir_;
    mutable std::mutex mutex_;
};

} // namespace imagine::thumbnail
