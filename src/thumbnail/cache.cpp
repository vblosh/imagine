#include "imagine/thumbnail/cache.hpp"
#include "imagine/thumbnail/generator.hpp"
#include <cstdlib>
#include <filesystem>

namespace imagine::thumbnail {

std::string Cache::defaultCacheDir() {
    const char* xdgCache = std::getenv("XDG_CACHE_HOME");
    if (xdgCache && *xdgCache) {
        return (std::filesystem::path(xdgCache) / "imagine" / "thumbs").string();
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return (std::filesystem::path(home) / ".cache" / "imagine" / "thumbs").string();
    }
    return "./.imagine/thumbs";
}

Cache::Cache(std::string cacheDir) {
    if (cacheDir.empty()) {
        cacheDir_ = defaultCacheDir();
    } else {
        cacheDir_ = std::move(cacheDir);
    }
}

void Cache::setCacheDir(const std::string& cacheDir) {
    std::lock_guard<std::mutex> lock(mutex_);
    cacheDir_ = cacheDir;
}

std::string Cache::getThumbnailPath(const std::string& hash, int size) const {
    if (hash.size() < 4) {
        return (std::filesystem::path(cacheDir_) / ("misc_" + hash + "_" + std::to_string(size) + ".jpg")).string();
    }
    std::string p1 = hash.substr(0, 2);
    std::string p2 = hash.substr(2, 2);
    std::string filename = hash + "_" + std::to_string(size) + ".jpg";

    return (std::filesystem::path(cacheDir_) / p1 / p2 / filename).string();
}

bool Cache::hasThumbnail(const std::string& hash, int size) const {
    std::string p = getThumbnailPath(hash, size);
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

Result<std::string> Cache::ensureThumbnail(
    const std::string& sourceImagePath,
    const std::string& hash,
    int size,
    int orientation
) {
    std::string targetPath = getThumbnailPath(hash, size);
    if (hasThumbnail(hash, size)) {
        return targetPath;
    }

    auto loadRes = Generator::loadImage(sourceImagePath);
    if (!loadRes.isOk()) {
        return loadRes.status();
    }

    auto img = std::move(loadRes.value());
    if (orientation > 1) {
        img = Generator::rotate(img, orientation);
    }

    auto resizeRes = Generator::resize(img, size);
    if (!resizeRes.isOk()) {
        return resizeRes.status();
    }

    Status saveStatus = Generator::saveJpeg(resizeRes.value(), targetPath);
    if (!saveStatus.isOk()) {
        return saveStatus;
    }

    return targetPath;
}

Result<std::pair<std::string, std::string>> Cache::ensureDualThumbnails(
    const std::string& sourceImagePath,
    const std::string& hash,
    int orientation
) {
    std::string smallPath = getThumbnailPath(hash, SmallSize);
    std::string largePath = getThumbnailPath(hash, LargeSize);

    bool hasSmall = hasThumbnail(hash, SmallSize);
    bool hasLarge = hasThumbnail(hash, LargeSize);

    if (hasSmall && hasLarge) {
        return std::make_pair(smallPath, largePath);
    }

    auto loadRes = Generator::loadImage(sourceImagePath);
    if (!loadRes.isOk()) {
        return loadRes.status();
    }

    auto img = std::move(loadRes.value());
    if (orientation > 1) {
        img = Generator::rotate(img, orientation);
    }

    // Generate large thumbnail first
    ImageBuffer largeImg;
    if (img.width > LargeSize || img.height > LargeSize) {
        auto resLarge = Generator::resize(img, LargeSize);
        if (!resLarge.isOk()) return resLarge.status();
        largeImg = std::move(resLarge.value());
    } else {
        largeImg = img;
    }

    Status saveLarge = Generator::saveJpeg(largeImg, largePath);
    if (!saveLarge.isOk()) return saveLarge;

    // Fast downscale from largeImg to small thumbnail
    auto resSmall = Generator::resize(largeImg, SmallSize);
    if (!resSmall.isOk()) return resSmall.status();

    Status saveSmall = Generator::saveJpeg(resSmall.value(), smallPath);
    if (!saveSmall.isOk()) return saveSmall;

    return std::make_pair(smallPath, largePath);
}

} // namespace imagine::thumbnail
