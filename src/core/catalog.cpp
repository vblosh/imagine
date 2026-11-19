#include "imagine/core/catalog.hpp"
#include "imagine/common/logger.hpp"
#include "imagine/metadata/hasher.hpp"
#include <atomic>
#include <ctime>
#include <cstdio>
#include <thread>
#include <unordered_set>

namespace imagine::core {

Catalog::Catalog(size_t threadPoolThreads)
    : threadCount_(std::max<size_t>(1, threadPoolThreads)) {}

Catalog::~Catalog() {
    close();
}

Status Catalog::open(const std::string& dbPath, const std::string& cacheDir, const std::string& photosDir) {
    std::unique_lock<std::shared_mutex> lock(rwMutex_);
    if (isOpen_) {
        closeInternal();
    }

    db_ = std::make_unique<db::CatalogDb>();
    Status s = db_->open(dbPath);
    if (!s.isOk()) {
        db_.reset();
        return s;
    }

    photosDir_ = photosDir;
    dbPath_ = dbPath;

    cache_ = std::make_unique<thumbnail::Cache>(cacheDir);
    threadPool_ = std::make_unique<concurrency::ThreadPool>(threadCount_);
    importer_ = std::make_unique<Importer>(*db_, *cache_, threadPool_.get(), photosDir_);

    isOpen_ = true;
    IMAGINE_LOG_INFO("Catalog opened successfully with DB: " + dbPath + ", photosDir: " + photosDir_);
    return Status::ok();
}

void Catalog::close() {
    {
        std::shared_lock<std::shared_mutex> rlock(rwMutex_);
        if (importer_) {
            importer_->cancel();
        }
    }
    std::unique_lock<std::shared_mutex> lock(rwMutex_);
    closeInternal();
}

void Catalog::closeInternal() {
    if (!isOpen_) {
        return;
    }

    if (importer_) {
        importer_->cancel();
        while (importer_->isRunning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    if (threadPool_) {
        threadPool_->stop();
    }

    importer_.reset();
    threadPool_.reset();
    cache_.reset();

    if (db_) {
        db_->close();
        db_.reset();
    }

    isOpen_ = false;
    IMAGINE_LOG_INFO("Catalog closed successfully");
}

bool Catalog::isOpen() const {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    return isOpen_;
}

void Catalog::setPhotosDir(std::string photosDir) {
    std::unique_lock<std::shared_mutex> lock(rwMutex_);
    setPhotosDirInternal(std::move(photosDir));
}

void Catalog::setPhotosDirInternal(std::string photosDir) {
    photosDir_ = std::move(photosDir);
    if (importer_) {
        importer_->setPhotosDir(photosDir_);
    }
}

const std::string& Catalog::photosDir() const {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    return photosDir_;
}

std::string Catalog::resolvePhotoPath(const std::string& recordedPath) const {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    return resolvePhotoPathInternal(recordedPath);
}

std::string Catalog::resolveRecordedPathInternal(const std::string& recordedPath) const {
    const auto path = pathFromUtf8(recordedPath);
    if (path.has_root_path()) {
        return recordedPath;
    }
    const auto root = photosDir_.empty() && importer_ ? importer_->photosDir() : photosDir_;
    if (!recordedPath.empty() && !root.empty()) {
        return pathToUtf8(pathFromUtf8(root) / path);
    }
    return resolvePhotoPathInternal(recordedPath);
}

std::string Catalog::resolvePhotoPathInternal(const std::string& recordedPath) const {
    if (recordedPath.empty()) {
        return "";
    }
    std::filesystem::path recPath = pathFromUtf8(recordedPath);
    std::error_code ec;

    // 1. If recordedPath is a relative path (without root directory)
    if (recPath.is_relative() && !recPath.has_root_directory()) {

        if (!photosDir_.empty()) {
            return pathToUtf8(pathFromUtf8(photosDir_) / recPath);
        }
        if (!dbPath_.empty()) {
            std::filesystem::path dbDir = pathFromUtf8(dbPath_).parent_path();
            if (std::filesystem::exists(dbDir / "photos" / recPath, ec) && std::filesystem::is_regular_file(dbDir / "photos" / recPath, ec)) {
                return pathToUtf8(dbDir / "photos" / recPath);
            }
            if (std::filesystem::exists(dbDir / recPath, ec) && std::filesystem::is_regular_file(dbDir / recPath, ec)) {
                return pathToUtf8(dbDir / recPath);
            }
            if (!dbDir.parent_path().empty() && std::filesystem::exists(dbDir.parent_path() / recPath, ec) && std::filesystem::is_regular_file(dbDir.parent_path() / recPath, ec)) {
                return pathToUtf8(dbDir.parent_path() / recPath);
            }
        }
        return recordedPath;
    }

    // 2. If recordedPath is an absolute path and exists directly on disk
    if (std::filesystem::exists(recPath, ec) && std::filesystem::is_regular_file(recPath, ec)) {
        return pathToUtf8(recPath);
    }

    // 3. Absolute path not found on disk, but photosDir_ is configured:
    // Try resolving under photosDir_ by relative suffix or filename (backward compatibility for moved libraries)

    if (!photosDir_.empty()) {
        std::filesystem::path pDir = pathFromUtf8(photosDir_);
        std::filesystem::path fname = recPath.filename();

        std::vector<std::string> parts;
        for (const auto& part : recPath) {
            std::string s = pathToUtf8(part);
            if (!s.empty() && s != "/" && s != "\\" && s.back() != ':') {
                parts.push_back(s);
            }
        }
        for (size_t i = 1; i < parts.size(); ++i) {
            std::filesystem::path sub;
            for (size_t j = i; j < parts.size(); ++j) {
                sub /= pathFromUtf8(parts[j]);
            }
            std::filesystem::path candidate = pDir / sub;
            if (std::filesystem::exists(candidate, ec) && std::filesystem::is_regular_file(candidate, ec)) {
                return pathToUtf8(candidate);
            }
        }

        std::filesystem::path directCandidate = pDir / fname;
        if (std::filesystem::exists(directCandidate, ec) && std::filesystem::is_regular_file(directCandidate, ec)) {
            return pathToUtf8(directCandidate);
        }

        return pathToUtf8(directCandidate);
    }

    return recordedPath;
}

Result<int64_t> Catalog::makePathsRelative(const std::string& photosDir, const std::string& thumbsDir) {
    std::unique_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    auto res = db_->makePathsRelative(photosDir, thumbsDir);
    if (res.isOk() && !photosDir.empty()) {
        setPhotosDirInternal(photosDir);
    }
    return res;
}

void Catalog::syncPhotosDirFromImporter() {
    std::unique_lock<std::shared_mutex> lock(rwMutex_);
    if (photosDir_.empty() && importer_ && !importer_->photosDir().empty()) {
        photosDir_ = importer_->photosDir();
    }
}

Result<ImportProgress> Catalog::importDirectory(
    const std::string& path,
    bool recursive,
    Importer::ProgressCallback progressCb,
    bool useDirectoryAsRoot
) {
    Importer* imp = nullptr;
    {
        std::shared_lock<std::shared_mutex> lock(rwMutex_);
        if (!isOpen_ || !importer_) {
            return Status::internal("Catalog is not open");
        }
        imp = importer_.get();
    }
    auto res = imp->importDirectory(path, recursive, std::move(progressCb), useDirectoryAsRoot);
    syncPhotosDirFromImporter();
    return res;
}

Result<MediaItem> Catalog::importFile(const std::string& path) {
    Importer* imp = nullptr;
    {
        std::shared_lock<std::shared_mutex> lock(rwMutex_);
        if (!isOpen_ || !importer_) {
            return Status::internal("Catalog is not open");
        }
        imp = importer_.get();
    }
    auto res = imp->importFile(path);
    if (res.isOk()) {
        syncPhotosDirFromImporter();
    }
    return res;
}

void Catalog::cancelImport() {
    Importer* imp = nullptr;
    {
        std::shared_lock<std::shared_mutex> lock(rwMutex_);
        imp = importer_.get();
    }
    if (imp) {
        imp->cancel();
    }
}

Result<QueryResult> Catalog::query(const QueryCriteria& criteria) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return QueryBuilder::execute(*db_, criteria);
}

Result<MediaItem> Catalog::getMedia(MediaId id) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->getMediaById(id);
}

Result<MediaItem> Catalog::getMediaByPath(const std::string& path) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    auto res = db_->getMediaByPath(path);
    if (res.isOk()) {
        return res;
    }

    // If path was absolute and not found, try relative to photosDir_
    std::string effPhotosDir = photosDir_;
    if (effPhotosDir.empty() && importer_) {
        effPhotosDir = importer_->photosDir();
    }
    if (!effPhotosDir.empty()) {
        std::string relPath = Importer::toRelativePath(path, effPhotosDir);
        if (relPath != path) {
            auto relRes = db_->getMediaByPath(relPath);
            if (relRes.isOk()) {
                return relRes;
            }
        }
    }

    std::string genericP = pathToUtf8(pathFromUtf8(path));
    std::replace(genericP.begin(), genericP.end(), '\\', '/');
    if (genericP != path) {
        auto gRes = db_->getMediaByPath(genericP);
        if (gRes.isOk()) {
            return gRes;
        }
    }

    return res;
}

Result<MediaItem> Catalog::getMediaByHash(const std::string& hash) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->getMediaByHash(hash);
}

Status Catalog::setRating(MediaId id, int32_t rating) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->updateRating(id, rating);
}

Status Catalog::setCaption(MediaId id, const std::string& caption) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->updateCaption(id, caption);
}

Status Catalog::renameMedia(MediaId id, const std::string& newFileName, std::string* outNewFilePath, std::string* outNewFileName) {
    std::unique_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }

    if (newFileName.empty()) {
        return Status::invalidArgument("File name cannot be empty");
    }
    for (char c : newFileName) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') {
            return Status::invalidArgument("Invalid file name: contains control or reserved characters");
        }
    }
    if (newFileName.find('/') != std::string::npos ||
        newFileName.find('\\') != std::string::npos ||
        newFileName.find(':') != std::string::npos ||
        newFileName == "." || newFileName == "..") {
        return Status::invalidArgument("Invalid file name: cannot contain path separators or illegal characters");
    }

    auto itemRes = db_->getMediaById(id);
    if (!itemRes.isOk()) {
        return itemRes.status();
    }
    const auto& item = itemRes.value();

    std::string actualNewFileName = newFileName;
    std::filesystem::path curFileP = pathFromUtf8(item.file_name);
    std::filesystem::path newFileP = pathFromUtf8(actualNewFileName);
    if (!newFileP.has_extension() && curFileP.has_extension()) {
        actualNewFileName += pathToUtf8(curFileP.extension());
    }

    if (item.file_name == actualNewFileName) {
        if (outNewFilePath) *outNewFilePath = item.file_path;
        if (outNewFileName) *outNewFileName = item.file_name;
        return Status::ok();
    }

    std::filesystem::path currentPath = pathFromUtf8(item.file_path);
    std::filesystem::path parent = currentPath.parent_path();
    std::filesystem::path newPath = parent.empty() ? pathFromUtf8(actualNewFileName) : (parent / pathFromUtf8(actualNewFileName));
    std::string newPathStr = pathToUtf8(newPath);
    std::replace(newPathStr.begin(), newPathStr.end(), '\\', '/');

    auto existingRes = db_->getMediaByPath(newPathStr);
    if (existingRes.isOk() && existingRes.value().id != id) {
        return Status::alreadyExists("A media item with path already exists in the catalog: " + newPathStr);
    }

    std::string oldDiskPath = resolveRecordedPathInternal(item.file_path);
    std::error_code ec;
    std::filesystem::path oldDiskFs;
    if (!oldDiskPath.empty()) {
        oldDiskFs = pathFromUtf8(oldDiskPath);
    }
    if (oldDiskPath.empty() || !std::filesystem::exists(oldDiskFs, ec) || !std::filesystem::is_regular_file(oldDiskFs, ec)) {
        return Status::notFound("Source file does not exist on disk: " + (oldDiskPath.empty() ? item.file_path : oldDiskPath));
    }

    std::filesystem::path newDiskFs = oldDiskFs.parent_path() / pathFromUtf8(actualNewFileName);
    if (std::filesystem::exists(newDiskFs, ec) && newDiskFs != oldDiskFs) {
        std::error_code eqEc;
        if (!std::filesystem::equivalent(oldDiskFs, newDiskFs, eqEc)) {
            return Status::alreadyExists("A file with this name already exists on disk: " + pathToUtf8(newDiskFs));
        }
    }
    std::filesystem::rename(oldDiskFs, newDiskFs, ec);
    if (ec) {
        return Status::ioError("Failed to rename file on disk: " + ec.message());
    }
    bool diskRenamed = true;

    Status dbStatus = db_->updateFileNameAndPath(id, actualNewFileName, newPathStr);
    if (!dbStatus.isOk()) {
        if (diskRenamed) {
            std::error_code ecRollback;
            std::filesystem::rename(newDiskFs, oldDiskFs, ecRollback);
        }
        return dbStatus;
    }

    if (outNewFilePath) {
        *outNewFilePath = newPathStr;
    }
    if (outNewFileName) {
        *outNewFileName = actualNewFileName;
    }
    return Status::ok();
}

Status Catalog::moveMedia(MediaId id, const std::string& destinationPath, std::string* outNewFilePath) {
    std::unique_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }

    std::string destClean = trimWhitespace(destinationPath);
    if (destClean.empty()) {
        return Status::invalidArgument("Destination path cannot be empty");
    }

    std::replace(destClean.begin(), destClean.end(), '\\', '/');
    while (destClean.size() > 1 && destClean.back() == '/') {
        destClean.pop_back();
    }

    auto itemRes = db_->getMediaById(id);
    if (!itemRes.isOk()) {
        return itemRes.status();
    }
    const auto& item = itemRes.value();

    std::string oldDiskPath = resolveRecordedPathInternal(item.file_path);
    std::error_code ec;
    std::filesystem::path oldDiskFs;
    if (!oldDiskPath.empty()) {
        oldDiskFs = pathFromUtf8(oldDiskPath);
    }
    if (oldDiskPath.empty() || !std::filesystem::exists(oldDiskFs, ec) || !std::filesystem::is_regular_file(oldDiskFs, ec)) {
        return Status::notFound("Source file does not exist on disk: " + (oldDiskPath.empty() ? item.file_path : oldDiskPath));
    }

    std::filesystem::path targetDirFs;
    std::string effPhotosDir = photosDir_;
    if (effPhotosDir.empty() && importer_) {
        effPhotosDir = importer_->photosDir();
    }

    std::filesystem::path photosDirFs;
    if (!effPhotosDir.empty()) {
        photosDirFs = pathFromUtf8(effPhotosDir);
    } else {
        std::filesystem::path itemRelFs = pathFromUtf8(item.file_path);
        if (itemRelFs.is_relative()) {
            auto cur = oldDiskFs.parent_path();
            auto relParent = itemRelFs.parent_path();
            while (!relParent.empty() && relParent != "." && !cur.empty()) {
                cur = cur.parent_path();
                relParent = relParent.parent_path();
            }
            photosDirFs = cur;
        } else {
            bool foundRoot = false;
            if (!dbPath_.empty()) {
                std::filesystem::path dbFs = pathFromUtf8(dbPath_);
                std::filesystem::path dbDir = dbFs.parent_path();
                std::filesystem::path baseDir = dbDir;
                if ((dbDir.filename() == "imagine" || dbDir.filename() == ".imagine") && !dbDir.parent_path().empty()) {
                    baseDir = dbDir.parent_path();
                }
                std::filesystem::path photosSub = baseDir / "photos";
                std::error_code ecSub;
                if (std::filesystem::exists(photosSub, ecSub) && std::filesystem::is_directory(photosSub, ecSub) &&
                    Importer::isInsideRootDir(oldDiskFs, photosSub)) {
                    photosDirFs = photosSub;
                    foundRoot = true;
                } else if (Importer::isInsideRootDir(oldDiskFs, baseDir)) {
                    photosDirFs = baseDir;
                    foundRoot = true;
                }
            }
            if (!foundRoot) {
                auto oldParent = oldDiskFs.parent_path();
                if (!oldParent.empty() && !oldParent.parent_path().empty() && oldParent != oldParent.parent_path()) {
                    photosDirFs = oldParent.parent_path();
                } else {
                    photosDirFs = oldParent;
                }
            }
        }
    }

    bool isAbs = false;
    std::filesystem::path destFs = pathFromUtf8(destClean);
    if (destFs.has_root_name()) {
        isAbs = true;
    } else if (destFs.is_absolute() && destClean != "/" && destClean != "\\") {
        // On POSIX, a leading '/' can either denote an absolute host path (e.g. /tmp/outside or /var/...)
        // or a catalog-relative path with a leading slash (e.g. /winter or /vacation/summer).
        // If it targets within photosDirFs, or points to an existing directory/ancestor outside photosDirFs,
        // treat it as an absolute host path. Otherwise, treat it as relative to the photos root.
        std::error_code ecCheck;
        std::filesystem::path normTargetCheck = stripExtendedPrefix(destFs).lexically_normal();
        std::filesystem::path normRootCheck = stripExtendedPrefix(photosDirFs).lexically_normal();
        if (!normRootCheck.empty() && (normTargetCheck == normRootCheck || Importer::isInsideRootDir(normTargetCheck, normRootCheck))) {
            isAbs = true;
        } else if (!normRootCheck.empty() && normRootCheck.has_parent_path() && normRootCheck.parent_path() != "/" &&
                   (normTargetCheck == normRootCheck.parent_path() || Importer::isInsideRootDir(normTargetCheck, normRootCheck.parent_path()))) {
            isAbs = true;
        }
    }
    if (!isAbs) {
        while (!destClean.empty() && (destClean.front() == '/' || destClean.front() == '\\')) {
            destClean.erase(0, 1);
        }
    }

    if (!isAbs && (destClean.empty() || destClean == ".")) {
        targetDirFs = photosDirFs;
    } else if (!isAbs) {
        targetDirFs = photosDirFs / pathFromUtf8(destClean);
    } else {
        targetDirFs = destFs;
    }

    // Enforce that move destination must reside inside the photos root directory
    std::filesystem::path normTarget = stripExtendedPrefix(targetDirFs).lexically_normal();
    std::filesystem::path normRoot = stripExtendedPrefix(photosDirFs).lexically_normal();

    bool isSameDir = false;
    std::error_code ecEq;
    if (std::filesystem::equivalent(normTarget, normRoot, ecEq) && !ecEq) {
        isSameDir = true;
    }
    if (!isSameDir) {
        std::string sTarget = normTarget.generic_string();
        std::string sRoot = normRoot.generic_string();
        while (sTarget.size() > 1 && sTarget.back() == '/') sTarget.pop_back();
        while (sRoot.size() > 1 && sRoot.back() == '/') sRoot.pop_back();
#if defined(_WIN32)
        auto toLower = [](std::string s) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        isSameDir = (toLower(sTarget) == toLower(sRoot));
#else
        isSameDir = (sTarget == sRoot);
#endif
    }

    bool insidePhotosDir = isSameDir || Importer::isInsideRootDir(normTarget, normRoot);
    if (!insidePhotosDir) {
        std::error_code ecCan;
        std::filesystem::path canTarget = stripExtendedPrefix(std::filesystem::weakly_canonical(targetDirFs, ecCan));
        std::filesystem::path canRoot = stripExtendedPrefix(std::filesystem::weakly_canonical(photosDirFs, ecCan));
        if (canTarget == canRoot || Importer::isInsideRootDir(canTarget, canRoot)) {
            insidePhotosDir = true;
        }
    }

    if (!insidePhotosDir) {
        return Status::invalidArgument("Move destination must be inside the photos directory (" +
                                      pathToUtf8(normRoot) + "): " + pathToUtf8(normTarget));
    }


    std::filesystem::path newDiskFs = targetDirFs / pathFromUtf8(item.file_name);

    std::string newStoredPath;
    if (!effPhotosDir.empty()) {
        newStoredPath = Importer::toRelativePath(newDiskFs, effPhotosDir);
    } else {
        if (isAbs || pathFromUtf8(item.file_path).is_absolute()) {
            newStoredPath = pathToUtf8(newDiskFs);
            std::replace(newStoredPath.begin(), newStoredPath.end(), '\\', '/');
        } else {
            newStoredPath = Importer::toRelativePath(newDiskFs, photosDirFs);
        }
    }

    std::string itemNorm = item.file_path;
    std::replace(itemNorm.begin(), itemNorm.end(), '\\', '/');
    std::string newNorm = newStoredPath;
    std::replace(newNorm.begin(), newNorm.end(), '\\', '/');
    if (itemNorm == newNorm) {
        if (outNewFilePath) *outNewFilePath = item.file_path;
        return Status::ok();
    }

    auto existingRes = db_->getMediaByPath(newStoredPath);
    if (existingRes.isOk() && existingRes.value().id != id) {
        return Status::alreadyExists("A media item with path already exists in the catalog: " + newStoredPath);
    }

    bool diskMoved = false;

    if (!oldDiskPath.empty()) {
        oldDiskFs = pathFromUtf8(oldDiskPath);
        if (std::filesystem::exists(oldDiskFs, ec) && std::filesystem::is_regular_file(oldDiskFs, ec)) {
            std::filesystem::create_directories(targetDirFs, ec);
            if (ec) {
                return Status::ioError("Failed to create destination directory: " + ec.message());
            }

            if (std::filesystem::exists(newDiskFs, ec)) {
                std::error_code eqEc;
                if (!std::filesystem::equivalent(oldDiskFs, newDiskFs, eqEc)) {
                    return Status::alreadyExists("A file already exists at destination on disk: " + pathToUtf8(newDiskFs));
                }
            } else {
                std::filesystem::rename(oldDiskFs, newDiskFs, ec);
                if (ec) {
                    ec.clear();
                    std::filesystem::copy_file(oldDiskFs, newDiskFs, std::filesystem::copy_options::overwrite_existing, ec);
                    if (!ec) {
                        std::filesystem::remove(oldDiskFs, ec);
                        if (!ec) {
                            diskMoved = true;
                        } else {
                            std::error_code ecClean;
                            std::filesystem::remove(newDiskFs, ecClean);
                            return Status::ioError("Failed to remove source file after copy during move: " + ec.message());
                        }
                    } else {
                        return Status::ioError("Failed to move file on disk: " + ec.message());
                    }
                } else {
                    diskMoved = true;
                }
            }
        }
    }

    Status dbStatus = db_->updateFileNameAndPath(id, item.file_name, newStoredPath);
    if (!dbStatus.isOk()) {
        if (diskMoved) {
            std::error_code ecRollback;
            std::filesystem::rename(newDiskFs, oldDiskFs, ecRollback);
            if (ecRollback) {
                ecRollback.clear();
                std::filesystem::copy_file(newDiskFs, oldDiskFs, std::filesystem::copy_options::overwrite_existing, ecRollback);
                if (!ecRollback) {
                    std::filesystem::remove(newDiskFs, ecRollback);
                }
            }
        }
        return dbStatus;
    }

    if (outNewFilePath) {
        *outNewFilePath = newStoredPath;
    }
    return Status::ok();
}

Status Catalog::setDateTaken(MediaId id, int64_t dateTaken, const std::string& dateTakenStr) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }

    std::string formattedStr = dateTakenStr;
    if (formattedStr.empty() && dateTaken != 0) {
        std::time_t tt = static_cast<std::time_t>(dateTaken);
        std::tm tm{};
        bool ok = false;
#if defined(_WIN32)
        ok = (gmtime_s(&tm, &tt) == 0);
#else
        ok = (gmtime_r(&tt, &tm) != nullptr);
#endif
        if (ok) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%04d:%02d:%02d %02d:%02d:%02d",
                          tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                          tm.tm_hour, tm.tm_min, tm.tm_sec);
            formattedStr = buf;
        } else {
            int64_t days = dateTaken / 86400;
            int64_t rem = dateTaken % 86400;
            if (rem < 0) { rem += 86400; days -= 1; }
            int hour = static_cast<int>(rem / 3600);
            int min = static_cast<int>((rem % 3600) / 60);
            int sec = static_cast<int>(rem % 60);

            days += 719468;
            const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
            const unsigned doe = static_cast<unsigned>(days - era * 146097);
            const unsigned yoe = (doe - doe / 1024 + doe / 1460 - doe / 36524) / 365;
            const int64_t y = static_cast<int64_t>(yoe) + era * 400;
            const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
            const unsigned mp = (5 * doy + 2) / 153;
            const unsigned d = doy - (153 * mp + 2) / 5 + 1;
            const unsigned m = mp < 10 ? mp + 3 : mp - 9;
            const int64_t yr = y + (m <= 2);

            char buf[32];
            std::snprintf(buf, sizeof(buf), "%04lld:%02u:%02u %02d:%02d:%02d",
                          static_cast<long long>(yr), m, d, hour, min, sec);
            formattedStr = buf;
        }
    }

    return db_->updateDateTaken(id, dateTaken, formattedStr);
}

Status Catalog::setFlag(MediaId id, FlagState flag) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->updateFlag(id, flag);
}

Status Catalog::setGps(MediaId id, bool hasGps, double latitude, double longitude, double altitude) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    Status updateStatus = db_->updateGps(id, hasGps, latitude, longitude, altitude);
    if (!updateStatus.isOk()) {
        return updateStatus;
    }
    Status tagStatus = db_->applyGpsPlaceTags(id, hasGps, latitude, longitude);
    if (!tagStatus.isOk()) {
        IMAGINE_LOG_WARN("Failed to apply GPS place tags for media " + std::to_string(id) + ": " + tagStatus.message());
    }
    return updateStatus;
}

namespace {
std::function<void(MediaId)> gClipEvictionHook = nullptr;

namespace fs = std::filesystem;

bool isSha256(std::string_view hash) {
    return hash.size() == 64 && hash.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

Status verifyDiskHash(const fs::path& path, const std::string& expectedHash) {
    auto hash = metadata::Hasher::computeFileSha256(pathToUtf8(path));
    if (!hash.isOk()) return hash.status();
    if (hash.value() != expectedHash) {
        return Status::alreadyExists("File changed since it was cataloged: " + pathToUtf8(path));
    }
    return Status::ok();
}

class StagedRemoval {
public:
    fs::path original;

    ~StagedRemoval() {
        if (moved_) {
            const auto status = restore();
            if (!status.isOk()) IMAGINE_LOG_ERROR(status.message());
        }
    }

    Status stage() {
        static std::atomic<uint64_t> sequence{0};
        std::error_code ec;
        for (;;) {
            const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                              + '-' + std::to_string(sequence.fetch_add(1));
            directory_ = original.parent_path() / (".imagine-delete-" + suffix);
            staged_ = directory_ / original.filename();
            staged_ += ".staged";
            if (fs::create_directory(directory_, ec)) break;
            if (ec) return Status::ioError("Cannot create deletion staging directory: " + ec.message());
        }
        fs::rename(original, staged_, ec);
        if (ec) {
            std::error_code cleanupError;
            fs::remove(directory_, cleanupError);
            return Status::ioError("Cannot stage " + pathToUtf8(original) + ": " + ec.message());
        }
        moved_ = true;
        return Status::ok();
    }

    Status verify(const std::string& hash) const {
        std::error_code ec;
        if (!fs::is_regular_file(fs::symlink_status(staged_, ec))) {
            return Status::ioError("Original is no longer a regular file: " + pathToUtf8(original));
        }
        if (!isSha256(hash)) return Status::ok();
        auto status = verifyDiskHash(staged_, hash);
        if (status.code() == StatusCode::AlreadyExists) {
            return Status::alreadyExists("File changed since it was cataloged: " + pathToUtf8(original));
        }
        return status;
    }

    Status restore() {
        if (!moved_) return Status::ok();
        moved_ = false;
        std::error_code ec;
        const auto existing = fs::symlink_status(original, ec);
        if (existing.type() == fs::file_type::not_found) {
            fs::rename(staged_, original, ec);
            if (!ec) {
                fs::remove(directory_, ec);
                return Status::ok();
            }
        }
        return Status::ioError("Cannot restore " + pathToUtf8(original) + ". Recover the original from "
                               + pathToUtf8(staged_) + (ec ? " (" + ec.message() + ")" : ""));
    }

    void finish(DeleteResult& result) {
        if (!moved_) return;
        moved_ = false;
        std::error_code ec;
        int64_t bytes = 0;
        if (fs::hard_link_count(staged_, ec) == 1 && !ec) {
            const auto size = fs::file_size(staged_, ec);
            if (!ec) bytes = static_cast<int64_t>(size);
        }
        if (fs::remove(staged_, ec)) {
            result.deleted_from_disk = true;
            result.freed_bytes = bytes;
            fs::remove(directory_, ec);
        } else {
            result.disk_error = "Cannot delete staged original " + pathToUtf8(original)
                              + (ec ? ": " + ec.message() : ": staging file is missing");
            result.recovery_path = pathToUtf8(staged_);
            IMAGINE_LOG_WARN(result.disk_error + " (" + result.recovery_path + ")");
        }
    }

private:
    fs::path directory_;
    fs::path staged_;
    bool moved_{false};
};
}

void setClipEvictionHook(std::function<void(MediaId)> hook) {
    gClipEvictionHook = std::move(hook);
}

Status Catalog::deleteMedia(MediaId id, bool deleteFromDisk) {
    auto result = deleteMediaWithResult(id, deleteFromDisk);
    if (!result.isOk()) return result.status();
    if (!result.value().disk_error.empty()) {
        return Status::ioError(result.value().disk_error + " (" + result.value().recovery_path + ")");
    }
    return Status::ok();
}

Result<DeleteResult> Catalog::deleteMediaWithResult(MediaId id, bool deleteFromDisk) {
    auto result = removeMedia(nullptr, {{id, {}}}, deleteFromDisk, false);
    if (!result.isOk()) return result.status();
    return std::move(result.value().front());
}

Result<std::vector<DeleteResult>> Catalog::resolveDuplicateGroup(
    const ReviewedMedia& keeper, const std::vector<ReviewedMedia>& remove,
    bool deleteFromDisk, bool mergeMetadata) {
    if (remove.empty()) return Status::invalidArgument("No duplicate items to remove");
    return removeMedia(&keeper, remove, deleteFromDisk, mergeMetadata);
}

Result<std::vector<DeleteResult>> Catalog::removeMedia(
    const ReviewedMedia* keeper, const std::vector<ReviewedMedia>& remove,
    bool deleteFromDisk, bool mergeMetadata) {
    std::unique_lock catalogLock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }

    std::vector<MediaItem> items;
    items.reserve(remove.size());
    std::vector<StagedRemoval> files(remove.size());
    std::vector<DeleteResult> results(remove.size());
    std::unordered_set<MediaId> removedIds;

    const auto loadReviewed = [&](const ReviewedMedia& reviewed) -> Result<MediaItem> {
        if (keeper && deleteFromDisk && !isSha256(reviewed.content_hash)) {
            return Status::invalidArgument("A reviewed content hash is required for media " + std::to_string(reviewed.id));
        }
        auto item = db_->getMediaById(reviewed.id);
        if (item.isOk() && !reviewed.content_hash.empty() && item.value().content_hash != reviewed.content_hash) {
            return Status::alreadyExists("Media changed since duplicate review: " + std::to_string(reviewed.id));
        }
        return item;
    };

    Status status = db_->runInTransaction([&]() -> Status {
        if (keeper) {
            auto item = loadReviewed(*keeper);
            if (!item.isOk()) return item.status();
            if (deleteFromDisk) {
                const auto path = pathFromUtf8(resolveRecordedPathInternal(item.value().file_path));
                std::error_code ec;
                if (!fs::is_regular_file(path, ec)) {
                    return Status::notFound("Keeper original is unavailable: " + item.value().file_path);
                }
                auto checked = verifyDiskHash(path, keeper->content_hash);
                if (!checked.isOk()) return checked;
            }
        }

        for (const auto& reviewed : remove) {
            if ((keeper && reviewed.id == keeper->id) || !removedIds.insert(reviewed.id).second) {
                return Status::invalidArgument("Repeated keeper or removal ID: " + std::to_string(reviewed.id));
            }
            auto item = loadReviewed(reviewed);
            if (!item.isOk()) return item.status();
            items.push_back(std::move(item.value()));
        }

        if (deleteFromDisk) {
            const auto root = photosDir_.empty() && importer_ ? importer_->photosDir() : photosDir_;
            std::error_code ec;
            const auto rootPath = root.empty() ? fs::path{} : fs::canonical(pathFromUtf8(root), ec);
            if (rootPath.empty() || ec) {
                return Status::invalidArgument("A reachable photos directory is required to delete files");
            }

            std::unordered_map<fs::path, size_t> pathsToRemove;
            for (size_t i = 0; i < items.size(); ++i) {
                const auto path = pathFromUtf8(resolveRecordedPathInternal(items[i].file_path));
                const auto fileStatus = fs::symlink_status(path, ec);
                if (!fs::is_regular_file(fileStatus) || ec) {
                    return Status::notFound("Recorded original is unavailable: " + pathToUtf8(path));
                }
                const auto canonical = stripExtendedPrefix(fs::canonical(path, ec));
                if (ec || !Importer::isInsideRootDir(canonical, stripExtendedPrefix(rootPath))) {
                    return Status::invalidArgument("Cannot delete file outside photos directory: " + pathToUtf8(path));
                }
#ifdef _WIN32
                if ((fileStatus.permissions() & fs::perms::owner_write) == fs::perms::none) {
                    return Status::ioError("Cannot delete read-only file: " + pathToUtf8(path));
                }
#endif
                if (pathsToRemove.emplace(canonical, i).second) {
                    files[i].original = canonical;
                }
            }

            // Canonical names identify entries, so sibling hardlinks keep their distinct paths.
            auto query = db_->connection().prepare("SELECT id, file_path FROM media_items;");
            if (!query.isOk()) return query.status();
            auto stmt = std::move(query.value());
            auto step = db::StepResult::Done;
            while (!pathsToRemove.empty() && (step = stmt.step()) == db::StepResult::Row) {
                if (removedIds.contains(stmt.getInt64(0))) continue;
                const auto recorded = stmt.getString(1);
                const auto path = pathFromUtf8(resolvePhotoPathInternal(recorded));
                const auto canonical = stripExtendedPrefix(fs::canonical(path, ec));
                if (ec) continue;
                const auto match = pathsToRemove.find(canonical);
                if (match != pathsToRemove.end()) {
                    files[match->second].original.clear();
                    pathsToRemove.erase(match);
                }
            }
            if (!pathsToRemove.empty() && step == db::StepResult::Error) {
                return Status::databaseError("Failed to check original-file references: " + db_->connection().lastErrorMessage());
            }
        }

        for (size_t i = 0; i < files.size(); ++i) {
            if (files[i].original.empty()) continue;
            auto staged = files[i].stage();
            if (!staged.isOk()) return staged;
            auto verified = files[i].verify(items[i].content_hash);
            if (!verified.isOk()) return verified;
        }
        for (const auto& item : items) {
            if (mergeMetadata && keeper) {
                auto merged = db_->mergeMediaMetadata(keeper->id, item.id);
                if (!merged.isOk()) return merged;
            }
            auto deleted = db_->deleteMedia(item.id);
            if (!deleted.isOk()) return deleted;
        }
        return Status::ok();
    });

    if (!status.isOk()) {
        auto message = status.message();
        for (auto& file : files) {
            auto restored = file.restore();
            if (!restored.isOk()) message += ". " + restored.message();
        }
        return Status(status.code(), std::move(message));
    }

    for (size_t i = 0; i < files.size(); ++i) files[i].finish(results[i]);
    if (cache_) {
        std::unordered_set<std::string> hashes;
        for (const auto& item : items) {
            if (item.content_hash.empty() || !hashes.insert(item.content_hash).second) continue;
            auto remaining = db_->countMedia("content_hash = ?", {item.content_hash});
            if (remaining.isOk() && remaining.value() == 0) cache_->clearThumbnail(item.content_hash);
        }
    }
    catalogLock.unlock();
    if (gClipEvictionHook) {
        for (const auto& item : items) gClipEvictionHook(item.id);
    }
    return results;
}

Result<size_t> Catalog::clearThumbnailCache(bool dryRun) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !cache_) {
        return Result<size_t>::failure(Status::invalidArgument("Catalog is not open"));
    }
    return cache_->clearAll(dryRun);
}

Result<size_t> Catalog::clearVideoThumbnailCache(bool dryRun) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !cache_ || !db_) {
        return Result<size_t>::failure(Status::invalidArgument("Catalog is not open"));
    }
    auto hashesRes = db_->getVideoHashes();
    if (!hashesRes.isOk()) {
        return Result<size_t>::failure(hashesRes.status());
    }
    return cache_->clearThumbnails(hashesRes.value(), dryRun);
}

Status Catalog::addTag(MediaId id, const std::string& tagName, const std::string& category) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    auto tagRes = db_->createOrGetTag(tagName, category);
    if (!tagRes.isOk()) {
        return tagRes.status();
    }
    return db_->addTagToMedia(id, tagRes.value());
}

Status Catalog::addTag(MediaId id, TagId tagId) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->addTagToMedia(id, tagId);
}

Status Catalog::removeTag(MediaId id, TagId tagId) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->removeTagFromMedia(id, tagId);
}

Status Catalog::deleteTag(TagId tagId) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->deleteTag(tagId);
}

Result<TagId> Catalog::createOrGetTag(const std::string& name, const std::string& category) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->createOrGetTag(name, category);
}

Result<std::vector<Tag>> Catalog::getTags() {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->getAllTags();
}

Result<std::vector<Tag>> Catalog::getTagsForMedia(MediaId id) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->getTagsForMedia(id);
}

Result<std::unordered_map<MediaId, std::vector<Tag>>> Catalog::getTagsForMediaBatch(const std::vector<MediaId>& mediaIds) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->getTagsForMediaBatch(mediaIds);
}

Result<AlbumId> Catalog::createAlbum(
    const std::string& name,
    const std::string& description,
    bool is_smart,
    const std::string& query_json
) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->createAlbum(name, description, is_smart, query_json);
}

Result<std::vector<Album>> Catalog::getAlbums() {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->getAllAlbums();
}

Result<Album> Catalog::getAlbum(AlbumId id) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->getAlbumById(id);
}

Status Catalog::deleteAlbum(AlbumId id) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->deleteAlbum(id);
}

Status Catalog::addMediaToAlbum(AlbumId albumId, MediaId mediaId, int position) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->addMediaToAlbum(albumId, mediaId, position);
}

Status Catalog::reorderAlbumMedia(AlbumId albumId, const std::vector<MediaId>& mediaIds) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->reorderAlbumMedia(albumId, mediaIds);
}

Status Catalog::setAlbumCover(AlbumId albumId, MediaId mediaId) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->setAlbumCover(albumId, mediaId);
}

Status Catalog::removeMediaFromAlbum(AlbumId albumId, MediaId mediaId) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->removeMediaFromAlbum(albumId, mediaId);
}

Result<std::vector<MediaItem>> Catalog::getMediaInAlbum(AlbumId albumId) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->getMediaInAlbum(albumId);
}

Result<std::vector<TimelineEntry>> Catalog::getTimeline() {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->getTimeline();
}

Result<CatalogStats> Catalog::getStats() {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->getStats();
}

Result<std::vector<std::string>> Catalog::getFolders() {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->getAllFolders();
}

db::CatalogDb& Catalog::db() {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!db_) throw std::runtime_error("Catalog db accessed while closed");
    return *db_;
}

const db::CatalogDb& Catalog::db() const {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!db_) throw std::runtime_error("Catalog db accessed while closed");
    return *db_;
}

thumbnail::Cache& Catalog::cache() {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!cache_) throw std::runtime_error("Catalog cache accessed while closed");
    return *cache_;
}

const thumbnail::Cache& Catalog::cache() const {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!cache_) throw std::runtime_error("Catalog cache accessed while closed");
    return *cache_;
}

concurrency::ThreadPool& Catalog::threadPool() {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!threadPool_) throw std::runtime_error("Catalog thread pool accessed while closed");
    return *threadPool_;
}

Importer& Catalog::importer() {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!importer_) throw std::runtime_error("Catalog importer accessed while closed");
    return *importer_;
}

Status Catalog::runInTransaction(std::function<Status()> fn) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->runInTransaction(fn);
}

Status Catalog::runWithExclusiveAccess(
    std::function<Status(db::CatalogDb&, const PhotoPathResolver&)> fn) {
    std::unique_lock lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    std::lock_guard dbLock(db_->mutex());
    return fn(*db_, [this](const std::string& path) { return resolveRecordedPathInternal(path); });
}

} // namespace imagine::core
