#include <gtest/gtest.h>
#include "imagine/db/catalog_db.hpp"
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
}

TEST_F(CatalogDbTest, UpdateRatingAndFlag) {
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
}

TEST_F(CatalogDbTest, TagManagement) {
    MediaItem item;
    item.file_path = "/photos/family.jpg";
    item.file_name = "family.jpg";
    item.file_size = 1024;
    item.file_modified_time = 1000;
    item.content_hash = "hash_family";
    item.date_taken = 1000;
    auto insertRes = db.insertMedia(item);
    ASSERT_TRUE(insertRes.isOk());
    MediaId mid = insertRes.value();

    // Create tags
    auto tagPeople = db.createOrGetTag("Alice", "people");
    ASSERT_TRUE(tagPeople.isOk());

    auto tagPlace = db.createOrGetTag("Paris", "places");
    ASSERT_TRUE(tagPlace.isOk());

    // Duplicate create returns existing
    auto tagPeople2 = db.createOrGetTag("Alice", "people");
    ASSERT_TRUE(tagPeople2.isOk());
    EXPECT_EQ(tagPeople.value(), tagPeople2.value());

    // Attach to media
    EXPECT_TRUE(db.addTagToMedia(mid, tagPeople.value()).isOk());
    EXPECT_TRUE(db.addTagToMedia(mid, tagPlace.value()).isOk());

    auto tagsRes = db.getTagsForMedia(mid);
    ASSERT_TRUE(tagsRes.isOk());
    EXPECT_EQ(tagsRes.value().size(), 2);

    // Remove one tag
    EXPECT_TRUE(db.removeTagFromMedia(mid, tagPeople.value()).isOk());
    auto tagsAfter = db.getTagsForMedia(mid);
    ASSERT_TRUE(tagsAfter.isOk());
    EXPECT_EQ(tagsAfter.value().size(), 1);
    EXPECT_EQ(tagsAfter.value()[0].name, "Paris");

    // Delete tag completely
    EXPECT_TRUE(db.deleteTag(tagPlace.value()).isOk());
    auto tagsAfterDelete = db.getTagsForMedia(mid);
    ASSERT_TRUE(tagsAfterDelete.isOk());
    EXPECT_EQ(tagsAfterDelete.value().size(), 0);
    auto allTags = db.getAllTags();
    ASSERT_TRUE(allTags.isOk());
    EXPECT_EQ(allTags.value().size(), 1); // Only "Alice" remains
}

TEST_F(CatalogDbTest, AlbumManagement) {
    MediaItem item1;
    item1.file_path = "/photos/1.jpg";
    item1.file_name = "1.jpg";
    item1.file_size = 1024;
    item1.file_modified_time = 1000;
    item1.content_hash = "hash_1";
    item1.date_taken = 1000;
    auto mid1 = db.insertMedia(item1).value();

    MediaItem item2;
    item2.file_path = "/photos/2.jpg";
    item2.file_name = "2.jpg";
    item2.file_size = 2048;
    item2.file_modified_time = 2000;
    item2.content_hash = "hash_2";
    item2.date_taken = 2000;
    auto mid2 = db.insertMedia(item2).value();

    auto albumRes = db.createAlbum("Favorites 2026", "Best memories");
    ASSERT_TRUE(albumRes.isOk());
    AlbumId aid = albumRes.value();

    EXPECT_TRUE(db.addMediaToAlbum(aid, mid1, 0).isOk());
    EXPECT_TRUE(db.addMediaToAlbum(aid, mid2, 1).isOk());

    auto inAlbum = db.getMediaInAlbum(aid);
    ASSERT_TRUE(inAlbum.isOk());
    EXPECT_EQ(inAlbum.value().size(), 2);

    auto allAlbums = db.getAllAlbums();
    ASSERT_TRUE(allAlbums.isOk());
    EXPECT_EQ(allAlbums.value().size(), 1);
    EXPECT_EQ(allAlbums.value()[0].item_count, 2);

    EXPECT_TRUE(db.removeMediaFromAlbum(aid, mid1).isOk());
    auto inAlbumAfter = db.getMediaInAlbum(aid);
    ASSERT_TRUE(inAlbumAfter.isOk());
    EXPECT_EQ(inAlbumAfter.value().size(), 1);
}
