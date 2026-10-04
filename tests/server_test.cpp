#include <gtest/gtest.h>
#include "imagine/core/catalog.hpp"
#include "imagine/server/web_server.hpp"
#include "imagine/server/api_router.hpp"
#include "imagine/db/catalog_db.hpp"
#include "imagine/thumbnail/cache.hpp"
#include "imagine/thumbnail/generator.hpp"
#include "imagine/faces/service.hpp"
#include "imagine/metadata/hasher.hpp"
#include "imagine/common/logger.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <future>

using namespace imagine;
using namespace imagine::core;
using namespace imagine::server;
using namespace imagine::db;
using namespace imagine::thumbnail;

class ServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        testDir_ = std::filesystem::temp_directory_path() / ("imagine_server_test_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(testDir_);

#if defined(_WIN32)
        _putenv_s("IMAGINE_API_TOKEN", "");
#else
        unsetenv("IMAGINE_API_TOKEN");
#endif

        dbPath_ = (testDir_ / "test_catalog.db").string();
        cacheDir_ = (testDir_ / "cache").string();
        webDir_ = testDir_ / "web";
        std::filesystem::create_directories(webDir_);

        // Create dummy web assets
        {
            std::ofstream ofs(webDir_ / "index.html");
            ofs << "<!DOCTYPE html><html><body><h1>IMAGINE Test</h1></body></html>";
        }
        {
            std::ofstream ofs(webDir_ / "app.css");
            ofs << "body { background: #1e1e1e; }";
        }

        catalog_ = std::make_unique<Catalog>(2);
        auto s = catalog_->open(dbPath_, cacheDir_);
        ASSERT_TRUE(s.isOk());

        port_ = 18080 + (std::rand() % 5000);
        server_ = std::make_unique<WebServer>(*catalog_, webDir_.string());
        auto startRes = server_->start("127.0.0.1", port_, webDir_.string());
        ASSERT_TRUE(startRes.isOk());

        // Wait brief moment for server to bind
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    void TearDown() override {
        if (server_) {
            server_->stop();
        }
        if (catalog_) {
            catalog_->close();
        }
        std::error_code ec;
        std::filesystem::remove_all(testDir_, ec);
    }

    std::filesystem::path testDir_;
    std::string dbPath_;
    std::string cacheDir_;
    std::filesystem::path webDir_;
    int port_{0};
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<WebServer> server_;
};

TEST_F(ServerTest, CorsPreflightAndHeaders) {
    httplib::Client client("127.0.0.1", port_);
    auto res = client.Options("/api/media");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 204);
    EXPECT_EQ(res->get_header_value("Access-Control-Allow-Origin"), "*");
}

TEST_F(ServerTest, StatsAndTimelineEmpty) {
    httplib::Client client("127.0.0.1", port_);

    auto res = client.Get("/api/stats");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["total_media"].get<int64_t>(), 0);

    auto tlRes = client.Get("/api/timeline");
    ASSERT_TRUE(tlRes);
    EXPECT_EQ(tlRes->status, 200);
    auto tl = nlohmann::json::parse(tlRes->body);
    EXPECT_TRUE(tl.is_array());
    EXPECT_EQ(tl.size(), 0u);
}

TEST_F(ServerTest, TagsApi) {
    httplib::Client client("127.0.0.1", port_);

    // POST /api/tags
    nlohmann::json newTag = {{"name", "Mountains"}, {"category", "places"}};
    auto postRes = client.Post("/api/tags", newTag.dump(), "application/json");
    ASSERT_TRUE(postRes);
    EXPECT_EQ(postRes->status, 201);
    auto created = nlohmann::json::parse(postRes->body);
    EXPECT_GT(created["id"].get<TagId>(), 0);
    EXPECT_EQ(created["name"].get<std::string>(), "Mountains");

    // GET /api/tags
    auto getRes = client.Get("/api/tags");
    ASSERT_TRUE(getRes);
    EXPECT_EQ(getRes->status, 200);
    auto tags = nlohmann::json::parse(getRes->body);
    EXPECT_EQ(tags.size(), 1u);
    EXPECT_EQ(tags[0]["name"].get<std::string>(), "Mountains");

    // DELETE /api/tags/:id
    TagId tagId = created["id"].get<TagId>();
    auto delRes = client.Delete(std::string("/api/tags/") + std::to_string(tagId));
    ASSERT_TRUE(delRes);
    EXPECT_EQ(delRes->status, 200);

    // Verify tag is deleted
    auto getResAfter = client.Get("/api/tags");
    ASSERT_TRUE(getResAfter);
    EXPECT_EQ(getResAfter->status, 200);
    auto tagsAfter = nlohmann::json::parse(getResAfter->body);
    EXPECT_EQ(tagsAfter.size(), 0u);
}

TEST_F(ServerTest, EventSuggestionsApiIsReadOnlyAndReportsSkippedItems) {
    MediaItem first;
    first.file_path = "trip/first.jpg";
    first.file_name = "first.jpg";
    first.content_hash = "event-first";
    first.media_type = "photo";
    first.date_taken = 1700000000;
    first.caption = "Museum";
    const auto firstId = catalog_->db().insertMedia(first).value();

    MediaItem second = first;
    second.file_path = "trip/second.jpg";
    second.file_name = "second.jpg";
    second.content_hash = "event-second";
    second.date_taken = 1700000100;
    const auto secondId = catalog_->db().insertMedia(second).value();

    MediaItem video = second;
    video.file_path = "trip/video.mp4";
    video.file_name = "video.mp4";
    video.content_hash = "event-video";
    video.media_type = "video";
    const auto videoId = catalog_->db().insertMedia(video).value();

    httplib::Client client("127.0.0.1", port_);
    const auto body = nlohmann::json{{"ids", {firstId, secondId, videoId, 999999}}}.dump();
    auto response = client.Post("/api/media/event-suggestions", body, "application/json");
    ASSERT_TRUE(response);
    ASSERT_EQ(response->status, 200);
    const auto result = nlohmann::json::parse(response->body);
    ASSERT_EQ(result["groups"].size(), 1u);
    EXPECT_EQ(result["groups"][0]["media_ids"].size(), 2u);
    EXPECT_EQ(result["groups"][0]["name_source"], "caption");
    EXPECT_EQ(result["groups"][0]["name_value"], "Museum");
    ASSERT_EQ(result["skipped"].size(), 2u);

    auto invalid = client.Post("/api/media/event-suggestions", R"({"ids":[0]})", "application/json");
    ASSERT_TRUE(invalid);
    EXPECT_EQ(invalid->status, 400);

    auto tags = catalog_->db().getAllTags().value();
    EXPECT_TRUE(tags.empty());
}

TEST_F(ServerTest, AlbumsApi) {
    httplib::Client client("127.0.0.1", port_);

    // POST /api/albums
    nlohmann::json newAlbum = {{"name", "Vacation 2026"}, {"description", "Trip to Alps"}};
    auto postRes = client.Post("/api/albums", newAlbum.dump(), "application/json");
    ASSERT_TRUE(postRes);
    EXPECT_EQ(postRes->status, 201);
    auto created = nlohmann::json::parse(postRes->body);
    AlbumId albumId = created["id"].get<AlbumId>();
    EXPECT_GT(albumId, 0);

    // GET /api/albums
    auto getRes = client.Get("/api/albums");
    ASSERT_TRUE(getRes);
    EXPECT_EQ(getRes->status, 200);
    auto albums = nlohmann::json::parse(getRes->body);
    EXPECT_EQ(albums.size(), 1u);
    EXPECT_EQ(albums[0]["name"].get<std::string>(), "Vacation 2026");
}

TEST_F(ServerTest, FoldersApi) {
    httplib::Client client("127.0.0.1", port_);

    // 1. GET /api/folders initially empty
    auto getRes1 = client.Get("/api/folders");
    ASSERT_TRUE(getRes1);
    EXPECT_EQ(getRes1->status, 200);
    auto folders1 = nlohmann::json::parse(getRes1->body);
    EXPECT_TRUE(folders1.is_array());
    EXPECT_EQ(folders1.size(), 0u);

    // 2. Insert media items in multiple folders
    MediaItem item1;
    item1.file_path = "vacation/beach.jpg";
    item1.file_name = "beach.jpg";
    item1.content_hash = "h_vacation";
    catalog_->db().insertMedia(item1);

    MediaItem item2;
    item2.file_path = "family/birthday.jpg";
    item2.file_name = "birthday.jpg";
    item2.content_hash = "h_family";
    catalog_->db().insertMedia(item2);

    MediaItem item3;
    item3.file_path = "root_pic.jpg";
    item3.file_name = "root_pic.jpg";
    item3.content_hash = "h_root";
    catalog_->db().insertMedia(item3);

    // 3. GET /api/folders returns discovered folders in alphabetical order
    auto getRes2 = client.Get("/api/folders");
    ASSERT_TRUE(getRes2);
    EXPECT_EQ(getRes2->status, 200);
    auto folders2 = nlohmann::json::parse(getRes2->body);
    EXPECT_EQ(folders2.size(), 2u);
    EXPECT_EQ(folders2[0].get<std::string>(), "family");
    EXPECT_EQ(folders2[1].get<std::string>(), "vacation");
}

TEST_F(ServerTest, MediaItemCrudAndRatings) {
    httplib::Client client("127.0.0.1", port_);

    // Insert media directly via DB to test API retrieval & update
    MediaItem item;
    item.file_path = (testDir_ / "photo1.jpg").string();
    item.file_name = "photo1.jpg";
    item.file_size = 10240;
    item.content_hash = "abc123hash";
    item.width = 1920;
    item.height = 1080;
    item.date_taken = 1788460000;
    item.rating = 0;
    item.flag = FlagState::Unflagged;

    auto insertRes = catalog_->db().insertMedia(item);
    ASSERT_TRUE(insertRes.isOk());
    MediaId mid = insertRes.value();

    // GET /api/media
    auto listRes = client.Get("/api/media");
    ASSERT_TRUE(listRes);
    EXPECT_EQ(listRes->status, 200);
    auto listJson = nlohmann::json::parse(listRes->body);
    EXPECT_EQ(listJson["total"].get<int64_t>(), 1);
    EXPECT_EQ(listJson["items"].size(), 1u);
    EXPECT_EQ(listJson["items"][0]["id"].get<MediaId>(), mid);

    // GET /api/media/:id
    auto getRes = client.Get("/api/media/" + std::to_string(mid));
    ASSERT_TRUE(getRes);
    EXPECT_EQ(getRes->status, 200);
    auto getJson = nlohmann::json::parse(getRes->body);
    EXPECT_EQ(getJson["file_name"].get<std::string>(), "photo1.jpg");

    // POST /api/media/:id/rating
    nlohmann::json ratingBody = {{"rating", 4}};
    auto rateRes = client.Post("/api/media/" + std::to_string(mid) + "/rating", ratingBody.dump(), "application/json");
    ASSERT_TRUE(rateRes);
    EXPECT_EQ(rateRes->status, 200);

    // POST /api/media/:id/flag
    nlohmann::json flagBody = {{"flag", 1}};
    auto flagRes = client.Post("/api/media/" + std::to_string(mid) + "/flag", flagBody.dump(), "application/json");
    ASSERT_TRUE(flagRes);
    EXPECT_EQ(flagRes->status, 200);

    // Verify rating and flag updated
    auto verifyRes = client.Get("/api/media/" + std::to_string(mid));
    ASSERT_TRUE(verifyRes);
    auto verified = nlohmann::json::parse(verifyRes->body);
    EXPECT_EQ(verified["rating"].get<int32_t>(), 4);
    EXPECT_EQ(verified["flag"].get<int8_t>(), 1);

    // POST /api/media/:id/tags
    nlohmann::json tagBody = {{"name", "Sunset"}, {"category", "places"}};
    auto tagRes = client.Post("/api/media/" + std::to_string(mid) + "/tags", tagBody.dump(), "application/json");
    ASSERT_TRUE(tagRes);
    EXPECT_EQ(tagRes->status, 200);

    // Verify tag attached
    auto withTagRes = client.Get("/api/media/" + std::to_string(mid));
    ASSERT_TRUE(withTagRes);
    auto withTag = nlohmann::json::parse(withTagRes->body);
    EXPECT_EQ(withTag["tags"].size(), 1u);
    TagId tagId = withTag["tags"][0]["id"].get<TagId>();

    // DELETE /api/media/:id/tags/:tag_id
    auto delTagRes = client.Delete("/api/media/" + std::to_string(mid) + "/tags/" + std::to_string(tagId));
    ASSERT_TRUE(delTagRes);
    EXPECT_EQ(delTagRes->status, 200);

    // Verify tag removed
    auto noTagRes = client.Get("/api/media/" + std::to_string(mid));
    ASSERT_TRUE(noTagRes);
    auto noTag = nlohmann::json::parse(noTagRes->body);
    EXPECT_EQ(noTag["tags"].size(), 0u);

    // POST /api/media/:id/caption
    nlohmann::json captionBody = {{"caption", "Beautiful Sunset in Alps"}};
    auto capRes = client.Post("/api/media/" + std::to_string(mid) + "/caption", captionBody.dump(), "application/json");
    ASSERT_TRUE(capRes);
    EXPECT_EQ(capRes->status, 200);

    // POST /api/media/:id/date
    nlohmann::json dateBody = {{"date_taken", 1720000000}};
    auto dateRes = client.Post("/api/media/" + std::to_string(mid) + "/date", dateBody.dump(), "application/json");
    ASSERT_TRUE(dateRes);
    EXPECT_EQ(dateRes->status, 200);

    // POST /api/media/:id/rename (invalid name)
    nlohmann::json badRenameBody = {{"name", "sub/folder/bad.jpg"}};
    auto badRenameRes = client.Post("/api/media/" + std::to_string(mid) + "/rename", badRenameBody.dump(), "application/json");
    ASSERT_TRUE(badRenameRes);
    EXPECT_EQ(badRenameRes->status, 400);

    // POST /api/media/:id/rename (valid name)
    nlohmann::json renameBody = {{"name", "photo1_renamed.jpg"}};
    auto renameRes = client.Post("/api/media/" + std::to_string(mid) + "/rename", renameBody.dump(), "application/json");
    ASSERT_TRUE(renameRes);
    EXPECT_EQ(renameRes->status, 200);
    auto renameJson = nlohmann::json::parse(renameRes->body);
    EXPECT_EQ(renameJson["file_name"].get<std::string>(), "photo1_renamed.jpg");

    // Verify all fields updated
    auto verifyAllRes = client.Get("/api/media/" + std::to_string(mid));
    ASSERT_TRUE(verifyAllRes);
    auto allJson = nlohmann::json::parse(verifyAllRes->body);
    EXPECT_EQ(allJson["caption"].get<std::string>(), "Beautiful Sunset in Alps");
    EXPECT_EQ(allJson["date_taken"].get<int64_t>(), 1720000000);
    EXPECT_EQ(allJson["file_name"].get<std::string>(), "photo1_renamed.jpg");

    // DELETE /api/media/:id
    auto delMediaRes = client.Delete("/api/media/" + std::to_string(mid));
    ASSERT_TRUE(delMediaRes);
    EXPECT_EQ(delMediaRes->status, 200);

    // Verify media is deleted
    auto getDeletedRes = client.Get("/api/media/" + std::to_string(mid));
    ASSERT_TRUE(getDeletedRes);
    EXPECT_EQ(getDeletedRes->status, 404);

    // Deleting again should return 404
    auto delAgainRes = client.Delete("/api/media/" + std::to_string(mid));
    ASSERT_TRUE(delAgainRes);
    EXPECT_EQ(delAgainRes->status, 404);

    // Test batch delete
    MediaItem batchItem1, batchItem2;
    batchItem1.file_path = (testDir_ / "batch1.jpg").string();
    batchItem1.file_name = "batch1.jpg";
    batchItem1.file_size = 1000;
    batchItem1.content_hash = "hash_b1";
    batchItem1.date_taken = 1000;
    batchItem2.file_path = (testDir_ / "batch2.jpg").string();
    batchItem2.file_name = "batch2.jpg";
    batchItem2.file_size = 1000;
    batchItem2.content_hash = "hash_b2";
    batchItem2.date_taken = 1000;

    MediaId bId1 = catalog_->db().insertMedia(batchItem1).value();
    MediaId bId2 = catalog_->db().insertMedia(batchItem2).value();

    nlohmann::json batchDelBody = {{"ids", {bId1, bId2}}};
    auto batchDelRes = client.Post("/api/media/batch-delete", batchDelBody.dump(), "application/json");
    ASSERT_TRUE(batchDelRes);
    EXPECT_EQ(batchDelRes->status, 200);
    auto batchDelJson = nlohmann::json::parse(batchDelRes->body);
    EXPECT_EQ(batchDelJson["deleted_count"].get<int>(), 2);

    EXPECT_EQ(client.Get("/api/media/" + std::to_string(bId1))->status, 404);
    EXPECT_EQ(client.Get("/api/media/" + std::to_string(bId2))->status, 404);
}

TEST_F(ServerTest, BatchRatingFlagTagsGps) {
    httplib::Client client("127.0.0.1", port_);

    MediaItem it1, it2;
    it1.file_path = (testDir_ / "bf1.jpg").string();
    it1.file_name = "bf1.jpg";
    it1.file_size = 1000;
    it1.content_hash = "hash_bf1";
    it1.date_taken = 1000;

    it2.file_path = (testDir_ / "bf2.jpg").string();
    it2.file_name = "bf2.jpg";
    it2.file_size = 1000;
    it2.content_hash = "hash_bf2";
    it2.date_taken = 1000;

    MediaId id1 = catalog_->db().insertMedia(it1).value();
    MediaId id2 = catalog_->db().insertMedia(it2).value();

    // 1. Batch rating
    nlohmann::json rateBody = {{"ids", {id1, id2}}, {"rating", 5}};
    auto rateRes = client.Post("/api/media/batch-rating", rateBody.dump(), "application/json");
    ASSERT_TRUE(rateRes);
    EXPECT_EQ(rateRes->status, 200);
    auto rateJson = nlohmann::json::parse(rateRes->body);
    EXPECT_EQ(rateJson["updated_count"].get<int>(), 2);
    EXPECT_EQ(rateJson["rating"].get<int>(), 5);

    auto get1 = nlohmann::json::parse(client.Get("/api/media/" + std::to_string(id1))->body);
    auto get2 = nlohmann::json::parse(client.Get("/api/media/" + std::to_string(id2))->body);
    EXPECT_EQ(get1["rating"].get<int>(), 5);
    EXPECT_EQ(get2["rating"].get<int>(), 5);

    // 2. Batch flag
    nlohmann::json flagBody = {{"ids", {id1, id2}}, {"flag", 1}};
    auto flagRes = client.Post("/api/media/batch-flag", flagBody.dump(), "application/json");
    ASSERT_TRUE(flagRes);
    EXPECT_EQ(flagRes->status, 200);
    auto flagJson = nlohmann::json::parse(flagRes->body);
    EXPECT_EQ(flagJson["updated_count"].get<int>(), 2);
    EXPECT_EQ(flagJson["flag"].get<int>(), 1);

    get1 = nlohmann::json::parse(client.Get("/api/media/" + std::to_string(id1))->body);
    get2 = nlohmann::json::parse(client.Get("/api/media/" + std::to_string(id2))->body);
    EXPECT_EQ(get1["flag"].get<int>(), 1);
    EXPECT_EQ(get2["flag"].get<int>(), 1);

    // 3. Batch tags
    nlohmann::json tagBody = {{"ids", {id1, id2}}, {"name", "Vacation2026"}, {"category", "events"}};
    auto tagRes = client.Post("/api/media/batch-tags", tagBody.dump(), "application/json");
    ASSERT_TRUE(tagRes);
    EXPECT_EQ(tagRes->status, 200);
    auto tagJson = nlohmann::json::parse(tagRes->body);
    EXPECT_EQ(tagJson["tagged_count"].get<int>(), 2);
    EXPECT_EQ(tagJson["name"].get<std::string>(), "Vacation2026");
    const TagId vacationTagId = tagJson["tag_id"].get<TagId>();

    get1 = nlohmann::json::parse(client.Get("/api/media/" + std::to_string(id1))->body);
    EXPECT_EQ(get1["tags"].size(), 1u);
    EXPECT_EQ(get1["tags"][0]["name"].get<std::string>(), "Vacation2026");

    // 3b. Batch remove tag: remove where present, ignore unchanged media, report missing media.
    ASSERT_EQ(client.Delete("/api/media/" + std::to_string(id2) + "/tags/" + std::to_string(vacationTagId))->status, 200);
    nlohmann::json removeTagBody = { {"ids", {id1, id2, id2, 999999999}}, {"tag_id", vacationTagId} };
    auto removeTagRes = client.Delete("/api/media/batch-tags", removeTagBody.dump(), "application/json");
    ASSERT_TRUE(removeTagRes);
    EXPECT_EQ(removeTagRes->status, 200);
    auto removeTagJson = nlohmann::json::parse(removeTagRes->body);
    EXPECT_EQ(removeTagJson["status"].get<std::string>(), "partial");
    EXPECT_EQ(removeTagJson["removed_count"].get<int>(), 1);
    EXPECT_EQ(removeTagJson["unchanged_count"].get<int>(), 1);
    ASSERT_EQ(removeTagJson["failed_ids"].size(), 1u);
    EXPECT_EQ(removeTagJson["failed_ids"][0].get<MediaId>(), 999999999);
    EXPECT_EQ(nlohmann::json::parse(client.Get("/api/media/" + std::to_string(id1))->body)["tags"].size(), 0u);
    EXPECT_EQ(nlohmann::json::parse(client.Get("/api/media/" + std::to_string(id2))->body)["tags"].size(), 0u);

    // Invalid, unknown-tag, and oversized requests are rejected.
    auto invalidRemoveRes = client.Delete("/api/media/batch-tags", R"({"ids":[],"tag_id":1})", "application/json");
    ASSERT_TRUE(invalidRemoveRes);
    EXPECT_EQ(invalidRemoveRes->status, 400);
    auto unknownTagRes = client.Delete("/api/media/batch-tags", nlohmann::json({{"ids", {id1}}, {"tag_id", 999999999}}).dump(), "application/json");
    ASSERT_TRUE(unknownTagRes);
    EXPECT_EQ(unknownTagRes->status, 404);
    nlohmann::json oversizedIds = nlohmann::json::array();
    for (int i = 0; i < 1001; ++i) oversizedIds.push_back(id1);
    auto oversizedRemoveRes = client.Delete("/api/media/batch-tags", nlohmann::json({{"ids", oversizedIds}, {"tag_id", vacationTagId}}).dump(), "application/json");
    ASSERT_TRUE(oversizedRemoveRes);
    EXPECT_EQ(oversizedRemoveRes->status, 400);

    // 4. Batch GPS
    nlohmann::json gpsBody = {{"ids", {id1, id2}}, {"has_gps", true}, {"latitude", 48.8566}, {"longitude", 2.3522}};
    auto gpsRes = client.Post("/api/media/batch-gps", gpsBody.dump(), "application/json");
    ASSERT_TRUE(gpsRes);
    EXPECT_EQ(gpsRes->status, 200);
    auto gpsJson = nlohmann::json::parse(gpsRes->body);
    EXPECT_EQ(gpsJson["updated_count"].get<int>(), 2);

    get1 = nlohmann::json::parse(client.Get("/api/media/" + std::to_string(id1))->body);
    EXPECT_TRUE(get1["exif"]["has_gps"].get<bool>());
    EXPECT_NEAR(get1["exif"]["latitude"].get<double>(), 48.8566, 0.0001);

    // 4b. Filter not_flag and stats
    nlohmann::json singleFlag = {{"flag", -1}};
    auto setRejectRes = client.Post("/api/media/" + std::to_string(id2) + "/flag", singleFlag.dump(), "application/json");
    ASSERT_TRUE(setRejectRes);
    EXPECT_EQ(setRejectRes->status, 200);

    auto notRejectRes = client.Get("/api/media?not_flag=-1");
    ASSERT_TRUE(notRejectRes);
    EXPECT_EQ(notRejectRes->status, 200);
    auto notRejectJson = nlohmann::json::parse(notRejectRes->body);
    EXPECT_EQ(notRejectJson["total"].get<int>(), 1);
    EXPECT_EQ(notRejectJson["items"][0]["id"].get<MediaId>(), id1);

    auto exRejectRes = client.Get("/api/media?exclude_rejects=1");
    ASSERT_TRUE(exRejectRes);
    EXPECT_EQ(exRejectRes->status, 200);
    auto exRejectJson = nlohmann::json::parse(exRejectRes->body);
    EXPECT_EQ(exRejectJson["total"].get<int>(), 1);

    auto statsRes = client.Get("/api/stats");
    ASSERT_TRUE(statsRes);
    auto statsJson = nlohmann::json::parse(statsRes->body);
    EXPECT_EQ(statsJson["total_picks"].get<int>(), 1);
    EXPECT_EQ(statsJson["total_rejects"].get<int>(), 1);
    EXPECT_EQ(statsJson["total_not_rejects"].get<int>(), 1);

    // 5. Geocode route parameter validation
    auto geoNoQ = client.Get("/api/geocode");
    ASSERT_TRUE(geoNoQ);
    EXPECT_EQ(geoNoQ->status, 400);

    auto geoEmptyQ = client.Get("/api/geocode?q=");
    ASSERT_TRUE(geoEmptyQ);
    EXPECT_EQ(geoEmptyQ->status, 200);
    auto emptyArr = nlohmann::json::parse(geoEmptyQ->body);
    EXPECT_TRUE(emptyArr.is_array());
    EXPECT_EQ(emptyArr.size(), 0u);

    // 6. Batch date
    nlohmann::json batchDateBody = {{"ids", {id1, id2}}, {"date_taken", 1700000000}};
    auto bDateRes = client.Post("/api/media/batch-date", batchDateBody.dump(), "application/json");
    ASSERT_TRUE(bDateRes);
    EXPECT_EQ(bDateRes->status, 200);
    auto bDateJson = nlohmann::json::parse(bDateRes->body);
    EXPECT_EQ(bDateJson["updated_count"].get<int>(), 2);

    get1 = nlohmann::json::parse(client.Get("/api/media/" + std::to_string(id1))->body);
    get2 = nlohmann::json::parse(client.Get("/api/media/" + std::to_string(id2))->body);
    EXPECT_EQ(get1["date_taken"].get<int64_t>(), 1700000000);
    EXPECT_EQ(get2["date_taken"].get<int64_t>(), 1700000000);

    // Batch date shift
    nlohmann::json shiftDateBody = {{"ids", {id1, id2}}, {"shift_seconds", 3600}};
    auto sDateRes = client.Post("/api/media/batch-date", shiftDateBody.dump(), "application/json");
    ASSERT_TRUE(sDateRes);
    EXPECT_EQ(sDateRes->status, 200);
    get1 = nlohmann::json::parse(client.Get("/api/media/" + std::to_string(id1))->body);
    get2 = nlohmann::json::parse(client.Get("/api/media/" + std::to_string(id2))->body);
    EXPECT_EQ(get1["date_taken"].get<int64_t>(), 1700003600);
    EXPECT_EQ(get2["date_taken"].get<int64_t>(), 1700003600);

    // Batch date with items array
    nlohmann::json itemsDateBody = {{"items", {
        {{"id", id1}, {"date_taken", 1700010000}},
        {{"id", id2}, {"date_taken", 1700020000}}
    }}};
    auto iDateRes = client.Post("/api/media/batch-date", itemsDateBody.dump(), "application/json");
    ASSERT_TRUE(iDateRes);
    EXPECT_EQ(iDateRes->status, 200);
    get1 = nlohmann::json::parse(client.Get("/api/media/" + std::to_string(id1))->body);
    get2 = nlohmann::json::parse(client.Get("/api/media/" + std::to_string(id2))->body);
    EXPECT_EQ(get1["date_taken"].get<int64_t>(), 1700010000);
    EXPECT_EQ(get2["date_taken"].get<int64_t>(), 1700020000);
}

TEST_F(ServerTest, MoveMediaSingleAndBatch) {
    httplib::Client client("127.0.0.1", port_);

    // Create test files on disk
    std::filesystem::path f1 = testDir_ / "move1.jpg";
    std::filesystem::path f2 = testDir_ / "move2.jpg";
    {
        std::ofstream(f1) << "img1";
        std::ofstream(f2) << "img2";
    }

    MediaItem it1, it2;
    it1.file_path = f1.string();
    it1.file_name = "move1.jpg";
    it1.file_size = 4;
    it1.content_hash = "h_m1";
    it1.date_taken = 1000;

    it2.file_path = f2.string();
    it2.file_name = "move2.jpg";
    it2.file_size = 4;
    it2.content_hash = "h_m2";
    it2.date_taken = 1000;

    MediaId id1 = catalog_->db().insertMedia(it1).value();
    MediaId id2 = catalog_->db().insertMedia(it2).value();

    // 1. Single move endpoint POST /api/media/:id/move
    nlohmann::json singleMoveBody = {{"destination_path", (testDir_ / "archive").string()}};
    auto singleRes = client.Post("/api/media/" + std::to_string(id1) + "/move", singleMoveBody.dump(), "application/json");
    ASSERT_TRUE(singleRes);
    EXPECT_EQ(singleRes->status, 200);
    auto singleJson = nlohmann::json::parse(singleRes->body);
    EXPECT_EQ(singleJson["status"].get<std::string>(), "ok");
    EXPECT_FALSE(std::filesystem::exists(f1));
    EXPECT_TRUE(std::filesystem::exists(testDir_ / "archive" / "move1.jpg"));

    // Check catalog media updated
    auto check1 = client.Get("/api/media/" + std::to_string(id1));
    ASSERT_TRUE(check1);
    auto cJson1 = nlohmann::json::parse(check1->body);
    EXPECT_TRUE(cJson1["file_path"].get<std::string>() == "archive/move1.jpg" ||
                cJson1["file_path"].get<std::string>() == (testDir_ / "archive" / "move1.jpg").generic_string());

    // 2. Batch move endpoint POST /api/media/batch-move
    nlohmann::json batchMoveBody = {
        {"ids", {id1, id2}},
        {"destination_path", (testDir_ / "trips" / "summer").string()}
    };
    auto batchRes = client.Post("/api/media/batch-move", batchMoveBody.dump(), "application/json");
    ASSERT_TRUE(batchRes);
    EXPECT_EQ(batchRes->status, 200);
    auto batchJson = nlohmann::json::parse(batchRes->body);
    EXPECT_EQ(batchJson["status"].get<std::string>(), "ok");
    EXPECT_EQ(batchJson["moved_count"].get<int>(), 2);
    EXPECT_TRUE(batchJson["failed_ids"].empty());

    // Verify files on disk
    EXPECT_FALSE(std::filesystem::exists(testDir_ / "archive" / "move1.jpg"));
    EXPECT_FALSE(std::filesystem::exists(f2));
    EXPECT_TRUE(std::filesystem::exists(testDir_ / "trips" / "summer" / "move1.jpg"));
    EXPECT_TRUE(std::filesystem::exists(testDir_ / "trips" / "summer" / "move2.jpg"));

    // 3. Validation errors
    nlohmann::json invalidBody = {{"ids", {id1}}, {"destination_path", ""}};
    auto errRes = client.Post("/api/media/batch-move", invalidBody.dump(), "application/json");
    ASSERT_TRUE(errRes);
    EXPECT_EQ(errRes->status, 400);

    // 4. Moving outside photos directory is rejected
    nlohmann::json outsideBody = {
        {"destination_path", (testDir_.parent_path() / "outside").string()}
    };
    auto outSingleRes = client.Post("/api/media/" + std::to_string(id1) + "/move", outsideBody.dump(), "application/json");
    ASSERT_TRUE(outSingleRes);
    EXPECT_EQ(outSingleRes->status, 400);

    nlohmann::json outsideBatchBody = {
        {"ids", {id1}},
        {"destination_path", "../outside"}
    };
    auto outBatchRes = client.Post("/api/media/batch-move", outsideBatchBody.dump(), "application/json");
    ASSERT_TRUE(outBatchRes);
    auto outBatchJson = nlohmann::json::parse(outBatchRes->body);
    EXPECT_EQ(outBatchJson["status"].get<std::string>(), "error");
    EXPECT_EQ(outBatchJson["moved_count"].get<int>(), 0);
    EXPECT_FALSE(outBatchJson["failed_ids"].empty());
}

TEST_F(ServerTest, StaticFilesAndSpaRouting) {
    httplib::Client client("127.0.0.1", port_);

    // GET / should serve index.html
    auto rootRes = client.Get("/");
    ASSERT_TRUE(rootRes);
    EXPECT_EQ(rootRes->status, 200);
    EXPECT_NE(rootRes->body.find("IMAGINE Test"), std::string::npos);
    EXPECT_NE(rootRes->get_header_value("Content-Type").find("text/html"), std::string::npos);

    // GET /app.css should serve app.css
    auto cssRes = client.Get("/app.css");
    ASSERT_TRUE(cssRes);
    EXPECT_EQ(cssRes->status, 200);
    EXPECT_NE(cssRes->body.find("#1e1e1e"), std::string::npos);
    EXPECT_NE(cssRes->get_header_value("Content-Type").find("text/css"), std::string::npos);

    // GET /unknown/spa/route should fall back to index.html
    auto spaRes = client.Get("/unknown/spa/route");
    ASSERT_TRUE(spaRes);
    EXPECT_EQ(spaRes->status, 200);
    EXPECT_NE(spaRes->body.find("IMAGINE Test"), std::string::npos);
}

TEST_F(ServerTest, ImportProgressEndpoint) {
    httplib::Client client("127.0.0.1", port_);

    auto res = client.Get("/api/import/progress");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_TRUE(j.contains("total_files"));
    EXPECT_TRUE(j.contains("processed_files"));
    EXPECT_TRUE(j.contains("is_running"));
}

TEST_F(ServerTest, ConcurrentRequestsNoSegfault) {
    // Populate media items and tags
    for (int i = 0; i < 20; ++i) {
        MediaItem item;
        item.file_path = (testDir_ / ("photo_" + std::to_string(i) + ".jpg")).string();
        item.file_name = "photo_" + std::to_string(i) + ".jpg";
        item.file_size = 1000 + i;
        item.content_hash = "hash_" + std::to_string(i);
        item.width = 800;
        item.height = 600;
        item.date_taken = 1700000000 + i * 3600;
        auto insRes = catalog_->db().insertMedia(item);
        ASSERT_TRUE(insRes.isOk());

        auto tagRes = catalog_->db().createOrGetTag("Tag" + std::to_string(i % 5));
        ASSERT_TRUE(tagRes.isOk());
        catalog_->db().addTagToMedia(insRes.value(), tagRes.value());
    }

    constexpr int kNumThreads = 8;
    constexpr int kRequestsPerThread = 25;
    std::vector<std::thread> threads;
    std::atomic<bool> failed{false};

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([this, &failed, t]() {
            httplib::Client client("127.0.0.1", port_);
            client.set_read_timeout(5, 0);

            for (int r = 0; r < kRequestsPerThread; ++r) {
                if (failed.load()) return;

                int mode = (t + r) % 5;
                if (mode == 0) {
                    auto res = client.Get("/api/media");
                    if (!res || res->status != 200) {
                        failed.store(true);
                        return;
                    }
                } else if (mode == 1) {
                    auto res = client.Get("/api/stats");
                    if (!res || res->status != 200) {
                        failed.store(true);
                        return;
                    }
                } else if (mode == 2) {
                    auto res = client.Get("/api/timeline");
                    if (!res || res->status != 200) {
                        failed.store(true);
                        return;
                    }
                } else if (mode == 3) {
                    auto res = client.Get("/api/tags");
                    if (!res || res->status != 200) {
                        failed.store(true);
                        return;
                    }
                } else {
                    auto res = client.Get("/api/media/1");
                    if (!res || (res->status != 200 && res->status != 404)) {
                        failed.store(true);
                        return;
                    }
                }
            }
        });
    }

    for (auto& th : threads) {
        if (th.joinable()) {
            th.join();
        }
    }

    EXPECT_FALSE(failed.load());
}

TEST_F(ServerTest, WebServerLifecycleAndStaticMimeTypes) {
    EXPECT_TRUE(server_->isRunning());

    // Starting already running server returns ok
    EXPECT_TRUE(server_->start("127.0.0.1", port_).isOk());

    // Access server()
    EXPECT_NO_THROW(server_->server());

    // Create various asset files to test MIME detection
    std::vector<std::pair<std::string, std::string>> files = {
        {"test.js", "application/javascript"},
        {"test.svg", "image/svg+xml"},
        {"test.png", "image/png"},
        {"test.jpg", "image/jpeg"},
        {"test.webp", "image/webp"},
        {"test.gif", "image/gif"},
        {"test.ico", "image/x-icon"},
        {"test.json", "application/json"},
        {"test.woff", "font/woff"},
        {"test.woff2", "font/woff2"},
        {"test.ttf", "font/ttf"},
        {"test.bin", "application/octet-stream"}
    };

    httplib::Client client("127.0.0.1", port_);
    for (const auto& [name, expectedMime] : files) {
        {
            std::ofstream ofs(webDir_ / name);
            ofs << "dummy content";
        }
        auto res = client.Get("/" + name);
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 200);
        EXPECT_NE(res->get_header_value("Content-Type").find(expectedMime), std::string::npos);
    }

    // Static request to /api/... returns 404 json
    auto apiNotFound = client.Get("/api/nonexistent_route");
    ASSERT_TRUE(apiNotFound);
    EXPECT_EQ(apiNotFound->status, 404);

    // When index.html does not exist, fallback returns 404 text
    std::filesystem::remove(webDir_ / "index.html");
    auto noIndex = client.Get("/nonexistent_spa_route");
    ASSERT_TRUE(noIndex);
    EXPECT_EQ(noIndex->status, 404);

    // Start with closed catalog returns error
    Catalog closedCat(1);
    WebServer closedServer(closedCat, webDir_.string());
    EXPECT_FALSE(closedServer.start("127.0.0.1", 19999).isOk());

    // Start on invalid port returns error
    WebServer badServer(*catalog_, webDir_.string());
    EXPECT_FALSE(badServer.start("127.0.0.1", -1).isOk());
}

TEST_F(ServerTest, ValidationAndErrorEndpoints) {
    httplib::Client client("127.0.0.1", port_);

    // 1. GET /api/media/99999 (not found)
    auto notFound = client.Get("/api/media/99999");
    ASSERT_TRUE(notFound);
    EXPECT_EQ(notFound->status, 404);

    // 2. Rating validation
    EXPECT_EQ(client.Post("/api/media/1/rating", "bad json", "application/json")->status, 400);
    EXPECT_EQ(client.Post("/api/media/1/rating", "{}", "application/json")->status, 400);
    EXPECT_EQ(client.Post("/api/media/1/rating", "{\"rating\": -1}", "application/json")->status, 400);
    EXPECT_EQ(client.Post("/api/media/1/rating", "{\"rating\": 6}", "application/json")->status, 400);

    // 3. Flag validation
    EXPECT_EQ(client.Post("/api/media/1/flag", "bad json", "application/json")->status, 400);
    EXPECT_EQ(client.Post("/api/media/1/flag", "{}", "application/json")->status, 400);
    EXPECT_EQ(client.Post("/api/media/1/flag", "{\"flag\": -2}", "application/json")->status, 400);
    EXPECT_EQ(client.Post("/api/media/1/flag", "{\"flag\": 2}", "application/json")->status, 400);

    // 4. Tags on media validation
    EXPECT_EQ(client.Post("/api/media/1/tags", "bad json", "application/json")->status, 400);
    EXPECT_EQ(client.Post("/api/media/1/tags", "{}", "application/json")->status, 400);

    // 5. POST /api/tags validation
    EXPECT_EQ(client.Post("/api/tags", "bad json", "application/json")->status, 400);
    EXPECT_EQ(client.Post("/api/tags", "{}", "application/json")->status, 400);

    // 6. POST /api/albums validation
    EXPECT_EQ(client.Post("/api/albums", "bad json", "application/json")->status, 400);
    EXPECT_EQ(client.Post("/api/albums", "{}", "application/json")->status, 400);

    // 7. POST /api/albums/:id/media with media_ids array and validation
    auto albumPost = client.Post("/api/albums", "{\"name\": \"AlbumMediaTest\"}", "application/json");
    ASSERT_TRUE(albumPost);
    auto albumJson = nlohmann::json::parse(albumPost->body);
    AlbumId aid = albumJson["id"].get<AlbumId>();

    EXPECT_EQ(client.Post("/api/albums/" + std::to_string(aid) + "/media", "bad json", "application/json")->status, 400);
    EXPECT_EQ(client.Post("/api/albums/" + std::to_string(aid) + "/media", "{}", "application/json")->status, 400);

    // Insert a media item
    MediaItem m;
    m.file_path = (testDir_ / "dummy.jpg").string();
    m.file_name = "dummy.jpg";
    m.file_size = 100;
    m.content_hash = "fake_hash_123456";
    m.date_taken = 1700000000;
    auto mid = catalog_->db().insertMedia(m).value();

    // POST with array of media_ids
    nlohmann::json addArray = {{"media_ids", {mid}}};
    auto addRes = client.Post("/api/albums/" + std::to_string(aid) + "/media", addArray.dump(), "application/json");
    ASSERT_TRUE(addRes);
    EXPECT_EQ(addRes->status, 200);

    auto albumOrderRes = client.Get("/api/media?album_id=" + std::to_string(aid) + "&sort=album_order-asc");
    ASSERT_TRUE(albumOrderRes);
    EXPECT_EQ(albumOrderRes->status, 200);
    EXPECT_EQ(client.Get("/api/media?sort=album_order-asc")->status, 400);

    const std::string albumOrderPath = "/api/albums/" + std::to_string(aid) + "/order";
    EXPECT_EQ(client.Post(albumOrderPath, "bad json", "application/json")->status, 400);
    EXPECT_EQ(client.Post(albumOrderPath, "{}", "application/json")->status, 400);
    EXPECT_EQ(client.Post(albumOrderPath, R"({"media_ids":"invalid"})", "application/json")->status, 400);
    EXPECT_EQ(client.Post(albumOrderPath, R"({"media_ids":["invalid"]})", "application/json")->status, 400);
    EXPECT_EQ(client.Post(albumOrderPath, nlohmann::json{{"media_ids", {mid, mid}}}.dump(), "application/json")->status, 400);
    EXPECT_EQ(client.Post("/api/albums/99999/order", R"({"media_ids":[]})", "application/json")->status, 404);

    nlohmann::json coverBody = {{"media_id", mid}};
    auto coverRes = client.Post("/api/albums/" + std::to_string(aid) + "/cover", coverBody.dump(), "application/json");
    ASSERT_TRUE(coverRes);
    EXPECT_EQ(coverRes->status, 200);
    auto albumsWithCover = nlohmann::json::parse(client.Get("/api/albums")->body);
    ASSERT_EQ(albumsWithCover.size(), 1u);
    EXPECT_EQ(albumsWithCover[0]["cover_media_id"].get<MediaId>(), mid);

    EXPECT_EQ(client.Post("/api/albums/" + std::to_string(aid) + "/cover", "{}", "application/json")->status, 400);
    EXPECT_EQ(client.Post("/api/albums/" + std::to_string(aid) + "/cover", "{\"media_id\":99999}", "application/json")->status, 404);
    EXPECT_EQ(client.Post("/api/albums/99999/cover", coverBody.dump(), "application/json")->status, 404);

    MediaItem video;
    video.file_path = (testDir_ / "dummy.mp4").string();
    video.file_name = "dummy.mp4";
    video.file_size = 200;
    video.content_hash = "fake_video_hash_123456";
    video.date_taken = 1700000001;
    video.media_type = "video";
    auto videoId = catalog_->db().insertMedia(video).value();
    ASSERT_TRUE(catalog_->db().addMediaToAlbum(aid, videoId).isOk());

    auto reorderRes = client.Post(albumOrderPath,
                                  nlohmann::json{{"media_ids", {videoId, mid}}}.dump(),
                                  "application/json");
    ASSERT_TRUE(reorderRes);
    EXPECT_EQ(reorderRes->status, 200);
    auto reorderedMedia = nlohmann::json::parse(
        client.Get("/api/media?album_id=" + std::to_string(aid) + "&sort=album_order-asc")->body);
    ASSERT_EQ(reorderedMedia["items"].size(), 2u);
    EXPECT_EQ(reorderedMedia["items"][0]["id"].get<MediaId>(), videoId);
    EXPECT_EQ(reorderedMedia["items"][1]["id"].get<MediaId>(), mid);

    nlohmann::json oversizedOrder = nlohmann::json::array();
    for (int i = 0; i < 100001; ++i) oversizedOrder.push_back(i + 1);
    EXPECT_EQ(client.Post(albumOrderPath,
                          nlohmann::json{{"media_ids", oversizedOrder}}.dump(),
                          "application/json")->status, 400);

    EXPECT_EQ(client.Post("/api/albums/" + std::to_string(aid) + "/cover",
                          nlohmann::json{{"media_id", videoId}}.dump(),
                          "application/json")->status, 404);

    // DELETE /api/albums/:id/media with validation, single, and batch payloads
    const std::string albumMediaPath = "/api/albums/" + std::to_string(aid) + "/media";
    EXPECT_EQ(client.Delete(albumMediaPath)->status, 400);
    EXPECT_EQ(client.Delete(albumMediaPath, "{}", "application/json")->status, 400);
    EXPECT_EQ(client.Delete("/api/albums/99999/media",
                            nlohmann::json{{"media_id", mid}}.dump(),
                            "application/json")->status, 404);

    nlohmann::json oversizedRemove = nlohmann::json::array();
    for (int i = 0; i < 1001; ++i) {
        oversizedRemove.push_back(i);
    }
    EXPECT_EQ(client.Delete(albumMediaPath,
                            nlohmann::json{{"media_ids", oversizedRemove}}.dump(),
                            "application/json")->status, 400);

    auto removeSingle = client.Delete(albumMediaPath,
                                      nlohmann::json{{"media_id", mid}}.dump(),
                                      "application/json");
    ASSERT_TRUE(removeSingle);
    EXPECT_EQ(removeSingle->status, 200);

    auto removeBatch = client.Delete(albumMediaPath,
                                     nlohmann::json{{"media_ids", {videoId}}}.dump(),
                                     "application/json");
    ASSERT_TRUE(removeBatch);
    EXPECT_EQ(removeBatch->status, 200);
    auto emptyAlbumMedia = client.Get("/api/media?album_id=" + std::to_string(aid));
    ASSERT_TRUE(emptyAlbumMedia);
    EXPECT_EQ(nlohmann::json::parse(emptyAlbumMedia->body)["total"], 0);

    // DELETE /api/albums/:id
    auto delAlb = client.Delete("/api/albums/" + std::to_string(aid));
    ASSERT_TRUE(delAlb);
    EXPECT_EQ(delAlb->status, 200);

    // 8. POST /api/import validation and execution
    EXPECT_EQ(client.Post("/api/import", "bad json", "application/json")->status, 400);
    EXPECT_EQ(client.Post("/api/import", "{}", "application/json")->status, 400);
    EXPECT_EQ(client.Post("/api/import", "{\"path\": \"/nonexistent_import_dir\"}", "application/json")->status, 404);

    auto photosDir = testDir_ / "photos_import";
    std::filesystem::create_directories(photosDir);
    nlohmann::json importBody = {{"path", photosDir.string()}};
    auto impRes = client.Post("/api/import", importBody.dump(), "application/json");
    ASSERT_TRUE(impRes);
    EXPECT_EQ(impRes->status, 200);

    // 9. Query params on /api/media
    auto qRes = client.Get("/api/media?rating=3&max_rating=5&flag=0&search=dummy&date_from=1000&date_to=2000000000&limit=10&offset=0&sort=rating-asc");
    ASSERT_TRUE(qRes);
    EXPECT_EQ(qRes->status, 200);

    auto qSortDesc = client.Get("/api/media?sort=file_name");
    ASSERT_TRUE(qSortDesc);
    EXPECT_EQ(qSortDesc->status, 200);

    EXPECT_EQ(client.Get("/api/media?last_imported=true")->status, 200);
    EXPECT_EQ(client.Get("/api/media?last_imported=0")->status, 200);
}

TEST_F(ServerTest, PhotosOriginalAndThumbnailsEndpoints) {
    httplib::Client client("127.0.0.1", port_);

    // 1. /api/photos/:id/original when item doesn't exist
    EXPECT_EQ(client.Get("/api/photos/99999/original")->status, 404);

    // Create real image on disk
    std::string realImgPath = (testDir_ / "real.jpg").string();
    {
        std::ofstream ofs(realImgPath);
        ofs << "JPEG_DATA_SIMULATION";
    }

    MediaItem item;
    item.file_path = realImgPath;
    item.file_name = "real.jpg";
    item.file_size = 20;
    item.content_hash = "real_hash_123456";
    item.date_taken = 1700000000;
    auto mid = catalog_->db().insertMedia(item).value();

    // 2. /api/photos/:id/original when item exists
    auto origRes = client.Get("/api/photos/" + std::to_string(mid) + "/original");
    ASSERT_TRUE(origRes);
    EXPECT_EQ(origRes->status, 200);
    EXPECT_EQ(origRes->get_header_value("Content-Type"), "image/jpeg");

    // 3. /api/photos/:id/original when file deleted from disk
    std::filesystem::remove(realImgPath);
    EXPECT_EQ(client.Get("/api/photos/" + std::to_string(mid) + "/original")->status, 404);

    // 4. /api/thumbnails/:hash/:size not found
    EXPECT_EQ(client.Get("/api/thumbnails/nonexistenthash/256")->status, 404);
}

TEST_F(ServerTest, ApiRouterStandaloneWithDbAndCache) {
    // Test ApiRouter when initialized with (db, cache) without Catalog
    CatalogDb db;
    ASSERT_TRUE(db.open(":memory:").isOk());
    Cache cache((testDir_ / "standalone_cache").string());

    ApiRouter router(db, cache);
    httplib::Server s;
    router.registerRoutes(s);

    EXPECT_NO_THROW(router.db());
    EXPECT_NO_THROW(router.cache());

    int port = 19500 + (std::rand() % 4000);
    ASSERT_TRUE(s.bind_to_port("127.0.0.1", port));

    std::thread th([&s]() { s.listen_after_bind(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    httplib::Client client("127.0.0.1", port);

    // Stats
    auto statsRes = client.Get("/api/stats");
    ASSERT_TRUE(statsRes);
    EXPECT_EQ(statsRes->status, 200);

    // Fallback import progress
    auto progRes = client.Get("/api/import/progress");
    ASSERT_TRUE(progRes);
    EXPECT_EQ(progRes->status, 200);

    // Operations requiring Catalog fail cleanly with 500 in standalone mode
    auto moveRes = client.Post("/api/media/1/move", "{\"destination_path\": \"dest\"}", "application/json");
    ASSERT_TRUE(moveRes);
    EXPECT_EQ(moveRes->status, 500);

    auto renameRes = client.Post("/api/media/1/rename", "{\"name\": \"test.jpg\"}", "application/json");
    ASSERT_TRUE(renameRes);
    EXPECT_EQ(renameRes->status, 500);

    auto delRes = client.Delete("/api/media/1?delete_from_disk=true");
    ASSERT_TRUE(delRes);
    EXPECT_EQ(delRes->status, 500);

    // Stop standalone server
    s.stop();
    if (th.joinable()) th.join();
}

TEST_F(ServerTest, WebServerRunAndMoreMimeTypes) {
    // Test WebServer::run failure
    WebServer s(*catalog_, webDir_.string());
    EXPECT_FALSE(s.run("127.0.0.1", -1).isOk());
    s.wait(); // wait on non-started server is safe

    // Test different image mime types on /api/photos/:id/original
    std::vector<std::pair<std::string, std::string>> imgTypes = {
        {"pic.png", "image/png"},
        {"pic.webp", "image/webp"},
        {"pic.bmp", "image/bmp"},
        {"pic.svg", "image/svg+xml"}
    };

    httplib::Client client("127.0.0.1", port_);
    for (const auto& [fname, expectedMime] : imgTypes) {
        std::string p = (testDir_ / fname).string();
        {
            std::ofstream ofs(p);
            ofs << "dummy_content";
        }
        MediaItem item;
        item.file_path = p;
        item.file_name = fname;
        item.content_hash = "hash_" + fname;
        auto mid = catalog_->db().insertMedia(item).value();

        auto res = client.Get("/api/photos/" + std::to_string(mid) + "/original");
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 200);
        EXPECT_EQ(res->get_header_value("Content-Type"), expectedMime);
    }

    // Single media_id in /api/albums/:id/media
    auto albRes = client.Post("/api/albums", "{\"name\": \"SingleItemAlbum\"}", "application/json");
    ASSERT_TRUE(albRes);
    AlbumId aid = nlohmann::json::parse(albRes->body)["id"].get<AlbumId>();

    nlohmann::json singleBody = {{"media_id", 1}};
    auto singleAdd = client.Post("/api/albums/" + std::to_string(aid) + "/media", singleBody.dump(), "application/json");
    ASSERT_TRUE(singleAdd);
    EXPECT_EQ(singleAdd->status, 200);
}

TEST_F(ServerTest, GpsCoordinatesEndpointAndQuery) {
    MediaItem item;
    item.file_path = (testDir_ / "gps_photo.jpg").string();
    item.file_name = "gps_photo.jpg";
    item.content_hash = "gps_hash_123";
    auto mid = catalog_->db().insertMedia(item).value();

    httplib::Client client("127.0.0.1", port_);

    // 1. Initial query: has_gps=1 returns 0 items
    auto resInitial = client.Get("/api/media?has_gps=1");
    ASSERT_TRUE(resInitial);
    EXPECT_EQ(resInitial->status, 200);
    EXPECT_EQ(nlohmann::json::parse(resInitial->body)["total"], 0);

    // 2. Set GPS via POST /api/media/:id/gps
    nlohmann::json setGpsBody = {
        {"has_gps", true},
        {"latitude", 45.4642},
        {"longitude", 9.1900},
        {"altitude", 120.0}
    };
    auto postRes = client.Post("/api/media/" + std::to_string(mid) + "/gps", setGpsBody.dump(), "application/json");
    ASSERT_TRUE(postRes);
    EXPECT_EQ(postRes->status, 200);
    auto postJson = nlohmann::json::parse(postRes->body);
    EXPECT_EQ(postJson["status"], "ok");
    EXPECT_TRUE(postJson["has_gps"]);
    EXPECT_DOUBLE_EQ(postJson["latitude"], 45.4642);
    EXPECT_DOUBLE_EQ(postJson["longitude"], 9.1900);

    // 3. Query has_gps=1 returns 1 item
    auto resWithGps = client.Get("/api/media?has_gps=1");
    ASSERT_TRUE(resWithGps);
    EXPECT_EQ(resWithGps->status, 200);
    EXPECT_EQ(nlohmann::json::parse(resWithGps->body)["total"], 1);

    // 4. Invalid latitude returns 400
    nlohmann::json invalidLat = {{"has_gps", true}, {"latitude", 95.0}, {"longitude", 10.0}};
    auto errRes = client.Post("/api/media/" + std::to_string(mid) + "/gps", invalidLat.dump(), "application/json");
    ASSERT_TRUE(errRes);
    EXPECT_EQ(errRes->status, 400);

    // 5. Clear GPS
    nlohmann::json clearGps = {{"has_gps", false}};
    auto clearRes = client.Post("/api/media/" + std::to_string(mid) + "/gps", clearGps.dump(), "application/json");
    ASSERT_TRUE(clearRes);
    EXPECT_EQ(clearRes->status, 200);

    auto resCleared = client.Get("/api/media?has_gps=1");
    ASSERT_TRUE(resCleared);
    EXPECT_EQ(nlohmann::json::parse(resCleared->body)["total"], 0);
}

TEST_F(ServerTest, RangeRequestsOnOriginalPhoto) {
    httplib::Client client("127.0.0.1", port_);

    std::string testContent = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::string photoPath = (testDir_ / "range_test.jpg").string();
    {
        std::ofstream ofs(photoPath, std::ios::binary);
        ofs << testContent;
    }

    MediaItem item;
    item.file_path = photoPath;
    item.file_name = "range_test.jpg";
    item.file_size = testContent.size();
    item.content_hash = "range_test_hash_123";
    item.date_taken = 1700000000;
    auto mid = catalog_->db().insertMedia(item).value();

    // 1. Full request without Range
    auto fullRes = client.Get("/api/photos/" + std::to_string(mid) + "/original");
    ASSERT_TRUE(fullRes);
    EXPECT_EQ(fullRes->status, 200);
    EXPECT_EQ(fullRes->body, testContent);
    EXPECT_EQ(fullRes->get_header_value("Accept-Ranges"), "bytes");

    // 2. Range: bytes=0-9 (first 10 bytes)
    httplib::Headers headers1 = {{"Range", "bytes=0-9"}};
    auto rangeRes1 = client.Get("/api/photos/" + std::to_string(mid) + "/original", headers1);
    ASSERT_TRUE(rangeRes1);
    EXPECT_EQ(rangeRes1->status, 206);
    EXPECT_EQ(rangeRes1->body, "0123456789");
    EXPECT_EQ(rangeRes1->get_header_value("Content-Range"), "bytes 0-9/" + std::to_string(testContent.size()));
    EXPECT_EQ(rangeRes1->get_header_value("Content-Length"), "10");

    // 3. Range: bytes=10- (from 10 to end)
    httplib::Headers headers2 = {{"Range", "bytes=10-"}};
    auto rangeRes2 = client.Get("/api/photos/" + std::to_string(mid) + "/original", headers2);
    ASSERT_TRUE(rangeRes2);
    EXPECT_EQ(rangeRes2->status, 206);
    EXPECT_EQ(rangeRes2->body, testContent.substr(10));
    EXPECT_EQ(rangeRes2->get_header_value("Content-Range"), "bytes 10-" + std::to_string(testContent.size() - 1) + "/" + std::to_string(testContent.size()));

    // 4. Suffix range: bytes=-5 (last 5 bytes)
    httplib::Headers headers3 = {{"Range", "bytes=-5"}};
    auto rangeRes3 = client.Get("/api/photos/" + std::to_string(mid) + "/original", headers3);
    ASSERT_TRUE(rangeRes3);
    EXPECT_EQ(rangeRes3->status, 206);
    EXPECT_EQ(rangeRes3->body, testContent.substr(testContent.size() - 5));

    // 5. Out of bounds range: bytes=100-200 -> 416
    httplib::Headers headers4 = {{"Range", "bytes=100-200"}};
    auto rangeRes4 = client.Get("/api/photos/" + std::to_string(mid) + "/original", headers4);
    ASSERT_TRUE(rangeRes4);
    EXPECT_EQ(rangeRes4->status, 416);
    EXPECT_EQ(rangeRes4->get_header_value("Content-Range"), "bytes */" + std::to_string(testContent.size()));
}

TEST_F(ServerTest, StaticFileTraversalAndSymlinkEscapeProtection) {
    httplib::Client client("127.0.0.1", port_);

    // 1. Create a secret file outside webDir_
    std::filesystem::path secretFile = testDir_ / "secret.txt";
    {
        std::ofstream ofs(secretFile);
        ofs << "SUPER_SECRET_DATA";
    }

    // Attempt path traversal via /../secret.txt
    auto travRes = client.Get("/../secret.txt");
    ASSERT_TRUE(travRes);
    EXPECT_EQ(travRes->status, 403);
    EXPECT_EQ(travRes->body.find("SUPER_SECRET_DATA"), std::string::npos);

    // 2. Create a symlink inside webDir pointing to secretFile outside
    std::filesystem::path symlinkPath = webDir_ / "symlink_escape.txt";
    std::error_code ec;
    std::filesystem::create_symlink(secretFile, symlinkPath, ec);
    if (!ec) {
        auto symRes = client.Get("/symlink_escape.txt");
        ASSERT_TRUE(symRes);
        EXPECT_EQ(symRes->status, 403);
        EXPECT_EQ(symRes->body.find("SUPER_SECRET_DATA"), std::string::npos);
    }
}

TEST_F(ServerTest, BatchLimitsAndFailedIdsTracking) {
    httplib::Client client("127.0.0.1", port_);

    MediaItem item;
    item.file_path = (testDir_ / "bitem.jpg").string();
    item.file_name = "bitem.jpg";
    item.file_size = 100;
    item.content_hash = "bitem_hash";
    item.date_taken = 1000;
    auto validId = catalog_->db().insertMedia(item).value();
    MediaId invalidId = 999999;

    // 1. Batch rating with one valid and one invalid ID
    nlohmann::json rateBody = {{"ids", {validId, invalidId}}, {"rating", 4}};
    auto rateRes = client.Post("/api/media/batch-rating", rateBody.dump(), "application/json");
    ASSERT_TRUE(rateRes);
    EXPECT_EQ(rateRes->status, 200);
    auto rateJson = nlohmann::json::parse(rateRes->body);
    EXPECT_EQ(rateJson["status"], "partial");
    EXPECT_EQ(rateJson["updated_count"], 1);
    EXPECT_EQ(rateJson["failed_ids"].size(), 1u);
    EXPECT_EQ(rateJson["failed_ids"][0].get<MediaId>(), invalidId);

    // 2. Batch flag with one valid and one invalid ID
    nlohmann::json flagBody = {{"ids", {validId, invalidId}}, {"flag", 1}};
    auto flagRes = client.Post("/api/media/batch-flag", flagBody.dump(), "application/json");
    ASSERT_TRUE(flagRes);
    EXPECT_EQ(flagRes->status, 200);
    auto flagJson = nlohmann::json::parse(flagRes->body);
    EXPECT_EQ(flagJson["status"], "partial");
    EXPECT_EQ(flagJson["updated_count"], 1);
    EXPECT_EQ(flagJson["failed_ids"].size(), 1u);
    EXPECT_EQ(flagJson["failed_ids"][0].get<MediaId>(), invalidId);

    // 3. Batch delete with invalid ID
    nlohmann::json delBody = {{"ids", {invalidId}}};
    auto delRes = client.Post("/api/media/batch-delete", delBody.dump(), "application/json");
    ASSERT_TRUE(delRes);
    EXPECT_EQ(delRes->status, 200);
    auto delJson = nlohmann::json::parse(delRes->body);
    EXPECT_EQ(delJson["status"], "partial");
    EXPECT_EQ(delJson["deleted_count"], 0);
    EXPECT_EQ(delJson["failed_ids"].size(), 1u);
    EXPECT_EQ(delJson["failed_ids"][0].get<MediaId>(), invalidId);

    // 4. Batch size limit (> 1000 items)
    std::vector<MediaId> hugeList(1001, validId);
    nlohmann::json hugeBody = {{"ids", hugeList}, {"rating", 3}};
    auto hugeRes = client.Post("/api/media/batch-rating", hugeBody.dump(), "application/json");
    ASSERT_TRUE(hugeRes);
    EXPECT_EQ(hugeRes->status, 400);
}

TEST_F(ServerTest, QueryParameterStrictValidation) {
    httplib::Client client("127.0.0.1", port_);

    // Invalid non-integer limit returns 400
    EXPECT_EQ(client.Get("/api/media?limit=abc")->status, 400);
    // Negative limit returns 400
    EXPECT_EQ(client.Get("/api/media?limit=-1")->status, 400);
    // Too large limit returns 400
    EXPECT_EQ(client.Get("/api/media?limit=1001")->status, 400);

    // Invalid offset
    EXPECT_EQ(client.Get("/api/media?offset=xyz")->status, 400);
    EXPECT_EQ(client.Get("/api/media?offset=-10")->status, 400);

    // Invalid rating ranges
    EXPECT_EQ(client.Get("/api/media?rating=6")->status, 400);
    EXPECT_EQ(client.Get("/api/media?rating=-1")->status, 400);
    EXPECT_EQ(client.Get("/api/media?max_rating=6")->status, 400);
    EXPECT_EQ(client.Get("/api/media?rating=4&max_rating=2")->status, 400);

    // Invalid dates
    EXPECT_EQ(client.Get("/api/media?date_from=bad")->status, 400);
    EXPECT_EQ(client.Get("/api/media?date_from=5000&date_to=1000")->status, 400);

    // Invalid sort
    EXPECT_EQ(client.Get("/api/media?sort=not_a_column")->status, 400);
    EXPECT_EQ(client.Get("/api/media?sort=date_taken-sideways")->status, 400);

    // Invalid coordinates
    EXPECT_EQ(client.Get("/api/media?min_lat=95")->status, 400);
    EXPECT_EQ(client.Get("/api/media?max_lat=-95")->status, 400);
    EXPECT_EQ(client.Get("/api/media?min_lat=20&max_lat=10")->status, 400);
    EXPECT_EQ(client.Get("/api/media?min_lon=200")->status, 400);

    // Invalid flag query
    EXPECT_EQ(client.Get("/api/media?flag=5")->status, 400);
    EXPECT_EQ(client.Get("/api/media?flag=not_a_num")->status, 400);
    EXPECT_EQ(client.Get("/api/media?not_flag=5")->status, 400);
    EXPECT_EQ(client.Get("/api/media?not_flag=not_a_num")->status, 400);

    // Invalid last-imported boolean query
    EXPECT_EQ(client.Get("/api/media?last_imported=yes")->status, 400);
    EXPECT_EQ(client.Get("/api/media?flag=1&not_flag=1")->status, 400);
}

TEST_F(ServerTest, ApiTokenAuthentication) {
    server_->setApiToken("secret123");

    httplib::Client client("127.0.0.1", port_);

    // 1. GET requests work without token
    EXPECT_EQ(client.Get("/api/media")->status, 200);
    EXPECT_EQ(client.Get("/api/stats")->status, 200);

    // 2. POST /api/tags without token returns 401
    nlohmann::json tagBody = {{"name", "SecretTag"}};
    auto noAuthRes = client.Post("/api/tags", tagBody.dump(), "application/json");
    ASSERT_TRUE(noAuthRes);
    EXPECT_EQ(noAuthRes->status, 401);

    // 3. POST /api/tags with wrong Bearer token returns 401
    httplib::Headers badHeaders = {{"Authorization", "Bearer wrong_token"}};
    auto badAuthRes = client.Post("/api/tags", badHeaders, tagBody.dump(), "application/json");
    ASSERT_TRUE(badAuthRes);
    EXPECT_EQ(badAuthRes->status, 401);

    // 4. POST /api/tags with valid Bearer token returns 201
    httplib::Headers goodHeaders = {{"Authorization", "Bearer secret123"}};
    auto goodAuthRes = client.Post("/api/tags", goodHeaders, tagBody.dump(), "application/json");
    ASSERT_TRUE(goodAuthRes);
    EXPECT_EQ(goodAuthRes->status, 201);

    // 5. POST with X-API-Key header returns 201
    httplib::Headers apiKeyHeaders = {{"X-API-Key", "secret123"}};
    nlohmann::json tag2 = {{"name", "KeyTag"}};
    auto keyRes = client.Post("/api/tags", apiKeyHeaders, tag2.dump(), "application/json");
    ASSERT_TRUE(keyRes);
    EXPECT_EQ(keyRes->status, 201);

    // Reset token for subsequent tests
    server_->setApiToken("");
}

TEST_F(ServerTest, FaceRoutesValidateRequestsAndProtectWrites) {
    httplib::Client client("127.0.0.1", port_);
    auto statusRes = client.Get("/api/faces/status");
    ASSERT_TRUE(statusRes);
    ASSERT_EQ(statusRes->status, 200);
    const auto status = nlohmann::json::parse(statusRes->body);
    EXPECT_TRUE(status.contains("built"));
    EXPECT_TRUE(status.contains("ready"));
    EXPECT_TRUE(status.contains("config"));
    EXPECT_TRUE(status.contains("runtime"));
    auto badReviewLimit = client.Get("/api/faces/review?limit=201");
    ASSERT_TRUE(badReviewLimit);
    EXPECT_EQ(badReviewLimit->status, 400);
    auto zeroJob = client.Get("/api/faces/jobs/0");
    ASSERT_TRUE(zeroJob);
    EXPECT_EQ(zeroJob->status, 400);
    auto zeroMedia = client.Get("/api/faces/media/0");
    ASSERT_TRUE(zeroMedia);
    EXPECT_EQ(zeroMedia->status, 400);
    auto unknownJob = client.Get("/api/faces/jobs/999999");
    ASSERT_TRUE(unknownJob);
    EXPECT_EQ(unknownJob->status, 404);

    MediaItem media;
    media.file_path = (testDir_ / "missing-face-photo.jpg").string();
    media.file_name = "missing-face-photo.jpg";
    media.content_hash = "face-api-test-hash";
    media.width = 32;
    media.height = 24;
    const MediaId mediaId = catalog_->db().insertMedia(media).value();

    server_->setApiToken("face-token");
    const auto body = nlohmann::json{{"scope", "selected"}, {"media_ids", {mediaId}}}.dump();
    auto unauth = client.Post("/api/faces/jobs", body, "application/json");
    ASSERT_TRUE(unauth);
    EXPECT_EQ(unauth->status, 401);

    httplib::Headers auth = {{"Authorization", "Bearer face-token"}};
    auto zeroId = client.Post("/api/faces/jobs", auth,
        nlohmann::json{{"scope", "selected"}, {"media_ids", {0}}}.dump(), "application/json");
    ASSERT_TRUE(zeroId);
    EXPECT_EQ(zeroId->status, 400);

    auto duplicate = client.Post("/api/faces/jobs", auth,
        nlohmann::json{{"scope", "selected"}, {"media_ids", {mediaId, mediaId}}}.dump(), "application/json");
    ASSERT_TRUE(duplicate);
    EXPECT_EQ(duplicate->status, 400);

    std::vector<MediaId> tooMany(1001, mediaId);
    auto oversized = client.Post("/api/faces/jobs", auth,
        nlohmann::json{{"scope", "selected"}, {"media_ids", tooMany}}.dump(), "application/json");
    ASSERT_TRUE(oversized);
    EXPECT_EQ(oversized->status, 400);

    auto catalogWithIds = client.Post("/api/faces/jobs", auth,
        nlohmann::json{{"scope", "catalog"}, {"media_ids", nlohmann::json::array()}}.dump(), "application/json");
    ASSERT_TRUE(catalogWithIds);
    EXPECT_EQ(catalogWithIds->status, 400);

    if (!status["ready"].get<bool>()) {
        auto unavailable = client.Post("/api/faces/jobs", auth, body, "application/json");
        ASSERT_TRUE(unavailable);
        EXPECT_EQ(unavailable->status, 503);
        auto job = catalog_->db().connection().prepare("SELECT COUNT(*) FROM face_analysis_jobs;");
        ASSERT_TRUE(job.isOk());
        auto stmt = std::move(job.value());
        ASSERT_EQ(stmt.step(), StepResult::Row);
        EXPECT_EQ(stmt.getInt64(0), 0);
    }

    auto malformedRevision = client.Post("/api/faces/1/dismiss", auth,
        R"({"revision":"old","dismissed":true})", "application/json");
    ASSERT_TRUE(malformedRevision);
    EXPECT_EQ(malformedRevision->status, 400);
    server_->setApiToken("");
}

TEST_F(ServerTest, FaceIdentityRevisionsAndTagProvenance) {
    auto addPhoto = [&](const std::string& suffix) {
        MediaItem media;
        media.file_path = suffix + ".jpg";
        media.file_name = suffix + ".jpg";
        media.content_hash = "hash-" + suffix;
        media.width = 100;
        media.height = 80;
        return catalog_->db().insertMedia(media).value();
    };
    auto addFaceResult = [&](MediaId mediaId) {
        auto& connection = catalog_->db().connection();
        std::vector<int64_t> ids;
        auto analysisRes = connection.prepare(R"SQL(
            INSERT INTO face_media_analysis(media_id,state,source_hash,width,height,detector_checksum,recognizer_checksum,
                pipeline_version,detector_provider,recognizer_provider,device,confidence,nms_threshold,analyzed_at)
            VALUES(?,'complete','test-hash',100,80,'det','rec','pipeline','cpu','cpu','cpu',0.5,0.4,1);
        )SQL");
        EXPECT_TRUE(analysisRes.isOk());
        if (!analysisRes.isOk()) return ids;
        auto analysis = std::move(analysisRes.value()); analysis.bind(1, mediaId);
        EXPECT_EQ(analysis.step(), StepResult::Done);
        for (int i = 0; i < 2; ++i) {
            auto faceRes = connection.prepare(R"SQL(
                INSERT INTO faces(media_id,x,y,width,height,score,landmarks,embedding,embedding_size,created_at,updated_at)
                VALUES(?,?,?,?,?,0.9,'[{"x":1,"y":1}]',?,512,1,1);
            )SQL");
            EXPECT_TRUE(faceRes.isOk());
            if (!faceRes.isOk()) return ids;
            auto face = std::move(faceRes.value());
            face.bind(1, mediaId); face.bind(2, 10.0 + i * 30); face.bind(3, 12.0);
            face.bind(4, 20.0); face.bind(5, 20.0);
            std::array<float, 512> embedding{};
            embedding[0] = 1.0f;
            EXPECT_EQ(sqlite3_bind_blob(face.raw(), 6, embedding.data(), sizeof(embedding), SQLITE_TRANSIENT), SQLITE_OK);
            EXPECT_EQ(face.step(), StepResult::Done);
            ids.push_back(connection.lastInsertRowId());
        }
        return ids;
    };

    const TagId personId = catalog_->db().createOrGetTag("Rhea", "people").value();
    const MediaId manualMedia = addPhoto("manual-face");
    ASSERT_TRUE(catalog_->db().addTagToMedia(manualMedia, personId).isOk());
    auto manualFaces = addFaceResult(manualMedia);

    const MediaId derivedMedia = addPhoto("derived-face");
    auto derivedFaces = addFaceResult(derivedMedia);

    server_->setApiToken("face-token");
    httplib::Headers auth = {{"Authorization", "Bearer face-token"}};
    httplib::Client client("127.0.0.1", port_);

    auto badTag = client.Post("/api/faces/" + std::to_string(manualFaces[0]) + "/identity", auth,
        nlohmann::json{{"revision", 1}, {"tag_id", 999999}}.dump(), "application/json");
    ASSERT_TRUE(badTag);
    EXPECT_EQ(badTag->status, 400);
    auto unchanged = client.Get("/api/faces/media/" + std::to_string(manualMedia));
    ASSERT_TRUE(unchanged);
    ASSERT_EQ(unchanged->status, 200);
    EXPECT_EQ(nlohmann::json::parse(unchanged->body)["faces"][0]["revision"], 1);

    auto stale = client.Post("/api/faces/" + std::to_string(manualFaces[0]) + "/identity", auth,
        nlohmann::json{{"revision", 2}, {"tag_id", personId}}.dump(), "application/json");
    ASSERT_TRUE(stale);
    EXPECT_EQ(stale->status, 409);
    auto staleName = client.Post("/api/faces/" + std::to_string(manualFaces[0]) + "/identity", auth,
        nlohmann::json{{"revision", 2}, {"name", "Rollback Candidate"}}.dump(), "application/json");
    ASSERT_TRUE(staleName);
    EXPECT_EQ(staleName->status, 409);
    auto rolledBackTag = catalog_->db().connection().prepare(
        "SELECT COUNT(*) FROM tags WHERE name='Rollback Candidate' AND category='people';");
    ASSERT_TRUE(rolledBackTag.isOk());
    auto rolledBackTagStmt = std::move(rolledBackTag.value());
    ASSERT_EQ(rolledBackTagStmt.step(), StepResult::Row);
    EXPECT_EQ(rolledBackTagStmt.getInt64(0), 0);
    auto initialFaces = client.Get("/api/faces/media/" + std::to_string(manualMedia));
    ASSERT_TRUE(initialFaces);
    std::string staleCropUrl = nlohmann::json::parse(initialFaces->body)["faces"][0]["crop_url"].get<std::string>();

    auto assigned = client.Post("/api/faces/" + std::to_string(manualFaces[0]) + "/identity", auth,
        nlohmann::json{{"revision", 1}, {"tag_id", personId}}.dump(), "application/json");
    ASSERT_TRUE(assigned);
    ASSERT_EQ(assigned->status, 200) << assigned->body;
    auto staleCrop = client.Get(staleCropUrl);
    ASSERT_TRUE(staleCrop);
    EXPECT_EQ(staleCrop->status, 409);
    auto suggestedMedia = client.Get("/api/faces/media/" + std::to_string(manualMedia));
    ASSERT_TRUE(suggestedMedia);
    const auto suggestions = nlohmann::json::parse(suggestedMedia->body)["faces"][1]["suggestions"];
    ASSERT_EQ(suggestions.size(), 1u);
    EXPECT_EQ(suggestions[0]["tag_id"], personId);
    auto accepted = client.Post("/api/faces/" + std::to_string(manualFaces[1]) + "/accept", auth,
        nlohmann::json{{"revision", 1}, {"tag_id", personId}}.dump(), "application/json");
    ASSERT_TRUE(accepted);
    ASSERT_EQ(accepted->status, 200) << accepted->body;
    auto rejected = client.Post("/api/faces/" + std::to_string(derivedFaces[0]) + "/reject", auth,
        nlohmann::json{{"revision", 1}, {"tag_id", personId}}.dump(), "application/json");
    ASSERT_TRUE(rejected);
    ASSERT_EQ(rejected->status, 200) << rejected->body;
    EXPECT_TRUE(nlohmann::json::parse(rejected->body)["suggestions"].empty());
    auto reassigned = client.Post("/api/faces/" + std::to_string(derivedFaces[0]) + "/identity", auth,
        nlohmann::json{{"revision", 2}, {"tag_id", personId}}.dump(), "application/json");
    ASSERT_TRUE(reassigned);
    ASSERT_EQ(reassigned->status, 200) << reassigned->body;
    auto secondAssigned = client.Post("/api/faces/" + std::to_string(derivedFaces[1]) + "/identity", auth,
        nlohmann::json{{"revision", 1}, {"tag_id", personId}}.dump(), "application/json");
    ASSERT_TRUE(secondAssigned);
    ASSERT_EQ(secondAssigned->status, 200) << secondAssigned->body;
    ASSERT_EQ(catalog_->db().getTagsForMedia(derivedMedia).value().size(), 1u);
    auto removed = client.Delete("/api/media/batch-tags", auth,
        nlohmann::json{{"ids", {derivedMedia}}, {"tag_id", personId}}.dump(), "application/json");
    ASSERT_TRUE(removed);
    ASSERT_EQ(removed->status, 200) << removed->body;
    EXPECT_TRUE(catalog_->db().getTagsForMedia(derivedMedia).value().empty());
    auto mediaFaces = client.Get("/api/faces/media/" + std::to_string(derivedMedia));
    ASSERT_TRUE(mediaFaces);
    ASSERT_EQ(mediaFaces->status, 200);
    auto faceRows = nlohmann::json::parse(mediaFaces->body)["faces"];
    ASSERT_EQ(faceRows.size(), 2u);
    for (const auto& face : faceRows) EXPECT_TRUE(face["person_tag_id"].is_null());
    auto rejectionCount = catalog_->db().connection().prepare(
        "SELECT COUNT(*) FROM face_rejections WHERE tag_id=? AND face_id IN (?,?);");
    ASSERT_TRUE(rejectionCount.isOk());
    auto rejection = std::move(rejectionCount.value()); rejection.bind(1, personId);
    rejection.bind(2, derivedFaces[0]); rejection.bind(3, derivedFaces[1]);
    ASSERT_EQ(rejection.step(), StepResult::Row);
    EXPECT_EQ(rejection.getInt64(0), 2);

    auto clearFirst = client.Post("/api/faces/" + std::to_string(manualFaces[0]) + "/identity", auth,
        nlohmann::json{{"revision", 2}, {"tag_id", nullptr}}.dump(), "application/json");
    ASSERT_TRUE(clearFirst);
    ASSERT_EQ(clearFirst->status, 200) << clearFirst->body;
    EXPECT_EQ(catalog_->db().getTagsForMedia(manualMedia).value().size(), 1u);
    auto clearSecond = client.Post("/api/faces/" + std::to_string(manualFaces[1]) + "/identity", auth,
        nlohmann::json{{"revision", 2}, {"tag_id", nullptr}}.dump(), "application/json");
    ASSERT_TRUE(clearSecond);
    ASSERT_EQ(clearSecond->status, 200) << clearSecond->body;
    EXPECT_EQ(catalog_->db().getTagsForMedia(manualMedia).value().size(), 1u);
    server_->setApiToken("");
}

TEST_F(ServerTest, FaceGridScopesByJobAndSupportsDismissedPagination) {
    auto addPhoto = [&](const std::string& suffix) {
        MediaItem media;
        media.file_path = suffix + ".jpg"; media.file_name = suffix + ".jpg";
        media.content_hash = "grid-hash-" + suffix; media.width = 100; media.height = 80;
        return catalog_->db().insertMedia(media).value();
    };
    auto addAnalysis = [&](MediaId mediaId) {
        auto& conn = catalog_->db().connection();
        auto stmtRes = conn.prepare(R"SQL(
            INSERT INTO face_media_analysis(media_id,state,source_hash,width,height,detector_checksum,
                recognizer_checksum,pipeline_version,detector_provider,recognizer_provider,device,
                confidence,nms_threshold,analyzed_at)
            VALUES(?,'complete','grid-source',100,80,'det','rec','grid-v1','cpu','cpu','cpu',0.5,0.4,1);
        )SQL");
        EXPECT_TRUE(stmtRes.isOk());
        if (!stmtRes.isOk()) return;
        auto stmt = std::move(stmtRes.value()); stmt.bind(1, mediaId);
        EXPECT_EQ(stmt.step(), StepResult::Done);
    };
    auto addFace = [&](MediaId mediaId, bool dismissed) {
        auto stmtRes = catalog_->db().connection().prepare(R"SQL(
            INSERT INTO faces(media_id,x,y,width,height,score,landmarks,dismissed,created_at,updated_at)
            VALUES(?,5,5,20,20,0.9,'[]',?,1,1);
        )SQL");
        EXPECT_TRUE(stmtRes.isOk());
        if (!stmtRes.isOk()) return int64_t{0};
        auto stmt = std::move(stmtRes.value()); stmt.bind(1, mediaId); stmt.bind(2, dismissed ? 1 : 0);
        EXPECT_EQ(stmt.step(), StepResult::Done);
        return catalog_->db().connection().lastInsertRowId();
    };
    auto addJob = [&](const std::string& state, int total) {
        auto stmtRes = catalog_->db().connection().prepare(
            "INSERT INTO face_analysis_jobs(state,scope,total,processed,remaining,created_at,updated_at) "
            "VALUES(?,'selected',?,?,0,1,1);");
        EXPECT_TRUE(stmtRes.isOk());
        if (!stmtRes.isOk()) return int64_t{0};
        auto stmt = std::move(stmtRes.value()); stmt.bind(1, state); stmt.bind(2, total); stmt.bind(3, total);
        EXPECT_EQ(stmt.step(), StepResult::Done);
        return catalog_->db().connection().lastInsertRowId();
    };

    const MediaId firstMedia = addPhoto("grid-first");
    const MediaId secondMedia = addPhoto("grid-second");
    const MediaId outsideMedia = addPhoto("grid-outside");
    for (MediaId mediaId : {firstMedia, secondMedia, outsideMedia}) addAnalysis(mediaId);
    const int64_t firstFace = addFace(firstMedia, false);
    const int64_t secondFace = addFace(secondMedia, true);
    addFace(outsideMedia, false);
    const int64_t newJobId = addJob("completed", 2);
    auto memberRes = catalog_->db().connection().prepare(
        "INSERT INTO face_analysis_job_media(job_id,media_id) VALUES(?,?);");
    ASSERT_TRUE(memberRes.isOk());
    auto member = std::move(memberRes.value());
    member.bind(1, newJobId); member.bind(2, firstMedia); ASSERT_EQ(member.step(), StepResult::Done);
    member.reset(); member.bind(1, newJobId); member.bind(2, secondMedia); ASSERT_EQ(member.step(), StepResult::Done);
    const int64_t historicalJobId = addJob("interrupted", 1); // Pre-v6 jobs have no membership rows.

    httplib::Client client("127.0.0.1", port_);
    auto scoped = client.Get("/api/faces/grid?job_id=" + std::to_string(newJobId));
    ASSERT_TRUE(scoped);
    ASSERT_EQ(scoped->status, 200) << scoped->body;
    const auto scopedJson = nlohmann::json::parse(scoped->body);
    EXPECT_EQ(scopedJson["total"], 1);
    ASSERT_EQ(scopedJson["items"].size(), 1u);
    EXPECT_EQ(scopedJson["items"][0]["id"], firstFace);

    auto includeDismissed = client.Get("/api/faces/grid?job_id=" + std::to_string(newJobId) + "&include_dismissed=true");
    ASSERT_TRUE(includeDismissed);
    ASSERT_EQ(includeDismissed->status, 200);
    const auto includedJson = nlohmann::json::parse(includeDismissed->body);
    EXPECT_EQ(includedJson["total"], 2);
    ASSERT_EQ(includedJson["items"].size(), 2u);
    EXPECT_EQ(includedJson["items"][1]["id"], secondFace);

    auto page = client.Get("/api/faces/grid?job_id=" + std::to_string(newJobId) + "&include_dismissed=true&offset=1&limit=1");
    ASSERT_TRUE(page);
    ASSERT_EQ(page->status, 200);
    EXPECT_EQ(nlohmann::json::parse(page->body)["items"][0]["id"], secondFace);

    auto historical = client.Get("/api/faces/grid?job_id=" + std::to_string(historicalJobId) + "&include_dismissed=true");
    ASSERT_TRUE(historical);
    ASSERT_EQ(historical->status, 200);
    EXPECT_EQ(nlohmann::json::parse(historical->body)["total"], 0);

    auto catalog = client.Get("/api/faces/grid");
    ASSERT_TRUE(catalog);
    ASSERT_EQ(catalog->status, 200);
    EXPECT_EQ(nlohmann::json::parse(catalog->body)["total"], 2);
    EXPECT_EQ(client.Get("/api/faces/grid?limit=1001")->status, 400);
    EXPECT_EQ(client.Get("/api/faces/grid?offset=-1")->status, 400);
    EXPECT_EQ(client.Get("/api/faces/grid?job_id=999999")->status, 404);
    EXPECT_EQ(client.Get("/api/faces/grid?job_id=0")->status, 400);
    EXPECT_EQ(client.Get("/api/faces/grid?include_dismissed=yes")->status, 400);
}

TEST_F(ServerTest, FaceGroupsPageLazilyAndAcceptWholeTopSuggestionGroup) {
    auto addPhoto = [&](const std::string& suffix) {
        MediaItem media;
        media.file_path = suffix + ".jpg"; media.file_name = suffix + ".jpg";
        media.content_hash = "group-face-hash-" + suffix;
        media.media_type = "photo"; media.width = 100; media.height = 80;
        return catalog_->db().insertMedia(media).value();
    };
    auto addAnalysis = [&](MediaId mediaId) {
        auto statementRes = catalog_->db().connection().prepare(R"SQL(
            INSERT INTO face_media_analysis(media_id,state,source_hash,width,height,detector_checksum,
                recognizer_checksum,pipeline_version,detector_provider,recognizer_provider,device,
                confidence,nms_threshold,analyzed_at)
            VALUES(?,'complete','group-source',100,80,'det','rec','lazy-group-v1','cpu','cpu','cpu',0.5,0.4,1);
        )SQL");
        EXPECT_TRUE(statementRes.isOk());
        if (!statementRes.isOk()) return;
        auto statement = std::move(statementRes.value()); statement.bind(1, mediaId);
        EXPECT_EQ(statement.step(), StepResult::Done);
    };
    auto addFace = [&](MediaId mediaId, std::optional<TagId> personId,
                       const std::optional<std::array<float, 512>>& embedding, bool dismissed = false) {
        auto statementRes = catalog_->db().connection().prepare(R"SQL(
            INSERT INTO faces(media_id,x,y,width,height,score,landmarks,person_tag_id,dismissed,
                embedding,embedding_size,created_at,updated_at)
            VALUES(?,10,10,20,20,0.9,'[]',?,?,?, ?,1,1);
        )SQL");
        EXPECT_TRUE(statementRes.isOk());
        if (!statementRes.isOk()) return int64_t{0};
        auto statement = std::move(statementRes.value());
        statement.bind(1, mediaId);
        if (personId) statement.bind(2, *personId); else statement.bindNull(2);
        statement.bind(3, dismissed ? 1 : 0);
        if (embedding) {
            EXPECT_EQ(sqlite3_bind_blob(statement.raw(), 4, embedding->data(), sizeof(*embedding), SQLITE_TRANSIENT), SQLITE_OK);
            statement.bind(5, 512);
        } else {
            statement.bindNull(4); statement.bind(5, 0);
        }
        EXPECT_EQ(statement.step(), StepResult::Done);
        return catalog_->db().connection().lastInsertRowId();
    };

    const TagId alphaId = catalog_->db().createOrGetTag("Alpha Group", "people").value();
    const TagId betaId = catalog_->db().createOrGetTag("Beta Group", "people").value();
    std::array<float, 512> alphaEmbedding{}; alphaEmbedding[0] = 1.0f;
    std::array<float, 512> betaEmbedding{}; betaEmbedding[0] = 1.0f;
    std::array<float, 512> unmatchedEmbedding{}; unmatchedEmbedding[1] = 1.0f;

    const MediaId alphaPhoto = addPhoto("alpha-exemplar"); addAnalysis(alphaPhoto);
    const int64_t alphaFace = addFace(alphaPhoto, alphaId, alphaEmbedding);
    const MediaId betaPhoto = addPhoto("beta-exemplar"); addAnalysis(betaPhoto);
    addFace(betaPhoto, betaId, betaEmbedding);
    std::vector<MediaId> suggestionMedia;
    std::vector<int64_t> suggestionFaces;
    for (int i = 0; i < 3; ++i) {
        // Two detections share a photo: the group accepts both faces but its
        // button must count that photo only once.
        const MediaId mediaId = i == 2 ? suggestionMedia[1] : addPhoto("candidate-" + std::to_string(i));
        if (i != 2) addAnalysis(mediaId);
        suggestionMedia.push_back(mediaId);
        suggestionFaces.push_back(addFace(mediaId, std::nullopt, alphaEmbedding));
    }
    const MediaId unmatchedPhoto = addPhoto("unmatched"); addAnalysis(unmatchedPhoto);
    const int64_t unmatchedFace = addFace(unmatchedPhoto, std::nullopt, unmatchedEmbedding);
    const MediaId dismissedPhoto = addPhoto("dismissed"); addAnalysis(dismissedPhoto);
    addFace(dismissedPhoto, std::nullopt, std::nullopt, true);

    auto jobInsertRes = catalog_->db().connection().prepare(
        "INSERT INTO face_analysis_jobs(state,scope,total,remaining,created_at,updated_at) "
        "VALUES('completed','selected',2,0,1,1);");
    ASSERT_TRUE(jobInsertRes.isOk());
    auto jobInsert = std::move(jobInsertRes.value()); ASSERT_EQ(jobInsert.step(), StepResult::Done);
    const int64_t jobId = catalog_->db().connection().lastInsertRowId();
    auto jobMemberRes = catalog_->db().connection().prepare(
        "INSERT INTO face_analysis_job_media(job_id,media_id) VALUES(?,?);");
    ASSERT_TRUE(jobMemberRes.isOk());
    auto jobMember = std::move(jobMemberRes.value());
    jobMember.bind(1, jobId); jobMember.bind(2, suggestionMedia[0]); ASSERT_EQ(jobMember.step(), StepResult::Done);
    jobMember.reset(); jobMember.bind(1, jobId); jobMember.bind(2, unmatchedPhoto); ASSERT_EQ(jobMember.step(), StepResult::Done);

    // Create a deterministic per-face stale result during whole-group acceptance.
    // Same-connection writes preserve the snapshot's data_version, so the later
    // face fails by revision while earlier faces remain accepted.
    auto triggerRes = catalog_->db().connection().prepare(
        "CREATE TRIGGER bump_later_candidate AFTER UPDATE OF person_tag_id ON faces "
        "WHEN OLD.id=" + std::to_string(suggestionFaces[0]) + " BEGIN UPDATE faces SET revision=revision+1 WHERE id=" +
        std::to_string(suggestionFaces[2]) + "; END;");
    ASSERT_TRUE(triggerRes.isOk());
    auto trigger = std::move(triggerRes.value());
    ASSERT_EQ(trigger.step(), StepResult::Done);

    httplib::Client client("127.0.0.1", port_);
    EXPECT_EQ(client.Get("/api/faces/0")->status, 400);
    EXPECT_EQ(client.Get("/api/faces/999999")->status, 404);
    auto directFace = client.Get("/api/faces/" + std::to_string(alphaFace));
    ASSERT_TRUE(directFace);
    ASSERT_EQ(directFace->status, 200) << directFace->body;
    EXPECT_EQ(nlohmann::json::parse(directFace->body)["id"], alphaFace);
    auto groupsResponse = client.Get("/api/faces/groups");
    ASSERT_TRUE(groupsResponse);
    ASSERT_EQ(groupsResponse->status, 200) << groupsResponse->body;
    const auto groupsJson = nlohmann::json::parse(groupsResponse->body);
    EXPECT_EQ(groupsJson["total"], 6);
    std::string snapshot = groupsJson["snapshot"].get<std::string>();
    auto repeated = client.Get("/api/faces/groups");
    ASSERT_TRUE(repeated);
    ASSERT_EQ(repeated->status, 200);
    EXPECT_EQ(nlohmann::json::parse(repeated->body)["snapshot"], snapshot);
    auto cachedLookup = client.Get("/api/faces/lookup?ids=" + std::to_string(alphaFace) + "," +
        std::to_string(suggestionFaces[0]) + "," + std::to_string(suggestionFaces[1]) + ",999999");
    ASSERT_TRUE(cachedLookup);
    ASSERT_EQ(cachedLookup->status, 200) << cachedLookup->body;
    const auto cachedLookupJson = nlohmann::json::parse(cachedLookup->body);
    ASSERT_EQ(cachedLookupJson["items"].size(), 3u);
    ASSERT_EQ(cachedLookupJson["failed"].size(), 1u);
    EXPECT_EQ(cachedLookupJson["failed"][0]["id"], 999999);
    EXPECT_EQ(cachedLookupJson["failed"][0]["status"], 404);
    EXPECT_EQ(cachedLookupJson["items"][1]["suggestions"][0]["tag_id"], alphaId);
    auto findGroup = [&](const nlohmann::json& groups, const std::string& key) {
        for (const auto& group : groups["groups"]) if (group["key"] == key) return group;
        return nlohmann::json();
    };
    const auto alphaGroup = findGroup(groupsJson, "person:" + std::to_string(alphaId));
    ASSERT_FALSE(alphaGroup.is_null());
    EXPECT_EQ(alphaGroup["faces_count"], 4);
    EXPECT_EQ(alphaGroup["photos_count"], 3);
    EXPECT_EQ(alphaGroup["suggestion_faces_count"], 3);
    EXPECT_EQ(alphaGroup["suggestion_photos_count"], 2);
    const auto betaGroup = findGroup(groupsJson, "person:" + std::to_string(betaId));
    ASSERT_FALSE(betaGroup.is_null());
    EXPECT_EQ(betaGroup["faces_count"], 1);
    EXPECT_EQ(betaGroup["suggestion_faces_count"], 0);
    EXPECT_EQ(client.Get("/api/faces/groups?show_named=false&show_unnamed=false")->status, 200);
    EXPECT_EQ(client.Get("/api/faces/groups?show_named=maybe")->status, 400);
    EXPECT_EQ(client.Get("/api/faces/groups?person_tag_id=999999")->status, 400);
    EXPECT_EQ(client.Get("/api/faces/lookup")->status, 400);
    EXPECT_EQ(client.Get("/api/faces/lookup?ids=")->status, 400);
    EXPECT_EQ(client.Get("/api/faces/lookup?ids=1,1")->status, 400);
    EXPECT_EQ(client.Get("/api/faces/lookup?ids=0")->status, 400);
    std::string oversizedLookup = "/api/faces/lookup?ids=";
    for (int i = 1; i <= 201; ++i) {
        if (i > 1) oversizedLookup += ',';
        oversizedLookup += std::to_string(i);
    }
    EXPECT_EQ(client.Get(oversizedLookup)->status, 400);

    const std::string alphaKey = "person:" + std::to_string(alphaId);
    auto firstPage = client.Get("/api/faces/group?snapshot=" + snapshot + "&key=" + alphaKey + "&offset=0&limit=2");
    ASSERT_TRUE(firstPage);
    ASSERT_EQ(firstPage->status, 200) << firstPage->body;
    const auto firstPageJson = nlohmann::json::parse(firstPage->body);
    ASSERT_EQ(firstPageJson["items"].size(), 2u);
    EXPECT_EQ(firstPageJson["items"][0]["id"], alphaFace);
    EXPECT_EQ(firstPageJson["items"][1]["suggestions"][0]["tag_id"], alphaId);
    auto secondPage = client.Get("/api/faces/group?snapshot=" + snapshot + "&key=" + alphaKey + "&offset=2&limit=2");
    ASSERT_TRUE(secondPage);
    ASSERT_EQ(secondPage->status, 200);
    EXPECT_EQ(nlohmann::json::parse(secondPage->body)["items"].size(), 2u);
    EXPECT_EQ(client.Get("/api/faces/group?snapshot=" + snapshot + "&key=" + alphaKey + "&limit=201")->status, 400);

    auto scoped = client.Get("/api/faces/groups?job_id=" + std::to_string(jobId));
    ASSERT_TRUE(scoped);
    ASSERT_EQ(scoped->status, 200) << scoped->body;
    const auto scopedJson = nlohmann::json::parse(scoped->body);
    EXPECT_EQ(scopedJson["total"], 2);
    EXPECT_EQ(findGroup(scopedJson, alphaKey)["suggestion_faces_count"], 1);
    EXPECT_EQ(client.Get("/api/faces/groups?job_id=999999")->status, 404);

    auto namedOnly = client.Get("/api/faces/groups?person_tag_id=" + std::to_string(alphaId));
    ASSERT_TRUE(namedOnly);
    ASSERT_EQ(namedOnly->status, 200);
    const auto namedJson = nlohmann::json::parse(namedOnly->body);
    EXPECT_EQ(namedJson["total"], 1);
    EXPECT_EQ(findGroup(namedJson, alphaKey)["suggestion_faces_count"], 0);
    auto dismissedIncluded = client.Get("/api/faces/groups?include_dismissed=true");
    ASSERT_TRUE(dismissedIncluded);
    ASSERT_EQ(dismissedIncluded->status, 200);
    const auto dismissedJson = nlohmann::json::parse(dismissedIncluded->body);
    EXPECT_EQ(dismissedJson["total"], 7);
    EXPECT_EQ(findGroup(dismissedJson, "dismissed")["faces_count"], 1);

    auto currentGroups = client.Get("/api/faces/groups");
    ASSERT_TRUE(currentGroups);
    ASSERT_EQ(currentGroups->status, 200);
    snapshot = nlohmann::json::parse(currentGroups->body)["snapshot"].get<std::string>();
    server_->setApiToken("face-group-token");
    httplib::Headers auth = {{"Authorization", "Bearer face-group-token"}};
    const auto acceptBody = nlohmann::json{{"snapshot", snapshot}, {"key", alphaKey}}.dump();
    auto unauthorized = client.Post("/api/faces/groups/accept", acceptBody, "application/json");
    ASSERT_TRUE(unauthorized);
    EXPECT_EQ(unauthorized->status, 401);
    auto accepted = client.Post("/api/faces/groups/accept", auth, acceptBody, "application/json");
    ASSERT_TRUE(accepted);
    ASSERT_EQ(accepted->status, 200) << accepted->body;
    const auto acceptedJson = nlohmann::json::parse(accepted->body);
    EXPECT_EQ(acceptedJson["updated_count"], 2);
    ASSERT_EQ(acceptedJson["failed_count"], 1);
    EXPECT_EQ(acceptedJson["failed"][0]["id"], suggestionFaces[2]);
    EXPECT_EQ(acceptedJson["failed"][0]["status"], 409);
    auto assigned = catalog_->db().connection().prepare(
        "SELECT person_tag_id,revision FROM faces WHERE id=?;");
    ASSERT_TRUE(assigned.isOk());
    auto assignment = std::move(assigned.value());
    for (int i : {0, 1}) {
        assignment.bind(1, suggestionFaces[i]);
        ASSERT_EQ(assignment.step(), StepResult::Row);
        EXPECT_EQ(assignment.getInt64(0), alphaId);
        ASSERT_TRUE(assignment.reset().isOk());
    }
    assignment.bind(1, suggestionFaces[2]);
    ASSERT_EQ(assignment.step(), StepResult::Row);
    EXPECT_TRUE(assignment.isNull(0));
    EXPECT_EQ(assignment.getInt64(1), 2);
    ASSERT_TRUE(assignment.reset().isOk());
    auto failedFace = client.Get("/api/faces/" + std::to_string(suggestionFaces[2]));
    ASSERT_TRUE(failedFace);
    ASSERT_EQ(failedFace->status, 200);
    EXPECT_EQ(nlohmann::json::parse(failedFace->body)["revision"], 2);
    auto stalePage = client.Get("/api/faces/group?snapshot=" + snapshot + "&key=" + alphaKey);
    ASSERT_TRUE(stalePage);
    EXPECT_EQ(stalePage->status, 409);

    auto refreshed = client.Get("/api/faces/groups");
    ASSERT_TRUE(refreshed);
    ASSERT_EQ(refreshed->status, 200);
    const std::string refreshedSnapshot = nlohmann::json::parse(refreshed->body)["snapshot"].get<std::string>();
    Connection external;
    ASSERT_TRUE(external.open(dbPath_).isOk());
    auto externalUpdateRes = external.prepare("UPDATE faces SET revision=revision+1 WHERE id=?;");
    ASSERT_TRUE(externalUpdateRes.isOk());
    auto externalUpdate = std::move(externalUpdateRes.value());
    externalUpdate.bind(1, suggestionFaces[2]);
    ASSERT_EQ(externalUpdate.step(), StepResult::Done);
    auto externalStalePage = client.Get("/api/faces/group?snapshot=" + refreshedSnapshot + "&key=" + alphaKey);
    ASSERT_TRUE(externalStalePage);
    EXPECT_EQ(externalStalePage->status, 409);
    auto fallbackLookup = client.Get("/api/faces/lookup?ids=" + std::to_string(suggestionFaces[2]) + "," +
        std::to_string(unmatchedFace));
    ASSERT_TRUE(fallbackLookup);
    ASSERT_EQ(fallbackLookup->status, 200) << fallbackLookup->body;
    const auto fallbackLookupJson = nlohmann::json::parse(fallbackLookup->body);
    ASSERT_EQ(fallbackLookupJson["items"].size(), 2u);
    EXPECT_EQ(fallbackLookupJson["items"][0]["suggestions"][0]["tag_id"], alphaId);
    EXPECT_TRUE(fallbackLookupJson["items"][1]["suggestions"].empty());
    server_->setApiToken("");
}

TEST_F(ServerTest, IncrementalReviewIndexMatchesExactSuggestionsAfterMutations) {
    auto addPhoto = [&](const std::string& suffix) {
        MediaItem media;
        media.file_path = suffix + ".jpg"; media.file_name = suffix + ".jpg";
        media.content_hash = "incremental-face-hash-" + suffix;
        media.media_type = "photo"; media.width = 100; media.height = 80;
        return catalog_->db().insertMedia(media).value();
    };
    auto addAnalysis = [&](MediaId mediaId) {
        auto statementRes = catalog_->db().connection().prepare(R"SQL(
            INSERT INTO face_media_analysis(media_id,state,source_hash,width,height,detector_checksum,
                recognizer_checksum,pipeline_version,detector_provider,recognizer_provider,device,
                confidence,nms_threshold,analyzed_at)
            VALUES(?,'complete','incremental-source',100,80,'det','rec','incremental-v1','cpu','cpu','cpu',0.5,0.4,1);
        )SQL");
        EXPECT_TRUE(statementRes.isOk());
        if (!statementRes.isOk()) return;
        auto statement = std::move(statementRes.value()); statement.bind(1, mediaId);
        EXPECT_EQ(statement.step(), StepResult::Done);
    };
    auto addFace = [&](MediaId mediaId, std::optional<TagId> personId,
                       const std::array<float, 512>& embedding) {
        auto statementRes = catalog_->db().connection().prepare(R"SQL(
            INSERT INTO faces(media_id,x,y,width,height,score,landmarks,person_tag_id,embedding,
                embedding_size,created_at,updated_at)
            VALUES(?,10,10,20,20,0.9,'[]',?,?,512,1,1);
        )SQL");
        EXPECT_TRUE(statementRes.isOk());
        if (!statementRes.isOk()) return int64_t{0};
        auto statement = std::move(statementRes.value());
        statement.bind(1, mediaId);
        if (personId) statement.bind(2, *personId); else statement.bindNull(2);
        EXPECT_EQ(sqlite3_bind_blob(statement.raw(), 3, embedding.data(), sizeof(embedding), SQLITE_TRANSIENT), SQLITE_OK);
        EXPECT_EQ(statement.step(), StepResult::Done);
        return catalog_->db().connection().lastInsertRowId();
    };
    auto makeVector = [](float first) {
        std::array<float, 512> vector{};
        vector[0] = first;
        vector[1] = std::sqrt(std::max(0.0f, 1.0f - first * first));
        return vector;
    };

    const std::vector<std::string> names = {
        "Person A", "Person B", "Person C", "Person D", "Person E", "Person F"};
    const std::vector<float> similarities = {0.99f, 0.97f, 0.95f, 0.93f, 0.93f, 0.89f};
    std::vector<TagId> people;
    std::vector<int64_t> exemplarsA;
    for (size_t i = 0; i < names.size(); ++i) {
        const TagId personId = catalog_->db().createOrGetTag(names[i], "people").value();
        people.push_back(personId);
        const int copies = i == 0 ? 2 : 1;
        for (int copy = 0; copy < copies; ++copy) {
            const float score = i == 0 && copy == 1 ? 0.985f : similarities[i];
            const MediaId mediaId = addPhoto("exemplar-" + std::to_string(i) + "-" + std::to_string(copy));
            addAnalysis(mediaId);
            const int64_t faceId = addFace(mediaId, personId, makeVector(score));
            if (i == 0) exemplarsA.push_back(faceId);
        }
    }
    std::array<float, 512> queryEmbedding{}; queryEmbedding[0] = 1.0f;
    std::vector<int64_t> queryFaces;
    for (int i = 0; i < 3; ++i) {
        const MediaId mediaId = addPhoto("query-" + std::to_string(i));
        addAnalysis(mediaId);
        queryFaces.push_back(addFace(mediaId, std::nullopt, queryEmbedding));
    }

    server_->setApiToken("incremental-review-token");
    httplib::Headers auth = {{"Authorization", "Bearer incremental-review-token"}};
    httplib::Client client("127.0.0.1", port_);
    auto refreshAndCompare = [&]() {
        auto groups = client.Get("/api/faces/groups?show_named=false");
        ASSERT_TRUE(groups);
        ASSERT_EQ(groups->status, 200) << groups->body;
        const auto groupsJson = nlohmann::json::parse(groups->body);
        const std::string snapshot = groupsJson["snapshot"].get<std::string>();
        size_t compared = 0;
        for (const auto& group : groupsJson["groups"]) {
            auto page = client.Get("/api/faces/group?snapshot=" + snapshot + "&key=" +
                group.at("key").get<std::string>() + "&offset=0&limit=200");
            ASSERT_TRUE(page);
            ASSERT_EQ(page->status, 200) << page->body;
            const auto pageJson = nlohmann::json::parse(page->body);
            for (const auto& indexed : pageJson["items"]) {
                ASSERT_TRUE(indexed["person_tag_id"].is_null());
                ASSERT_FALSE(indexed.value("dismissed", false));
                const int64_t faceId = indexed["id"].get<int64_t>();
                auto exact = client.Get("/api/faces/" + std::to_string(faceId));
                ASSERT_TRUE(exact);
                ASSERT_EQ(exact->status, 200) << exact->body;
                const auto exactJson = nlohmann::json::parse(exact->body);
                EXPECT_EQ(indexed["suggestions"], exactJson["suggestions"]) << "face id " << faceId;
                const std::string expectedKey = exactJson["suggestions"].empty() ? "unnamed"
                    : "person:" + std::to_string(exactJson["suggestions"][0]["tag_id"].get<TagId>());
                EXPECT_EQ(group["key"], expectedKey) << "face id " << faceId;
                ++compared;
            }
        }
        EXPECT_GT(compared, 0u) << "mutation parity check must exercise eligible unnamed faces";
        EXPECT_EQ(compared, groupsJson["total"].get<size_t>());
    };

    refreshAndCompare();

    // Removing ranks three and four together must expose the old fifth person,
    // even though that person was outside the retained matching prefix.
    auto setBoundaryDismissed = [&](bool dismissed) {
        auto statementRes = catalog_->db().connection().prepare(
            "UPDATE faces SET dismissed=? WHERE person_tag_id IN (?,?);");
        ASSERT_TRUE(statementRes.isOk());
        auto statement = std::move(statementRes.value());
        statement.bind(1, dismissed ? 1 : 0);
        statement.bind(2, people[2]);
        statement.bind(3, people[3]);
        ASSERT_EQ(statement.step(), StepResult::Done);
    };
    setBoundaryDismissed(true);
    refreshAndCompare();
    auto exposed = client.Get("/api/faces/" + std::to_string(queryFaces[1]));
    ASSERT_TRUE(exposed);
    ASSERT_EQ(exposed->status, 200);
    EXPECT_EQ(nlohmann::json::parse(exposed->body)["suggestions"][2]["tag_id"], people[4]);
    setBoundaryDismissed(false);
    refreshAndCompare();

    // Rejections remove each of the original top three in turn. Every cached
    // page is checked against the uncached, full matcher response.
    for (int i = 0; i < 3; ++i) {
        auto exact = client.Get("/api/faces/" + std::to_string(queryFaces[0]));
        ASSERT_TRUE(exact);
        ASSERT_EQ(exact->status, 200);
        const auto current = nlohmann::json::parse(exact->body);
        ASSERT_FALSE(current["suggestions"].empty());
        const TagId rejected = current["suggestions"][0]["tag_id"].get<TagId>();
        auto response = client.Post("/api/faces/" + std::to_string(queryFaces[0]) + "/reject", auth,
            nlohmann::json{{"revision", current["revision"]}, {"tag_id", rejected}}.dump(), "application/json");
        ASSERT_TRUE(response);
        ASSERT_EQ(response->status, 200) << response->body;
        refreshAndCompare();
    }

    // Removing and restoring the best exemplar exercises alternate promotion
    // without losing the exact next-example state, then removing the second
    // exemplar tests the guarded exact fallback.
    auto clearIdentity = [&](int64_t faceId, int64_t revision) {
        auto response = client.Post("/api/faces/" + std::to_string(faceId) + "/identity", auth,
            nlohmann::json{{"revision", revision}, {"tag_id", nullptr}}.dump(), "application/json");
        ASSERT_TRUE(response);
        ASSERT_EQ(response->status, 200) << response->body;
    };
    clearIdentity(exemplarsA[0], 1);
    refreshAndCompare();
    auto restoreBest = client.Post("/api/faces/" + std::to_string(exemplarsA[0]) + "/identity", auth,
        nlohmann::json{{"revision", 2}, {"tag_id", people[0]}}.dump(), "application/json");
    ASSERT_TRUE(restoreBest);
    ASSERT_EQ(restoreBest->status, 200) << restoreBest->body;
    refreshAndCompare();
    clearIdentity(exemplarsA[0], 3);
    refreshAndCompare();
    clearIdentity(exemplarsA[1], 1);
    refreshAndCompare();

    // Adding a high-scoring exemplar and moving it to another person must keep
    // all unaffected query results exact as ranks change.
    auto assign = [&](int64_t faceId, TagId tagId, int64_t revision) {
        auto response = client.Post("/api/faces/" + std::to_string(faceId) + "/identity", auth,
            nlohmann::json{{"revision", revision}, {"tag_id", tagId}}.dump(), "application/json");
        ASSERT_TRUE(response);
        ASSERT_EQ(response->status, 200) << response->body;
    };
    assign(queryFaces[2], people[0], 1);
    refreshAndCompare();
    assign(queryFaces[2], people[1], 2);
    refreshAndCompare();

    // A lexical rename at a tied rank-four/five boundary uses exact fallback.
    auto renameRes = catalog_->db().connection().prepare("UPDATE tags SET name='Person 0' WHERE id=?;");
    ASSERT_TRUE(renameRes.isOk());
    auto rename = std::move(renameRes.value()); rename.bind(1, people[4]);
    ASSERT_EQ(rename.step(), StepResult::Done);
    refreshAndCompare();

    // External embedding edits can bypass revision increments. The semantic
    // fingerprint must still recompute that query before serving the new page.
    std::array<float, 512> externallyChanged{};
    externallyChanged[0] = 0.8f; externallyChanged[2] = 0.6f;
    auto editRes = catalog_->db().connection().prepare("UPDATE faces SET embedding=? WHERE id=?;");
    ASSERT_TRUE(editRes.isOk());
    auto edit = std::move(editRes.value());
    ASSERT_EQ(sqlite3_bind_blob(edit.raw(), 1, externallyChanged.data(), sizeof(externallyChanged), SQLITE_TRANSIENT), SQLITE_OK);
    edit.bind(2, queryFaces[1]);
    ASSERT_EQ(edit.step(), StepResult::Done);
    refreshAndCompare();

    auto filtered = client.Get("/api/faces/groups?show_named=true&show_unnamed=false&person_tag_id=" +
        std::to_string(people[1]));
    ASSERT_TRUE(filtered);
    ASSERT_EQ(filtered->status, 200) << filtered->body;
    refreshAndCompare();
    server_->setApiToken("");
}

TEST_F(ServerTest, FaceBatchReviewSupportsPartialIdentityAcceptAndDismiss) {
    auto addPhoto = [&](const std::string& suffix) {
        MediaItem media;
        media.file_path = suffix + ".jpg"; media.file_name = suffix + ".jpg";
        media.content_hash = "batch-face-hash-" + suffix; media.width = 100; media.height = 80;
        return catalog_->db().insertMedia(media).value();
    };
    auto addAnalysis = [&](MediaId mediaId) {
        auto stmtRes = catalog_->db().connection().prepare(R"SQL(
            INSERT INTO face_media_analysis(media_id,state,source_hash,width,height,detector_checksum,
                recognizer_checksum,pipeline_version,detector_provider,recognizer_provider,device,
                confidence,nms_threshold,analyzed_at)
            VALUES(?,'complete','batch-source',100,80,'det','rec','batch-v1','cpu','cpu','cpu',0.5,0.4,1);
        )SQL");
        EXPECT_TRUE(stmtRes.isOk());
        if (!stmtRes.isOk()) return;
        auto stmt = std::move(stmtRes.value()); stmt.bind(1, mediaId);
        EXPECT_EQ(stmt.step(), StepResult::Done);
    };
    auto addFace = [&](MediaId mediaId) {
        auto stmtRes = catalog_->db().connection().prepare(R"SQL(
            INSERT INTO faces(media_id,x,y,width,height,score,landmarks,embedding,embedding_size,created_at,updated_at)
            VALUES(?,10,10,20,20,0.9,'[]',?,512,1,1);
        )SQL");
        EXPECT_TRUE(stmtRes.isOk());
        if (!stmtRes.isOk()) return int64_t{0};
        auto stmt = std::move(stmtRes.value()); stmt.bind(1, mediaId);
        std::array<float, 512> vector{}; vector[0] = 1.0f;
        EXPECT_EQ(sqlite3_bind_blob(stmt.raw(), 2, vector.data(), sizeof(vector), SQLITE_TRANSIENT), SQLITE_OK);
        EXPECT_EQ(stmt.step(), StepResult::Done);
        return catalog_->db().connection().lastInsertRowId();
    };
    const MediaId firstMedia = addPhoto("batch-first");
    const MediaId secondMedia = addPhoto("batch-second");
    addAnalysis(firstMedia); addAnalysis(secondMedia);
    const int64_t firstFace = addFace(firstMedia);
    const int64_t secondFace = addFace(secondMedia);

    httplib::Client client("127.0.0.1", port_);
    auto payload = nlohmann::json{{"action", "identity"},
        {"faces", {{{"id", firstFace}, {"revision", 1}}, {{"id", secondFace}, {"revision", 2}}}},
        {"name", "Batch Person"}}.dump();
    server_->setApiToken("face-batch-token");
    auto unauthenticated = client.Post("/api/faces/batch", payload, "application/json");
    ASSERT_TRUE(unauthenticated);
    EXPECT_EQ(unauthenticated->status, 401);
    httplib::Headers auth = {{"Authorization", "Bearer face-batch-token"}};
    auto duplicate = client.Post("/api/faces/batch", auth,
        nlohmann::json{{"action", "dismiss"}, {"dismissed", true},
            {"faces", {{{"id", firstFace}, {"revision", 1}}, {{"id", firstFace}, {"revision", 1}}}}}.dump(),
        "application/json");
    ASSERT_TRUE(duplicate);
    EXPECT_EQ(duplicate->status, 400);

    auto identity = client.Post("/api/faces/batch", auth, payload, "application/json");
    ASSERT_TRUE(identity);
    ASSERT_EQ(identity->status, 200) << identity->body;
    const auto identityJson = nlohmann::json::parse(identity->body);
    ASSERT_EQ(identityJson["updated"].size(), 1u);
    ASSERT_EQ(identityJson["failed"].size(), 1u);
    EXPECT_EQ(identityJson["updated"][0]["person_name"], "Batch Person");
    EXPECT_EQ(identityJson["failed"][0]["id"], secondFace);
    EXPECT_EQ(identityJson["failed"][0]["status"], 409);
    const TagId personId = identityJson["updated"][0]["person_tag_id"].get<TagId>();

    auto accept = client.Post("/api/faces/batch", auth,
        nlohmann::json{{"action", "accept"}, {"tag_id", personId},
            {"faces", {{{"id", secondFace}, {"revision", 1}}}}}.dump(), "application/json");
    ASSERT_TRUE(accept);
    ASSERT_EQ(accept->status, 200) << accept->body;
    const auto acceptJson = nlohmann::json::parse(accept->body);
    ASSERT_EQ(acceptJson["updated"].size(), 1u);
    EXPECT_TRUE(acceptJson["failed"].empty());
    EXPECT_EQ(acceptJson["updated"][0]["person_tag_id"], personId);

    auto dismiss = client.Post("/api/faces/batch", auth,
        nlohmann::json{{"action", "dismiss"}, {"dismissed", true},
            {"faces", {{{"id", firstFace}, {"revision", 2}}, {{"id", secondFace}, {"revision", 2}}}}}.dump(),
        "application/json");
    ASSERT_TRUE(dismiss);
    ASSERT_EQ(dismiss->status, 200) << dismiss->body;
    const auto dismissJson = nlohmann::json::parse(dismiss->body);
    ASSERT_EQ(dismissJson["updated"].size(), 2u);
    EXPECT_TRUE(dismissJson["failed"].empty());
    for (const auto& face : dismissJson["updated"]) EXPECT_TRUE(face["dismissed"].get<bool>());
    server_->setApiToken("");
}

TEST_F(ServerTest, BatchAcceptPreservesSequentialNewExemplarEligibility) {
    auto addPhoto = [&](const std::string& suffix) {
        MediaItem media;
        media.file_path = suffix + ".jpg"; media.file_name = suffix + ".jpg";
        media.content_hash = "sequential-face-hash-" + suffix; media.width = 100; media.height = 80;
        return catalog_->db().insertMedia(media).value();
    };
    auto addAnalysis = [&](MediaId mediaId) {
        auto statementRes = catalog_->db().connection().prepare(R"SQL(
            INSERT INTO face_media_analysis(media_id,state,source_hash,width,height,detector_checksum,
                recognizer_checksum,pipeline_version,detector_provider,recognizer_provider,device,
                confidence,nms_threshold,analyzed_at)
            VALUES(?,'complete','sequential-source',100,80,'det','rec','sequential-v1','cpu','cpu','cpu',0.5,0.4,1);
        )SQL");
        EXPECT_TRUE(statementRes.isOk());
        if (!statementRes.isOk()) return;
        auto statement = std::move(statementRes.value()); statement.bind(1, mediaId);
        EXPECT_EQ(statement.step(), StepResult::Done);
    };
    auto addFace = [&](MediaId mediaId, TagId personId, const std::array<float, 512>& embedding) {
        auto statementRes = catalog_->db().connection().prepare(R"SQL(
            INSERT INTO faces(media_id,x,y,width,height,score,landmarks,person_tag_id,embedding,
                embedding_size,created_at,updated_at)
            VALUES(?,10,10,20,20,0.9,'[]',?,?,512,1,1);
        )SQL");
        EXPECT_TRUE(statementRes.isOk());
        if (!statementRes.isOk()) return int64_t{0};
        auto statement = std::move(statementRes.value());
        statement.bind(1, mediaId);
        if (personId > 0) statement.bind(2, personId); else statement.bindNull(2);
        EXPECT_EQ(sqlite3_bind_blob(statement.raw(), 3, embedding.data(), sizeof(embedding), SQLITE_TRANSIENT), SQLITE_OK);
        EXPECT_EQ(statement.step(), StepResult::Done);
        return catalog_->db().connection().lastInsertRowId();
    };

    const TagId personId = catalog_->db().createOrGetTag("Sequential Person", "people").value();
    std::array<float, 512> exemplar{};
    exemplar[0] = 1.0f;
    const MediaId exemplarMedia = addPhoto("exemplar");
    addAnalysis(exemplarMedia);
    addFace(exemplarMedia, personId, exemplar);

    std::array<float, 512> firstEmbedding{};
    firstEmbedding[0] = static_cast<float>(std::cos(50.0 * 3.14159265358979323846 / 180.0));
    firstEmbedding[1] = static_cast<float>(std::sin(50.0 * 3.14159265358979323846 / 180.0));
    const MediaId firstMedia = addPhoto("first");
    addAnalysis(firstMedia);
    const int64_t firstFace = addFace(firstMedia, 0, firstEmbedding);

    std::array<float, 512> staleEmbedding{};
    staleEmbedding[0] = static_cast<float>(std::cos(60.0 * 3.14159265358979323846 / 180.0));
    staleEmbedding[1] = static_cast<float>(std::sin(60.0 * 3.14159265358979323846 / 180.0));
    const MediaId staleMedia = addPhoto("stale");
    addAnalysis(staleMedia);
    const int64_t staleFace = addFace(staleMedia, 0, staleEmbedding);

    std::array<float, 512> rejectedEmbedding{};
    rejectedEmbedding[0] = static_cast<float>(std::cos(70.0 * 3.14159265358979323846 / 180.0));
    rejectedEmbedding[1] = static_cast<float>(std::sin(70.0 * 3.14159265358979323846 / 180.0));
    const MediaId rejectedMedia = addPhoto("rejected");
    addAnalysis(rejectedMedia);
    const int64_t rejectedFace = addFace(rejectedMedia, 0, rejectedEmbedding);
    {
        auto rejectionRes = catalog_->db().connection().prepare(
            "INSERT INTO face_rejections(face_id,tag_id,rejected_at) VALUES(?,?,1);");
        ASSERT_TRUE(rejectionRes.isOk());
        auto rejection = std::move(rejectionRes.value());
        rejection.bind(1, rejectedFace);
        rejection.bind(2, personId);
        ASSERT_EQ(rejection.step(), StepResult::Done);
    }

    std::array<float, 512> secondEmbedding{};
    secondEmbedding[0] = static_cast<float>(std::cos(80.0 * 3.14159265358979323846 / 180.0));
    secondEmbedding[1] = static_cast<float>(std::sin(80.0 * 3.14159265358979323846 / 180.0));
    const MediaId secondMedia = addPhoto("second");
    addAnalysis(secondMedia);
    const int64_t secondFace = addFace(secondMedia, 0, secondEmbedding);

    httplib::Client client("127.0.0.1", port_);
    auto beforeSecond = client.Get("/api/faces/media/" + std::to_string(secondMedia));
    ASSERT_TRUE(beforeSecond);
    ASSERT_EQ(beforeSecond->status, 200);
    EXPECT_TRUE(nlohmann::json::parse(beforeSecond->body)["faces"][0]["suggestions"].empty());

    server_->setApiToken("face-sequential-token");
    httplib::Headers auth = {{"Authorization", "Bearer face-sequential-token"}};
    auto accepted = client.Post("/api/faces/batch", auth,
        nlohmann::json{{"action", "accept"}, {"tag_id", personId},
            {"faces", {{{"id", firstFace}, {"revision", 1}},
                       {{"id", staleFace}, {"revision", 2}},
                       {{"id", rejectedFace}, {"revision", 1}},
                       {{"id", secondFace}, {"revision", 1}}}}}.dump(),
        "application/json");
    ASSERT_TRUE(accepted);
    ASSERT_EQ(accepted->status, 200) << accepted->body;
    const auto result = nlohmann::json::parse(accepted->body);
    ASSERT_EQ(result["updated"].size(), 2u);
    ASSERT_EQ(result["failed"].size(), 2u);
    EXPECT_EQ(result["failed"][0]["id"], staleFace);
    EXPECT_EQ(result["failed"][0]["status"], 409);
    EXPECT_EQ(result["failed"][1]["id"], rejectedFace);
    EXPECT_EQ(result["failed"][1]["status"], 409);
    EXPECT_EQ(result["updated"][0]["person_tag_id"], personId);
    EXPECT_EQ(result["updated"][1]["person_tag_id"], personId);
    EXPECT_TRUE(result["updated"][0]["suggestions"].empty());
    EXPECT_TRUE(result["updated"][1]["suggestions"].empty());
    auto verifyUnchanged = [&](int64_t faceId, int64_t expectedRevision) {
        auto verifyRes = catalog_->db().connection().prepare(
            "SELECT person_tag_id,revision FROM faces WHERE id=?;");
        ASSERT_TRUE(verifyRes.isOk());
        auto verify = std::move(verifyRes.value());
        verify.bind(1, faceId);
        ASSERT_EQ(verify.step(), StepResult::Row);
        EXPECT_TRUE(verify.isNull(0));
        EXPECT_EQ(verify.getInt64(1), expectedRevision);
    };
    verifyUnchanged(staleFace, 1);
    verifyUnchanged(rejectedFace, 1);
    server_->setApiToken("");
}

TEST_F(ServerTest, FaceSuggestionsRequireMatchingPipelineAndHealthyEmbeddings) {
    auto addPhoto = [&](const std::string& suffix) {
        MediaItem media;
        media.file_path = suffix + ".jpg"; media.file_name = suffix + ".jpg";
        media.content_hash = "pipeline-hash-" + suffix;
        media.width = 100; media.height = 80;
        return catalog_->db().insertMedia(media).value();
    };
    auto addAnalysis = [&](MediaId mediaId, const std::string& pipeline, const std::string& state) {
        auto& connection = catalog_->db().connection();
        auto stmtRes = connection.prepare(R"SQL(
            INSERT INTO face_media_analysis(media_id,state,source_hash,width,height,detector_checksum,
                recognizer_checksum,pipeline_version,detector_provider,recognizer_provider,device,
                confidence,nms_threshold,analyzed_at)
            VALUES(?,?, 'pipeline-test',100,80,'det','rec',?,'cpu','cpu','cpu',0.5,0.4,1);
        )SQL");
        EXPECT_TRUE(stmtRes.isOk());
        if (!stmtRes.isOk()) return;
        auto stmt = std::move(stmtRes.value());
        stmt.bind(1, mediaId); stmt.bind(2, state); stmt.bind(3, pipeline);
        EXPECT_EQ(stmt.step(), StepResult::Done);
    };
    auto addFace = [&](MediaId mediaId, std::optional<TagId> personId, const std::string& embeddingError,
                       size_t activeEmbeddingIndex = 0) {
        auto& connection = catalog_->db().connection();
        auto stmtRes = connection.prepare(R"SQL(
            INSERT INTO faces(media_id,x,y,width,height,score,landmarks,person_tag_id,embedding,
                embedding_size,embedding_error,created_at,updated_at)
            VALUES(?,10,10,20,20,0.9,'[]',?,?,512,?,1,1);
        )SQL");
        EXPECT_TRUE(stmtRes.isOk());
        if (!stmtRes.isOk()) return int64_t{0};
        auto stmt = std::move(stmtRes.value());
        stmt.bind(1, mediaId);
        if (personId) stmt.bind(2, *personId); else stmt.bindNull(2);
        std::array<float, 512> embedding{};
        embedding[activeEmbeddingIndex] = 1.0f;
        EXPECT_EQ(sqlite3_bind_blob(stmt.raw(), 3, embedding.data(), sizeof(embedding), SQLITE_TRANSIENT), SQLITE_OK);
        stmt.bind(4, embeddingError);
        EXPECT_EQ(stmt.step(), StepResult::Done);
        return connection.lastInsertRowId();
    };

    const TagId incompatibleId = catalog_->db().createOrGetTag("Old Pipeline", "people").value();
    const TagId compatibleId = catalog_->db().createOrGetTag("Current Pipeline", "people").value();
    const TagId brokenEmbeddingId = catalog_->db().createOrGetTag("Broken Embedding", "people").value();
    const TagId failedAnalysisId = catalog_->db().createOrGetTag("Failed Analysis", "people").value();
    const TagId rejectedId = catalog_->db().createOrGetTag("Rejected Candidate", "people").value();
    const TagId selfOnlyId = catalog_->db().createOrGetTag("Self Only", "people").value();

    const MediaId candidateMedia = addPhoto("candidate");
    addAnalysis(candidateMedia, "pipeline-current", "complete");
    const int64_t candidateFace = addFace(candidateMedia, std::nullopt, "");
    const MediaId incompatibleMedia = addPhoto("incompatible");
    addAnalysis(incompatibleMedia, "pipeline-old", "complete");
    addFace(incompatibleMedia, incompatibleId, "");
    const MediaId compatibleMedia = addPhoto("compatible");
    addAnalysis(compatibleMedia, "pipeline-current", "complete");
    addFace(compatibleMedia, compatibleId, "");
    const MediaId rejectedMedia = addPhoto("rejected");
    addAnalysis(rejectedMedia, "pipeline-current", "complete");
    addFace(rejectedMedia, rejectedId, "");
    const MediaId selfOnlyMedia = addPhoto("self-only");
    addAnalysis(selfOnlyMedia, "pipeline-current", "complete");
    const int64_t selfOnlyFace = addFace(selfOnlyMedia, selfOnlyId, "", 1);
    const MediaId brokenMedia = addPhoto("broken");
    addAnalysis(brokenMedia, "pipeline-current", "complete");
    addFace(brokenMedia, brokenEmbeddingId, "embedding refresh failed");
    const MediaId failedMedia = addPhoto("failed");
    addAnalysis(failedMedia, "pipeline-current", "failed");
    addFace(failedMedia, failedAnalysisId, "");

    auto rejectionRes = catalog_->db().connection().prepare(
        "INSERT INTO face_rejections(face_id,tag_id,rejected_at) VALUES(?,?,1);");
    ASSERT_TRUE(rejectionRes.isOk());
    auto rejection = std::move(rejectionRes.value());
    rejection.bind(1, candidateFace);
    rejection.bind(2, rejectedId);
    ASSERT_EQ(rejection.step(), StepResult::Done);

    httplib::Client client("127.0.0.1", port_);
    auto response = client.Get("/api/faces/media/" + std::to_string(candidateMedia));
    ASSERT_TRUE(response);
    ASSERT_EQ(response->status, 200) << response->body;
    const auto suggestions = nlohmann::json::parse(response->body)["faces"][0]["suggestions"];
    ASSERT_EQ(suggestions.size(), 1u);
    EXPECT_EQ(suggestions[0]["tag_id"], compatibleId);
    EXPECT_DOUBLE_EQ(suggestions[0]["score"].get<double>(), 1.0);

    auto gridResponse = client.Get("/api/faces/grid?include_dismissed=true&limit=100");
    ASSERT_TRUE(gridResponse);
    ASSERT_EQ(gridResponse->status, 200) << gridResponse->body;
    const auto grid = nlohmann::json::parse(gridResponse->body);
    auto gridFace = [&](int64_t id) -> nlohmann::json {
        for (const auto& face : grid["items"]) {
            if (face["id"].get<int64_t>() == id) return face;
        }
        return nullptr;
    };
    const auto gridCandidate = gridFace(candidateFace);
    ASSERT_FALSE(gridCandidate.is_null());
    ASSERT_EQ(gridCandidate["suggestions"].size(), 1u);
    EXPECT_EQ(gridCandidate["suggestions"][0]["tag_id"], compatibleId);
    EXPECT_DOUBLE_EQ(gridCandidate["suggestions"][0]["score"].get<double>(), 1.0);
    const auto gridSelfOnly = gridFace(selfOnlyFace);
    ASSERT_FALSE(gridSelfOnly.is_null());
    for (const auto& suggestion : gridSelfOnly["suggestions"]) {
        EXPECT_NE(suggestion["tag_id"], selfOnlyId);
    }
    auto selfOnlyMediaResponse = client.Get("/api/faces/media/" + std::to_string(selfOnlyMedia));
    ASSERT_TRUE(selfOnlyMediaResponse);
    ASSERT_EQ(selfOnlyMediaResponse->status, 200);
    const auto selfOnlySuggestions = nlohmann::json::parse(selfOnlyMediaResponse->body)["faces"][0]["suggestions"];
    EXPECT_TRUE(selfOnlySuggestions.empty());

    auto updateTargetError = catalog_->db().connection().prepare("UPDATE faces SET embedding_error='no embedding' WHERE id=?;");
    ASSERT_TRUE(updateTargetError.isOk());
    auto targetError = std::move(updateTargetError.value()); targetError.bind(1, candidateFace);
    ASSERT_EQ(targetError.step(), StepResult::Done);
    auto errorResponse = client.Get("/api/faces/media/" + std::to_string(candidateMedia));
    ASSERT_TRUE(errorResponse);
    ASSERT_EQ(errorResponse->status, 200);
    EXPECT_TRUE(nlohmann::json::parse(errorResponse->body)["faces"][0]["suggestions"].empty());
    auto restoreTarget = catalog_->db().connection().prepare("UPDATE faces SET embedding_error='' WHERE id=?;");
    ASSERT_TRUE(restoreTarget.isOk());
    auto restore = std::move(restoreTarget.value()); restore.bind(1, candidateFace);
    ASSERT_EQ(restore.step(), StepResult::Done);
    auto failAnalysis = catalog_->db().connection().prepare("UPDATE face_media_analysis SET state='failed' WHERE media_id=?;");
    ASSERT_TRUE(failAnalysis.isOk());
    auto fail = std::move(failAnalysis.value()); fail.bind(1, candidateMedia);
    ASSERT_EQ(fail.step(), StepResult::Done);
    auto failedTargetResponse = client.Get("/api/faces/media/" + std::to_string(candidateMedia));
    ASSERT_TRUE(failedTargetResponse);
    ASSERT_EQ(failedTargetResponse->status, 200);
    EXPECT_TRUE(nlohmann::json::parse(failedTargetResponse->body)["faces"][0]["suggestions"].empty());
}

TEST(FaceServiceTest, EmptyDetectionIsSuccessfulAndInferenceFailureIsDistinct) {
    const auto root = std::filesystem::temp_directory_path() /
        ("imagine_faces_service_test_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    CatalogDb db;
    ASSERT_TRUE(db.open(":memory:").isOk());
    thumbnail::ImageBuffer image;
    image.width = 16; image.height = 12; image.channels = 3;
    image.data.assign(static_cast<size_t>(image.width * image.height * 3), 127);
    const std::string firstPath = (root / "empty.jpg").string();
    const std::string secondPath = (root / "failure.jpg").string();
    ASSERT_TRUE(thumbnail::Generator::saveJpeg(image, firstPath).isOk());
    ASSERT_TRUE(thumbnail::Generator::saveJpeg(image, secondPath).isOk());
    auto addMedia = [&](const std::string& path) {
        MediaItem item;
        item.file_path = path; item.file_name = std::filesystem::path(path).filename().string();
        item.content_hash = metadata::Hasher::computeFileSha256(path).value();
        item.width = image.width; item.height = image.height; item.media_type = "photo";
        return db.insertMedia(item).value();
    };
    MediaId emptyId = addMedia(firstPath);
    MediaId failedId = addMedia(secondPath);
    std::atomic<int> call{0};
    faces::Service service(db, (root / "cache").string(), [](const std::string& path) { return path; },
        [&call](const thumbnail::ImageBuffer&) -> Result<std::vector<faces::Detection>> {
            if (call.fetch_add(1) == 0) return std::vector<faces::Detection>{};
            return Status::parseError("deliberate inference failure");
        });
    int64_t jobId = 0;
    ASSERT_TRUE(service.startJob("selected", {emptyId, failedId}, false, jobId).isOk());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    Result<nlohmann::json> job = service.getJob(jobId);
    while (std::chrono::steady_clock::now() < deadline) {
        job = service.getJob(jobId);
        if (!job.isOk() || (job.value()["state"] != "running" && job.value()["state"] != "cancelling")) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(job.isOk());
    EXPECT_EQ(job.value()["state"], "completed");
    EXPECT_EQ(job.value()["processed"], 1);
    EXPECT_EQ(job.value()["failed"], 1);
    EXPECT_EQ(job.value()["remaining"], 0);
    auto membershipRes = db.connection().prepare(
        "SELECT COUNT(*) FROM face_analysis_job_media WHERE job_id=?;");
    ASSERT_TRUE(membershipRes.isOk());
    auto membership = std::move(membershipRes.value()); membership.bind(1, jobId);
    ASSERT_EQ(membership.step(), StepResult::Row);
    EXPECT_EQ(membership.getInt64(0), 2);
    auto empty = service.getMedia(emptyId);
    ASSERT_TRUE(empty.isOk());
    EXPECT_EQ(empty.value()["state"], "complete");
    EXPECT_TRUE(empty.value()["faces"].empty());
    auto persistedRuntimeRes = db.connection().prepare(
        "SELECT runtime_version,fallback_reason FROM face_media_analysis WHERE media_id=?;");
    ASSERT_TRUE(persistedRuntimeRes.isOk());
    auto persistedRuntime = std::move(persistedRuntimeRes.value()); persistedRuntime.bind(1, emptyId);
    ASSERT_EQ(persistedRuntime.step(), StepResult::Row);
    EXPECT_EQ(persistedRuntime.getString(0), "test-runtime");
    EXPECT_EQ(persistedRuntime.getString(1), "test-fallback");
    auto failed = service.getMedia(failedId);
    ASSERT_TRUE(failed.isOk());
    EXPECT_EQ(failed.value()["state"], "failed");
    EXPECT_NE(failed.value()["error"], "");
    EXPECT_TRUE(failed.value()["faces"].empty());

    thumbnail::ImageBuffer changedImage = image;
    std::fill(changedImage.data.begin(), changedImage.data.end(), 0);
    ASSERT_TRUE(thumbnail::Generator::saveJpeg(changedImage, firstPath).isOk());
    int64_t changedSourceJobId = 0;
    ASSERT_TRUE(service.startJob("selected", {emptyId}, false, changedSourceJobId).isOk());
    const auto changedSourceDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    Result<nlohmann::json> changedSourceJob = service.getJob(changedSourceJobId);
    while (std::chrono::steady_clock::now() < changedSourceDeadline) {
        changedSourceJob = service.getJob(changedSourceJobId);
        if (!changedSourceJob.isOk() || (changedSourceJob.value()["state"] != "running" && changedSourceJob.value()["state"] != "cancelling")) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(changedSourceJob.isOk());
    EXPECT_EQ(changedSourceJob.value()["failed"], 1);
    EXPECT_EQ(changedSourceJob.value()["skipped"], 0);
    EXPECT_EQ(call.load(), 2) << "A changed original must be checked before reusing stored analysis";
    auto changedSource = service.getMedia(emptyId);
    ASSERT_TRUE(changedSource.isOk());
    // The failed attempt is reported by the job; the committed empty result survives.
    EXPECT_EQ(changedSource.value()["state"], "complete");
    EXPECT_NE(changedSourceJob.value()["error"], "");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(FaceServiceTest, CatalogGeometryChangeDuringInferenceDoesNotCommitStaleFaces) {
    const auto root = std::filesystem::temp_directory_path() /
        ("imagine_faces_geometry_race_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    CatalogDb db;
    ASSERT_TRUE(db.open(":memory:").isOk());
    thumbnail::ImageBuffer image;
    image.width = 16; image.height = 12; image.channels = 3;
    image.data.assign(static_cast<size_t>(image.width * image.height * 3), 127);
    const std::string photoPath = (root / "photo.jpg").string();
    ASSERT_TRUE(thumbnail::Generator::saveJpeg(image, photoPath).isOk());
    MediaItem media;
    media.file_path = photoPath;
    media.file_name = "photo.jpg";
    media.content_hash = metadata::Hasher::computeFileSha256(photoPath).value();
    media.width = image.width; media.height = image.height; media.media_type = "photo";
    const MediaId mediaId = db.insertMedia(media).value();

    faces::Service service(db, (root / "cache").string(), [](const std::string& path) { return path; },
        [&db, mediaId](const thumbnail::ImageBuffer&) -> Result<std::vector<faces::Detection>> {
            auto changed = db.getMediaById(mediaId);
            if (!changed.isOk()) return changed.status();
            changed.value().width += 1;
            Status updated = db.updateMedia(changed.value());
            if (!updated.isOk()) return updated;
            faces::Detection detection;
            detection.x = 1; detection.y = 2; detection.width = 3; detection.height = 4;
            detection.score = 0.9f;
            return std::vector<faces::Detection>{detection};
        });
    int64_t jobId = 0;
    ASSERT_TRUE(service.startJob("selected", {mediaId}, false, jobId).isOk());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    Result<nlohmann::json> job = service.getJob(jobId);
    while (std::chrono::steady_clock::now() < deadline) {
        job = service.getJob(jobId);
        if (!job.isOk() || (job.value()["state"] != "running" && job.value()["state"] != "cancelling")) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(job.isOk());
    EXPECT_EQ(job.value()["state"], "completed");
    EXPECT_EQ(job.value()["processed"], 0);
    EXPECT_EQ(job.value()["skipped"], 1);
    EXPECT_EQ(job.value()["remaining"], 0);
    auto result = service.getMedia(mediaId);
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(result.value()["state"], "unscanned");
    EXPECT_TRUE(result.value()["faces"].empty());
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(FaceServiceTest, CancellationStopsAtPhotoBoundary) {
    const auto root = std::filesystem::temp_directory_path() /
        ("imagine_faces_cancel_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    CatalogDb db;
    ASSERT_TRUE(db.open(":memory:").isOk());
    thumbnail::ImageBuffer image;
    image.width = 16; image.height = 12; image.channels = 3;
    image.data.assign(static_cast<size_t>(image.width * image.height * 3), 127);
    const std::string firstPath = (root / "first.jpg").string();
    const std::string secondPath = (root / "second.jpg").string();
    ASSERT_TRUE(thumbnail::Generator::saveJpeg(image, firstPath).isOk());
    ASSERT_TRUE(thumbnail::Generator::saveJpeg(image, secondPath).isOk());
    auto addMedia = [&](const std::string& path) {
        MediaItem item;
        item.file_path = path; item.file_name = std::filesystem::path(path).filename().string();
        item.content_hash = metadata::Hasher::computeFileSha256(path).value();
        item.width = image.width; item.height = image.height; item.media_type = "photo";
        return db.insertMedia(item).value();
    };
    const MediaId firstId = addMedia(firstPath);
    const MediaId secondId = addMedia(secondPath);

    std::promise<void> analyzerEnteredPromise;
    auto analyzerEntered = analyzerEnteredPromise.get_future();
    std::promise<void> releaseAnalyzerPromise;
    auto releaseAnalyzer = releaseAnalyzerPromise.get_future().share();
    std::atomic<int> analyzerCalls{0};
    faces::Service service(db, (root / "cache").string(), [](const std::string& path) { return path; },
        [&analyzerEnteredPromise, releaseAnalyzer, &analyzerCalls](const thumbnail::ImageBuffer&)
            -> Result<std::vector<faces::Detection>> {
            if (analyzerCalls.fetch_add(1) == 0) {
                analyzerEnteredPromise.set_value();
                releaseAnalyzer.wait();
            }
            return std::vector<faces::Detection>{};
        });
    int64_t jobId = 0;
    ASSERT_TRUE(service.startJob("selected", {firstId, secondId}, false, jobId).isOk());
    const bool entered = analyzerEntered.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    if (!entered) {
        releaseAnalyzerPromise.set_value();
        FAIL() << "Fake inference did not reach its synchronization point";
    }
    Status cancellation = service.cancelJob(jobId);
    releaseAnalyzerPromise.set_value();
    ASSERT_TRUE(cancellation.isOk());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    Result<nlohmann::json> job = service.getJob(jobId);
    while (std::chrono::steady_clock::now() < deadline) {
        job = service.getJob(jobId);
        if (!job.isOk() || (job.value()["state"] != "running" && job.value()["state"] != "cancelling")) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(job.isOk());
    EXPECT_EQ(job.value()["state"], "cancelled");
    EXPECT_EQ(job.value()["processed"], 1);
    EXPECT_EQ(job.value()["remaining"], 1);
    EXPECT_EQ(analyzerCalls.load(), 1);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(FaceServiceTest, StartupMarksPersistedActiveJobInterrupted) {
    const auto root = std::filesystem::temp_directory_path() /
        ("imagine_faces_restart_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    CatalogDb db;
    ASSERT_TRUE(db.open(":memory:").isOk());
    auto insertRes = db.connection().prepare(
        "INSERT INTO face_analysis_jobs(state,scope,total,remaining,created_at,updated_at) "
        "VALUES('running','catalog',3,3,1,1);");
    ASSERT_TRUE(insertRes.isOk());
    auto insert = std::move(insertRes.value());
    ASSERT_EQ(insert.step(), StepResult::Done);
    const int64_t interruptedId = db.connection().lastInsertRowId();

    thumbnail::ImageBuffer image;
    image.width = 16; image.height = 12; image.channels = 3;
    image.data.assign(static_cast<size_t>(image.width * image.height * 3), 127);
    const std::string path = (root / "photo.jpg").string();
    ASSERT_TRUE(thumbnail::Generator::saveJpeg(image, path).isOk());
    MediaItem media;
    media.file_path = path; media.file_name = "photo.jpg";
    media.content_hash = metadata::Hasher::computeFileSha256(path).value();
    media.width = image.width; media.height = image.height; media.media_type = "photo";
    const MediaId mediaId = db.insertMedia(media).value();

    faces::Service service(db, (root / "cache").string(), [](const std::string& value) { return value; },
        [](const thumbnail::ImageBuffer&) -> Result<std::vector<faces::Detection>> {
            return std::vector<faces::Detection>{};
        });
    auto interrupted = service.getJob(interruptedId);
    ASSERT_TRUE(interrupted.isOk());
    EXPECT_EQ(interrupted.value()["state"], "interrupted");
    EXPECT_EQ(interrupted.value()["remaining"], 3);
    EXPECT_NE(interrupted.value()["error"], "");

    int64_t newJobId = 0;
    ASSERT_TRUE(service.startJob("selected", {mediaId}, false, newJobId).isOk());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    Result<nlohmann::json> job = service.getJob(newJobId);
    while (std::chrono::steady_clock::now() < deadline) {
        job = service.getJob(newJobId);
        if (!job.isOk() || (job.value()["state"] != "running" && job.value()["state"] != "cancelling")) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(job.isOk());
    EXPECT_EQ(job.value()["state"], "completed");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(FaceServiceTest, LatestJobSupersedesOlderInterruptionButKeepsRetryableFailure) {
    CatalogDb db;
    ASSERT_TRUE(db.open(":memory:").isOk());
    auto addJob = [&](const std::string& state, int failed) {
        auto insertRes = db.connection().prepare(
            "INSERT INTO face_analysis_jobs(state,scope,total,failed,remaining,created_at,updated_at) "
            "VALUES(?,'catalog',1,?,0,1,1);");
        EXPECT_TRUE(insertRes.isOk());
        if (!insertRes.isOk()) return int64_t{0};
        auto insert = std::move(insertRes.value());
        insert.bind(1, state); insert.bind(2, failed);
        EXPECT_EQ(insert.step(), StepResult::Done);
        return db.connection().lastInsertRowId();
    };
    const int64_t interruptedId = addJob("running", 0);
    faces::Service service(db, "", [](const std::string& value) { return value; },
        [](const thumbnail::ImageBuffer&) -> Result<std::vector<faces::Detection>> {
            return std::vector<faces::Detection>{};
        });
    auto interruptedStatus = service.status();
    ASSERT_EQ(interruptedStatus["job"]["id"], interruptedId);
    EXPECT_EQ(interruptedStatus["job"]["state"], "interrupted");

    const int64_t newerSuccessId = addJob("completed", 0);
    EXPECT_TRUE(service.status()["job"].is_null());

    const int64_t newerFailureId = addJob("completed", 1);
    const auto failedStatus = service.status();
    ASSERT_EQ(failedStatus["job"]["id"], newerFailureId);
    EXPECT_NE(failedStatus["job"]["id"], newerSuccessId);
    EXPECT_EQ(failedStatus["job"]["failed"], 1);
}

TEST(FaceServiceTest, MatchThresholdAcceptsFiniteCosineRangeOnly) {
    CatalogDb db;
    ASSERT_TRUE(db.open(":memory:").isOk());
    const char* priorEnv = std::getenv("IMAGINE_FACE_MATCH_THRESHOLD");
    const bool hadPriorEnv = priorEnv != nullptr;
    const std::string priorValue = priorEnv ? priorEnv : "";
    auto setThreshold = [](const char* value) {
#if defined(_WIN32)
        _putenv_s("IMAGINE_FACE_MATCH_THRESHOLD", value ? value : "");
#else
        if (value) setenv("IMAGINE_FACE_MATCH_THRESHOLD", value, 1);
        else unsetenv("IMAGINE_FACE_MATCH_THRESHOLD");
#endif
    };
    auto validate = [&](const std::string& value, bool expectedReady) {
        setThreshold(value.c_str());
        faces::Service service(db, "", [](const std::string& path) { return path; },
            [](const thumbnail::ImageBuffer&) -> Result<std::vector<faces::Detection>> {
                return std::vector<faces::Detection>{};
            });
        const auto status = service.status();
        EXPECT_EQ(status["ready"].get<bool>(), expectedReady) << "threshold=" << value;
        if (expectedReady) {
            EXPECT_FLOAT_EQ(status["config"]["match_threshold"].get<float>(), std::stof(value));
        } else {
            EXPECT_NE(status["error"].get<std::string>().find("finite and between -1 and 1"), std::string::npos);
        }
    };

    for (const std::string value : {"-1", "-0.1", "0", "1"}) validate(value, true);
    for (const std::string value : {"-1.01", "1.01", "nan"}) validate(value, false);
    setThreshold(hadPriorEnv ? priorValue.c_str() : nullptr);
}

TEST(FaceServiceTest, FailedChangedFileAttemptPreservesSuccessWhenOriginalReturns) {
    const auto root = std::filesystem::temp_directory_path() /
        ("imagine_faces_restore_source_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    CatalogDb db;
    ASSERT_TRUE(db.open(":memory:").isOk());
    thumbnail::ImageBuffer image;
    image.width = 16; image.height = 12; image.channels = 3;
    image.data.assign(static_cast<size_t>(image.width * image.height * 3), 127);
    const std::string path = (root / "photo.jpg").string();
    const std::string originalPath = (root / "photo.original.jpg").string();
    ASSERT_TRUE(thumbnail::Generator::saveJpeg(image, path).isOk());
    std::filesystem::copy_file(path, originalPath);
    MediaItem media;
    media.file_path = path; media.file_name = "photo.jpg";
    media.content_hash = metadata::Hasher::computeFileSha256(path).value();
    media.width = image.width; media.height = image.height; media.media_type = "photo";
    const MediaId mediaId = db.insertMedia(media).value();
    std::atomic<int> analyzerCalls{0};
    faces::Service service(db, (root / "cache").string(), [](const std::string& value) { return value; },
        [&analyzerCalls](const thumbnail::ImageBuffer&) -> Result<std::vector<faces::Detection>> {
            ++analyzerCalls;
            faces::Detection detection;
            detection.x = 1; detection.y = 2; detection.width = 6; detection.height = 7;
            detection.score = 0.9f;
            detection.embedding.assign(512, 0.0f);
            detection.embedding[0] = 1.0f;
            return std::vector<faces::Detection>{detection};
        });
    auto waitForJob = [&](int64_t jobId) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        Result<nlohmann::json> job = service.getJob(jobId);
        while (std::chrono::steady_clock::now() < deadline) {
            job = service.getJob(jobId);
            if (!job.isOk() || (job.value()["state"] != "running" && job.value()["state"] != "cancelling")) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return job;
    };

    int64_t initialJobId = 0;
    ASSERT_TRUE(service.startJob("selected", {mediaId}, false, initialJobId).isOk());
    auto initialJob = waitForJob(initialJobId);
    ASSERT_TRUE(initialJob.isOk());
    ASSERT_EQ(initialJob.value()["processed"], 1);
    auto initialFaces = service.getMedia(mediaId);
    ASSERT_TRUE(initialFaces.isOk());
    ASSERT_EQ(initialFaces.value()["faces"].size(), 1u);
    const int64_t faceId = initialFaces.value()["faces"][0]["id"].get<int64_t>();
    const TagId personId = db.createOrGetTag("Restored Person", "people").value();
    ASSERT_TRUE(service.setIdentity(faceId, 1, personId, std::nullopt).isOk());

    {
        std::ofstream changed(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(changed.is_open());
        changed << "temporary changed source";
        ASSERT_TRUE(changed.good());
    }
    int64_t failedJobId = 0;
    ASSERT_TRUE(service.startJob("selected", {mediaId}, false, failedJobId).isOk());
    auto failedJob = waitForJob(failedJobId);
    ASSERT_TRUE(failedJob.isOk());
    EXPECT_EQ(failedJob.value()["state"], "completed");
    EXPECT_EQ(failedJob.value()["failed"], 1);
    ASSERT_EQ(service.status()["job"]["id"], failedJobId);

    auto retainedAnalysisRes = db.connection().prepare(
        "SELECT state,source_hash,recognizer_checksum FROM face_media_analysis WHERE media_id=?;");
    ASSERT_TRUE(retainedAnalysisRes.isOk());
    auto retainedAnalysis = std::move(retainedAnalysisRes.value()); retainedAnalysis.bind(1, mediaId);
    ASSERT_EQ(retainedAnalysis.step(), StepResult::Row);
    EXPECT_EQ(retainedAnalysis.getString(0), "complete");
    EXPECT_EQ(retainedAnalysis.getString(1), media.content_hash);
    EXPECT_EQ(retainedAnalysis.getString(2), "test-recognizer");

    std::filesystem::copy_file(originalPath, path, std::filesystem::copy_options::overwrite_existing);
    int64_t restoredJobId = 0;
    ASSERT_TRUE(service.startJob("selected", {mediaId}, false, restoredJobId).isOk());
    auto restoredJob = waitForJob(restoredJobId);
    ASSERT_TRUE(restoredJob.isOk());
    EXPECT_EQ(restoredJob.value()["state"], "completed");
    EXPECT_EQ(restoredJob.value()["skipped"], 1);
    EXPECT_EQ(restoredJob.value()["failed"], 0);
    EXPECT_TRUE(service.status()["job"].is_null());
    EXPECT_EQ(analyzerCalls.load(), 1);
    auto restoredFaces = service.getMedia(mediaId);
    ASSERT_TRUE(restoredFaces.isOk());
    ASSERT_EQ(restoredFaces.value()["faces"].size(), 1u);
    EXPECT_EQ(restoredFaces.value()["faces"][0]["id"], faceId);
    EXPECT_EQ(restoredFaces.value()["faces"][0]["person_tag_id"], personId);
    EXPECT_EQ(restoredFaces.value()["faces"][0]["revision"], 2);
    auto retainedMediaTags = db.getTagsForMedia(mediaId);
    ASSERT_TRUE(retainedMediaTags.isOk());
    ASSERT_EQ(retainedMediaTags.value().size(), 1u);
    EXPECT_EQ(retainedMediaTags.value()[0].id, personId);
    auto provenanceRes = db.connection().prepare(
        "SELECT manual FROM media_tag_provenance WHERE media_id=? AND tag_id=?;");
    ASSERT_TRUE(provenanceRes.isOk());
    auto provenance = std::move(provenanceRes.value());
    provenance.bind(1, mediaId); provenance.bind(2, personId);
    ASSERT_EQ(provenance.step(), StepResult::Row);
    EXPECT_EQ(provenance.getInt(0), 0);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(FaceServiceTest, FailedAttemptPreservesCompletedSnapshotEvenWhenStoredHashDiffers) {
    const auto root = std::filesystem::temp_directory_path() /
        ("imagine_faces_keep_hash_mismatch_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    CatalogDb db;
    ASSERT_TRUE(db.open(":memory:").isOk());
    thumbnail::ImageBuffer image;
    image.width = 16; image.height = 12; image.channels = 3;
    image.data.assign(static_cast<size_t>(image.width * image.height * 3), 127);
    const std::string path = (root / "photo.jpg").string();
    const std::string originalPath = (root / "photo.original.jpg").string();
    ASSERT_TRUE(thumbnail::Generator::saveJpeg(image, path).isOk());
    std::filesystem::copy_file(path, originalPath);
    MediaItem media;
    media.file_path = path; media.file_name = "photo.jpg";
    media.content_hash = metadata::Hasher::computeFileSha256(path).value();
    media.width = image.width; media.height = image.height; media.media_type = "photo";
    const MediaId mediaId = db.insertMedia(media).value();
    faces::Service service(db, (root / "cache").string(), [](const std::string& value) { return value; },
        [](const thumbnail::ImageBuffer&) -> Result<std::vector<faces::Detection>> {
            faces::Detection detection;
            detection.x = 1; detection.y = 2; detection.width = 6; detection.height = 7;
            detection.score = 0.9f;
            detection.embedding.assign(512, 0.0f);
            detection.embedding[0] = 1.0f;
            return std::vector<faces::Detection>{detection};
        });
    auto waitForJob = [&](int64_t jobId) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        Result<nlohmann::json> job = service.getJob(jobId);
        while (std::chrono::steady_clock::now() < deadline) {
            job = service.getJob(jobId);
            if (!job.isOk() || (job.value()["state"] != "running" && job.value()["state"] != "cancelling")) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return job;
    };

    int64_t initialJobId = 0;
    ASSERT_TRUE(service.startJob("selected", {mediaId}, false, initialJobId).isOk());
    auto initialJob = waitForJob(initialJobId);
    ASSERT_TRUE(initialJob.isOk());
    auto initialFaces = service.getMedia(mediaId);
    ASSERT_TRUE(initialFaces.isOk());
    ASSERT_EQ(initialFaces.value()["faces"].size(), 1u);
    const int64_t faceId = initialFaces.value()["faces"][0]["id"].get<int64_t>();
    const TagId personId = db.createOrGetTag("Hash Mismatch Person", "people").value();
    ASSERT_TRUE(service.setIdentity(faceId, 1, personId, std::nullopt).isOk());

    auto staleHashRes = db.connection().prepare(
        "UPDATE face_media_analysis SET source_hash='previous-successful-source' WHERE media_id=?;");
    ASSERT_TRUE(staleHashRes.isOk());
    auto staleHash = std::move(staleHashRes.value()); staleHash.bind(1, mediaId);
    ASSERT_EQ(staleHash.step(), StepResult::Done);
    {
        std::ofstream changed(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(changed.is_open());
        changed << "catalog hash mismatch while the prior snapshot is retained";
        ASSERT_TRUE(changed.good());
    }
    int64_t failedJobId = 0;
    ASSERT_TRUE(service.startJob("selected", {mediaId}, false, failedJobId).isOk());
    auto failedJob = waitForJob(failedJobId);
    ASSERT_TRUE(failedJob.isOk());
    EXPECT_EQ(failedJob.value()["failed"], 1);
    auto retained = service.getMedia(mediaId);
    ASSERT_TRUE(retained.isOk());
    ASSERT_EQ(retained.value()["faces"].size(), 1u);
    EXPECT_EQ(retained.value()["faces"][0]["id"], faceId);
    EXPECT_EQ(retained.value()["faces"][0]["revision"], 2);
    EXPECT_EQ(retained.value()["faces"][0]["person_tag_id"], personId);
    {
        auto analysisRes = db.connection().prepare(
            "SELECT state,source_hash FROM face_media_analysis WHERE media_id=?;");
        ASSERT_TRUE(analysisRes.isOk());
        auto analysis = std::move(analysisRes.value()); analysis.bind(1, mediaId);
        ASSERT_EQ(analysis.step(), StepResult::Row);
        EXPECT_EQ(analysis.getString(0), "complete");
        EXPECT_EQ(analysis.getString(1), "previous-successful-source");
    }

    std::filesystem::copy_file(originalPath, path, std::filesystem::copy_options::overwrite_existing);
    int64_t replaceJobId = 0;
    ASSERT_TRUE(service.startJob("selected", {mediaId}, false, replaceJobId).isOk());
    auto replaceJob = waitForJob(replaceJobId);
    ASSERT_TRUE(replaceJob.isOk());
    EXPECT_EQ(replaceJob.value()["processed"], 1);
    auto replaced = service.getMedia(mediaId);
    ASSERT_TRUE(replaced.isOk());
    ASSERT_EQ(replaced.value()["faces"].size(), 1u);
    EXPECT_NE(replaced.value()["faces"][0]["id"], faceId);
    EXPECT_TRUE(replaced.value()["faces"][0]["person_tag_id"].is_null());
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(FaceServiceTest, DetectorChangesRequireForceBeforeReplacingReviewedIdentity) {
    const auto root = std::filesystem::temp_directory_path() /
        ("imagine_faces_force_review_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    CatalogDb db;
    ASSERT_TRUE(db.open(":memory:").isOk());
    thumbnail::ImageBuffer image;
    image.width = 16; image.height = 12; image.channels = 3;
    image.data.assign(static_cast<size_t>(image.width * image.height * 3), 127);
    const std::string path = (root / "photo.jpg").string();
    ASSERT_TRUE(thumbnail::Generator::saveJpeg(image, path).isOk());
    MediaItem media;
    media.file_path = path; media.file_name = "photo.jpg";
    media.content_hash = metadata::Hasher::computeFileSha256(path).value();
    media.width = image.width; media.height = image.height; media.media_type = "photo";
    const MediaId mediaId = db.insertMedia(media).value();
    std::atomic<int> analyzerCalls{0};
    faces::Service service(db, (root / "cache").string(), [](const std::string& value) { return value; },
        [&analyzerCalls](const thumbnail::ImageBuffer&) -> Result<std::vector<faces::Detection>> {
            ++analyzerCalls;
            faces::Detection detection;
            detection.x = 1; detection.y = 2; detection.width = 6; detection.height = 7;
            detection.score = 0.9f;
            return std::vector<faces::Detection>{detection};
        });
    auto waitForJob = [&](int64_t jobId) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        Result<nlohmann::json> job = service.getJob(jobId);
        while (std::chrono::steady_clock::now() < deadline) {
            job = service.getJob(jobId);
            if (!job.isOk() || (job.value()["state"] != "running" && job.value()["state"] != "cancelling")) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return job;
    };
    int64_t firstJobId = 0;
    ASSERT_TRUE(service.startJob("selected", {mediaId}, false, firstJobId).isOk());
    auto firstJob = waitForJob(firstJobId);
    ASSERT_TRUE(firstJob.isOk());
    ASSERT_EQ(firstJob.value()["processed"], 1);
    auto faces = service.getMedia(mediaId);
    ASSERT_TRUE(faces.isOk());
    ASSERT_EQ(faces.value()["faces"].size(), 1u);
    const int64_t faceId = faces.value()["faces"][0]["id"].get<int64_t>();
    const TagId personId = db.createOrGetTag("Reviewed Person", "people").value();
    ASSERT_TRUE(service.setIdentity(faceId, 1, personId, std::nullopt).isOk());
    int64_t reusedJobId = 0;
    ASSERT_TRUE(service.startJob("selected", {mediaId}, false, reusedJobId).isOk());
    auto reusedJob = waitForJob(reusedJobId);
    ASSERT_TRUE(reusedJob.isOk());
    EXPECT_EQ(reusedJob.value()["skipped"], 1);
    auto reusedGrid = service.getGrid(reusedJobId, 0, 1000, false);
    ASSERT_TRUE(reusedGrid.isOk());
    EXPECT_EQ(reusedGrid.value()["total"], 1);
    EXPECT_EQ(reusedGrid.value()["items"][0]["person_tag_id"], personId);
    auto unknownGridJob = service.getGrid(999999, 0, 1000, false);
    ASSERT_FALSE(unknownGridJob.isOk());
    EXPECT_EQ(unknownGridJob.status().code(), StatusCode::NotFound);
    EXPECT_EQ(analyzerCalls.load(), 1);
    auto changeDetector = db.connection().prepare("UPDATE face_media_analysis SET detector_checksum='previous-detector' WHERE media_id=?;");
    ASSERT_TRUE(changeDetector.isOk());
    auto change = std::move(changeDetector.value()); change.bind(1, mediaId);
    ASSERT_EQ(change.step(), StepResult::Done);

    int64_t blockedJobId = 0;
    ASSERT_TRUE(service.startJob("selected", {mediaId}, false, blockedJobId).isOk());
    auto blockedJob = waitForJob(blockedJobId);
    ASSERT_TRUE(blockedJob.isOk());
    EXPECT_EQ(blockedJob.value()["skipped"], 1);
    EXPECT_NE(blockedJob.value()["error"].get<std::string>().find("use force"), std::string::npos);
    EXPECT_EQ(analyzerCalls.load(), 1);
    auto retained = service.getMedia(mediaId);
    ASSERT_TRUE(retained.isOk());
    ASSERT_EQ(retained.value()["faces"].size(), 1u);
    EXPECT_EQ(retained.value()["faces"][0]["person_tag_id"], personId);

    int64_t forcedJobId = 0;
    ASSERT_TRUE(service.startJob("selected", {mediaId}, true, forcedJobId).isOk());
    auto forcedJob = waitForJob(forcedJobId);
    ASSERT_TRUE(forcedJob.isOk());
    EXPECT_EQ(forcedJob.value()["processed"], 1);
    EXPECT_EQ(analyzerCalls.load(), 2);
    auto reset = service.getMedia(mediaId);
    ASSERT_TRUE(reset.isOk());
    ASSERT_EQ(reset.value()["faces"].size(), 1u);
    EXPECT_TRUE(reset.value()["faces"][0]["person_tag_id"].is_null());
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(FaceServiceTest, RecognizerOnlyRefreshKeepsIdentityAndClearsUnrefreshedEmbeddings) {
    const auto root = std::filesystem::temp_directory_path() /
        ("imagine_faces_recognizer_refresh_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    CatalogDb db;
    ASSERT_TRUE(db.open(":memory:").isOk());
    thumbnail::ImageBuffer image;
    image.width = 16; image.height = 12; image.channels = 3;
    image.data.assign(static_cast<size_t>(image.width * image.height * 3), 127);
    const std::string path = (root / "photo.jpg").string();
    ASSERT_TRUE(thumbnail::Generator::saveJpeg(image, path).isOk());
    MediaItem media;
    media.file_path = path; media.file_name = "photo.jpg";
    media.content_hash = metadata::Hasher::computeFileSha256(path).value();
    media.width = image.width; media.height = image.height; media.media_type = "photo";
    const MediaId mediaId = db.insertMedia(media).value();
    std::atomic<int> analyzerCalls{0};
    faces::Service service(db, (root / "cache").string(), [](const std::string& value) { return value; },
        [&analyzerCalls](const thumbnail::ImageBuffer&) -> Result<std::vector<faces::Detection>> {
            if (analyzerCalls.fetch_add(1) != 0) return std::vector<faces::Detection>{};
            faces::Detection detection;
            detection.x = 1; detection.y = 2; detection.width = 6; detection.height = 7;
            detection.score = 0.9f;
            detection.embedding.assign(512, 0.0f);
            detection.embedding[0] = 1.0f;
            return std::vector<faces::Detection>{detection};
        });
    auto waitForJob = [&](int64_t jobId) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        Result<nlohmann::json> job = service.getJob(jobId);
        while (std::chrono::steady_clock::now() < deadline) {
            job = service.getJob(jobId);
            if (!job.isOk() || (job.value()["state"] != "running" && job.value()["state"] != "cancelling")) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return job;
    };
    int64_t firstJobId = 0;
    ASSERT_TRUE(service.startJob("selected", {mediaId}, false, firstJobId).isOk());
    auto firstJob = waitForJob(firstJobId);
    ASSERT_TRUE(firstJob.isOk());
    ASSERT_EQ(firstJob.value()["processed"], 1);
    auto first = service.getMedia(mediaId);
    ASSERT_TRUE(first.isOk());
    ASSERT_EQ(first.value()["faces"].size(), 1u);
    const int64_t faceId = first.value()["faces"][0]["id"].get<int64_t>();
    const TagId personId = db.createOrGetTag("Recognized Person", "people").value();
    ASSERT_TRUE(service.setIdentity(faceId, 1, personId, std::nullopt).isOk());
    auto changeRecognizer = db.connection().prepare("UPDATE face_media_analysis SET recognizer_checksum='previous-recognizer' WHERE media_id=?;");
    ASSERT_TRUE(changeRecognizer.isOk());
    auto change = std::move(changeRecognizer.value()); change.bind(1, mediaId);
    ASSERT_EQ(change.step(), StepResult::Done);

    int64_t refreshJobId = 0;
    ASSERT_TRUE(service.startJob("selected", {mediaId}, false, refreshJobId).isOk());
    auto refreshJob = waitForJob(refreshJobId);
    ASSERT_TRUE(refreshJob.isOk());
    EXPECT_EQ(refreshJob.value()["processed"], 1);
    auto refreshed = service.getMedia(mediaId);
    ASSERT_TRUE(refreshed.isOk());
    ASSERT_EQ(refreshed.value()["faces"].size(), 1u);
    EXPECT_EQ(refreshed.value()["faces"][0]["id"], faceId);
    EXPECT_EQ(refreshed.value()["faces"][0]["person_tag_id"], personId);
    auto embeddingRes = db.connection().prepare("SELECT embedding,embedding_size,embedding_error FROM faces WHERE id=?;");
    ASSERT_TRUE(embeddingRes.isOk());
    auto embedding = std::move(embeddingRes.value()); embedding.bind(1, faceId);
    ASSERT_EQ(embedding.step(), StepResult::Row);
    EXPECT_TRUE(embedding.isNull(0));
    EXPECT_EQ(embedding.getInt(1), 0);
    EXPECT_EQ(embedding.getString(2), "No compatible refreshed embedding was produced");
    auto checksumRes = db.connection().prepare("SELECT recognizer_checksum FROM face_media_analysis WHERE media_id=?;");
    ASSERT_TRUE(checksumRes.isOk());
    auto checksum = std::move(checksumRes.value()); checksum.bind(1, mediaId);
    ASSERT_EQ(checksum.step(), StepResult::Row);
    EXPECT_EQ(checksum.getString(0), "test-recognizer");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(FaceServiceTest, ForcedRecognizerRefreshResetsMatchedAndUnmatchedReviews) {
    const auto root = std::filesystem::temp_directory_path() /
        ("imagine_faces_force_recognizer_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    CatalogDb db;
    ASSERT_TRUE(db.open(":memory:").isOk());
    thumbnail::ImageBuffer image;
    image.width = 16; image.height = 12; image.channels = 3;
    image.data.assign(static_cast<size_t>(image.width * image.height * 3), 127);
    const std::string path = (root / "photo.jpg").string();
    ASSERT_TRUE(thumbnail::Generator::saveJpeg(image, path).isOk());
    MediaItem media;
    media.file_path = path; media.file_name = "photo.jpg";
    media.content_hash = metadata::Hasher::computeFileSha256(path).value();
    media.width = image.width; media.height = image.height; media.media_type = "photo";
    const MediaId mediaId = db.insertMedia(media).value();
    faces::Service service(db, (root / "cache").string(), [](const std::string& value) { return value; },
        [](const thumbnail::ImageBuffer&) -> Result<std::vector<faces::Detection>> {
            faces::Detection detection;
            detection.x = 1; detection.y = 2; detection.width = 6; detection.height = 7;
            detection.score = 0.9f;
            detection.embedding.assign(512, 0.0f);
            detection.embedding[0] = 1.0f;
            return std::vector<faces::Detection>{detection};
        });
    auto waitForJob = [&](int64_t jobId) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        Result<nlohmann::json> job = service.getJob(jobId);
        while (std::chrono::steady_clock::now() < deadline) {
            job = service.getJob(jobId);
            if (!job.isOk() || (job.value()["state"] != "running" && job.value()["state"] != "cancelling")) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return job;
    };

    int64_t initialJobId = 0;
    ASSERT_TRUE(service.startJob("selected", {mediaId}, false, initialJobId).isOk());
    auto initialJob = waitForJob(initialJobId);
    ASSERT_TRUE(initialJob.isOk());
    ASSERT_EQ(initialJob.value()["processed"], 1);
    auto initialMedia = service.getMedia(mediaId);
    ASSERT_TRUE(initialMedia.isOk());
    ASSERT_EQ(initialMedia.value()["faces"].size(), 1u);
    const int64_t matchedFaceId = initialMedia.value()["faces"][0]["id"].get<int64_t>();
    const TagId matchedPersonId = db.createOrGetTag("Matched Review", "people").value();
    const TagId unmatchedPersonId = db.createOrGetTag("Unmatched Review", "people").value();
    ASSERT_TRUE(service.setIdentity(matchedFaceId, 1, matchedPersonId, std::nullopt).isOk());

    auto insertOldFaceRes = db.connection().prepare(R"SQL(
        INSERT INTO faces(media_id,x,y,width,height,score,landmarks,created_at,updated_at)
        VALUES(?,10,1,5,5,0.9,'[]',1,1);
    )SQL");
    ASSERT_TRUE(insertOldFaceRes.isOk());
    auto insertOldFace = std::move(insertOldFaceRes.value()); insertOldFace.bind(1, mediaId);
    ASSERT_EQ(insertOldFace.step(), StepResult::Done);
    const int64_t unmatchedFaceId = db.connection().lastInsertRowId();
    ASSERT_TRUE(service.setIdentity(unmatchedFaceId, 1, unmatchedPersonId, std::nullopt).isOk());
    ASSERT_TRUE(service.setDismissed(unmatchedFaceId, 2, true).isOk());
    auto insertRejectionRes = db.connection().prepare(
        "INSERT INTO face_rejections(face_id,tag_id,rejected_at) VALUES(?,?,1);");
    ASSERT_TRUE(insertRejectionRes.isOk());
    auto insertRejection = std::move(insertRejectionRes.value());
    insertRejection.bind(1, unmatchedFaceId); insertRejection.bind(2, matchedPersonId);
    ASSERT_EQ(insertRejection.step(), StepResult::Done);
    const TagId manualTagId = db.createOrGetTag("Manual Survivor", "events").value();
    ASSERT_TRUE(db.addTagToMedia(mediaId, manualTagId).isOk());

    auto changeRecognizer = db.connection().prepare(
        "UPDATE face_media_analysis SET recognizer_checksum='previous-recognizer' WHERE media_id=?;");
    ASSERT_TRUE(changeRecognizer.isOk());
    auto change = std::move(changeRecognizer.value()); change.bind(1, mediaId);
    ASSERT_EQ(change.step(), StepResult::Done);
    int64_t forcedJobId = 0;
    ASSERT_TRUE(service.startJob("selected", {mediaId}, true, forcedJobId).isOk());
    auto forcedJob = waitForJob(forcedJobId);
    ASSERT_TRUE(forcedJob.isOk());
    EXPECT_EQ(forcedJob.value()["processed"], 1);

    auto after = service.getMedia(mediaId);
    ASSERT_TRUE(after.isOk());
    ASSERT_EQ(after.value()["faces"].size(), 1u);
    const auto newFace = after.value()["faces"][0];
    EXPECT_NE(newFace["id"], matchedFaceId);
    EXPECT_NE(newFace["id"], unmatchedFaceId);
    EXPECT_TRUE(newFace["person_tag_id"].is_null());
    EXPECT_FALSE(newFace["dismissed"].get<bool>());
    EXPECT_EQ(newFace["revision"], 1);

    auto rejectionsRes = db.connection().prepare("SELECT COUNT(*) FROM face_rejections WHERE face_id IN (?,?);");
    ASSERT_TRUE(rejectionsRes.isOk());
    auto rejections = std::move(rejectionsRes.value());
    rejections.bind(1, matchedFaceId); rejections.bind(2, unmatchedFaceId);
    ASSERT_EQ(rejections.step(), StepResult::Row);
    EXPECT_EQ(rejections.getInt64(0), 0);
    auto mediaTags = db.getTagsForMedia(mediaId);
    ASSERT_TRUE(mediaTags.isOk());
    EXPECT_EQ(mediaTags.value().size(), 1u);
    EXPECT_EQ(mediaTags.value()[0].id, manualTagId);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}


TEST_F(ServerTest, HttpStatusCodesNotFoundAndConflict) {
    httplib::Client client("127.0.0.1", port_);

    // Deleting non-existent tag returns 404
    EXPECT_EQ(client.Delete("/api/tags/99999")->status, 404);

    // Deleting non-existent album returns 404
    EXPECT_EQ(client.Delete("/api/albums/99999")->status, 404);

    // Deleting non-existent media returns 404
    EXPECT_EQ(client.Delete("/api/media/99999")->status, 404);
}

TEST_F(ServerTest, ImportThreadManagedLifecycle) {
    auto photosDir = testDir_ / "managed_import";
    std::filesystem::create_directories(photosDir);
    for (int i = 0; i < 5; ++i) {
        std::ofstream ofs(photosDir / ("img" + std::to_string(i) + ".jpg"));
        ofs << "dummy_content_" << i;
    }

    httplib::Client client("127.0.0.1", port_);
    nlohmann::json importBody = {{"path", photosDir.string()}};
    auto impRes = client.Post("/api/import", importBody.dump(), "application/json");
    ASSERT_TRUE(impRes);
    EXPECT_EQ(impRes->status, 200);

    // Safe stop during / after import
    server_->stop();
    EXPECT_FALSE(server_->isRunning());
}

TEST_F(ServerTest, GeocodeValidationAndCaching) {
    httplib::Client client("127.0.0.1", port_);

    // Limit out of bounds (< 1)
    EXPECT_EQ(client.Get("/api/geocode?q=Paris&limit=0")->status, 400);

    // Limit out of bounds (> 10)
    EXPECT_EQ(client.Get("/api/geocode?q=Paris&limit=15")->status, 400);

    // Limit not a number
    EXPECT_EQ(client.Get("/api/geocode?q=Paris&limit=abc")->status, 400);
}

TEST_F(ServerTest, ServeRelocatedPhotosAndThumbnails) {
    // 1. Set up initial photo and thumbnail directories
    auto initPhotos = testDir_ / "initial_photos";
    auto subDir = initPhotos / "vacation" / "2026";
    std::filesystem::create_directories(subDir);
    std::string photoPath = (subDir / "summer.jpg").string();

    ImageBuffer buf;
    buf.width = 300;
    buf.height = 200;
    buf.channels = 3;
    buf.data.resize(300 * 200 * 3, 150);
    ASSERT_TRUE(Generator::saveJpeg(buf, photoPath).isOk());

    auto initThumbs = testDir_ / "initial_thumbs";
    std::filesystem::create_directories(initThumbs);

    std::string customDb = (testDir_ / "relocated.db").string();
    Catalog cat1(2);
    ASSERT_TRUE(cat1.open(customDb, initThumbs.string(), initPhotos.string()).isOk());

    auto impRes = cat1.importDirectory(initPhotos.string(), true, nullptr);
    ASSERT_TRUE(impRes.isOk());
    EXPECT_EQ(impRes.value().imported_files.load(), 1);

    auto mRes = cat1.getMediaByPath("vacation/2026/summer.jpg");
    ASSERT_TRUE(mRes.isOk());
    MediaId mid = mRes.value().id;
    std::string hash = mRes.value().content_hash;
    cat1.close();

    // 2. Physically move both photos and thumbnails directories to new locations
    auto movedPhotos = testDir_ / "moved_photos";
    auto movedThumbs = testDir_ / "moved_thumbs";
    std::filesystem::rename(initPhotos, movedPhotos);
    std::filesystem::rename(initThumbs, movedThumbs);

    // 3. Open catalog with new photosDir and thumbsDir locations
    Catalog cat2(2);
    ASSERT_TRUE(cat2.open(customDb, movedThumbs.string(), movedPhotos.string()).isOk());

    int newPort = 19500 + (std::rand() % 4000);
    WebServer server2(cat2, webDir_.string());
    ASSERT_TRUE(server2.start("127.0.0.1", newPort, webDir_.string()).isOk());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    httplib::Client client2("127.0.0.1", newPort);

    // 4. Verify GET /api/photos/:id/original serves photo from movedPhotos
    auto origRes = client2.Get("/api/photos/" + std::to_string(mid) + "/original");
    ASSERT_TRUE(origRes);
    EXPECT_EQ(origRes->status, 200);
    EXPECT_EQ(origRes->get_header_value("Content-Type"), "image/jpeg");
    EXPECT_EQ(origRes->body.size(), std::filesystem::file_size(movedPhotos / "vacation" / "2026" / "summer.jpg"));

    // 5. Verify GET /api/thumbnails/:hash/256 serves thumbnail from movedThumbs
    auto thumbRes = client2.Get("/api/thumbnails/" + hash + "/256");
    ASSERT_TRUE(thumbRes);
    EXPECT_EQ(thumbRes->status, 200);

    // 6. Delete thumbnail on disk, verify on-demand generation regenerates it from movedPhotos into movedThumbs
    std::string smallThumbPath = cat2.cache().getThumbnailPath(hash, 256);
    EXPECT_TRUE(std::filesystem::exists(smallThumbPath));
    std::error_code ec;
    bool removed = false;
    for (int retry = 0; retry < 50; ++retry) {
        ec.clear();
        if (std::filesystem::remove(smallThumbPath, ec)) {
            removed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(removed);
    EXPECT_FALSE(std::filesystem::exists(smallThumbPath));

    auto regenRes = client2.Get("/api/thumbnails/" + hash + "/256");
    ASSERT_TRUE(regenRes);
    EXPECT_EQ(regenRes->status, 200);
    EXPECT_TRUE(std::filesystem::exists(smallThumbPath));

    server2.stop();
}

TEST_F(ServerTest, DeleteMediaSingleAndBatchWithDiskOption) {
    httplib::Client client("127.0.0.1", port_);

    // 1. Create test files on disk
    std::filesystem::path f1 = testDir_ / "disk_test1.jpg";
    std::filesystem::path f2 = testDir_ / "disk_test2.jpg";
    std::filesystem::path f3 = testDir_ / "disk_test3.jpg";
    std::filesystem::path f4 = testDir_ / "disk_test4.jpg";

    {
        std::ofstream ofs1(f1); ofs1 << "test data 1";
        std::ofstream ofs2(f2); ofs2 << "test data 2";
        std::ofstream ofs3(f3); ofs3 << "test data 3";
        std::ofstream ofs4(f4); ofs4 << "test data 4";
    }

    ASSERT_TRUE(std::filesystem::exists(f1));
    ASSERT_TRUE(std::filesystem::exists(f2));
    ASSERT_TRUE(std::filesystem::exists(f3));
    ASSERT_TRUE(std::filesystem::exists(f4));

    MediaItem it1, it2, it3, it4;
    it1.file_path = f1.string(); it1.file_name = "disk_test1.jpg"; it1.content_hash = "h_dt1";
    it2.file_path = f2.string(); it2.file_name = "disk_test2.jpg"; it2.content_hash = "h_dt2";
    it3.file_path = f3.string(); it3.file_name = "disk_test3.jpg"; it3.content_hash = "h_dt3";
    it4.file_path = f4.string(); it4.file_name = "disk_test4.jpg";

    MediaId id1 = catalog_->db().insertMedia(it1).value();
    MediaId id2 = catalog_->db().insertMedia(it2).value();
    MediaId id3 = catalog_->db().insertMedia(it3).value();
    MediaId id4 = catalog_->db().insertMedia(it4).value();

    // 2. Single delete without delete_from_disk (default catalog only)
    auto delRes1 = client.Delete("/api/media/" + std::to_string(id1));
    ASSERT_TRUE(delRes1);
    EXPECT_EQ(delRes1->status, 200);
    EXPECT_EQ(client.Get("/api/media/" + std::to_string(id1))->status, 404);
    EXPECT_TRUE(std::filesystem::exists(f1)); // File still exists on disk

    // 3. Single delete with delete_from_disk=true
    auto delRes2 = client.Delete("/api/media/" + std::to_string(id2) + "?delete_from_disk=true");
    ASSERT_TRUE(delRes2);
    EXPECT_EQ(delRes2->status, 200);
    auto delJson2 = nlohmann::json::parse(delRes2->body);
    EXPECT_EQ(delJson2["deleted_from_disk"], true);
    EXPECT_EQ(client.Get("/api/media/" + std::to_string(id2))->status, 404);
    EXPECT_FALSE(std::filesystem::exists(f2)); // File deleted from disk

    // 4. Batch delete with delete_from_disk: true
    nlohmann::json bDelBody = {{"ids", {id3, id4}}, {"delete_from_disk", true}};
    auto bDelRes = client.Post("/api/media/batch-delete", bDelBody.dump(), "application/json");
    ASSERT_TRUE(bDelRes);
    EXPECT_EQ(bDelRes->status, 200);
    auto bDelJson = nlohmann::json::parse(bDelRes->body);
    EXPECT_EQ(bDelJson["deleted_count"].get<int>(), 2);
    EXPECT_EQ(bDelJson["deleted_from_disk"], true);
    EXPECT_EQ(client.Get("/api/media/" + std::to_string(id3))->status, 404);
    EXPECT_EQ(client.Get("/api/media/" + std::to_string(id4))->status, 404);
    EXPECT_FALSE(std::filesystem::exists(f3)); // File deleted from disk
    EXPECT_FALSE(std::filesystem::exists(f4)); // File deleted from disk
}

TEST_F(ServerTest, EditPhotoEndpointOverwriteAndCopy) {
    auto photosDir = testDir_ / "photos";
    std::filesystem::create_directories(photosDir);
    std::string photoPath = (photosDir / "test_photo.jpg").string();

    ImageBuffer buf;
    buf.width = 400;
    buf.height = 300;
    buf.channels = 3;
    buf.data.resize(400 * 300 * 3, 120);
    ASSERT_TRUE(Generator::saveJpeg(buf, photoPath).isOk());

    auto impRes = catalog_->importDirectory(photosDir.string(), true, nullptr);
    ASSERT_TRUE(impRes.isOk());

    auto mRes = catalog_->getMediaByPath(photoPath);
    ASSERT_TRUE(mRes.isOk());
    MediaId id = mRes.value().id;
    std::string oldHash = mRes.value().content_hash;

    httplib::Client client("127.0.0.1", port_);

    // Check caching headers before edit
    auto origBefore = client.Get("/api/photos/" + std::to_string(id) + "/original");
    ASSERT_TRUE(origBefore);
    EXPECT_EQ(origBefore->status, 200);
    EXPECT_EQ(origBefore->get_header_value("Cache-Control"), "no-store, no-cache, must-revalidate");
    std::string etagBefore = origBefore->get_header_value("ETag");
    EXPECT_FALSE(etagBefore.empty());

    // Conditional request before edit returns 304 Not Modified
    httplib::Headers condBefore = {{"If-None-Match", etagBefore}};
    EXPECT_EQ(client.Get("/api/photos/" + std::to_string(id) + "/original", condBefore)->status, 304);

    // 1. Overwrite edit with operations (crop 200x150 and rotate 90)
    nlohmann::json editPayload = {
        {"mode", "overwrite"},
        {"operations", {
            {"crop", {{"x", 0}, {"y", 0}, {"width", 200}, {"height", 150}}},
            {"rotation", 90}
        }}
    };

    auto editRes = client.Post("/api/photos/" + std::to_string(id) + "/edit", editPayload.dump(), "application/json");
    ASSERT_TRUE(editRes);
    EXPECT_EQ(editRes->status, 200);

    // Verify that after overwrite, GET with previous ETag returns 200 OK (not 304) and new content
    auto origAfter = client.Get("/api/photos/" + std::to_string(id) + "/original", condBefore);
    ASSERT_TRUE(origAfter);
    EXPECT_EQ(origAfter->status, 200);
    EXPECT_EQ(origAfter->get_header_value("Cache-Control"), "no-store, no-cache, must-revalidate");
    EXPECT_NE(origAfter->get_header_value("ETag"), etagBefore);

    auto resJson = nlohmann::json::parse(editRes->body);
    EXPECT_EQ(resJson["id"].get<MediaId>(), id);
    // After 200x150 crop + 90 deg rotation, dimensions are 150x200
    EXPECT_EQ(resJson["width"].get<int>(), 150);
    EXPECT_EQ(resJson["height"].get<int>(), 200);
    std::string newHash = resJson["content_hash"].get<std::string>();
    EXPECT_NE(newHash, oldHash);

    // 2. Copy edit mode
    nlohmann::json copyPayload = {
        {"mode", "copy"},
        {"operations", {
            {"rotation", 180}
        }}
    };

    auto copyRes = client.Post("/api/photos/" + std::to_string(id) + "/edit", copyPayload.dump(), "application/json");
    ASSERT_TRUE(copyRes);
    EXPECT_EQ(copyRes->status, 201);

    auto copyJson = nlohmann::json::parse(copyRes->body);
    MediaId copyId = copyJson["id"].get<MediaId>();
    EXPECT_NE(copyId, id);
    EXPECT_TRUE(copyJson["file_name"].get<std::string>().find("test_photo_edited") != std::string::npos);
}

TEST_F(ServerTest, EditPhotoRejectsVideoAndAudio) {
    auto photosDir = testDir_ / "photos";
    std::filesystem::create_directories(photosDir);
    std::string videoPath = (photosDir / "sample_video.mp4").string();
    std::string audioPath = (photosDir / "sample_audio.mp3").string();

    {
        std::ofstream vout(videoPath, std::ios::binary);
        vout << "fake video content";
    }
    {
        std::ofstream aout(audioPath, std::ios::binary);
        aout << "fake audio content";
    }

    MediaItem videoItem;
    videoItem.file_path = videoPath;
    videoItem.file_name = "sample_video.mp4";
    videoItem.media_type = "video";
    videoItem.file_size = 18;
    videoItem.content_hash = "fakevideohash1234";
    auto vidRes = catalog_->db().insertMedia(videoItem);
    ASSERT_TRUE(vidRes.isOk());
    MediaId videoId = vidRes.value();

    MediaItem audioItem;
    audioItem.file_path = audioPath;
    audioItem.file_name = "sample_audio.mp3";
    audioItem.media_type = "audio";
    audioItem.file_size = 18;
    audioItem.content_hash = "fakeaudiohash1234";
    auto aidRes = catalog_->db().insertMedia(audioItem);
    ASSERT_TRUE(aidRes.isOk());
    MediaId audioId = aidRes.value();

    httplib::Client client("127.0.0.1", port_);
    nlohmann::json editPayload = {
        {"mode", "overwrite"},
        {"operations", {{"rotation", 90}}}
    };

    auto resVideo = client.Post("/api/photos/" + std::to_string(videoId) + "/edit", editPayload.dump(), "application/json");
    ASSERT_TRUE(resVideo);
    EXPECT_EQ(resVideo->status, 400);

    auto resAudio = client.Post("/api/photos/" + std::to_string(audioId) + "/edit", editPayload.dump(), "application/json");
    ASSERT_TRUE(resAudio);
    EXPECT_EQ(resAudio->status, 400);
}

TEST_F(ServerTest, EditPhotoEndpointBinaryPayload) {
    auto photosDir = testDir_ / "photos";
    std::filesystem::create_directories(photosDir);
    std::string photoPath = (photosDir / "binary_test.jpg").string();

    ImageBuffer buf;
    buf.width = 100;
    buf.height = 100;
    buf.channels = 3;
    buf.data.resize(100 * 100 * 3, 150);
    ASSERT_TRUE(Generator::saveJpeg(buf, photoPath).isOk());

    auto impRes = catalog_->importDirectory(photosDir.string(), true, nullptr);
    ASSERT_TRUE(impRes.isOk());

    auto mRes = catalog_->getMediaByPath(photoPath);
    ASSERT_TRUE(mRes.isOk());
    MediaId id = mRes.value().id;

    // Create a modified JPEG
    std::string newPhotoPath = (photosDir / "new_binary.jpg").string();
    buf.width = 120;
    buf.height = 80;
    buf.data.resize(120 * 80 * 3, 200);
    ASSERT_TRUE(Generator::saveJpeg(buf, newPhotoPath).isOk());

    std::ifstream ifs(newPhotoPath, std::ios::binary);
    std::string jpegBytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();

    httplib::Client client("127.0.0.1", port_);

    // 1. Overwrite via binary POST with query parameter ?mode=overwrite
    auto resOverwrite = client.Post("/api/photos/" + std::to_string(id) + "/edit?mode=overwrite",
                                    jpegBytes, "image/jpeg");
    ASSERT_TRUE(resOverwrite);
    EXPECT_EQ(resOverwrite->status, 200);
    auto jsonOverwrite = nlohmann::json::parse(resOverwrite->body);
    EXPECT_EQ(jsonOverwrite["width"].get<int>(), 120);
    EXPECT_EQ(jsonOverwrite["height"].get<int>(), 80);

    // 2. Copy via binary POST with X-Edit-Mode header
    httplib::Headers headers = {{"X-Edit-Mode", "copy"}};
    auto resCopy = client.Post("/api/photos/" + std::to_string(id) + "/edit",
                               headers, jpegBytes, "image/jpeg");
    ASSERT_TRUE(resCopy);
    EXPECT_EQ(resCopy->status, 201);
    auto jsonCopy = nlohmann::json::parse(resCopy->body);
    EXPECT_NE(jsonCopy["id"].get<MediaId>(), id);
    EXPECT_TRUE(jsonCopy["file_name"].get<std::string>().find("_edited") != std::string::npos);
}

TEST_F(ServerTest, EditPhotoEndpointLargePayload) {
    auto photosDir = testDir_ / "photos";
    std::filesystem::create_directories(photosDir);
    std::string photoPath = (photosDir / "large_photo.jpg").string();

    ImageBuffer buf;
    buf.width = 200;
    buf.height = 200;
    buf.channels = 3;
    buf.data.resize(200 * 200 * 3, 100);
    ASSERT_TRUE(Generator::saveJpeg(buf, photoPath).isOk());

    auto impRes = catalog_->importDirectory(photosDir.string(), true, nullptr);
    ASSERT_TRUE(impRes.isOk());

    auto mRes = catalog_->getMediaByPath(photoPath);
    ASSERT_TRUE(mRes.isOk());
    MediaId id = mRes.value().id;

    // Read valid JPEG bytes
    std::ifstream ifs(photoPath, std::ios::binary);
    std::string jpegBytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();

    // Pad bytes to 11MB (exceeding old 10MB limit)
    size_t targetSize = 11 * 1024 * 1024;
    ASSERT_GT(targetSize, jpegBytes.size());
    jpegBytes.resize(targetSize, 0);

    httplib::Client client("127.0.0.1", port_);
    // Sending >10MB payload should NOT return 413 Payload Too Large
    auto res = client.Post("/api/photos/" + std::to_string(id) + "/edit?mode=overwrite",
                           jpegBytes, "image/jpeg");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
}

TEST_F(ServerTest, WebServerPayloadLimitConfig) {
    EXPECT_EQ(server_->payloadMaxLength(), WebServer::kDefaultPayloadMaxLength);
    server_->setPayloadMaxLength(50 * 1024 * 1024);
    EXPECT_EQ(server_->payloadMaxLength(), 50 * 1024 * 1024);
}

TEST_F(ServerTest, MediaFilterOrModeTagsAndFolders) {
    httplib::Client client("127.0.0.1", port_);

    auto photosDir = testDir_ / "photos";
    auto dirA = photosDir / "dirA";
    auto dirB = photosDir / "dirB";
    std::filesystem::create_directories(dirA);
    std::filesystem::create_directories(dirB);

    std::string pathA = (dirA / "photo_a.jpg").string();
    std::string pathB = (dirB / "photo_b.jpg").string();

    ImageBuffer buf;
    buf.width = 100;
    buf.height = 100;
    buf.channels = 3;
    buf.data.resize(100 * 100 * 3, 150);
    ASSERT_TRUE(Generator::saveJpeg(buf, pathA).isOk());
    ASSERT_TRUE(Generator::saveJpeg(buf, pathB).isOk());

    ASSERT_TRUE(catalog_->importDirectory(photosDir.string(), true, nullptr).isOk());

    auto mA = catalog_->getMediaByPath(pathA).value();
    auto mB = catalog_->getMediaByPath(pathB).value();

    auto tag1 = catalog_->db().createOrGetTag("TagAlpha", "people").value();
    auto tag2 = catalog_->db().createOrGetTag("TagBeta", "places").value();
    ASSERT_TRUE(catalog_->db().addTagToMedia(mA.id, tag1).isOk());
    ASSERT_TRUE(catalog_->db().addTagToMedia(mB.id, tag2).isOk());

    auto res1 = client.Get("/api/media?tag_ids=" + std::to_string(tag1) + "," + std::to_string(tag2) + "&tag_folder_mode=or");
    ASSERT_TRUE(res1);
    EXPECT_EQ(res1->status, 200);
    auto j1 = nlohmann::json::parse(res1->body);
    EXPECT_EQ(j1["items"].size(), 2);

    auto res2 = client.Get("/api/media?folder=dirA&folder=dirB&tag_folder_mode=or");
    ASSERT_TRUE(res2);
    EXPECT_EQ(res2->status, 200);
    auto j2 = nlohmann::json::parse(res2->body);
    EXPECT_EQ(j2["items"].size(), 2);

    auto res3 = client.Get("/api/media?tag_id=" + std::to_string(tag1) + "&folder=dirB&tag_folder_mode=or");
    ASSERT_TRUE(res3);
    EXPECT_EQ(res3->status, 200);
    auto j3 = nlohmann::json::parse(res3->body);
    EXPECT_EQ(j3["items"].size(), 2);
}

TEST_F(ServerTest, FailedWritePreservesOriginalPhoto) {
    auto photosDir = testDir_ / "photos_write_fail";
    std::filesystem::create_directories(photosDir);
    std::string photoPath = (photosDir / "preserve_test.jpg").string();

    ImageBuffer buf;
    buf.width = 100;
    buf.height = 100;
    buf.channels = 3;
    buf.data.resize(100 * 100 * 3, 200);
    ASSERT_TRUE(Generator::saveJpeg(buf, photoPath).isOk());

    auto origSize = std::filesystem::file_size(photoPath);
    ASSERT_GT(origSize, 0u);

    ASSERT_TRUE(catalog_->importDirectory(photosDir.string(), true, nullptr).isOk());
    auto mRes = catalog_->getMediaByPath(photoPath);
    ASSERT_TRUE(mRes.isOk());
    MediaId id = mRes.value().id;

    // Simulate write failure by creating a directory where the temporary file needs to go
    std::string tmpPath = photoPath + ".edit.tmp";
    std::filesystem::create_directories(tmpPath);

    httplib::Client client("127.0.0.1", port_);
    nlohmann::json editPayload = {
        {"mode", "overwrite"},
        {"operations", {
            {"rotation", 90}
        }}
    };

    auto editRes = client.Post("/api/photos/" + std::to_string(id) + "/edit", editPayload.dump(), "application/json");
    ASSERT_TRUE(editRes);
    EXPECT_EQ(editRes->status, 500);

    // Verify original photo was preserved intact
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::exists(photoPath, ec));
    EXPECT_EQ(std::filesystem::file_size(photoPath, ec), origSize);

    // Clean up temporary directory
    std::filesystem::remove_all(tmpPath, ec);
}

TEST_F(ServerTest, WebServerConcurrentStopAndWait) {
    auto testCatalog = std::make_unique<Catalog>(1);
    std::string cDb = (testDir_ / "concurrent_cat.db").string();
    std::string cCache = (testDir_ / "concurrent_cache").string();
    ASSERT_TRUE(testCatalog->open(cDb, cCache).isOk());

    int p = 19800 + (std::rand() % 4000);
    WebServer ws(*testCatalog, webDir_.string());
    ASSERT_TRUE(ws.start("127.0.0.1", p, webDir_.string()).isOk());

    std::atomic<bool> waitFinished{false};
    std::thread waitThread([&ws, &waitFinished]() {
        ws.wait();
        waitFinished = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(waitFinished.load());

    // Concurrently stop the server from multiple threads
    std::vector<std::thread> stopThreads;
    for (int i = 0; i < 4; ++i) {
        stopThreads.emplace_back([&ws]() {
            ws.stop();
        });
    }

    for (auto& t : stopThreads) {
        t.join();
    }
    waitThread.join();

    EXPECT_TRUE(waitFinished.load());
    EXPECT_FALSE(ws.isRunning());

    // Calling wait() again on already stopped server returns immediately
    ws.wait();

    // Calling run on a background thread and stopping concurrently
    int p2 = 19800 + (std::rand() % 4000);
    std::atomic<bool> runFinished{false};
    std::thread runThread([&ws, p2, &runFinished, this]() {
        auto status = ws.run("127.0.0.1", p2, webDir_.string());
        EXPECT_TRUE(status.isOk());
        runFinished = true;
    });

    for (int i = 0; i < 200 && !ws.isRunning(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(ws.isRunning());
    ws.stop();
    runThread.join();
    EXPECT_TRUE(runFinished.load());
}

TEST_F(ServerTest, ImportRootRestrictionsWindowsPaths) {
    auto allowedDir1 = testDir_ / "allowed_root1";
    auto allowedDir2 = testDir_ / "allowed_root2";
    auto disallowedDir = testDir_ / "disallowed_dir";
    std::filesystem::create_directories(allowedDir1 / "subfolder");
    std::filesystem::create_directories(allowedDir2);
    std::filesystem::create_directories(disallowedDir);

#if defined(_WIN32)
    std::string envVal = allowedDir1.string() + ";" + allowedDir2.string();
    _putenv_s("IMAGINE_ALLOWED_IMPORT_ROOTS", envVal.c_str());
#else
    std::string envVal = allowedDir1.string() + ":" + allowedDir2.string();
    setenv("IMAGINE_ALLOWED_IMPORT_ROOTS", envVal.c_str(), 1);
#endif

    httplib::Client client("127.0.0.1", port_);

    // 1. Importing within allowed root 1 (subfolder) should succeed
    nlohmann::json body1 = {{"path", (allowedDir1 / "subfolder").string()}};
    auto res1 = client.Post("/api/import", body1.dump(), "application/json");
    ASSERT_TRUE(res1);
    EXPECT_EQ(res1->status, 200);

    // 2. Importing allowed root 2 itself should succeed
    nlohmann::json body2 = {{"path", allowedDir2.string()}};
    auto res2 = client.Post("/api/import", body2.dump(), "application/json");
    ASSERT_TRUE(res2);
    EXPECT_EQ(res2->status, 200);

    // 3. Importing outside allowed roots should be rejected with 403 Forbidden
    nlohmann::json body3 = {{"path", disallowedDir.string()}};
    auto res3 = client.Post("/api/import", body3.dump(), "application/json");
    ASSERT_TRUE(res3);
    EXPECT_EQ(res3->status, 403);
    auto errJson = nlohmann::json::parse(res3->body);
    EXPECT_NE(errJson["error"].get<std::string>().find("not within allowed import roots"), std::string::npos);

#if defined(_WIN32)
    _putenv_s("IMAGINE_ALLOWED_IMPORT_ROOTS", "");
#else
    unsetenv("IMAGINE_ALLOWED_IMPORT_ROOTS");
#endif
}

TEST_F(ServerTest, EditedPortraitThumbnailNormalizesOrientation) {
    auto photosDir = testDir_ / "photos_portrait";
    std::filesystem::create_directories(photosDir);
    std::string photoPath = (photosDir / "portrait.jpg").string();

    // 1. Create raw image on disk that is LANDSCAPE (400x300) with colored halves:
    // Top half (y < 150): Red (255, 0, 0)
    // Bottom half (y >= 150): Blue (0, 0, 255)
    ImageBuffer buf;
    buf.width = 400;
    buf.height = 300;
    buf.channels = 3;
    buf.data.resize(400 * 300 * 3);
    for (int y = 0; y < 300; ++y) {
        for (int x = 0; x < 400; ++x) {
            int idx = (y * 400 + x) * 3;
            if (y < 150) {
                buf.data[idx] = 255; buf.data[idx + 1] = 0; buf.data[idx + 2] = 0; // Red
            } else {
                buf.data[idx] = 0; buf.data[idx + 1] = 0; buf.data[idx + 2] = 255; // Blue
            }
        }
    }
    ASSERT_TRUE(Generator::saveJpeg(buf, photoPath).isOk());

    ASSERT_TRUE(catalog_->importDirectory(photosDir.string(), true, nullptr).isOk());
    auto mRes = catalog_->getMediaByPath(photoPath);
    ASSERT_TRUE(mRes.isOk());
    MediaId id = mRes.value().id;

    // Simulate original photo having EXIF orientation 6 (Rotate 90 CW).
    // Visually, rotating 90 CW turns 400x300 landscape into 300x400 portrait:
    // Top (Red) moves to Right half (x >= 150), Bottom (Blue) moves to Left half (x < 150).
    auto item = mRes.value();
    item.exif.orientation = 6;
    ASSERT_TRUE(catalog_->db().updateMedia(item).isOk());

    auto checkBefore = catalog_->db().getMediaById(id);
    ASSERT_TRUE(checkBefore.isOk());
    EXPECT_EQ(checkBefore.value().exif.orientation, 6);

    // 2. Perform an operation-based edit with rotation: 0 (and overwrite mode).
    // The server must normalize the raw pixels using EXIF orientation 6 before applying operations,
    // saving an upright 300x400 image and resetting orientation to 1.
    httplib::Client client("127.0.0.1", port_);
    nlohmann::json editPayload = {
        {"mode", "overwrite"},
        {"operations", {
            {"rotation", 0}
        }}
    };

    auto editRes = client.Post("/api/photos/" + std::to_string(id) + "/edit", editPayload.dump(), "application/json");
    ASSERT_TRUE(editRes);
    EXPECT_EQ(editRes->status, 200);

    auto resJson = nlohmann::json::parse(editRes->body);
    EXPECT_EQ(resJson["exif"]["orientation"].get<int>(), 1);
    // Visual portrait dimensions
    EXPECT_EQ(resJson["width"].get<int>(), 300);
    EXPECT_EQ(resJson["height"].get<int>(), 400);

    // Verify DB orientation and dimensions
    auto checkAfter = catalog_->db().getMediaById(id);
    ASSERT_TRUE(checkAfter.isOk());
    EXPECT_EQ(checkAfter.value().exif.orientation, 1);
    EXPECT_EQ(checkAfter.value().width, 300);
    EXPECT_EQ(checkAfter.value().height, 400);

    // 3. Verify pixel orientation of the overwritten photo on disk
    auto diskImgRes = Generator::loadImage(photoPath);
    ASSERT_TRUE(diskImgRes.isOk());
    const auto& diskImg = diskImgRes.value();
    EXPECT_EQ(diskImg.width, 300);
    EXPECT_EQ(diskImg.height, 400);

    // Check pixel colors: Left half is Blue, Right half is Red
    int leftPixelIdx = (200 * 300 + 50) * 3;
    int rightPixelIdx = (200 * 300 + 250) * 3;
    EXPECT_GT(diskImg.data[leftPixelIdx + 2], 180); // Blue high
    EXPECT_LT(diskImg.data[leftPixelIdx], 80);       // Red low
    EXPECT_GT(diskImg.data[rightPixelIdx], 180);     // Red high
    EXPECT_LT(diskImg.data[rightPixelIdx + 2], 80);   // Blue low

    // 4. Verify thumbnail generated in cache is portrait and correctly oriented
    std::string newHash = resJson["content_hash"].get<std::string>();
    auto thumbRes = client.Get("/api/thumbnails/" + newHash + "/256");
    ASSERT_TRUE(thumbRes);
    EXPECT_EQ(thumbRes->status, 200);

    auto thumbImgRes = Generator::loadImageFromMemory(reinterpret_cast<const uint8_t*>(thumbRes->body.data()), thumbRes->body.size());
    ASSERT_TRUE(thumbImgRes.isOk());
    const auto& thumb = thumbImgRes.value();
    // Portrait thumbnail: height is 256, width is 192 (300/400 * 256)
    EXPECT_LT(thumb.width, thumb.height);
    EXPECT_EQ(thumb.height, 256);
    EXPECT_EQ(thumb.width, 192);

    // Pixel colors in thumbnail: Left half is Blue, Right half is Red
    int thumbLeftIdx = (128 * thumb.width + 40) * 3;
    int thumbRightIdx = (128 * thumb.width + (thumb.width - 40)) * 3;
    EXPECT_GT(thumb.data[thumbLeftIdx + 2], 150); // Blue
    EXPECT_LT(thumb.data[thumbLeftIdx], 100);
    EXPECT_GT(thumb.data[thumbRightIdx], 150);     // Red
    EXPECT_LT(thumb.data[thumbRightIdx + 2], 100);
}

TEST_F(ServerTest, AccessLogOutput) {
    auto& logger = Logger::instance();
    auto origLevel = logger.level();

    auto logPath = testDir_ / "access_test.log";
    logger.setLevel(LogLevel::Debug);
    logger.setConsoleEnabled(false);
    logger.setFileFormat(LogFormat::Json);
    logger.setLogFile(logPath.string());

    httplib::Client client("127.0.0.1", port_);

    // 1. Successful API call
    auto res1 = client.Get("/api/stats");
    ASSERT_TRUE(res1);
    EXPECT_EQ(res1->status, 200);

    // 2. 404 API call
    auto res2 = client.Get("/api/nonexistent_endpoint");
    ASSERT_TRUE(res2);
    EXPECT_EQ(res2->status, 404);

    // 3. Static asset call
    auto res3 = client.Get("/index.html");
    ASSERT_TRUE(res3);
    EXPECT_EQ(res3->status, 200);

    auto preflight = client.Options("/api/stats");
    ASSERT_TRUE(preflight);
    EXPECT_EQ(preflight->status, 204);
    EXPECT_FALSE(preflight->has_header("X-Internal-Start-Ns"));
    EXPECT_FALSE(res1->has_header("X-Internal-Start-Ns"));

    auto malformed = client.Get("/api/%FF");
    ASSERT_TRUE(malformed);
    EXPECT_EQ(malformed->status, 404);

    logger.closeLogFile();

    // Verify access logs in file
    std::ifstream ifs(logPath);
    ASSERT_TRUE(ifs.is_open());
    std::string line;
    bool foundStats = false;
    bool found404 = false;
    bool foundStatic = false;
    bool foundMalformed = false;

    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        try {
            auto j = nlohmann::json::parse(line);
            std::string msg = j.value("msg", "");
            std::string lvl = j.value("level", "");
            EXPECT_EQ(msg.find("OPTIONS "), std::string::npos);
            if (msg.find("GET /api/\xef\xbf\xbd 404") != std::string::npos && lvl == "WARN") {
                foundMalformed = true;
            }
            if (msg.find("GET /api/stats 200") != std::string::npos && lvl == "INFO") {
                foundStats = true;
            }
            if (msg.find("GET /api/nonexistent_endpoint 404") != std::string::npos && lvl == "WARN") {
                found404 = true;
            }
            if (msg.find("GET /index.html 200") != std::string::npos && lvl == "DEBUG") {
                foundStatic = true;
            }
        } catch (const std::exception& e) {
            ADD_FAILURE() << "Invalid JSON access log: " << e.what();
        }
    }

    EXPECT_TRUE(foundStats);
    EXPECT_TRUE(found404);
    EXPECT_TRUE(foundStatic);
    EXPECT_TRUE(foundMalformed);

    logger.setConsoleEnabled(true);
    logger.setLevel(origLevel);
}

