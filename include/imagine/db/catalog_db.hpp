#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <mutex>
#include <memory>
#include "imagine/db/connection.hpp"
#include "imagine/common/types.hpp"
#include "imagine/common/error.hpp"

namespace imagine::db {

class CatalogDb {
public:
    CatalogDb();
    ~CatalogDb();

    Status open(const std::string& dbPath);
    void close();
    bool isOpen() const;

    // Media Items
    Result<MediaId> insertMedia(MediaItem& item);
    Result<size_t> insertMediaBatch(std::vector<MediaItem>& items);
    Status updateMedia(const MediaItem& item);
    Result<MediaItem> getMediaById(MediaId id);
    Result<MediaItem> getMediaByPath(const std::string& path);
    Result<MediaItem> getMediaByHash(const std::string& hash);
    Status updateRating(MediaId id, int32_t rating);
    Status updateFlag(MediaId id, FlagState flag);
    Status updateThumbnails(MediaId id, const std::string& smallPath, const std::string& largePath);
    Status updateGps(MediaId id, bool hasGps, double latitude, double longitude, double altitude = 0.0);
    Status deleteMedia(MediaId id);

    // Tags
    Result<TagId> createOrGetTag(const std::string& name, const std::string& category = "keyword", std::optional<TagId> parent_id = std::nullopt);
    Result<std::vector<Tag>> getAllTags();
    Result<std::vector<Tag>> getTagsForMedia(MediaId id);
    Result<std::unordered_map<MediaId, std::vector<Tag>>> getTagsForMediaBatch(const std::vector<MediaId>& mediaIds);
    Status addTagToMedia(MediaId mediaId, TagId tagId);
    Status removeTagFromMedia(MediaId mediaId, TagId tagId);
    Status deleteTag(TagId tagId);

    // Albums
    Result<AlbumId> createAlbum(const std::string& name, const std::string& description = "", bool is_smart = false, const std::string& query_json = "");
    Result<std::vector<Album>> getAllAlbums();
    Result<Album> getAlbumById(AlbumId id);
    Status deleteAlbum(AlbumId id);
    Status addMediaToAlbum(AlbumId albumId, MediaId mediaId, int position = 0);
    Status removeMediaFromAlbum(AlbumId albumId, MediaId mediaId);
    Result<std::vector<MediaItem>> getMediaInAlbum(AlbumId albumId);

    // Timeline & Aggregates
    Result<std::vector<TimelineEntry>> getTimeline();
    Result<CatalogStats> getStats();

    // Query execution
    Result<std::vector<MediaItem>> queryMedia(
        const std::string& whereClause,
        const std::vector<std::string>& params,
        const std::string& orderBy,
        int limit,
        int offset
    );

    Result<int64_t> countMedia(
        const std::string& whereClause,
        const std::vector<std::string>& params
    );

    Connection& connection() { return conn_; }

private:
    MediaItem extractMediaItem(Statement& stmt);

    mutable std::recursive_mutex mutex_;
    Connection conn_;
};

} // namespace imagine::db
