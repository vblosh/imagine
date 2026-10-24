#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <mutex>
#include <memory>
#include <functional>
#include "imagine/db/connection.hpp"
#include "imagine/common/types.hpp"
#include "imagine/common/error.hpp"

namespace imagine::faces { class Service; }
namespace imagine::clip { class Service; }
namespace imagine::core { class ReverseGeocoder; }

namespace imagine::db {

struct SmartTagRecord {
    MediaId mediaId{0};
    std::vector<TagId> tagIds;
    std::string contentHash;
};

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
    Result<size_t> updateMediaBatch(const std::vector<MediaItem>& items);
    Result<MediaItem> getMediaById(MediaId id);
    Result<MediaItem> getMediaByPath(const std::string& path);
    Result<MediaItem> getMediaByHash(const std::string& hash);
    Status updateRating(MediaId id, int32_t rating);
    Status updateFlag(MediaId id, FlagState flag);
    Status updateCaption(MediaId id, const std::string& caption);
    Status updateFileNameAndPath(MediaId id, const std::string& newFileName, const std::string& newFilePath);
    Status updateDateTaken(MediaId id, int64_t dateTaken, const std::string& dateTakenStr);
    Status updateThumbnails(MediaId id, const std::string& smallPath, const std::string& largePath);
    Status updateGps(MediaId id, bool hasGps, double latitude, double longitude, double altitude = 0.0);
    Status applyGpsPlaceTags(MediaId id, bool hasGps, double latitude, double longitude);
    Status deleteMedia(MediaId id);
    Result<int64_t> makePathsRelative(const std::string& photosDir, const std::string& thumbsDir = "");
    Result<std::vector<std::string>> getVideoHashes();
    Result<std::vector<std::string>> getMediaHashesByType(const std::string& mediaType);

    // Tags
    Result<TagId> createOrGetTag(const std::string& name, const std::string& category = "keyword", std::optional<TagId> parent_id = std::nullopt);
    Result<std::vector<Tag>> getAllTags();
    Result<std::vector<Tag>> getTagsForMedia(MediaId id);
    Result<std::unordered_map<MediaId, std::vector<Tag>>> getTagsForMediaBatch(const std::vector<MediaId>& mediaIds);
    Status addTagToMedia(MediaId mediaId, TagId tagId);
    Status removeTagFromMedia(MediaId mediaId, TagId tagId);
    Result<TagId> replaceTagAssignments(
        const std::string& name,
        const std::string& category,
        const std::vector<MediaId>& mediaIds
    );
    Status deleteTag(TagId tagId);

    // Smart Tags
    Status replaceSmartTags(
        const std::vector<SmartTagRecord>& records,
        const std::string& modelId,
        const std::string& modelRevision,
        const std::string& taxonomyVersion
    );
    Status clearSmartTags();

    // Albums
    Result<AlbumId> createAlbum(const std::string& name, const std::string& description = "", bool is_smart = false, const std::string& query_json = "");
    Result<std::vector<Album>> getAllAlbums();
    Result<Album> getAlbumById(AlbumId id);
    Status deleteAlbum(AlbumId id);
    Status addMediaToAlbum(AlbumId albumId, MediaId mediaId, int position = 0);
    Status reorderAlbumMedia(AlbumId albumId, const std::vector<MediaId>& mediaIds);
    Status setAlbumCover(AlbumId albumId, MediaId mediaId);
    Status removeMediaFromAlbum(AlbumId albumId, MediaId mediaId);
    Result<std::vector<MediaItem>> getMediaInAlbum(AlbumId albumId);

    // Timeline & Aggregates
    Result<std::vector<TimelineEntry>> getTimeline();
    Result<CatalogStats> getStats();
    Result<std::vector<std::string>> getAllFolders();

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

    // Deduplication
    Result<std::vector<std::vector<MediaId>>> findExactDuplicateGroups(const std::vector<MediaId>& mediaIds = {});
    Status mergeMediaMetadata(MediaId keeperId, MediaId sourceId);
    Status dismissDuplicatePair(MediaId a, MediaId b);
    Result<std::vector<std::pair<MediaId, MediaId>>> getDismissedDuplicatePairs();
    Result<int> clearDismissedDuplicatePairs();

    Status runInTransaction(std::function<Status()> fn);

    // Returns the resolved GeoNames CSV path, or empty when none is available.
    std::string geonamesDatasetInfo() const;

    Connection& connection() { return conn_; }
    std::recursive_mutex& mutex() const { return mutex_; }

private:
    friend class ::imagine::faces::Service;
    friend class ::imagine::clip::Service;
    MediaItem extractMediaItem(Statement& stmt);
    bool ensureGeocoderLoaded();

    mutable std::recursive_mutex mutex_;
    Connection conn_;

    std::mutex geocoderMutex_;
    std::unique_ptr<core::ReverseGeocoder> geocoder_;
    bool geocoderLoadAttempted_{false};
    bool geocoderAvailable_{false};
};

} // namespace imagine::db
