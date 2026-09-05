#include "imagine/core/catalog.hpp"
#include "imagine/common/logger.hpp"

namespace imagine::core {

Catalog::Catalog(size_t threadPoolThreads)
    : threadCount_(std::max<size_t>(1, threadPoolThreads)) {}

Catalog::~Catalog() {
    close();
}

Status Catalog::open(const std::string& dbPath, const std::string& cacheDir) {
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

    cache_ = std::make_unique<thumbnail::Cache>(cacheDir);
    threadPool_ = std::make_unique<concurrency::ThreadPool>(threadCount_);
    importer_ = std::make_unique<Importer>(*db_, *cache_, threadPool_.get());

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

Result<ImportProgress> Catalog::importDirectory(
    const std::string& path,
    bool recursive,
    Importer::ProgressCallback progressCb
) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !importer_) {
        return Status::internal("Catalog is not open");
    }
    return importer_->importDirectory(path, recursive, std::move(progressCb));
}

Result<MediaItem> Catalog::importFile(const std::string& path) {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!isOpen_ || !importer_) {
        return Status::internal("Catalog is not open");
    }
    return importer_->importFile(path);
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
    return db_->getMediaByPath(path);
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
