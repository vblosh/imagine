#include <gtest/gtest.h>
#include "imagine/db/catalog_db.hpp"
#include "imagine/db/connection.hpp"
#include <chrono>

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

    // Open invalid path
    EXPECT_FALSE(conn.open("/proc/invalid_dir/db.sqlite").isOk());

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
}

TEST(TransactionTest, CommitRollbackAndAutoRollback) {
    Connection conn;
    ASSERT_TRUE(conn.open(":memory:").isOk());
    ASSERT_TRUE(conn.execute("CREATE TABLE kv (k TEXT, v TEXT);").isOk());

    // 1. Explicit commit
    {
        Transaction tx(conn);
        EXPECT_TRUE(conn.execute("INSERT INTO kv VALUES ('key1', 'val1');").isOk());
        EXPECT_TRUE(tx.commit().isOk());
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
