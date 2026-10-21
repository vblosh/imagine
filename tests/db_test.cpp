#include <gtest/gtest.h>
#include "imagine/db/catalog_db.hpp"
#include "imagine/db/connection.hpp"
#include "imagine/db/schema.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

using namespace imagine;
using namespace imagine::db;

class CatalogDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(db.open(":memory:").isOk());
    }

    void TearDown() override {
        db.close();
    }

    CatalogDb db;
};

TEST_F(CatalogDbTest, OpenAndCheckSchema) {
    EXPECT_TRUE(db.isOpen());
    auto statsRes = db.getStats();
    ASSERT_TRUE(statsRes.isOk());
    EXPECT_EQ(statsRes.value().total_media, 0);
}

TEST_F(CatalogDbTest, InsertAndRetrieveMedia) {
    MediaItem item;
    item.file_path = "/photos/summer2026/beach.jpg";
    item.file_name = "beach.jpg";
    item.file_size = 2048576;
    item.file_modified_time = 1750000000;
    item.content_hash = "abcdef0123456789abcdef0123456789";
    item.width = 4000;
    item.height = 3000;
    item.date_taken = 1750000000;
    item.rating = 4;
    item.flag = FlagState::Pick;
    item.exif.camera_make = "Sony";
    item.exif.camera_model = "A7 IV";
    item.exif.lens = "FE 24-70mm F2.8 GM II";
    item.exif.exposure_time = 0.002;
    item.exif.f_number = 2.8;
    item.exif.iso = 100;
    item.exif.focal_length = 50.0;
    item.exif.has_gps = true;
    item.exif.latitude = 36.5;
    item.exif.longitude = -121.9;

    auto insertRes = db.insertMedia(item);
    ASSERT_TRUE(insertRes.isOk());
    MediaId id = insertRes.value();
    EXPECT_GT(id, 0);

    // Retrieve by ID
    auto getRes = db.getMediaById(id);
    ASSERT_TRUE(getRes.isOk());
    const auto& retrieved = getRes.value();
    EXPECT_EQ(retrieved.id, id);
    EXPECT_EQ(retrieved.file_path, item.file_path);
    EXPECT_EQ(retrieved.file_name, item.file_name);
    EXPECT_EQ(retrieved.file_size, item.file_size);
    EXPECT_EQ(retrieved.content_hash, item.content_hash);
    EXPECT_EQ(retrieved.rating, 4);
    EXPECT_EQ(retrieved.flag, FlagState::Pick);
    EXPECT_EQ(retrieved.exif.camera_make, "Sony");
    EXPECT_EQ(retrieved.exif.camera_model, "A7 IV");
    EXPECT_TRUE(retrieved.exif.has_gps);
    EXPECT_DOUBLE_EQ(retrieved.exif.latitude, 36.5);

    // Retrieve by Path
    auto pathRes = db.getMediaByPath(item.file_path);
    ASSERT_TRUE(pathRes.isOk());
    EXPECT_EQ(pathRes.value().id, id);

    // Retrieve by Hash
    auto hashRes = db.getMediaByHash(item.content_hash);
    ASSERT_TRUE(hashRes.isOk());
    EXPECT_EQ(hashRes.value().id, id);

    // Not found lookups
    EXPECT_EQ(db.getMediaById(99999).status().code(), StatusCode::NotFound);
    EXPECT_EQ(db.getMediaByPath("/no/such/path").status().code(), StatusCode::NotFound);
    EXPECT_EQ(db.getMediaByHash("non_existent_hash").status().code(), StatusCode::NotFound);
}

TEST(CatalogDbMigrationTest, PersistedV5CatalogUpgradesToV6JobMembership) {
    const auto path = std::filesystem::temp_directory_path() /
        ("imagine_catalog_v5_upgrade_" + std::to_string(
            std::chrono::system_clock::now().time_since_epoch().count()) + ".db");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    MediaId mediaId = 0;
    int64_t jobId = 0;
    {
        CatalogDb initial;
        ASSERT_TRUE(initial.open(path.string()).isOk());
        MediaItem media;
        media.file_path = "persisted/photo.jpg";
        media.file_name = "photo.jpg";
        media.content_hash = "persisted-v5-hash";
        media.media_type = "photo";
        mediaId = initial.insertMedia(media).value();
        auto jobRes = initial.connection().prepare(
            "INSERT INTO face_analysis_jobs(state,scope,total,remaining,created_at,updated_at) "
            "VALUES('completed','selected',1,0,1,1);");
        ASSERT_TRUE(jobRes.isOk());
        auto job = std::move(jobRes.value());
        ASSERT_EQ(job.step(), StepResult::Done);
        jobId = initial.connection().lastInsertRowId();

        // A v5 catalog predates the membership table. Recreate that persisted
        // state while leaving all other schema and catalog data intact.
        ASSERT_TRUE(initial.connection().execute("DROP TABLE face_analysis_job_media;").isOk());
        ASSERT_TRUE(initial.connection().execute("DELETE FROM schema_version WHERE version>=6;").isOk());
        initial.close();
    }

    {
        CatalogDb upgraded;
        ASSERT_TRUE(upgraded.open(path.string()).isOk());
        auto versionRes = upgraded.connection().prepare("SELECT MAX(version) FROM schema_version;");
        ASSERT_TRUE(versionRes.isOk());
        auto version = std::move(versionRes.value());
        ASSERT_EQ(version.step(), StepResult::Row);
        EXPECT_EQ(version.getInt(0), Schema::CurrentVersion);
        EXPECT_TRUE(upgraded.getMediaById(mediaId).isOk());
        auto existingJobRes = upgraded.connection().prepare("SELECT state FROM face_analysis_jobs WHERE id=?;");
        ASSERT_TRUE(existingJobRes.isOk());
        auto existingJob = std::move(existingJobRes.value());
        existingJob.bind(1, jobId);
        ASSERT_EQ(existingJob.step(), StepResult::Row);
        EXPECT_EQ(existingJob.getString(0), "completed");

        auto membershipRes = upgraded.connection().prepare(
            "INSERT INTO face_analysis_job_media(job_id,media_id) VALUES(?,?);");
        ASSERT_TRUE(membershipRes.isOk());
        auto membership = std::move(membershipRes.value());
        membership.bind(1, jobId);
        membership.bind(2, mediaId);
        EXPECT_EQ(membership.step(), StepResult::Done);
        upgraded.close();
    }
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
}

TEST(CatalogDbMigrationTest, PersistedV7CatalogUpgradesToV9AndReMigrateIsIdempotent) {
    const auto path = std::filesystem::temp_directory_path() /
        ("imagine_catalog_v7_upgrade_" + std::to_string(
            std::chrono::system_clock::now().time_since_epoch().count()) + ".db");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    MediaId mediaId1 = 0;
    MediaId mediaId2 = 0;
    {
        CatalogDb initial;
        ASSERT_TRUE(initial.open(path.string()).isOk());
        MediaItem media1;
        media1.file_path = "persisted/photo1.jpg";
        media1.file_name = "photo1.jpg";
        media1.content_hash = "hash_v7_1";
        media1.media_type = "photo";
        mediaId1 = initial.insertMedia(media1).value();

        MediaItem media2;
        media2.file_path = "persisted/photo2.jpg";
        media2.file_name = "photo2.jpg";
        media2.content_hash = "hash_v7_1";
        media2.media_type = "photo";
        mediaId2 = initial.insertMedia(media2).value();

        // Roll back schema to V7 state:
        // Drop duplicate_dismissals table and V8 partial index, delete schema_version >= 8
        ASSERT_TRUE(initial.connection().execute("DROP TABLE IF EXISTS duplicate_dismissals;").isOk());
        ASSERT_TRUE(initial.connection().execute("DROP INDEX IF EXISTS idx_media_content_hash;").isOk());
        ASSERT_TRUE(initial.connection().execute("DELETE FROM schema_version WHERE version >= 8;").isOk());
        initial.close();
    }

    {
        CatalogDb upgraded;
        ASSERT_TRUE(upgraded.open(path.string()).isOk());

        // Check schema version is current
        auto versionRes = upgraded.connection().prepare("SELECT MAX(version) FROM schema_version;");
        ASSERT_TRUE(versionRes.isOk());
        auto version = std::move(versionRes.value());
        ASSERT_EQ(version.step(), StepResult::Row);
        EXPECT_EQ(version.getInt(0), Schema::CurrentVersion);

        // Check media items still exist
        EXPECT_TRUE(upgraded.getMediaById(mediaId1).isOk());
        EXPECT_TRUE(upgraded.getMediaById(mediaId2).isOk());

        // Check duplicate dismissals table works
        EXPECT_TRUE(upgraded.dismissDuplicatePair(mediaId1, mediaId2).isOk());
        auto dismissed = upgraded.getDismissedDuplicatePairs();
        ASSERT_TRUE(dismissed.isOk());
        ASSERT_EQ(dismissed.value().size(), 1u);

        // Test idempotent re-migrate: calling Schema::migrate again should succeed without error
        EXPECT_TRUE(Schema::migrate(upgraded.connection()).isOk());

        // Verify again after re-migrate
        auto dismissedAfter = upgraded.getDismissedDuplicatePairs();
        ASSERT_TRUE(dismissedAfter.isOk());
        ASSERT_EQ(dismissedAfter.value().size(), 1u);

        upgraded.close();
    }

    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
}

TEST_F(CatalogDbTest, MediaChangesInvalidateFaceResultsAndKeepManualTags) {
    auto makeMedia = [&](const std::string& path, const std::string& hash) {
        MediaItem item;
        item.file_path = path; item.file_name = path; item.content_hash = hash;
        item.width = 100; item.height = 80; item.media_type = "photo";
        return db.insertMedia(item).value();
    };
    auto seedFace = [&](MediaId mediaId, TagId tagId) {
        auto& conn = db.connection();
        auto analysisRes = conn.prepare(R"SQL(
            INSERT INTO face_media_analysis(media_id,state,source_hash,width,height,detector_checksum,recognizer_checksum,
                pipeline_version,detector_provider,recognizer_provider,device,confidence,nms_threshold,analyzed_at)
            VALUES(?,'complete','old-hash',100,80,'det','rec','pipeline','cpu','cpu','cpu',0.5,0.4,1);
        )SQL");
        EXPECT_TRUE(analysisRes.isOk());
        auto analysis = std::move(analysisRes.value()); analysis.bind(1, mediaId);
        EXPECT_EQ(analysis.step(), StepResult::Done);
        auto faceRes = conn.prepare(R"SQL(
            INSERT INTO faces(media_id,x,y,width,height,score,landmarks,person_tag_id,created_at,updated_at)
            VALUES(?,1,2,20,20,0.9,'[]',?,1,1);
        )SQL");
        EXPECT_TRUE(faceRes.isOk());
        auto face = std::move(faceRes.value()); face.bind(1, mediaId); face.bind(2, tagId);
        EXPECT_EQ(face.step(), StepResult::Done);
    };

    const TagId person = db.createOrGetTag("Face invalidation", "people").value();
    const MediaId manuallyOwned = makeMedia("manual.jpg", "manual-old");
    ASSERT_TRUE(db.addTagToMedia(manuallyOwned, person).isOk());
    seedFace(manuallyOwned, person);

    const MediaId faceOwned = makeMedia("derived.jpg", "derived-old");
    auto& conn = db.connection();
    auto linkRes = conn.prepare("INSERT INTO media_tags(media_id,tag_id) VALUES(?,?);");
    ASSERT_TRUE(linkRes.isOk());
    auto link = std::move(linkRes.value()); link.bind(1, faceOwned); link.bind(2, person);
    ASSERT_EQ(link.step(), StepResult::Done);
    auto provenanceRes = conn.prepare("INSERT INTO media_tag_provenance(media_id,tag_id,manual) VALUES(?,?,0);");
    ASSERT_TRUE(provenanceRes.isOk());
    auto provenance = std::move(provenanceRes.value()); provenance.bind(1, faceOwned); provenance.bind(2, person);
    ASSERT_EQ(provenance.step(), StepResult::Done);
    seedFace(faceOwned, person);

    auto changedManual = db.getMediaById(manuallyOwned).value();
    changedManual.content_hash = "manual-new";
    ASSERT_TRUE(db.updateMedia(changedManual).isOk());
    auto changedDerived = db.getMediaById(faceOwned).value();
    changedDerived.exif.orientation = 6;
    std::vector<MediaItem> batch{changedDerived};
    ASSERT_TRUE(db.updateMediaBatch(batch).isOk());

    auto countFaces = conn.prepare("SELECT COUNT(*) FROM faces WHERE media_id IN (?,?);");
    ASSERT_TRUE(countFaces.isOk());
    auto count = std::move(countFaces.value()); count.bind(1, manuallyOwned); count.bind(2, faceOwned);
    ASSERT_EQ(count.step(), StepResult::Row);
    EXPECT_EQ(count.getInt64(0), 0);
    auto countAnalysis = conn.prepare("SELECT COUNT(*) FROM face_media_analysis WHERE media_id IN (?,?);");
    ASSERT_TRUE(countAnalysis.isOk());
    auto analyses = std::move(countAnalysis.value()); analyses.bind(1, manuallyOwned); analyses.bind(2, faceOwned);
    ASSERT_EQ(analyses.step(), StepResult::Row);
    EXPECT_EQ(analyses.getInt64(0), 0);
    EXPECT_EQ(db.getTagsForMedia(manuallyOwned).value().size(), 1u);
    EXPECT_TRUE(db.getTagsForMedia(faceOwned).value().empty());
}

TEST_F(CatalogDbTest, InsertMediaBatch) {
    std::vector<MediaItem> batch;
    for (int i = 0; i < 5; ++i) {
        MediaItem item;
        item.file_path = "/photos/batch_" + std::to_string(i) + ".jpg";
        item.file_name = "batch_" + std::to_string(i) + ".jpg";
        item.file_size = 1000 + i;
        item.file_modified_time = 1750000000 + i;
        item.content_hash = "batch_hash_" + std::to_string(i);
        item.width = 1920;
        item.height = 1080;
        item.date_taken = 1750000000 + i;
        item.rating = i;
        batch.push_back(item);
    }

    auto batchRes = db.insertMediaBatch(batch);
    ASSERT_TRUE(batchRes.isOk());
    EXPECT_EQ(batchRes.value(), 5u);

    // Verify IDs were populated and items exist
    for (const auto& item : batch) {
        EXPECT_GT(item.id, 0);
        auto retrieved = db.getMediaById(item.id);
        ASSERT_TRUE(retrieved.isOk());
        EXPECT_EQ(retrieved.value().file_name, item.file_name);
    }

    // Empty batch returns 0
    std::vector<MediaItem> empty;
    auto emptyRes = db.insertMediaBatch(empty);
    ASSERT_TRUE(emptyRes.isOk());
    EXPECT_EQ(emptyRes.value(), 0u);
}

TEST_F(CatalogDbTest, UpdateAndDeleteMedia) {
    MediaItem item;
    item.file_path = "/photos/test.jpg";
    item.file_name = "test.jpg";
    item.file_size = 1024;
    item.file_modified_time = 1000;
    item.content_hash = "hash123";
    item.date_taken = 1000;

    auto insertRes = db.insertMedia(item);
    ASSERT_TRUE(insertRes.isOk());
    MediaId id = insertRes.value();

    EXPECT_TRUE(db.updateRating(id, 5).isOk());
    EXPECT_TRUE(db.updateFlag(id, FlagState::Reject).isOk());

    auto getRes = db.getMediaById(id);
    ASSERT_TRUE(getRes.isOk());
    EXPECT_EQ(getRes.value().rating, 5);
    EXPECT_EQ(getRes.value().flag, FlagState::Reject);

    // updateThumbnails
    EXPECT_TRUE(db.updateThumbnails(id, "/th/sm.jpg", "/th/lg.jpg").isOk());
    auto withThumbs = db.getMediaById(id).value();
    EXPECT_EQ(withThumbs.thumb_small, "/th/sm.jpg");
    EXPECT_EQ(withThumbs.thumb_large, "/th/lg.jpg");

    // updateMedia full update
    withThumbs.file_name = "updated.jpg";
    withThumbs.rating = 2;
    withThumbs.exif.camera_make = "Fujifilm";
    EXPECT_TRUE(db.updateMedia(withThumbs).isOk());

    auto updated = db.getMediaById(id).value();
    EXPECT_EQ(updated.file_name, "updated.jpg");
    EXPECT_EQ(updated.rating, 2);
    EXPECT_EQ(updated.exif.camera_make, "Fujifilm");

    // deleteMedia
    EXPECT_TRUE(db.deleteMedia(id).isOk());
    EXPECT_EQ(db.getMediaById(id).status().code(), StatusCode::NotFound);

    // updateMedia on non-existent media returns NotFound
    MediaItem nonExistent = withThumbs;
    nonExistent.id = 999999;
    EXPECT_EQ(db.updateMedia(nonExistent).code(), StatusCode::NotFound);
}

TEST_F(CatalogDbTest, TagManagementAndBatch) {
    MediaItem item1;
    item1.file_path = "/photos/family.jpg";
    item1.file_name = "family.jpg";
    item1.file_size = 1024;
    item1.file_modified_time = 1000;
    item1.content_hash = "hash_family";
    item1.date_taken = 1000;
    MediaId mid1 = db.insertMedia(item1).value();

    MediaItem item2;
    item2.file_path = "/photos/nature.jpg";
    item2.file_name = "nature.jpg";
    item2.file_size = 2048;
    item2.file_modified_time = 2000;
    item2.content_hash = "hash_nature";
    item2.date_taken = 2000;
    MediaId mid2 = db.insertMedia(item2).value();

    // Create tags (including parent_id)
    auto tagPeople = db.createOrGetTag("Alice", "people");
    ASSERT_TRUE(tagPeople.isOk());

    auto tagPlace = db.createOrGetTag("Paris", "places", tagPeople.value());
    ASSERT_TRUE(tagPlace.isOk());

    // Duplicate create returns existing
    auto tagPeople2 = db.createOrGetTag("Alice", "people");
    ASSERT_TRUE(tagPeople2.isOk());
    EXPECT_EQ(tagPeople.value(), tagPeople2.value());

    // Attach to media
    EXPECT_TRUE(db.addTagToMedia(mid1, tagPeople.value()).isOk());
    EXPECT_TRUE(db.addTagToMedia(mid1, tagPlace.value()).isOk());
    EXPECT_TRUE(db.addTagToMedia(mid2, tagPlace.value()).isOk());

    // Batch get tags
    auto batchEmpty = db.getTagsForMediaBatch({});
    ASSERT_TRUE(batchEmpty.isOk());
    EXPECT_TRUE(batchEmpty.value().empty());

    auto batchRes = db.getTagsForMediaBatch({mid1, mid2});
    ASSERT_TRUE(batchRes.isOk());
    EXPECT_EQ(batchRes.value()[mid1].size(), 2u);
    EXPECT_EQ(batchRes.value()[mid2].size(), 1u);

    // Replacing a managed tag's assignments is atomic and reuses the tag.
    auto managedTag = db.replaceTagAssignments("Last Imported", "keyword", {mid1});
    ASSERT_TRUE(managedTag.isOk());
    auto replacedTag = db.replaceTagAssignments("Last Imported", "keyword", {mid2});
    ASSERT_TRUE(replacedTag.isOk());
    EXPECT_EQ(replacedTag.value(), managedTag.value());
    auto managedTags1 = db.getTagsForMedia(mid1).value();
    auto managedTags2 = db.getTagsForMedia(mid2).value();
    EXPECT_TRUE(std::none_of(managedTags1.begin(), managedTags1.end(), [](const Tag& tag) {
        return tag.name == "Last Imported";
    }));
    EXPECT_TRUE(std::any_of(managedTags2.begin(), managedTags2.end(), [](const Tag& tag) {
        return tag.name == "Last Imported";
    }));

    // A failed replacement rolls back the cleared assignments.
    auto invalidReplace = db.replaceTagAssignments("Last Imported", "keyword", {999999});
    EXPECT_FALSE(invalidReplace.isOk());
    auto tagsAfterRollback = db.getTagsForMedia(mid2).value();
    EXPECT_TRUE(std::any_of(tagsAfterRollback.begin(), tagsAfterRollback.end(), [](const Tag& tag) {
        return tag.name == "Last Imported";
    }));

    // Remove one tag
    EXPECT_TRUE(db.removeTagFromMedia(mid1, tagPeople.value()).isOk());
    auto tagsAfter = db.getTagsForMedia(mid1);
    ASSERT_TRUE(tagsAfter.isOk());
    EXPECT_EQ(tagsAfter.value().size(), 1u);
    EXPECT_EQ(tagsAfter.value()[0].name, "Paris");

    // Delete tag completely
    EXPECT_TRUE(db.deleteTag(tagPlace.value()).isOk());
    auto tagsAfterDelete = db.getTagsForMedia(mid1);
    ASSERT_TRUE(tagsAfterDelete.isOk());
    EXPECT_EQ(tagsAfterDelete.value().size(), 0u);
}

TEST_F(CatalogDbTest, AlbumManagementAndQueries) {
    MediaItem item1;
    item1.file_path = "/photos/1.jpg";
    item1.file_name = "1.jpg";
    item1.file_size = 1024;
    item1.file_modified_time = 1000;
    item1.content_hash = "hash_1";
    item1.date_taken = 1700000000;
    auto mid1 = db.insertMedia(item1).value();

    auto albumRes = db.createAlbum("Favorites 2026", "Best memories");
    ASSERT_TRUE(albumRes.isOk());
    AlbumId aid = albumRes.value();

    // getAlbumById
    auto getAlbum = db.getAlbumById(aid);
    ASSERT_TRUE(getAlbum.isOk());
    EXPECT_EQ(getAlbum.value().name, "Favorites 2026");

    // getAlbumById not found
    EXPECT_EQ(db.getAlbumById(99999).status().code(), StatusCode::NotFound);

    EXPECT_TRUE(db.addMediaToAlbum(aid, mid1, 0).isOk());
    EXPECT_EQ(db.getMediaInAlbum(aid).value().size(), 1u);

    EXPECT_TRUE(db.setAlbumCover(aid, mid1).isOk());
    auto albumWithCover = db.getAlbumById(aid);
    ASSERT_TRUE(albumWithCover.isOk());
    ASSERT_TRUE(albumWithCover.value().cover_media_id.has_value());
    EXPECT_EQ(*albumWithCover.value().cover_media_id, mid1);

    // Timeline check
    auto timeline = db.getTimeline();
    ASSERT_TRUE(timeline.isOk());
    EXPECT_FALSE(timeline.value().empty());

    // countMedia & queryMedia
    auto countAll = db.countMedia("", {});
    ASSERT_TRUE(countAll.isOk());
    EXPECT_EQ(countAll.value(), 1);

    auto countFiltered = db.countMedia("rating >= ?", {"0"});
    ASSERT_TRUE(countFiltered.isOk());
    EXPECT_EQ(countFiltered.value(), 1);

    auto queryEmptyOrder = db.queryMedia("", {}, "", 10, 0);
    ASSERT_TRUE(queryEmptyOrder.isOk());
    EXPECT_EQ(queryEmptyOrder.value().size(), 1u);

    // Covers must refer to photos that are members of the album.
    MediaItem video;
    video.file_path = "/videos/1.mp4";
    video.file_name = "1.mp4";
    video.file_size = 2048;
    video.file_modified_time = 1001;
    video.content_hash = "hash_video_1";
    video.date_taken = 1700000001;
    video.media_type = "video";
    auto videoId = db.insertMedia(video).value();
    EXPECT_TRUE(db.addMediaToAlbum(aid, videoId, 1).isOk());
    EXPECT_FALSE(db.setAlbumCover(aid, videoId).isOk());

    EXPECT_TRUE(db.reorderAlbumMedia(aid, {videoId, mid1}).isOk());
    auto reorderedAlbum = db.getMediaInAlbum(aid);
    ASSERT_TRUE(reorderedAlbum.isOk());
    ASSERT_EQ(reorderedAlbum.value().size(), 2u);
    EXPECT_EQ(reorderedAlbum.value()[0].id, videoId);
    EXPECT_EQ(reorderedAlbum.value()[1].id, mid1);
    EXPECT_FALSE(db.reorderAlbumMedia(aid, {mid1, mid1}).isOk());
    EXPECT_FALSE(db.reorderAlbumMedia(aid, {mid1}).isOk());
    EXPECT_EQ(db.reorderAlbumMedia(99999, {}).code(), StatusCode::NotFound);

    MediaItem unassociatedPhoto;
    unassociatedPhoto.file_path = "/photos/2.jpg";
    unassociatedPhoto.file_name = "2.jpg";
    unassociatedPhoto.file_size = 1024;
    unassociatedPhoto.file_modified_time = 1002;
    unassociatedPhoto.content_hash = "hash_2";
    unassociatedPhoto.date_taken = 1700000002;
    auto unassociatedPhotoId = db.insertMedia(unassociatedPhoto).value();
    EXPECT_FALSE(db.setAlbumCover(aid, unassociatedPhotoId).isOk());
    EXPECT_FALSE(db.setAlbumCover(99999, mid1).isOk());

    // Removing the current cover also clears the album's cover reference.
    EXPECT_TRUE(db.removeMediaFromAlbum(aid, mid1).isOk());
    auto albumAfterCoverRemoval = db.getAlbumById(aid);
    ASSERT_TRUE(albumAfterCoverRemoval.isOk());
    EXPECT_FALSE(albumAfterCoverRemoval.value().cover_media_id.has_value());

    // deleteAlbum
    EXPECT_TRUE(db.deleteAlbum(aid).isOk());
    EXPECT_EQ(db.getAlbumById(aid).status().code(), StatusCode::NotFound);
}

TEST(ConnectionAndStatementTest, LowLevelOperationsAndErrors) {
    Connection conn;
    EXPECT_FALSE(conn.isOpen());
    EXPECT_EQ(conn.execute("SELECT 1;").code(), StatusCode::DatabaseError);
    EXPECT_FALSE(conn.prepare("SELECT 1;").isOk());
    EXPECT_EQ(conn.lastInsertRowId(), 0);
    EXPECT_EQ(conn.changes(), 0);
    EXPECT_EQ(conn.lastErrorCode(), -1);
    EXPECT_EQ(conn.lastErrorMessage(), "Database closed");

    // Open invalid path (cannot create directory inside a regular file)
    std::string dummyFile = (std::filesystem::temp_directory_path() / "imagine_dummy_not_a_dir.tmp").string();
    { std::ofstream ofs(dummyFile); ofs << "x"; }
    EXPECT_FALSE(conn.open(dummyFile + "/sub/db.sqlite").isOk());
    std::filesystem::remove(dummyFile);

    // Valid open
    ASSERT_TRUE(conn.open(":memory:").isOk());
    EXPECT_TRUE(conn.isOpen());

    // Move constructor and move assignment
    Connection connMoved = std::move(conn);
    EXPECT_TRUE(connMoved.isOpen());
    Connection connAssigned;
    connAssigned = std::move(connMoved);
    EXPECT_TRUE(connAssigned.isOpen());

    // Prepare invalid SQL
    auto badStmt = connAssigned.prepare("INVALID SQL STATEMENT;");
    EXPECT_FALSE(badStmt.isOk());

    // Table creation and Statement testing
    ASSERT_TRUE(connAssigned.execute("CREATE TABLE test (id INTEGER PRIMARY KEY, d REAL, t TEXT, opt TEXT);").isOk());

    auto insStmt = connAssigned.prepare("INSERT INTO test (id, d, t, opt) VALUES (?, ?, ?, ?);");
    ASSERT_TRUE(insStmt.isOk());
    auto stmt = std::move(insStmt.value());

    // Move constructor and move assignment of Statement
    Statement stmtMoved = std::move(stmt);
    Statement stmtAssigned(nullptr);
    stmtAssigned = std::move(stmtMoved);

    // Statement binding
    EXPECT_TRUE(stmtAssigned.bind(1, static_cast<int64_t>(100)).isOk());
    EXPECT_TRUE(stmtAssigned.bind(2, 3.14159).isOk());
    EXPECT_TRUE(stmtAssigned.bind(3, "sample text").isOk());
    EXPECT_TRUE(stmtAssigned.bindNull(4).isOk());
    EXPECT_EQ(stmtAssigned.step(), StepResult::Done);

    // Statement reset and re-insert
    EXPECT_TRUE(stmtAssigned.reset().isOk());
    EXPECT_TRUE(stmtAssigned.bind(1, static_cast<int32_t>(101)).isOk());
    EXPECT_TRUE(stmtAssigned.bind(2, 2.718).isOk());
    EXPECT_TRUE(stmtAssigned.bind(3, "row 2").isOk());
    EXPECT_TRUE(stmtAssigned.bind(4, "not null").isOk());
    EXPECT_EQ(stmtAssigned.step(), StepResult::Done);

    // Query back
    auto selStmt = connAssigned.prepare("SELECT id, d, t, opt FROM test ORDER BY id ASC;");
    ASSERT_TRUE(selStmt.isOk());
    auto q = std::move(selStmt.value());

    // Row 1
    EXPECT_EQ(q.step(), StepResult::Row);
    EXPECT_EQ(q.getInt64(0), 100);
    EXPECT_DOUBLE_EQ(q.getDouble(1), 3.14159);
    EXPECT_EQ(q.getString(2), "sample text");
    EXPECT_TRUE(q.isNull(3));
    EXPECT_FALSE(q.getOptionalString(3).has_value());

    // Row 2
    EXPECT_EQ(q.step(), StepResult::Row);
    EXPECT_EQ(q.getInt(0), 101);
    EXPECT_DOUBLE_EQ(q.getDouble(1), 2.718);
    EXPECT_EQ(q.getString(2), "row 2");
    EXPECT_FALSE(q.isNull(3));
    ASSERT_TRUE(q.getOptionalString(3).has_value());
    EXPECT_EQ(*q.getOptionalString(3), "not null");

    EXPECT_EQ(q.step(), StepResult::Done);

    // Calling on null statement handle returns databaseError
    Statement nullStmt(nullptr);
    EXPECT_FALSE(nullStmt.bind(1, 1).isOk());
    EXPECT_FALSE(nullStmt.bind(1, int64_t{1}).isOk());
    EXPECT_FALSE(nullStmt.bind(1, 1.0).isOk());
    EXPECT_FALSE(nullStmt.bind(1, "str").isOk());
    EXPECT_FALSE(nullStmt.bindNull(1).isOk());
    EXPECT_FALSE(nullStmt.reset().isOk());
    EXPECT_EQ(nullStmt.step(), StepResult::Error);
    EXPECT_TRUE(nullStmt.isNull(0));
    EXPECT_EQ(nullStmt.getInt(0), 0);
    EXPECT_EQ(nullStmt.getInt64(0), 0);
    EXPECT_DOUBLE_EQ(nullStmt.getDouble(0), 0.0);
    EXPECT_EQ(nullStmt.getString(0), "");
    EXPECT_EQ(nullStmt.getBlob(0), nullptr);
    EXPECT_EQ(nullStmt.getBlobBytes(0), 0u);
    EXPECT_FALSE(nullStmt.getOptionalString(0).has_value());
    EXPECT_FALSE(nullStmt.getOptionalInt(0).has_value());
    EXPECT_FALSE(nullStmt.getOptionalInt64(0).has_value());
    EXPECT_FALSE(nullStmt.getOptionalDouble(0).has_value());
}

TEST(TransactionTest, CommitRollbackAndAutoRollback) {
    Connection conn;
    ASSERT_TRUE(conn.open(":memory:").isOk());
    ASSERT_TRUE(conn.execute("CREATE TABLE kv (k TEXT, v TEXT);").isOk());

    // 1. Explicit commit
    {
        Transaction tx(conn);
        EXPECT_TRUE(tx.isActive());
        EXPECT_TRUE(conn.execute("INSERT INTO kv VALUES ('key1', 'val1');").isOk());
        EXPECT_TRUE(tx.commit().isOk());
        EXPECT_FALSE(tx.isActive());
        // Double commit should error
        EXPECT_FALSE(tx.commit().isOk());
    }

    // 2. Explicit rollback
    {
        Transaction tx(conn);
        EXPECT_TRUE(conn.execute("INSERT INTO kv VALUES ('key2', 'val2');").isOk());
        EXPECT_TRUE(tx.rollback().isOk());
        // Double rollback should error
        EXPECT_FALSE(tx.rollback().isOk());
    }

    // 3. Auto rollback on destruction
    {
        Transaction tx(conn);
        EXPECT_TRUE(conn.execute("INSERT INTO kv VALUES ('key3', 'val3');").isOk());
        // Destructor called here without commit
    }

    // Verify only key1 was committed
    auto q = conn.prepare("SELECT COUNT(*) FROM kv;").value();
    EXPECT_EQ(q.step(), StepResult::Row);
    EXPECT_EQ(q.getInt(0), 1);
}

TEST_F(CatalogDbTest, UpdateGpsCoordinates) {
    MediaItem item;
    item.file_path = "/photos/gps_test.jpg";
    item.file_name = "gps_test.jpg";
    item.file_size = 1024;
    item.file_modified_time = 1750000000;
    item.content_hash = "11112222333344445555666677778888";
    item.date_taken = 1750000000;
    auto insertRes = db.insertMedia(item);
    ASSERT_TRUE(insertRes.isOk());
    MediaId id = insertRes.value();

    auto initial = db.getMediaById(id);
    ASSERT_TRUE(initial.isOk());
    EXPECT_FALSE(initial.value().exif.has_gps);

    auto updateRes = db.updateGps(id, true, 48.8584, 2.2945, 35.0);
    ASSERT_TRUE(updateRes.isOk());

    auto updated = db.getMediaById(id);
    ASSERT_TRUE(updated.isOk());
    EXPECT_TRUE(updated.value().exif.has_gps);
    EXPECT_DOUBLE_EQ(updated.value().exif.latitude, 48.8584);
    EXPECT_DOUBLE_EQ(updated.value().exif.longitude, 2.2945);
    EXPECT_DOUBLE_EQ(updated.value().exif.altitude, 35.0);

    auto clearRes = db.updateGps(id, false, 0.0, 0.0, 0.0);
    ASSERT_TRUE(clearRes.isOk());
    auto cleared = db.getMediaById(id);
    ASSERT_TRUE(cleared.isOk());
    EXPECT_FALSE(cleared.value().exif.has_gps);
}

TEST_F(CatalogDbTest, MakePathsRelative) {
    MediaItem item1;
    item1.file_path = "/home/user/Pictures/2026/img1.jpg";
    item1.file_name = "img1.jpg";
    item1.content_hash = "h1";
    item1.thumb_small = "/home/user/.cache/thumbs/ab/cd/h1_256.jpg";
    item1.thumb_large = "/home/user/.cache/thumbs/ab/cd/h1_1024.jpg";
    auto id1 = db.insertMedia(item1).value();

    MediaItem item2;
    item2.file_path = "/home/user/Pictures/nature.jpg";
    item2.file_name = "nature.jpg";
    item2.content_hash = "h2";
    item2.thumb_small = "/home/user/.cache/thumbs/12/34/h2_256.jpg";
    auto id2 = db.insertMedia(item2).value();

    MediaItem item3;
    item3.file_path = "/var/other/img3.jpg";
    item3.file_name = "img3.jpg";
    item3.content_hash = "h3";
    auto id3 = db.insertMedia(item3).value();

    auto relRes = db.makePathsRelative("/home/user/Pictures", "/home/user/.cache/thumbs");
    ASSERT_TRUE(relRes.isOk());
    EXPECT_EQ(relRes.value(), 2);

    auto m1 = db.getMediaById(id1).value();
    EXPECT_EQ(m1.file_path, "2026/img1.jpg");
    EXPECT_EQ(m1.thumb_small, "ab/cd/h1_256.jpg");
    EXPECT_EQ(m1.thumb_large, "ab/cd/h1_1024.jpg");

    auto m2 = db.getMediaById(id2).value();
    EXPECT_EQ(m2.file_path, "nature.jpg");
    EXPECT_EQ(m2.thumb_small, "12/34/h2_256.jpg");

    auto m3 = db.getMediaById(id3).value();
    EXPECT_EQ(m3.file_path, "/var/other/img3.jpg");
}

TEST_F(CatalogDbTest, GetAllFolders) {
    // 1. Initially empty database has no folders
    auto emptyRes = db.getAllFolders();
    ASSERT_TRUE(emptyRes.isOk());
    EXPECT_TRUE(emptyRes.value().empty());

    // 2. Insert items across multiple folders, duplicates, and root
    MediaItem item1;
    item1.file_path = "family/birthday.jpg";
    item1.file_name = "birthday.jpg";
    item1.content_hash = "h1";
    db.insertMedia(item1);

    MediaItem item2;
    item2.file_path = "family/portrait.jpg";
    item2.file_name = "portrait.jpg";
    item2.content_hash = "h2";
    db.insertMedia(item2);

    MediaItem item3;
    item3.file_path = "nature/mountain.jpg";
    item3.file_name = "mountain.jpg";
    item3.content_hash = "h3";
    auto id3 = db.insertMedia(item3).value();

    MediaItem item4;
    item4.file_path = "Egypt/Betlehem/pic.jpg";
    item4.file_name = "pic.jpg";
    item4.content_hash = "h4";
    db.insertMedia(item4);

    MediaItem item5;
    item5.file_path = "root_file.jpg";
    item5.file_name = "root_file.jpg";
    item5.content_hash = "h5";
    db.insertMedia(item5);

    auto foldersRes = db.getAllFolders();
    ASSERT_TRUE(foldersRes.isOk());
    const auto& folders = foldersRes.value();
    EXPECT_EQ(folders.size(), 3u);
    EXPECT_EQ(folders[0], "Egypt/Betlehem");
    EXPECT_EQ(folders[1], "family");
    EXPECT_EQ(folders[2], "nature");

    // 3. Delete nature item -> "nature" folder disappears
    ASSERT_TRUE(db.deleteMedia(id3).isOk());
    auto afterDelRes = db.getAllFolders();
    ASSERT_TRUE(afterDelRes.isOk());
    const auto& afterDel = afterDelRes.value();
    EXPECT_EQ(afterDel.size(), 2u);
    EXPECT_EQ(afterDel[0], "Egypt/Betlehem");
    EXPECT_EQ(afterDel[1], "family");
}

TEST_F(CatalogDbTest, GetAllTagsPhotoCoverPriority) {
    // 1. Tag with both video and photo, where video has newer date_taken than photo.
    // Tag should prioritize photo for cover_media_id and cover_hash.
    auto eventTagRes = db.createOrGetTag("Conference 2026", "events");
    ASSERT_TRUE(eventTagRes.isOk());
    TagId eventTagId = eventTagRes.value();

    MediaItem photoItem;
    photoItem.file_path = "conf/stage_photo.jpg";
    photoItem.file_name = "stage_photo.jpg";
    photoItem.content_hash = "hash_photo_1";
    photoItem.media_type = "photo";
    photoItem.date_taken = 1000;
    auto photoId = db.insertMedia(photoItem).value();
    ASSERT_TRUE(db.addTagToMedia(photoId, eventTagId).isOk());

    MediaItem videoItem;
    videoItem.file_path = "conf/keynote_video.mp4";
    videoItem.file_name = "keynote_video.mp4";
    videoItem.content_hash = "hash_video_1";
    videoItem.media_type = "video";
    videoItem.date_taken = 2000; // Newer than photo!
    auto videoId = db.insertMedia(videoItem).value();
    ASSERT_TRUE(db.addTagToMedia(videoId, eventTagId).isOk());

    // 2. Tag with only a video -> falls back to video
    auto videoOnlyTagRes = db.createOrGetTag("Concert", "events");
    ASSERT_TRUE(videoOnlyTagRes.isOk());
    TagId videoOnlyTagId = videoOnlyTagRes.value();

    MediaItem concertVideo;
    concertVideo.file_path = "concert/live.mp4";
    concertVideo.file_name = "live.mp4";
    concertVideo.content_hash = "hash_video_concert";
    concertVideo.media_type = "video";
    concertVideo.date_taken = 3000;
    auto concertVideoId = db.insertMedia(concertVideo).value();
    ASSERT_TRUE(db.addTagToMedia(concertVideoId, videoOnlyTagId).isOk());

    auto tagsRes = db.getAllTags();
    ASSERT_TRUE(tagsRes.isOk());
    const auto& tags = tagsRes.value();

    // Verify Conference 2026 prefers photo over newer video
    auto confIt = std::find_if(tags.begin(), tags.end(), [](const Tag& t) { return t.name == "Conference 2026"; });
    ASSERT_NE(confIt, tags.end());
    EXPECT_EQ(confIt->media_count, 2);
    ASSERT_TRUE(confIt->cover_media_id.has_value());
    EXPECT_EQ(*confIt->cover_media_id, photoId);
    EXPECT_FALSE(confIt->cover_hash.empty());
    EXPECT_EQ(confIt->cover_hash, "hash_photo_1");
    ASSERT_TRUE(confIt->first_date.has_value());
    EXPECT_EQ(*confIt->first_date, 1000);
    ASSERT_TRUE(confIt->last_date.has_value());
    EXPECT_EQ(*confIt->last_date, 2000);

    // Verify Concert with only video falls back to video
    auto concertIt = std::find_if(tags.begin(), tags.end(), [](const Tag& t) { return t.name == "Concert"; });
    ASSERT_NE(concertIt, tags.end());
    EXPECT_EQ(concertIt->media_count, 1);
    ASSERT_TRUE(concertIt->cover_media_id.has_value());
    EXPECT_EQ(*concertIt->cover_media_id, concertVideoId);
    EXPECT_FALSE(concertIt->cover_hash.empty());
    EXPECT_EQ(concertIt->cover_hash, "hash_video_concert");
    ASSERT_TRUE(concertIt->first_date.has_value());
    EXPECT_EQ(*concertIt->first_date, 3000);
    ASSERT_TRUE(concertIt->last_date.has_value());
    EXPECT_EQ(*concertIt->last_date, 3000);
}

TEST_F(CatalogDbTest, MigrationV9AndDuplicateDismissals) {
    EXPECT_EQ(Schema::CurrentVersion, 9);

    MediaItem itemA;
    itemA.file_path = "dup/item_a.jpg";
    itemA.file_name = "item_a.jpg";
    itemA.content_hash = "hash_exact_1";
    itemA.file_size = 1000;
    auto idA = db.insertMedia(itemA).value();

    MediaItem itemB;
    itemB.file_path = "dup/item_b.jpg";
    itemB.file_name = "item_b.jpg";
    itemB.content_hash = "hash_exact_1";
    itemB.file_size = 1000;
    auto idB = db.insertMedia(itemB).value();

    EXPECT_NE(idA, idB);
    MediaId minId = std::min(idA, idB);
    MediaId maxId = std::max(idA, idB);

    // Initial dismissals should be empty
    auto initialPairs = db.getDismissedDuplicatePairs();
    ASSERT_TRUE(initialPairs.isOk());
    EXPECT_TRUE(initialPairs.value().empty());

    // Dismiss with inverted order: should canonicalize minId < maxId
    ASSERT_TRUE(db.dismissDuplicatePair(maxId, minId).isOk());

    auto pairs = db.getDismissedDuplicatePairs();
    ASSERT_TRUE(pairs.isOk());
    ASSERT_EQ(pairs.value().size(), 1u);
    EXPECT_EQ(pairs.value()[0].first, minId);
    EXPECT_EQ(pairs.value()[0].second, maxId);

    // Idempotent dismiss
    EXPECT_TRUE(db.dismissDuplicatePair(minId, maxId).isOk());
    EXPECT_EQ(db.getDismissedDuplicatePairs().value().size(), 1u);

    // Reject self-dismissal
    EXPECT_FALSE(db.dismissDuplicatePair(minId, minId).isOk());

    // Foreign key cascade: deleting itemA should remove the dismissal pair
    ASSERT_TRUE(db.deleteMedia(idA).isOk());
    auto afterDelete = db.getDismissedDuplicatePairs();
    ASSERT_TRUE(afterDelete.isOk());
    EXPECT_TRUE(afterDelete.value().empty());
}

TEST_F(CatalogDbTest, FindExactDuplicateGroups) {
    // 3 items with hash1, size 1000
    MediaItem item1;
    item1.file_path = "group1/1.jpg";
    item1.file_name = "1.jpg";
    item1.content_hash = "sha256_group1";
    item1.file_size = 1000;
    auto id1 = db.insertMedia(item1).value();

    MediaItem item2;
    item2.file_path = "group1/2.jpg";
    item2.file_name = "2.jpg";
    item2.content_hash = "sha256_group1";
    item2.file_size = 1000;
    auto id2 = db.insertMedia(item2).value();

    MediaItem item3;
    item3.file_path = "group1/3.jpg";
    item3.file_name = "3.jpg";
    item3.content_hash = "sha256_group1";
    item3.file_size = 1000;
    auto id3 = db.insertMedia(item3).value();

    // 2 items with hash2, size 2000
    MediaItem item4;
    item4.file_path = "group2/4.jpg";
    item4.file_name = "4.jpg";
    item4.content_hash = "sha256_group2";
    item4.file_size = 2000;
    auto id4 = db.insertMedia(item4).value();

    MediaItem item5;
    item5.file_path = "group2/5.jpg";
    item5.file_name = "5.jpg";
    item5.content_hash = "sha256_group2";
    item5.file_size = 2000;
    auto id5 = db.insertMedia(item5).value();

    // 1 item with hash1 but different size (should NOT group with group1)
    MediaItem itemTrunc;
    itemTrunc.file_path = "group1/trunc.jpg";
    itemTrunc.file_name = "trunc.jpg";
    itemTrunc.content_hash = "sha256_group1";
    itemTrunc.file_size = 500;
    auto idTrunc = db.insertMedia(itemTrunc).value();

    // 1 item with unique hash
    MediaItem itemUnique;
    itemUnique.file_path = "unique/u.jpg";
    itemUnique.file_name = "u.jpg";
    itemUnique.content_hash = "sha256_unique";
    itemUnique.file_size = 1000;
    auto idUnique = db.insertMedia(itemUnique).value();

    // 1 item with empty hash (legacy, should be excluded by partial index)
    MediaItem itemEmptyHash;
    itemEmptyHash.file_path = "legacy/empty.jpg";
    itemEmptyHash.file_name = "empty.jpg";
    itemEmptyHash.content_hash = "";
    itemEmptyHash.file_size = 1000;
    db.insertMedia(itemEmptyHash);

    // Global scan: returns groups largest-first
    auto groupsRes = db.findExactDuplicateGroups();
    ASSERT_TRUE(groupsRes.isOk());
    const auto& groups = groupsRes.value();
    ASSERT_EQ(groups.size(), 2u);

    // First group has 3 members (id1, id2, id3)
    EXPECT_EQ(groups[0].size(), 3u);
    EXPECT_EQ(groups[0][0], id1);
    EXPECT_EQ(groups[0][1], id2);
    EXPECT_EQ(groups[0][2], id3);

    // Second group has 2 members (id4, id5)
    EXPECT_EQ(groups[1].size(), 2u);
    EXPECT_EQ(groups[1][0], id4);
    EXPECT_EQ(groups[1][1], id5);

    // Scoped scan: restrict to id1, id2, idTrunc, idUnique
    auto scopedRes = db.findExactDuplicateGroups({id1, id2, idTrunc, idUnique});
    ASSERT_TRUE(scopedRes.isOk());
    const auto& scopedGroups = scopedRes.value();
    ASSERT_EQ(scopedGroups.size(), 1u);
    EXPECT_EQ(scopedGroups[0].size(), 2u);
    EXPECT_EQ(scopedGroups[0][0], id1);
    EXPECT_EQ(scopedGroups[0][1], id2);
}

TEST_F(CatalogDbTest, MergeMediaMetadataAndCovers) {
    // 1. Create keeper and source items
    MediaItem keeper;
    keeper.file_path = "photos/keeper.jpg";
    keeper.file_name = "keeper.jpg";
    keeper.content_hash = "hash_merge";
    keeper.file_size = 10000;
    keeper.rating = 0; // empty
    keeper.flag = FlagState::Unflagged; // empty
    keeper.date_taken = 0; // empty
    keeper.caption = ""; // empty
    keeper.exif.has_gps = false; // empty
    auto keeperId = db.insertMedia(keeper).value();

    MediaItem source;
    source.file_path = "photos/source.jpg";
    source.file_name = "source.jpg";
    source.content_hash = "hash_merge";
    source.file_size = 10000;
    source.rating = 5;
    source.flag = FlagState::Pick;
    source.date_taken = 1710000000;
    source.exif.date_taken_str = "2024:03:09 12:00:00";
    source.caption = "Family Reunion";
    source.exif.has_gps = true;
    source.exif.latitude = 37.7749;
    source.exif.longitude = -122.4194;
    source.exif.altitude = 15.0;
    auto sourceId = db.insertMedia(source).value();

    // 2. Tags setup
    auto tagVacation = db.createOrGetTag("Vacation", "keyword").value();
    auto tagPeople = db.createOrGetTag("People", "keyword").value();
    // Keeper has tagVacation (provenance: auto, manual=0)
    ASSERT_TRUE(db.addTagToMedia(keeperId, tagVacation).isOk());
    ASSERT_TRUE(db.connection().execute(
        "UPDATE media_tag_provenance SET manual = 0 WHERE media_id = " +
        std::to_string(keeperId) + " AND tag_id = " + std::to_string(tagVacation) + ";").isOk());
    // Source has tagVacation (provenance: manual=1) and tagPeople (provenance: auto, manual=0)
    ASSERT_TRUE(db.addTagToMedia(sourceId, tagVacation).isOk());
    ASSERT_TRUE(db.addTagToMedia(sourceId, tagPeople).isOk());
    ASSERT_TRUE(db.connection().execute(
        "UPDATE media_tag_provenance SET manual = 0 WHERE media_id = " +
        std::to_string(sourceId) + " AND tag_id = " + std::to_string(tagPeople) + ";").isOk());

    // 3. Album setup
    // Album 1: source only at pos 0 -> keeper should take its place at pos 0
    auto album1 = db.createAlbum("Album 1").value();
    ASSERT_TRUE(db.addMediaToAlbum(album1, sourceId, 0).isOk());

    // Album 2: keeper at pos 0, source at pos 1, thirdItem at pos 2 -> source deleted, thirdItem compacted to pos 1
    MediaItem thirdItem;
    thirdItem.file_path = "photos/third.jpg";
    thirdItem.file_name = "third.jpg";
    thirdItem.content_hash = "hash_third";
    auto thirdId = db.insertMedia(thirdItem).value();

    auto album2 = db.createAlbum("Album 2").value();
    ASSERT_TRUE(db.addMediaToAlbum(album2, keeperId, 0).isOk());
    ASSERT_TRUE(db.addMediaToAlbum(album2, sourceId, 1).isOk());
    ASSERT_TRUE(db.addMediaToAlbum(album2, thirdId, 2).isOk());

    // Album 3: cover is sourceId -> should be re-pointed to keeperId
    auto album3 = db.createAlbum("Album 3").value();
    ASSERT_TRUE(db.addMediaToAlbum(album3, sourceId, 0).isOk());
    ASSERT_TRUE(db.setAlbumCover(album3, sourceId).isOk());
    EXPECT_EQ(db.getAlbumById(album3).value().cover_media_id, sourceId);

    // 4. Perform merge
    ASSERT_TRUE(db.mergeMediaMetadata(keeperId, sourceId).isOk());

    // 5. Verify keeper inherited empty fields
    auto mergedKeeper = db.getMediaById(keeperId).value();
    EXPECT_EQ(mergedKeeper.rating, 5);
    EXPECT_EQ(mergedKeeper.flag, FlagState::Pick);
    EXPECT_EQ(mergedKeeper.date_taken, 1710000000);
    EXPECT_EQ(mergedKeeper.exif.date_taken_str, "2024:03:09 12:00:00");
    EXPECT_EQ(mergedKeeper.caption, "Family Reunion");
    EXPECT_TRUE(mergedKeeper.exif.has_gps);
    EXPECT_DOUBLE_EQ(mergedKeeper.exif.latitude, 37.7749);
    EXPECT_DOUBLE_EQ(mergedKeeper.exif.longitude, -122.4194);
    EXPECT_DOUBLE_EQ(mergedKeeper.exif.altitude, 15.0);

    // 6. Verify tags unioned and tag provenance manual = MAX(existing, excluded)
    auto keeperTags = db.getTagsForMedia(keeperId).value();
    EXPECT_EQ(keeperTags.size(), 2u);

    auto checkProv = [&](MediaId mid, TagId tid) -> int {
        auto stmtRes = db.connection().prepare("SELECT manual FROM media_tag_provenance WHERE media_id = ? AND tag_id = ?;");
        if (!stmtRes.isOk()) return -1;
        auto stmt = std::move(stmtRes.value());
        stmt.bind(1, mid);
        stmt.bind(2, tid);
        if (stmt.step() == StepResult::Row) return stmt.getInt(0);
        return -1;
    };
    // tagVacation: keeper was manual=0, source was manual=1 -> MAX is 1
    EXPECT_EQ(checkProv(keeperId, tagVacation), 1);
    // tagPeople: keeper was absent, source was manual=0 -> inherited as 0
    EXPECT_EQ(checkProv(keeperId, tagPeople), 0);

    // 7. Verify album 1 membership (update-in-place)
    auto album1Media = db.getMediaInAlbum(album1).value();
    ASSERT_EQ(album1Media.size(), 1u);
    EXPECT_EQ(album1Media[0].id, keeperId);

    // 8. Verify album 2 membership (delete + compact)
    auto album2Media = db.getMediaInAlbum(album2).value();
    ASSERT_EQ(album2Media.size(), 2u);
    EXPECT_EQ(album2Media[0].id, keeperId);
    EXPECT_EQ(album2Media[1].id, thirdId);

    // 9. Verify album 3 cover re-pointed
    auto album3Obj = db.getAlbumById(album3).value();
    ASSERT_TRUE(album3Obj.cover_media_id.has_value());
    EXPECT_EQ(*album3Obj.cover_media_id, keeperId);
}

TEST_F(CatalogDbTest, MergeMediaMetadataPre1970Dates) {
    // Pre-1970 negative dates are valid and should not be treated as empty
    int64_t pre1970Timestamp = -631152000; // year 1950

    // Case A: Keeper has pre-1970 date; source has modern date -> keeper is NOT overwritten
    MediaItem keeperA;
    keeperA.file_path = "pre1970/a.jpg";
    keeperA.file_name = "a.jpg";
    keeperA.content_hash = "hash_preA";
    keeperA.date_taken = pre1970Timestamp;
    keeperA.exif.date_taken_str = "1950:01:01 00:00:00";
    auto idKA = db.insertMedia(keeperA).value();

    MediaItem sourceA;
    sourceA.file_path = "pre1970/srcA.jpg";
    sourceA.file_name = "srcA.jpg";
    sourceA.content_hash = "hash_preA";
    sourceA.date_taken = 1700000000;
    sourceA.exif.date_taken_str = "2023:11:14 22:13:20";
    auto idSA = db.insertMedia(sourceA).value();

    ASSERT_TRUE(db.mergeMediaMetadata(idKA, idSA).isOk());
    auto resA = db.getMediaById(idKA).value();
    EXPECT_EQ(resA.date_taken, pre1970Timestamp);
    EXPECT_EQ(resA.exif.date_taken_str, "1950:01:01 00:00:00");

    // Case B: Keeper has 0 (empty); source has pre-1970 date -> keeper inherits pre-1970 date
    MediaItem keeperB;
    keeperB.file_path = "pre1970/b.jpg";
    keeperB.file_name = "b.jpg";
    keeperB.content_hash = "hash_preB";
    keeperB.date_taken = 0; // empty
    auto idKB = db.insertMedia(keeperB).value();

    MediaItem sourceB;
    sourceB.file_path = "pre1970/srcB.jpg";
    sourceB.file_name = "srcB.jpg";
    sourceB.content_hash = "hash_preB";
    sourceB.date_taken = pre1970Timestamp;
    sourceB.exif.date_taken_str = "1950:01:01 00:00:00";
    auto idSB = db.insertMedia(sourceB).value();

    ASSERT_TRUE(db.mergeMediaMetadata(idKB, idSB).isOk());
    auto resB = db.getMediaById(idKB).value();
    EXPECT_EQ(resB.date_taken, pre1970Timestamp);
    EXPECT_EQ(resB.exif.date_taken_str, "1950:01:01 00:00:00");
}

TEST_F(CatalogDbTest, MergeMediaMetadataRollbackOnMidMergeFailure) {
    // 1. Setup keeper and source
    MediaItem keeper;
    keeper.file_path = "photos/keeper_rollback.jpg";
    keeper.file_name = "keeper_rollback.jpg";
    keeper.content_hash = "hash_rb";
    keeper.rating = 1;
    keeper.caption = "Original Keeper Caption";
    auto keeperId = db.insertMedia(keeper).value();

    MediaItem source;
    source.file_path = "photos/source_rollback.jpg";
    source.file_name = "source_rollback.jpg";
    source.content_hash = "hash_rb";
    source.rating = 5;
    source.caption = "Source Caption";
    auto sourceId = db.insertMedia(source).value();

    auto tagSource = db.createOrGetTag("RollbackTag", "keyword").value();
    ASSERT_TRUE(db.addTagToMedia(sourceId, tagSource).isOk());

    auto album = db.createAlbum("Rollback Album").value();
    ASSERT_TRUE(db.addMediaToAlbum(album, sourceId, 0).isOk());
    ASSERT_TRUE(db.setAlbumCover(album, sourceId).isOk());

    // 2. Install a trigger that causes failure when updating album cover (late in mergeMediaMetadata)
    ASSERT_TRUE(db.connection().execute(
        "CREATE TRIGGER abort_cover_update BEFORE UPDATE OF cover_media_id ON albums "
        "BEGIN SELECT RAISE(ABORT, 'Simulated mid-merge album cover failure'); END;").isOk());

    // 3. Attempt merge: should fail mid-merge
    Status mergeStatus = db.mergeMediaMetadata(keeperId, sourceId);
    EXPECT_FALSE(mergeStatus.isOk());

    // 4. Verify transaction rolled back:
    // Keeper metadata must remain unchanged
    auto keeperAfter = db.getMediaById(keeperId).value();
    EXPECT_EQ(keeperAfter.rating, 1);
    EXPECT_EQ(keeperAfter.caption, "Original Keeper Caption");

    // Keeper must not have the tag from source
    auto keeperTags = db.getTagsForMedia(keeperId).value();
    EXPECT_TRUE(keeperTags.empty());

    // Album cover must still be sourceId
    auto albumObj = db.getAlbumById(album).value();
    ASSERT_TRUE(albumObj.cover_media_id.has_value());
    EXPECT_EQ(*albumObj.cover_media_id, sourceId);

    // 5. Clean up trigger and verify merge succeeds cleanly
    ASSERT_TRUE(db.connection().execute("DROP TRIGGER abort_cover_update;").isOk());
    ASSERT_TRUE(db.mergeMediaMetadata(keeperId, sourceId).isOk());

    auto keeperSuccess = db.getMediaById(keeperId).value();
    EXPECT_EQ(keeperSuccess.caption, "Original Keeper Caption"); // keeper caption not empty, retained
    EXPECT_EQ(keeperSuccess.rating, 1); // keeper rating not 0, retained
    auto keeperTagsSuccess = db.getTagsForMedia(keeperId).value();
    EXPECT_EQ(keeperTagsSuccess.size(), 1u);
}



namespace imagine::faces {
Status syncFaceTagOwnership(db::Connection& conn, MediaId mediaId, TagId tagId);
}

TEST(CatalogDbMigrationTest, PersistedV7CatalogUpgradesToV8SmartTags) {
    const auto path = std::filesystem::temp_directory_path() /
        ("imagine_catalog_v7_upgrade_" + std::to_string(
            std::chrono::system_clock::now().time_since_epoch().count()) + ".db");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    MediaId mediaId = 0;
    TagId manualTag = 0;
    TagId faceTag = 0;
    {
        CatalogDb initial;
        ASSERT_TRUE(initial.open(path.string()).isOk());

        MediaItem media;
        media.file_path = "persisted/v7_photo.jpg";
        media.file_name = "v7_photo.jpg";
        media.content_hash = "v7-hash-123";
        media.media_type = "photo";
        mediaId = initial.insertMedia(media).value();

        manualTag = initial.createOrGetTag("Manual Label", "keyword").value();
        faceTag = initial.createOrGetTag("Alice", "people").value();

        // Simulate v7 catalog:
        // 1. manual tag has manual = 1
        ASSERT_TRUE(initial.addTagToMedia(mediaId, manualTag).isOk());

        // 2. face tag has manual = 0
        auto& conn = initial.connection();
        ASSERT_TRUE(conn.execute("INSERT OR IGNORE INTO media_tags(media_id, tag_id) VALUES(" +
                                 std::to_string(mediaId) + "," + std::to_string(faceTag) + ");").isOk());
        ASSERT_TRUE(conn.execute("INSERT OR REPLACE INTO media_tag_provenance(media_id, tag_id, manual) VALUES(" +
                                 std::to_string(mediaId) + "," + std::to_string(faceTag) + ", 0);").isOk());

        // 3. insert a semantic job
        ASSERT_TRUE(conn.execute(
            "INSERT INTO semantic_jobs(state, scope, model_id, total, processed, skipped, failed, remaining, error, created_at, updated_at) "
            "VALUES('completed', 'catalog', 'model1', 1, 1, 0, 0, 0, '', 100, 200);"
        ).isOk());

        // Now revert to a true v7 schema state:
        ASSERT_TRUE(conn.execute("DROP TABLE IF EXISTS smart_tag_state;").isOk());
        ASSERT_TRUE(conn.execute("DROP TABLE IF EXISTS smart_tag_rejections;").isOk());
        ASSERT_TRUE(conn.execute("DROP INDEX IF EXISTS idx_media_tag_provenance_source;").isOk());
        ASSERT_TRUE(conn.execute("DROP INDEX IF EXISTS idx_semantic_jobs_kind;").isOk());

        // Recreate media_tag_provenance without 'source' column
        ASSERT_TRUE(conn.execute(R"SQL(
            CREATE TABLE mtp_old (
                media_id INTEGER NOT NULL REFERENCES media_items(id) ON DELETE CASCADE,
                tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
                manual INTEGER NOT NULL DEFAULT 1,
                PRIMARY KEY(media_id, tag_id)
            );
            INSERT INTO mtp_old SELECT media_id, tag_id, manual FROM media_tag_provenance;
            DROP TABLE media_tag_provenance;
            ALTER TABLE mtp_old RENAME TO media_tag_provenance;
        )SQL").isOk());

        // Recreate semantic_jobs without 'kind' column
        ASSERT_TRUE(conn.execute(R"SQL(
            CREATE TABLE sj_old (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                state TEXT NOT NULL,
                scope TEXT NOT NULL,
                model_id TEXT NOT NULL DEFAULT '',
                total INTEGER NOT NULL DEFAULT 0,
                processed INTEGER NOT NULL DEFAULT 0,
                skipped INTEGER NOT NULL DEFAULT 0,
                failed INTEGER NOT NULL DEFAULT 0,
                remaining INTEGER NOT NULL DEFAULT 0,
                error TEXT NOT NULL DEFAULT '',
                created_at INTEGER NOT NULL,
                updated_at INTEGER NOT NULL
            );
            INSERT INTO sj_old SELECT id, state, scope, model_id, total, processed, skipped, failed, remaining, error, created_at, updated_at FROM semantic_jobs;
            DROP TABLE semantic_jobs;
            ALTER TABLE sj_old RENAME TO semantic_jobs;
        )SQL").isOk());

        ASSERT_TRUE(conn.execute("DELETE FROM schema_version WHERE version >= 8;").isOk());
        initial.close();
    }

    {
        CatalogDb upgraded;
        ASSERT_TRUE(upgraded.open(path.string()).isOk());

        auto versionRes = upgraded.connection().prepare("SELECT MAX(version) FROM schema_version;");
        ASSERT_TRUE(versionRes.isOk());
        auto version = std::move(versionRes.value());
        ASSERT_EQ(version.step(), StepResult::Row);
        EXPECT_EQ(version.getInt(0), Schema::CurrentVersion);

        // Verify backfill: manual=0 was set to source='face'
        auto provFaceRes = upgraded.connection().prepare(
            "SELECT source, manual FROM media_tag_provenance WHERE media_id = ? AND tag_id = ?;");
        ASSERT_TRUE(provFaceRes.isOk());
        auto provFace = std::move(provFaceRes.value());
        provFace.bind(1, mediaId);
        provFace.bind(2, faceTag);
        ASSERT_EQ(provFace.step(), StepResult::Row);
        EXPECT_EQ(provFace.getString(0), "face");
        EXPECT_EQ(provFace.getInt(1), 0);

        // Verify manual=1 has source='manual'
        auto provManRes = upgraded.connection().prepare(
            "SELECT source, manual FROM media_tag_provenance WHERE media_id = ? AND tag_id = ?;");
        ASSERT_TRUE(provManRes.isOk());
        auto provMan = std::move(provManRes.value());
        provMan.bind(1, mediaId);
        provMan.bind(2, manualTag);
        ASSERT_EQ(provMan.step(), StepResult::Row);
        EXPECT_EQ(provMan.getString(0), "manual");
        EXPECT_EQ(provMan.getInt(1), 1);

        // Verify semantic_jobs has kind='index'
        auto jobRes = upgraded.connection().prepare("SELECT kind FROM semantic_jobs LIMIT 1;");
        ASSERT_TRUE(jobRes.isOk());
        auto job = std::move(jobRes.value());
        ASSERT_EQ(job.step(), StepResult::Row);
        EXPECT_EQ(job.getString(0), "index");

        // Verify tables exist
        auto stateRes = upgraded.connection().prepare("SELECT COUNT(*) FROM smart_tag_state;");
        ASSERT_TRUE(stateRes.isOk());
        auto stateStmt = std::move(stateRes.value());
        EXPECT_EQ(stateStmt.step(), StepResult::Row);

        auto rejRes = upgraded.connection().prepare("SELECT COUNT(*) FROM smart_tag_rejections;");
        ASSERT_TRUE(rejRes.isOk());
        auto rejStmt = std::move(rejRes.value());
        EXPECT_EQ(rejStmt.step(), StepResult::Row);

        upgraded.close();
    }
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
}

TEST_F(CatalogDbTest, ReplaceSmartTagsPreservesManualAndFaceTagsAndUpsertsState) {
    MediaItem m1;
    m1.file_path = "photos/photo1.jpg";
    m1.file_name = "photo1.jpg";
    m1.content_hash = "hash-photo-1";
    m1.media_type = "photo";
    MediaId mid1 = db.insertMedia(m1).value();

    MediaItem m2;
    m2.file_path = "photos/photo2.jpg";
    m2.file_name = "photo2.jpg";
    m2.content_hash = "hash-photo-2";
    m2.media_type = "photo";
    MediaId mid2 = db.insertMedia(m2).value();

    TagId tManual = db.createOrGetTag("Landscape", "keyword").value();
    TagId tFace = db.createOrGetTag("Alice", "people").value();
    TagId tClip1 = db.createOrGetTag("Mountain", "keyword").value();
    TagId tClip2 = db.createOrGetTag("Snow", "keyword").value();
    TagId tClip3 = db.createOrGetTag("Sky", "keyword").value();

    ASSERT_TRUE(db.addTagToMedia(mid1, tManual).isOk());

    // Attach face tag manually with source='face'
    auto& conn = db.connection();
    ASSERT_TRUE(conn.execute("INSERT INTO media_tags(media_id, tag_id) VALUES(" +
                             std::to_string(mid1) + "," + std::to_string(tFace) + ");").isOk());
    ASSERT_TRUE(conn.execute("INSERT INTO media_tag_provenance(media_id, tag_id, manual, source) VALUES(" +
                             std::to_string(mid1) + "," + std::to_string(tFace) + ", 0, 'face');").isOk());

    std::vector<SmartTagRecord> records = {
        {mid1, {tClip1, tClip2}, "hash-photo-1"},
        {mid2, {tClip2}, "hash-photo-2"}
    };
    ASSERT_TRUE(db.replaceSmartTags(records, "clip-vit-b-32", "rev1", "tax-v1").isOk());

    // Check tags on mid1
    auto tags1 = db.getTagsForMedia(mid1).value();
    EXPECT_EQ(tags1.size(), 4u);
    auto findTag = [&](const std::vector<Tag>& tags, TagId id) -> const Tag* {
        for (const auto& t : tags) if (t.id == id) return &t;
        return nullptr;
    };
    ASSERT_NE(findTag(tags1, tManual), nullptr);
    EXPECT_EQ(findTag(tags1, tManual)->source, "manual");

    ASSERT_NE(findTag(tags1, tFace), nullptr);
    EXPECT_EQ(findTag(tags1, tFace)->source, "face");

    ASSERT_NE(findTag(tags1, tClip1), nullptr);
    EXPECT_EQ(findTag(tags1, tClip1)->source, "clip");

    ASSERT_NE(findTag(tags1, tClip2), nullptr);
    EXPECT_EQ(findTag(tags1, tClip2)->source, "clip");

    // Check batch get tags
    auto batchTags = db.getTagsForMediaBatch({mid1, mid2}).value();
    EXPECT_EQ(batchTags[mid1].size(), 4u);
    EXPECT_EQ(batchTags[mid2].size(), 1u);
    EXPECT_EQ(batchTags[mid2][0].source, "clip");

    // Check getAllTags metadata
    auto allTags = db.getAllTags().value();
    for (const auto& t : allTags) {
        if (t.id == tClip2) {
            EXPECT_EQ(t.ai_count, 2);
            EXPECT_EQ(t.media_count, 2);
        } else if (t.id == tClip1) {
            EXPECT_EQ(t.ai_count, 1);
            EXPECT_EQ(t.media_count, 1);
        } else if (t.id == tManual) {
            EXPECT_EQ(t.ai_count, 0);
            EXPECT_EQ(t.media_count, 1);
        } else if (t.id == tFace) {
            EXPECT_EQ(t.ai_count, 0);
            EXPECT_EQ(t.media_count, 1);
        }
    }

    // Verify smart_tag_state rows
    auto stateStmtRes = conn.prepare("SELECT content_hash, model_id, model_revision, taxonomy_version, label_count, updated_at FROM smart_tag_state WHERE media_id = ?;");
    ASSERT_TRUE(stateStmtRes.isOk());
    auto stateStmt = std::move(stateStmtRes.value());
    stateStmt.bind(1, mid1);
    ASSERT_EQ(stateStmt.step(), StepResult::Row);
    EXPECT_EQ(stateStmt.getString(0), "hash-photo-1");
    EXPECT_EQ(stateStmt.getString(1), "clip-vit-b-32");
    EXPECT_EQ(stateStmt.getString(2), "rev1");
    EXPECT_EQ(stateStmt.getString(3), "tax-v1");
    EXPECT_EQ(stateStmt.getInt64(4), 2);
    EXPECT_GT(stateStmt.getInt64(5), 0);

    // Update mid1: drop tClip1, keep tClip2, add tClip3
    records = {{mid1, {tClip2, tClip3}, "hash-photo-1"}};
    ASSERT_TRUE(db.replaceSmartTags(records, "clip-vit-b-32", "rev1", "tax-v1").isOk());

    tags1 = db.getTagsForMedia(mid1).value();
    EXPECT_EQ(tags1.size(), 4u);
    EXPECT_EQ(findTag(tags1, tClip1), nullptr);
    ASSERT_NE(findTag(tags1, tClip2), nullptr);
    ASSERT_NE(findTag(tags1, tClip3), nullptr);
    ASSERT_NE(findTag(tags1, tManual), nullptr);
    ASSERT_NE(findTag(tags1, tFace), nullptr);

    // If replaceSmartTags includes a tag that is already manual, manual ownership is preserved
    records = {{mid1, {tManual, tClip3}, "hash-photo-1"}};
    ASSERT_TRUE(db.replaceSmartTags(records, "clip-vit-b-32", "rev1", "tax-v1").isOk());
    tags1 = db.getTagsForMedia(mid1).value();
    ASSERT_NE(findTag(tags1, tManual), nullptr);
    EXPECT_EQ(findTag(tags1, tManual)->source, "manual");

    auto provRes = conn.prepare("SELECT manual, source FROM media_tag_provenance WHERE media_id = ? AND tag_id = ?;");
    ASSERT_TRUE(provRes.isOk());
    auto prov = std::move(provRes.value());
    prov.bind(1, mid1);
    prov.bind(2, tManual);
    ASSERT_EQ(prov.step(), StepResult::Row);
    EXPECT_EQ(prov.getInt(0), 1);
    EXPECT_EQ(prov.getString(1), "manual");
}

TEST_F(CatalogDbTest, RemoveClipTagRecordsRejectionAndReAddClearsIt) {
    MediaItem m;
    m.file_path = "photos/rejection_test.jpg";
    m.file_name = "rejection_test.jpg";
    m.content_hash = "rej-hash";
    m.media_type = "photo";
    MediaId mid = db.insertMedia(m).value();

    TagId tManual = db.createOrGetTag("Outdoor", "keyword").value();
    TagId tClip = db.createOrGetTag("Forest", "keyword").value();
    ASSERT_TRUE(db.addTagToMedia(mid, tManual).isOk());

    std::vector<SmartTagRecord> records = {{mid, {tClip}, "rej-hash"}};
    ASSERT_TRUE(db.replaceSmartTags(records, "model", "rev", "tax").isOk());

    // Removing manual tag does NOT record a rejection
    ASSERT_TRUE(db.removeTagFromMedia(mid, tManual).isOk());
    auto& conn = db.connection();
    auto countRej = conn.prepare("SELECT COUNT(*) FROM smart_tag_rejections WHERE media_id = ? AND tag_id = ?;");
    ASSERT_TRUE(countRej.isOk());
    auto countStmt = std::move(countRej.value());
    countStmt.bind(1, mid);
    countStmt.bind(2, tManual);
    ASSERT_EQ(countStmt.step(), StepResult::Row);
    EXPECT_EQ(countStmt.getInt64(0), 0);

    // Removing clip tag DOES record rejection
    ASSERT_TRUE(db.removeTagFromMedia(mid, tClip).isOk());
    countStmt.reset();
    countStmt.bind(1, mid);
    countStmt.bind(2, tClip);
    ASSERT_EQ(countStmt.step(), StepResult::Row);
    EXPECT_EQ(countStmt.getInt64(0), 1);

    // Re-adding manually clears rejection and sets source='manual'
    ASSERT_TRUE(db.addTagToMedia(mid, tClip).isOk());
    countStmt.reset();
    countStmt.bind(1, mid);
    countStmt.bind(2, tClip);
    ASSERT_EQ(countStmt.step(), StepResult::Row);
    EXPECT_EQ(countStmt.getInt64(0), 0);

    auto tags = db.getTagsForMedia(mid).value();
    ASSERT_EQ(tags.size(), 1u);
    EXPECT_EQ(tags[0].id, tClip);
    EXPECT_EQ(tags[0].source, "manual");

    // Test replaceTagAssignments also clears rejections
    records = {{mid, {tClip}, "rej-hash"}};
    // replaceSmartTags will not downgrade manual
    ASSERT_TRUE(db.replaceSmartTags(records, "model", "rev", "tax").isOk());
    // remove tag to create rejection
    ASSERT_TRUE(conn.execute("INSERT OR REPLACE INTO smart_tag_rejections(media_id, tag_id, rejected_at) VALUES(" +
                             std::to_string(mid) + "," + std::to_string(tClip) + ", 1234);").isOk());
    // now call replaceTagAssignments
    ASSERT_TRUE(db.replaceTagAssignments("Forest", "keyword", {mid}).isOk());
    countStmt.reset();
    countStmt.bind(1, mid);
    countStmt.bind(2, tClip);
    ASSERT_EQ(countStmt.step(), StepResult::Row);
    EXPECT_EQ(countStmt.getInt64(0), 0);
}

TEST_F(CatalogDbTest, ClearSmartTagsLeavesManualAndFaceTagsIntact) {
    MediaItem m;
    m.file_path = "photos/clear_test.jpg";
    m.file_name = "clear_test.jpg";
    m.content_hash = "clear-hash";
    m.media_type = "photo";
    MediaId mid = db.insertMedia(m).value();

    TagId tManual = db.createOrGetTag("Trip", "keyword").value();
    TagId tFace = db.createOrGetTag("Bob", "people").value();
    TagId tClip = db.createOrGetTag("Desert", "keyword").value();

    ASSERT_TRUE(db.addTagToMedia(mid, tManual).isOk());
    auto& conn = db.connection();
    ASSERT_TRUE(conn.execute("INSERT INTO media_tags(media_id, tag_id) VALUES(" +
                             std::to_string(mid) + "," + std::to_string(tFace) + ");").isOk());
    ASSERT_TRUE(conn.execute("INSERT INTO media_tag_provenance(media_id, tag_id, manual, source) VALUES(" +
                             std::to_string(mid) + "," + std::to_string(tFace) + ", 0, 'face');").isOk());

    std::vector<SmartTagRecord> records = {{mid, {tClip}, "clear-hash"}};
    ASSERT_TRUE(db.replaceSmartTags(records, "model", "rev", "tax").isOk());

    // Insert a rejection row
    TagId tRej = db.createOrGetTag("RejectedLabel", "keyword").value();
    ASSERT_TRUE(conn.execute("INSERT INTO smart_tag_rejections(media_id, tag_id, rejected_at) VALUES(" +
                             std::to_string(mid) + ", " + std::to_string(tRej) + ", 123);").isOk());

    ASSERT_TRUE(db.clearSmartTags().isOk());

    auto tags = db.getTagsForMedia(mid).value();
    EXPECT_EQ(tags.size(), 2u);
    for (const auto& t : tags) {
        EXPECT_TRUE(t.id == tManual || t.id == tFace);
        EXPECT_NE(t.source, "clip");
    }

    auto countState = conn.prepare("SELECT COUNT(*) FROM smart_tag_state;");
    ASSERT_TRUE(countState.isOk());
    auto countStateStmt = std::move(countState.value());
    ASSERT_EQ(countStateStmt.step(), StepResult::Row);
    EXPECT_EQ(countStateStmt.getInt64(0), 0);

    auto countRej = conn.prepare("SELECT COUNT(*) FROM smart_tag_rejections;");
    ASSERT_TRUE(countRej.isOk());
    auto countRejStmt = std::move(countRej.value());
    ASSERT_EQ(countRejStmt.step(), StepResult::Row);
    EXPECT_EQ(countRejStmt.getInt64(0), 0);

    auto allTags = db.getAllTags().value();
    for (const auto& t : allTags) {
        EXPECT_EQ(t.ai_count, 0);
    }
}

TEST_F(CatalogDbTest, MediaInvalidationClearsSmartTagsAndRejections) {
    MediaItem m;
    m.file_path = "photos/inval_test.jpg";
    m.file_name = "inval_test.jpg";
    m.content_hash = "inval-hash-orig";
    m.media_type = "photo";
    m.width = 100;
    m.height = 100;
    MediaId mid = db.insertMedia(m).value();

    TagId tManual = db.createOrGetTag("KeepMe", "keyword").value();
    TagId tClip = db.createOrGetTag("RemoveMe", "keyword").value();
    ASSERT_TRUE(db.addTagToMedia(mid, tManual).isOk());

    std::vector<SmartTagRecord> records = {{mid, {tClip}, "inval-hash-orig"}};
    ASSERT_TRUE(db.replaceSmartTags(records, "model", "rev", "tax").isOk());

    auto& conn = db.connection();
    TagId tRej = db.createOrGetTag("RejectedLabel2", "keyword").value();
    ASSERT_TRUE(conn.execute("INSERT INTO smart_tag_rejections(media_id, tag_id, rejected_at) VALUES(" +
                             std::to_string(mid) + ", " + std::to_string(tRej) + ", 555);").isOk());

    // Update geometry/content_hash to trigger invalidation
    auto changed = db.getMediaById(mid).value();
    changed.content_hash = "inval-hash-new";
    ASSERT_TRUE(db.updateMedia(changed).isOk());

    auto tags = db.getTagsForMedia(mid).value();
    EXPECT_EQ(tags.size(), 1u);
    EXPECT_EQ(tags[0].id, tManual);
    EXPECT_EQ(tags[0].source, "manual");

    auto countState = conn.prepare("SELECT COUNT(*) FROM smart_tag_state WHERE media_id = ?;");
    ASSERT_TRUE(countState.isOk());
    auto cs = std::move(countState.value());
    cs.bind(1, mid);
    ASSERT_EQ(cs.step(), StepResult::Row);
    EXPECT_EQ(cs.getInt64(0), 0);

    auto countRej = conn.prepare("SELECT COUNT(*) FROM smart_tag_rejections WHERE media_id = ?;");
    ASSERT_TRUE(countRej.isOk());
    auto cr = std::move(countRej.value());
    cr.bind(1, mid);
    ASSERT_EQ(cr.step(), StepResult::Row);
    EXPECT_EQ(cr.getInt64(0), 0);
}

TEST_F(CatalogDbTest, SyncFaceTagOwnershipPreservesClipTags) {
    MediaItem m;
    m.file_path = "photos/face_sync_test.jpg";
    m.file_name = "face_sync_test.jpg";
    m.content_hash = "face-sync-hash";
    m.media_type = "photo";
    MediaId mid = db.insertMedia(m).value();

    TagId tPerson = db.createOrGetTag("Charlie", "people").value();

    // Attach as clip tag
    auto& conn = db.connection();
    ASSERT_TRUE(conn.execute("INSERT INTO media_tags(media_id, tag_id) VALUES(" +
                             std::to_string(mid) + "," + std::to_string(tPerson) + ");").isOk());
    ASSERT_TRUE(conn.execute("INSERT INTO media_tag_provenance(media_id, tag_id, manual, source) VALUES(" +
                             std::to_string(mid) + "," + std::to_string(tPerson) + ", 0, 'clip');").isOk());

    // Run syncFaceTagOwnership with 0 faces assigned
    ASSERT_TRUE(imagine::faces::syncFaceTagOwnership(conn, mid, tPerson).isOk());

    // Verify clip tag is NOT deleted
    auto tags = db.getTagsForMedia(mid).value();
    ASSERT_EQ(tags.size(), 1u);
    EXPECT_EQ(tags[0].id, tPerson);
    EXPECT_EQ(tags[0].source, "clip");

    // Now assign a face to tPerson
    ASSERT_TRUE(conn.execute(
        "INSERT INTO faces(media_id, x, y, width, height, score, landmarks, person_tag_id, created_at, updated_at) "
        "VALUES(" + std::to_string(mid) + ", 0, 0, 10, 10, 0.9, '[]', " + std::to_string(tPerson) + ", 1, 1);"
    ).isOk());

    // Now test a separate media item with a new face tag to test assignment
    MediaItem m2;
    m2.file_path = "photos/face_sync_test2.jpg";
    m2.file_name = "face_sync_test2.jpg";
    m2.content_hash = "face-sync-hash2";
    m2.media_type = "photo";
    MediaId mid2 = db.insertMedia(m2).value();
    TagId tDavid = db.createOrGetTag("David", "people").value();

    ASSERT_TRUE(conn.execute(
        "INSERT INTO faces(media_id, x, y, width, height, score, landmarks, person_tag_id, created_at, updated_at) "
        "VALUES(" + std::to_string(mid2) + ", 0, 0, 10, 10, 0.9, '[]', " + std::to_string(tDavid) + ", 1, 1);"
    ).isOk());
    int64_t faceId2 = conn.lastInsertRowId();

    ASSERT_TRUE(imagine::faces::syncFaceTagOwnership(conn, mid2, tDavid).isOk());
    auto tags2 = db.getTagsForMedia(mid2).value();
    ASSERT_EQ(tags2.size(), 1u);
    EXPECT_EQ(tags2[0].id, tDavid);
    EXPECT_EQ(tags2[0].source, "face");

    // Unassign face from mid2
    ASSERT_TRUE(conn.execute("UPDATE faces SET person_tag_id = NULL WHERE id = " + std::to_string(faceId2) + ";").isOk());
    ASSERT_TRUE(imagine::faces::syncFaceTagOwnership(conn, mid2, tDavid).isOk());
    tags2 = db.getTagsForMedia(mid2).value();
    EXPECT_TRUE(tags2.empty());
}
