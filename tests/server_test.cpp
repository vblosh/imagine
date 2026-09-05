#include <gtest/gtest.h>
#include "imagine/core/catalog.hpp"
#include "imagine/server/web_server.hpp"
#include "imagine/server/api_router.hpp"
#include "imagine/db/catalog_db.hpp"
#include "imagine/thumbnail/cache.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

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
    auto noTag = nlohmann::json::parse(noTagRes->body);
    EXPECT_EQ(noTag["tags"].size(), 0u);

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

    get1 = nlohmann::json::parse(client.Get("/api/media/" + std::to_string(id1))->body);
    EXPECT_EQ(get1["tags"].size(), 1u);
    EXPECT_EQ(get1["tags"][0]["name"].get<std::string>(), "Vacation2026");

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
