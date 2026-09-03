#include <gtest/gtest.h>
#include "imagine/core/catalog.hpp"
#include "imagine/server/web_server.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

using namespace imagine;
using namespace imagine::core;
using namespace imagine::server;

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
