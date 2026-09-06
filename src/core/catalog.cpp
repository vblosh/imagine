#include "imagine/core/catalog.hpp"
#include "imagine/common/logger.hpp"

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
    cache_ = std::make_unique<thumbnail::Cache>(cacheDir);
    threadPool_ = std::make_unique<concurrency::ThreadPool>(threadCount_);
    importer_ = std::make_unique<Importer>(*db_, *cache_, threadPool_.get(), photosDir_);

    isOpen_ = true;
    IMAGINE_LOG_INFO("Catalog opened successfully with DB: " + dbPath);
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
    if (recordedPath.empty()) {
        return "";
    }
    std::filesystem::path recPath(recordedPath);
    std::error_code ec;

    // 1. If recordedPath is a relative path
    if (recPath.is_relative()) {
        std::shared_lock<std::shared_mutex> lock(rwMutex_);
        if (!photosDir_.empty()) {
            return (std::filesystem::path(photosDir_) / recPath).string();
        }
        return recordedPath;
    }

    // 2. If recordedPath is an absolute path and exists directly on disk
    if (std::filesystem::exists(recPath, ec) && std::filesystem::is_regular_file(recPath, ec)) {
        return recPath.string();
    }

    // 3. Absolute path not found on disk, but photosDir_ is configured:
    // Try resolving under photosDir_ by relative suffix or filename (backward compatibility for moved libraries)
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!photosDir_.empty()) {
        std::filesystem::path pDir(photosDir_);
        std::filesystem::path fname = recPath.filename();
        std::filesystem::path directCandidate = pDir / fname;
        if (std::filesystem::exists(directCandidate, ec) && std::filesystem::is_regular_file(directCandidate, ec)) {
            return directCandidate.string();
        }

        std::vector<std::string> parts;
        for (const auto& part : recPath) {
            std::string s = part.string();
            if (!s.empty() && s != "/" && s != "\\" && s.back() != ':') {
                parts.push_back(s);
            }
        }
        for (size_t i = 1; i < parts.size(); ++i) {
            std::filesystem::path sub;
            for (size_t j = i; j < parts.size(); ++j) {
                sub /= parts[j];
            }
            std::filesystem::path candidate = pDir / sub;
            if (std::filesystem::exists(candidate, ec) && std::filesystem::is_regular_file(candidate, ec)) {
                return candidate.string();
            }
        }

        return (pDir / fname).string();
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
        photosDir_ = photosDir;
        if (importer_) {
            importer_->setPhotosDir(photosDir_);
        }
    }
    return res;
}

Result<ImportProgress> Catalog::importDirectory(
    const std::string& path,
    bool recursive,
    Importer::ProgressCallback progressCb
) {
    std::unique_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !importer_) {
        return Status::internal("Catalog is not open");
    }
    auto res = importer_->importDirectory(path, recursive, std::move(progressCb));
    if (photosDir_.empty() && !importer_->photosDir().empty()) {
        photosDir_ = importer_->photosDir();
    }
    return res;
}

Result<MediaItem> Catalog::importFile(const std::string& path) {
    std::unique_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !importer_) {
        return Status::internal("Catalog is not open");
    }
    auto res = importer_->importFile(path);
    if (photosDir_.empty() && !importer_->photosDir().empty()) {
        photosDir_ = importer_->photosDir();
    }
    return res;
}

void Catalog::cancelImport() {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (importer_) {
        importer_->cancel();
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

    std::string genericP = std::filesystem::path(path).generic_string();
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
    return db_->updateGps(id, hasGps, latitude, longitude, altitude);
}

Status Catalog::deleteMedia(MediaId id) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !db_) {
        return Status::internal("Catalog is not open");
    }
    return db_->deleteMedia(id);
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
    if (!db_) throw std::runtime_error("Catalog db accessed while closed");
    return *db_;
}

const db::CatalogDb& Catalog::db() const {
    if (!db_) throw std::runtime_error("Catalog db accessed while closed");
    return *db_;
}

thumbnail::Cache& Catalog::cache() {
    if (!cache_) throw std::runtime_error("Catalog cache accessed while closed");
    return *cache_;
}

const thumbnail::Cache& Catalog::cache() const {
    if (!cache_) throw std::runtime_error("Catalog cache accessed while closed");
    return *cache_;
}

concurrency::ThreadPool& Catalog::threadPool() {
    if (!threadPool_) throw std::runtime_error("Catalog thread pool accessed while closed");
    return *threadPool_;
}

Importer& Catalog::importer() {
    if (!importer_) throw std::runtime_error("Catalog importer accessed while closed");
    return *importer_;
}

} // namespace imagine::core
