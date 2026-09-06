#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <optional>
#include "imagine/common/types.hpp"
#include "imagine/common/error.hpp"
#include "imagine/db/catalog_db.hpp"
#include "imagine/thumbnail/cache.hpp"
#include "imagine/concurrency/thread_pool.hpp"
#include "imagine/core/query.hpp"
#include "imagine/core/importer.hpp"

namespace imagine::core {

class Catalog {
public:
    explicit Catalog(size_t threadPoolThreads = std::max(1u, std::thread::hardware_concurrency()));
    ~Catalog();

    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;
    Catalog(Catalog&&) = delete;
    Catalog& operator=(Catalog&&) = delete;

    // Lifecycle
    Status open(const std::string& dbPath, const std::string& cacheDir = "", const std::string& photosDir = "");
    void close();
    bool isOpen() const;

    // Photos Directory & Path Resolution
    void setPhotosDir(std::string photosDir);
    const std::string& photosDir() const;
    std::string resolvePhotoPath(const std::string& recordedPath) const;
    Result<int64_t> makePathsRelative(const std::string& photosDir, const std::string& thumbsDir = "");

    // Importer
    Result<ImportProgress> importDirectory(
        const std::string& path,
        bool recursive = true,
        Importer::ProgressCallback progressCb = nullptr
    );
    Result<MediaItem> importFile(const std::string& path);
    void cancelImport();

    // Query
    Result<QueryResult> query(const QueryCriteria& criteria);

    // Media Items
    Result<MediaItem> getMedia(MediaId id);
    Result<MediaItem> getMediaByPath(const std::string& path);
    Result<MediaItem> getMediaByHash(const std::string& hash);
    Status setRating(MediaId id, int32_t rating);
    Status setFlag(MediaId id, FlagState flag);
    Status setGps(MediaId id, bool hasGps, double latitude, double longitude, double altitude = 0.0);
    Status deleteMedia(MediaId id);

    // Tags
    Status addTag(MediaId id, const std::string& tagName, const std::string& category = "keyword");
    Status addTag(MediaId id, TagId tagId);
    Status removeTag(MediaId id, TagId tagId);
    Status deleteTag(TagId tagId);
    Result<TagId> createOrGetTag(const std::string& name, const std::string& category = "keyword");
    Result<std::vector<Tag>> getTags();
    Result<std::vector<Tag>> getTagsForMedia(MediaId id);
    Result<std::unordered_map<MediaId, std::vector<Tag>>> getTagsForMediaBatch(const std::vector<MediaId>& mediaIds);

    // Albums
    Result<AlbumId> createAlbum(const std::string& name, const std::string& description = "", bool is_smart = false, const std::string& query_json = "");
    Result<std::vector<Album>> getAlbums();
    Result<Album> getAlbum(AlbumId id);
    Status deleteAlbum(AlbumId id);
    Status addMediaToAlbum(AlbumId albumId, MediaId mediaId, int position = 0);
    Status removeMediaFromAlbum(AlbumId albumId, MediaId mediaId);
    Result<std::vector<MediaItem>> getMediaInAlbum(AlbumId albumId);

    // Timeline, Stats & Folders
    Result<std::vector<TimelineEntry>> getTimeline();
    Result<CatalogStats> getStats();
    Result<std::vector<std::string>> getFolders();

    // Component accessors
    db::CatalogDb& db();
    const db::CatalogDb& db() const;
    thumbnail::Cache& cache();
    const thumbnail::Cache& cache() const;
    concurrency::ThreadPool& threadPool();
    Importer& importer();

private:
    void closeInternal();

    size_t threadCount_{1};
    std::unique_ptr<db::CatalogDb> db_;
    std::unique_ptr<thumbnail::Cache> cache_;
    std::unique_ptr<concurrency::ThreadPool> threadPool_;
    std::unique_ptr<Importer> importer_;
    std::string photosDir_;
    bool isOpen_{false};
    mutable std::shared_mutex rwMutex_;
};

} // namespace imagine::core

namespace imagine {
using core::Catalog;
}
