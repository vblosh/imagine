#include <gtest/gtest.h>
#include "imagine/core/query.hpp"
#include "imagine/db/catalog_db.hpp"

using namespace imagine;
using namespace imagine::core;
using namespace imagine::db;

class QueryTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(db.open(":memory:").isOk());

        // Insert 3 test media items
        MediaItem item1;
        item1.file_path = "/photos/2026/img1.jpg";
        item1.file_name = "img1.jpg";
        item1.file_size = 1000;
        item1.file_modified_time = 1000;
        item1.content_hash = "hash1";
        item1.date_taken = 1000;
        item1.rating = 5;
        item1.flag = FlagState::Pick;
        item1.exif.camera_make = "Canon";
        item1.exif.camera_model = "EOS R5";
        item1.exif.lens = "RF 24-70mm";
        id1 = db.insertMedia(item1).value();

        MediaItem item2;
        item2.file_path = "/photos/2026/img2.jpg";
        item2.file_name = "img2.jpg";
        item2.file_size = 2000;
        item2.file_modified_time = 2000;
        item2.content_hash = "hash2";
        item2.date_taken = 2000;
        item2.rating = 3;
        item2.flag = FlagState::Unflagged;
        item2.exif.camera_make = "Sony";
        item2.exif.camera_model = "A7 IV";
        item2.exif.lens = "FE 50mm";
        id2 = db.insertMedia(item2).value();

        MediaItem item3;
        item3.file_path = "/photos/2026/img3.jpg";
        item3.file_name = "img3.jpg";
        item3.file_size = 3000;
        item3.file_modified_time = 3000;
        item3.content_hash = "hash3";
        item3.date_taken = 3000;
        item3.rating = 1;
        item3.flag = FlagState::Reject;
        item3.exif.camera_make = "Nikon";
        item3.exif.camera_model = "Z8";
        item3.exif.lens = "Nikkor 85mm";
        id3 = db.insertMedia(item3).value();

        // Add tags
        tagNature = db.createOrGetTag("nature", "keyword").value();
        tagPortrait = db.createOrGetTag("portrait", "keyword").value();
        ASSERT_TRUE(db.addTagToMedia(id1, tagNature).isOk());
        ASSERT_TRUE(db.addTagToMedia(id2, tagNature).isOk());
        ASSERT_TRUE(db.addTagToMedia(id2, tagPortrait).isOk());

        // Add album
        albumVacation = db.createAlbum("Vacation").value();
        ASSERT_TRUE(db.addMediaToAlbum(albumVacation, id1).isOk());
        ASSERT_TRUE(db.addMediaToAlbum(albumVacation, id3).isOk());
    }

    void TearDown() override {
        db.close();
    }

    CatalogDb db;
    MediaId id1{0};
    MediaId id2{0};
    MediaId id3{0};
    TagId tagNature{0};
    TagId tagPortrait{0};
    AlbumId albumVacation{0};
};

TEST_F(QueryTest, QueryAll) {
    QueryCriteria criteria;
    auto res = QueryBuilder::execute(db, criteria);
    ASSERT_TRUE(res.isOk());
    EXPECT_EQ(res.value().total_count, 3);
    EXPECT_EQ(res.value().items.size(), 3);
}

TEST_F(QueryTest, FilterRatingAndFlag) {
    QueryCriteria criteria;
    criteria.min_rating = 3;
    criteria.flag = FlagState::Pick;

    auto res = QueryBuilder::execute(db, criteria);
    ASSERT_TRUE(res.isOk());
    EXPECT_EQ(res.value().total_count, 1);
    EXPECT_EQ(res.value().items[0].id, id1);
}

TEST_F(QueryTest, FilterTags) {
    QueryCriteria criteria;
    criteria.tag_ids = {tagNature, tagPortrait};

    auto res = QueryBuilder::execute(db, criteria);
    ASSERT_TRUE(res.isOk());
    EXPECT_EQ(res.value().total_count, 1);
    EXPECT_EQ(res.value().items[0].id, id2);
}

TEST_F(QueryTest, FilterAlbum) {
    QueryCriteria criteria;
    criteria.album_id = albumVacation;

    auto res = QueryBuilder::execute(db, criteria);
    ASSERT_TRUE(res.isOk());
    EXPECT_EQ(res.value().total_count, 2);
}

TEST_F(QueryTest, FilterCameraAndSearchText) {
    // Camera make
    QueryCriteria c1;
    c1.camera_make = "Sony";
    auto res1 = QueryBuilder::execute(db, c1);
    ASSERT_TRUE(res1.isOk());
    EXPECT_EQ(res1.value().total_count, 1);
    EXPECT_EQ(res1.value().items[0].id, id2);

    // Search text matching lens
    QueryCriteria c2;
    c2.search_text = "Nikkor";
    auto res2 = QueryBuilder::execute(db, c2);
    ASSERT_TRUE(res2.isOk());
    EXPECT_EQ(res2.value().total_count, 1);
    EXPECT_EQ(res2.value().items[0].id, id3);

    // Search text matching file_name
    QueryCriteria c3;
    c3.search_text = "img1";
    auto res3 = QueryBuilder::execute(db, c3);
    ASSERT_TRUE(res3.isOk());
    EXPECT_EQ(res3.value().total_count, 1);
    EXPECT_EQ(res3.value().items[0].id, id1);

    // Search text matching tag
    QueryCriteria c4;
    c4.search_text = "portrait";
    auto res4 = QueryBuilder::execute(db, c4);
    ASSERT_TRUE(res4.isOk());
    EXPECT_EQ(res4.value().total_count, 1);
    EXPECT_EQ(res4.value().items[0].id, id2);

    QueryCriteria c5;
    c5.search_text = "nature";
    auto res5 = QueryBuilder::execute(db, c5);
    ASSERT_TRUE(res5.isOk());
    EXPECT_EQ(res5.value().total_count, 2);
}

TEST_F(QueryTest, FilterDateRange) {
    QueryCriteria criteria;
    criteria.date_from = 1500;
    criteria.date_to = 2500;

    auto res = QueryBuilder::execute(db, criteria);
    ASSERT_TRUE(res.isOk());
    EXPECT_EQ(res.value().total_count, 1);
    EXPECT_EQ(res.value().items[0].id, id2);
}

TEST_F(QueryTest, SortAndPagination) {
    QueryCriteria criteria;
    criteria.sort_by = "rating";
    criteria.sort_descending = false; // Ascending rating: 1, 3, 5
    criteria.limit = 2;
    criteria.offset = 1;

    auto res = QueryBuilder::execute(db, criteria);
    ASSERT_TRUE(res.isOk());
    EXPECT_EQ(res.value().total_count, 3);
    ASSERT_EQ(res.value().items.size(), 2);
    EXPECT_EQ(res.value().items[0].rating, 3);
    EXPECT_EQ(res.value().items[1].rating, 5);
}

TEST_F(QueryTest, JsonSerialization) {
    QueryCriteria criteria;
    criteria.min_rating = 4;
    criteria.camera_make = "Canon";
    criteria.tag_ids = {1, 2, 3};

    nlohmann::json j = criteria;
    EXPECT_EQ(j["camera_make"], "Canon");
    EXPECT_EQ(j["min_rating"], 4);

    QueryCriteria deserialized = j.get<QueryCriteria>();
    EXPECT_EQ(deserialized.camera_make, "Canon");
    ASSERT_TRUE(deserialized.min_rating.has_value());
    EXPECT_EQ(*deserialized.min_rating, 4);
    EXPECT_EQ(deserialized.tag_ids.size(), 3);
}

TEST_F(QueryTest, FluentBuilderAndExecutionMethods) {
    QueryBuilder qb;
    qb.minRating(1)
      .maxRating(4)
      .flag(FlagState::Unflagged)
      .camera("Sony", "A7 IV")
      .dateRange(1000, 3000)
      .search("img2")
      .sort("file_name", true)
      .paginate(5, 0);

    auto result = qb.execute(db);
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(result.value().total_count, 1);
    EXPECT_EQ(result.value().items[0].id, id2);

    // executeQuery member
    auto itemsRes = qb.executeQuery(db);
    ASSERT_TRUE(itemsRes.isOk());
    EXPECT_EQ(itemsRes.value().size(), 1u);

    // executeCount member
    auto countRes = qb.executeCount(db);
    ASSERT_TRUE(countRes.isOk());
    EXPECT_EQ(countRes.value(), 1);

    // Static executeQuery and executeCount
    QueryCriteria c;
    c.camera_model = "Z8";
    auto staticItems = QueryBuilder::executeQuery(db, c);
    ASSERT_TRUE(staticItems.isOk());
    EXPECT_EQ(staticItems.value().size(), 1u);
    EXPECT_EQ(staticItems.value()[0].id, id3);

    auto staticCount = QueryBuilder::executeCount(db, c);
    ASSERT_TRUE(staticCount.isOk());
    EXPECT_EQ(staticCount.value(), 1);

    // addTag and setTags in builder
    QueryBuilder qbTags;
    qbTags.addTag(tagNature);
    EXPECT_EQ(qbTags.executeCount(db).value(), 2);

    qbTags.setTags({tagNature, tagPortrait});
    EXPECT_EQ(qbTags.executeCount(db).value(), 1);

    // album filter
    QueryBuilder qbAlbum;
    qbAlbum.album(albumVacation);
    EXPECT_EQ(qbAlbum.executeCount(db).value(), 2);

    // setCriteria
    QueryCriteria newCrit;
    newCrit.search_text = "Nikon";
    qbAlbum.setCriteria(newCrit);
    EXPECT_EQ(qbAlbum.executeCount(db).value(), 1);
}

TEST_F(QueryTest, SortVariants) {
    // Sort by file_name ascending
    QueryCriteria c1;
    c1.sort_by = "file_name";
    c1.sort_descending = false;
    auto res1 = QueryBuilder::execute(db, c1);
    ASSERT_TRUE(res1.isOk());
    EXPECT_EQ(res1.value().items[0].file_name, "img1.jpg");
    EXPECT_EQ(res1.value().items[2].file_name, "img3.jpg");

    // Sort by file_size descending
    QueryCriteria c2;
    c2.sort_by = "file_size";
    c2.sort_descending = true;
    auto res2 = QueryBuilder::execute(db, c2);
    ASSERT_TRUE(res2.isOk());
    EXPECT_EQ(res2.value().items[0].file_size, 3000);
    EXPECT_EQ(res2.value().items[2].file_size, 1000);
}

TEST_F(QueryTest, QueryResultJsonSerialization) {
    QueryResult qr;
    qr.total_count = 1;
    MediaItem item;
    item.id = 55;
    item.file_name = "test.jpg";
    qr.items.push_back(item);

    nlohmann::json j = qr;
    EXPECT_EQ(j["total_count"], 1);

    QueryResult d = j.get<QueryResult>();
    EXPECT_EQ(d.total_count, 1);
    ASSERT_EQ(d.items.size(), 1u);
    EXPECT_EQ(d.items[0].id, 55);

    nlohmann::json emptyJ = nlohmann::json::object();
    QueryResult dEmpty = emptyJ.get<QueryResult>();
    EXPECT_EQ(dEmpty.total_count, 0);
    EXPECT_TRUE(dEmpty.items.empty());
}

TEST_F(QueryTest, FullQueryCriteriaJsonSerialization) {
    QueryCriteria c;
    c.min_rating = 1;
    c.max_rating = 4;
    c.flag = FlagState::Reject;
    c.album_id = 42;
    c.camera_make = "Leica";
    c.camera_model = "M11";
    c.date_from = 1000;
    c.date_to = 2000;
    c.search_text = "Street";
    c.sort_by = "rating";
    c.sort_descending = false;
    c.limit = 25;
    c.offset = 5;
    c.tag_ids = {10, 20};

    nlohmann::json j = c;
    EXPECT_EQ(j["max_rating"], 4);
    EXPECT_EQ(j["flag"], -1);
    EXPECT_EQ(j["album_id"], 42);

    QueryCriteria d = j.get<QueryCriteria>();
    ASSERT_TRUE(d.max_rating.has_value());
    EXPECT_EQ(*d.max_rating, 4);
    ASSERT_TRUE(d.flag.has_value());
    EXPECT_EQ(*d.flag, FlagState::Reject);
    ASSERT_TRUE(d.album_id.has_value());
    EXPECT_EQ(*d.album_id, 42);
    EXPECT_EQ(d.camera_make, "Leica");
    EXPECT_EQ(d.camera_model, "M11");
}
