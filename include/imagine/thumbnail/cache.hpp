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
    std::string cacheDir() const;

    std::string getThumbnailPath(const std::string& hash, int size) const;
    static std::string getRelativeThumbnailPath(const std::string& hash, int size);
    bool hasThumbnail(const std::string& hash, int size) const;

    /// Generates or returns a single cached thumbnail.
    /// @note Cached thumbnails on disk are pre-rotated according to orientation.
    Result<std::string> ensureThumbnail(
        const std::string& sourceImagePath,
        const std::string& hash,
        int size,
        int orientation = 1
    );

    /// Ensures both small and large thumbnails exist for a file on disk.
    /// @note Cached thumbnails on disk are pre-rotated according to orientation.
    Result<std::pair<std::string, std::string>> ensureDualThumbnails(
        const std::string& sourceImagePath,
        const std::string& hash,
        int orientation = 1
    );

    /// Ensures both small and large thumbnails exist for an in-memory image buffer.
    /// @note Cached thumbnails on disk are pre-rotated according to orientation.
    ///       outWidth and outHeight receive authentic pre-orientation sensor dimensions,
    ///       matching the MediaItem and database schema expectations.
    Result<std::pair<std::string, std::string>> ensureDualThumbnailsFromMemory(
        const uint8_t* data,
        size_t size,
        const std::string& hash,
        int orientation = 1,
        int* outWidth = nullptr,
        int* outHeight = nullptr
    );

    Result<std::pair<std::string, std::string>> ensureProceduralThumbnails(
        const std::string& hash,
        const std::string& mediaType,
        const std::string& label = ""
    );

    static std::string defaultCacheDir();

private:
    std::string cacheDir_;
    mutable std::mutex mutex_;
};

} // namespace imagine::thumbnail
