#include "imagine/thumbnail/cache.hpp"
#include "imagine/thumbnail/generator.hpp"
#include "imagine/common/types.hpp"
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace imagine::thumbnail {

namespace {

// Marker lives next to the small thumbnail, e.g. ab/cd/hash_256.jpg.placeholder.
std::string placeholderMarkerRelativePath(const std::string& hash) {
    return Cache::getRelativeThumbnailPath(hash, Cache::SmallSize) + ".placeholder";
}

} // anonymous namespace

std::string Cache::defaultCacheDir() {
    const char* xdgCache = std::getenv("XDG_CACHE_HOME");
    if (xdgCache && *xdgCache) {
        return pathToUtf8(pathFromUtf8(xdgCache) / "imagine" / "thumbs");
    }
#if defined(_WIN32)
    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (localAppData && *localAppData) {
        return pathToUtf8(pathFromUtf8(localAppData) / "imagine" / "thumbs");
    }
#endif
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return pathToUtf8(pathFromUtf8(home) / ".cache" / "imagine" / "thumbs");
    }
#if defined(_WIN32)
    const char* userProfile = std::getenv("USERPROFILE");
    if (userProfile && *userProfile) {
        return pathToUtf8(pathFromUtf8(userProfile) / ".cache" / "imagine" / "thumbs");
    }
#endif
    return "./.imagine/thumbs";
}

Cache::Cache(std::string cacheDir) {
    if (cacheDir.empty()) {
        cacheDir_ = defaultCacheDir();
    } else {
        cacheDir_ = std::move(cacheDir);
    }
}

std::string Cache::cacheDir() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cacheDir_;
}

void Cache::setCacheDir(const std::string& cacheDir) {
    std::lock_guard<std::mutex> lock(mutex_);
    cacheDir_ = cacheDir;
}

std::string Cache::getRelativeThumbnailPath(const std::string& hash, int size) {
    std::string safeHash;
    safeHash.reserve(hash.size());
    for (unsigned char c : hash) {
        if (std::isalnum(c)) {
            safeHash.push_back(static_cast<char>(c));
        }
    }
    if (safeHash.size() < 4) {
        return "misc_" + safeHash + "_" + std::to_string(size) + ".jpg";
    }
    std::string p1 = safeHash.substr(0, 2);
    std::string p2 = safeHash.substr(2, 2);
    std::string filename = safeHash + "_" + std::to_string(size) + ".jpg";

    return (std::filesystem::path(p1) / p2 / filename).generic_string();
}

std::string Cache::getThumbnailPath(const std::string& hash, int size) const {
    std::string baseDir;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        baseDir = cacheDir_;
    }
    return pathToUtf8((pathFromUtf8(baseDir) / pathFromUtf8(getRelativeThumbnailPath(hash, size))).make_preferred());
}

bool Cache::hasThumbnail(const std::string& hash, int size) const {
    std::string p = getThumbnailPath(hash, size);
    std::error_code ec;
    auto fsPath = pathFromUtf8(p);
    return std::filesystem::is_regular_file(fsPath, ec) && !ec &&
           std::filesystem::file_size(fsPath, ec) > 0 && !ec;
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
    // Derive small sizes from the cached large thumbnail to avoid re-decoding
    // the source image. Larger sizes decode the source to preserve quality;
    // the video on-demand path derives explicitly from the cached frame.
    if (size <= SmallSize && hasThumbnail(hash, LargeSize)) {
        auto largeRes = Generator::loadImage(getThumbnailPath(hash, LargeSize));
        if (largeRes.isOk()) {
            auto resSmall = Generator::resize(largeRes.value(), size);
            if (resSmall.isOk()) {
                Status saveSmall = Generator::saveJpegFast(resSmall.value(), targetPath);
                if (saveSmall.isOk()) {
                    return targetPath;
                }
            }
        }
    }

    auto loadRes = Generator::loadImage(sourceImagePath);
    if (!loadRes.isOk()) {
        return loadRes.status();
    }

    auto img = std::move(loadRes.value());
    auto resizeRes = Generator::resize(img, size);
    if (!resizeRes.isOk()) {
        return resizeRes.status();
    }
    img = std::move(resizeRes.value());

    if (orientation > 1) {
        img = Generator::rotate(img, orientation);
    }

    Status saveStatus = Generator::saveJpegFast(img, targetPath);
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

    if (hasLarge && !hasSmall) {
        auto largeRes = Generator::loadImage(largePath);
        if (largeRes.isOk()) {
            auto resSmall = Generator::resize(largeRes.value(), SmallSize);
            if (resSmall.isOk()) {
                Status saveSmall = Generator::saveJpegFast(resSmall.value(), smallPath);
                if (saveSmall.isOk()) {
                    return std::make_pair(smallPath, largePath);
                }
            }
        }
    }

    auto loadBytesRes = Generator::loadImageBytes(sourceImagePath);
    if (!loadBytesRes.isOk()) {
        return loadBytesRes.status();
    }

    return ensureDualThumbnailsFromMemory(
        loadBytesRes.value().data(),
        loadBytesRes.value().size(),
        hash,
        orientation
    );
}

Result<std::pair<std::string, std::string>> Cache::ensureDualThumbnailsFromMemory(
    const uint8_t* data,
    size_t size,
    const std::string& hash,
    int orientation,
    int* outWidth,
    int* outHeight
) {
    if (outWidth) *outWidth = 0;
    if (outHeight) *outHeight = 0;

    std::string smallPath = getThumbnailPath(hash, SmallSize);
    std::string largePath = getThumbnailPath(hash, LargeSize);

    bool hasSmall = hasThumbnail(hash, SmallSize);
    bool hasLarge = hasThumbnail(hash, LargeSize);

    if (hasSmall && hasLarge) {
        if (outWidth || outHeight) {
            auto dimRes = Generator::getImageDimensionsFromMemory(data, size);
            if (dimRes.isOk()) {
                if (outWidth) *outWidth = dimRes.value().first;
                if (outHeight) *outHeight = dimRes.value().second;
            }
        }
        return std::make_pair(smallPath, largePath);
    }

    if (hasLarge && !hasSmall) {
        auto largeRes = Generator::loadImage(largePath);
        if (largeRes.isOk()) {
            auto resSmall = Generator::resize(largeRes.value(), SmallSize);
            if (resSmall.isOk()) {
                Status saveSmall = Generator::saveJpegFast(resSmall.value(), smallPath);
                if (saveSmall.isOk()) {
                    if (outWidth || outHeight) {
                        auto dimRes = Generator::getImageDimensionsFromMemory(data, size);
                        if (dimRes.isOk()) {
                            if (outWidth) *outWidth = dimRes.value().first;
                            if (outHeight) *outHeight = dimRes.value().second;
                        }
                    }
                    return std::make_pair(smallPath, largePath);
                }
            }
        }
    }

    ImageBuffer img;
    int origW = 0, origH = 0;

#if IMAGINE_HAS_TURBOJPEG
    if (Generator::isJpeg(data, size)) {
        // scaleDenom = 0 triggers adaptive 1/2 IDCT decode when max(w,h) >= 2048,
        // while populating authentic origW and origH
        auto tjRes = Generator::loadImageFromMemoryTurboJpeg(data, size, 0, &origW, &origH);
        if (tjRes.isOk()) {
            img = std::move(tjRes.value());
            if (outWidth) *outWidth = origW;
            if (outHeight) *outHeight = origH;
        }
    }
#endif

    if (img.data.empty()) {
        auto loadRes = Generator::loadImageFromMemory(data, size);
        if (!loadRes.isOk()) {
            return loadRes.status();
        }
        img = std::move(loadRes.value());
        if (outWidth) *outWidth = img.width;
        if (outHeight) *outHeight = img.height;
    }

    // Generate large thumbnail first: resize before rotate
    ImageBuffer largeImg;
    if (img.width > LargeSize || img.height > LargeSize) {
        auto resLarge = Generator::resize(img, LargeSize);
        if (!resLarge.isOk()) return resLarge.status();
        largeImg = std::move(resLarge.value());
    } else {
        largeImg = std::move(img);
    }

    if (orientation > 1) {
        largeImg = Generator::rotate(largeImg, orientation);
    }

    Status saveLarge = Generator::saveJpegFast(largeImg, largePath);
    if (!saveLarge.isOk()) return saveLarge;

    // Fast downscale from largeImg to small thumbnail
    auto resSmall = Generator::resize(largeImg, SmallSize);
    if (!resSmall.isOk()) return resSmall.status();

    Status saveSmall = Generator::saveJpegFast(resSmall.value(), smallPath);
    if (!saveSmall.isOk()) return saveSmall;

    return std::make_pair(smallPath, largePath);
}

Result<std::pair<std::string, std::string>> Cache::ensureProceduralThumbnails(
    const std::string& hash,
    const std::string& mediaType,
    const std::string& label
) {
    std::string smallPath = getThumbnailPath(hash, SmallSize);
    std::string largePath = getThumbnailPath(hash, LargeSize);

    bool hasSmall = hasThumbnail(hash, SmallSize);
    bool hasLarge = hasThumbnail(hash, LargeSize);
    if (hasSmall && hasLarge) {
        return std::make_pair(smallPath, largePath);
    }

    if (hasLarge && !hasSmall) {
        auto largeRes = Generator::loadImage(largePath);
        if (largeRes.isOk()) {
            auto resSmall = Generator::resize(largeRes.value(), SmallSize);
            if (resSmall.isOk()) {
                Status saveSmall = Generator::saveJpegFast(resSmall.value(), smallPath);
                if (saveSmall.isOk()) {
                    return std::make_pair(smallPath, largePath);
                }
            }
        }
    }

    auto genRes = Generator::generateProceduralThumbnail(mediaType, label, LargeSize);
    if (!genRes.isOk()) {
        return genRes.status();
    }

    auto largeImg = std::move(genRes.value());
    Status saveLarge = Generator::saveJpegFast(largeImg, largePath);
    if (!saveLarge.isOk()) return saveLarge;

    auto resSmall = Generator::resize(largeImg, SmallSize);
    if (!resSmall.isOk()) return resSmall.status();

    Status saveSmall = Generator::saveJpegFast(resSmall.value(), smallPath);
    if (!saveSmall.isOk()) return saveSmall;

    return std::make_pair(smallPath, largePath);
}

std::string Cache::placeholderMarkerPath(const std::string& hash) const {
    std::string baseDir;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        baseDir = cacheDir_;
    }
    return pathToUtf8(
        (pathFromUtf8(baseDir) / pathFromUtf8(placeholderMarkerRelativePath(hash))).make_preferred());
}

Status Cache::setPlaceholderMarker(const std::string& hash) const {
    const auto markerPath = pathFromUtf8(placeholderMarkerPath(hash));
    std::error_code ec;
    if (markerPath.has_parent_path()) {
        std::filesystem::create_directories(markerPath.parent_path(), ec);
    }
    // Rewriting refreshes the modification time, which backs off retries.
    std::ofstream ofs(markerPath, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        return Status::ioError("Failed to write placeholder marker: " + pathToUtf8(markerPath));
    }
    return Status::ok();
}

Status Cache::clearPlaceholderMarker(const std::string& hash) const {
    std::error_code ec;
    std::filesystem::remove(pathFromUtf8(placeholderMarkerPath(hash)), ec);
    return Status::ok();
}

bool Cache::hasPlaceholderMarker(const std::string& hash) const {
    std::error_code ec;
    return std::filesystem::is_regular_file(pathFromUtf8(placeholderMarkerPath(hash)), ec) && !ec;
}

bool Cache::shouldRetryPlaceholder(const std::string& hash, int minAgeSeconds) const {
    const auto markerPath = pathFromUtf8(placeholderMarkerPath(hash));
    std::error_code ec;
    if (!std::filesystem::is_regular_file(markerPath, ec) || ec) {
        return false;
    }
    const auto lastAttempt = std::filesystem::last_write_time(markerPath, ec);
    if (ec) {
        return false;
    }
    const auto now = std::filesystem::file_time_type::clock::now();
    if (now <= lastAttempt) {
        // A clock change moved the marker into the future; do not spin on it.
        return false;
    }
    return std::chrono::duration_cast<std::chrono::seconds>(now - lastAttempt).count() >= minAgeSeconds;
}

Result<size_t> Cache::clearThumbnail(const std::string& hash, bool dryRun) const {
    std::string safeHash;
    safeHash.reserve(hash.size());
    for (unsigned char c : hash) {
        if (std::isalnum(c)) {
            safeHash.push_back(static_cast<char>(c));
        }
    }
    if (safeHash.empty()) {
        return size_t{0};
    }

    std::string baseDir;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        baseDir = cacheDir_;
    }
    if (baseDir.empty()) {
        return size_t{0};
    }

    std::filesystem::path targetDir;
    std::string prefix;
    if (safeHash.size() < 4) {
        targetDir = pathFromUtf8(baseDir);
        prefix = "misc_" + safeHash + "_";
    } else {
        std::string p1 = safeHash.substr(0, 2);
        std::string p2 = safeHash.substr(2, 2);
        targetDir = pathFromUtf8(baseDir) / pathFromUtf8(p1) / pathFromUtf8(p2);
        prefix = safeHash + "_";
    }

    size_t count = 0;
    std::error_code ec;
    if (std::filesystem::is_directory(targetDir, ec) && !ec) {
        for (const auto& entry : std::filesystem::directory_iterator(targetDir, ec)) {
            if (ec) break;
            if (entry.is_regular_file(ec)) {
                std::string fname = pathToUtf8(entry.path().filename());
                if (fname.rfind(prefix, 0) == 0) {
                    count++;
                    if (!dryRun) {
                        std::filesystem::remove(entry.path(), ec);
                    }
                }
            }
        }
    }
    return count;
}

Result<size_t> Cache::clearThumbnails(const std::vector<std::string>& hashes, bool dryRun) const {
    size_t total = 0;
    for (const auto& hash : hashes) {
        auto res = clearThumbnail(hash, dryRun);
        if (res.isOk()) {
            total += res.value();
        }
    }
    return total;
}

Result<size_t> Cache::clearAll(bool dryRun) const {
    std::string baseDir;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        baseDir = cacheDir_;
    }
    if (baseDir.empty()) {
        return size_t{0};
    }
    const auto dirPath = pathFromUtf8(baseDir);
    std::error_code ec;
    if (!std::filesystem::exists(dirPath, ec)) {
        return size_t{0};
    }
    size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dirPath, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || ec) continue;
        // Only touch thumbnail artifacts; never wipe foreign files in the cache dir.
        const std::string fname = pathToUtf8(entry.path().filename());
        const std::string ext = pathToUtf8(entry.path().extension());
        const bool isThumb = (ext == ".jpg" || ext == ".jpeg" || ext == ".placeholder") ||
                             fname.rfind("tmp_ondemand_", 0) == 0;
        if (!isThumb) continue;
        if (!dryRun) {
            std::error_code removeEc;
            std::filesystem::remove(entry.path(), removeEc);
            if (removeEc) continue;
        }
        count++;
    }
    return count;
}

} // namespace imagine::thumbnail
