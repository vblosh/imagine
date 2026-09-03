#include <gtest/gtest.h>
#include "imagine/core/importer.hpp"
#include "imagine/core/catalog.hpp"
#include "imagine/thumbnail/generator.hpp"
#include <filesystem>
#include <fstream>

using namespace imagine;
using namespace imagine::core;
using namespace imagine::db;
using namespace imagine::thumbnail;
using namespace imagine::concurrency;

class ImporterTest : public ::testing::Test {
protected:
    void SetUp() override {
        testDir = std::filesystem::temp_directory_path() / "imagine_importer_test";
        std::filesystem::create_directories(testDir);
        photosDir = testDir / "photos";
        thumbsDir = testDir / "thumbs";
        std::filesystem::create_directories(photosDir);
        std::filesystem::create_directories(thumbsDir);

        // Create two synthetic JPEG images
        ImageBuffer buf;
        buf.width = 300;
        buf.height = 200;
        buf.channels = 3;
        buf.data.resize(300 * 200 * 3, 100);

        img1Path = (photosDir / "img1.jpg").string();
        img2Path = (photosDir / "img2.jpg").string();

        ASSERT_TRUE(Generator::saveJpeg(buf, img1Path).isOk());
        buf.data[0] = 250; // slightly different content for img2
        ASSERT_TRUE(Generator::saveJpeg(buf, img2Path).isOk());

        dbPath = (testDir / "catalog.db").string();
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
    }

    std::filesystem::path testDir;
    std::filesystem::path photosDir;
    std::filesystem::path thumbsDir;
    std::string img1Path;
    std::string img2Path;
    std::string dbPath;
};

TEST_F(ImporterTest, ImportDirectoryAndSkipUnchanged) {
    CatalogDb db;
    ASSERT_TRUE(db.open(dbPath).isOk());
    Cache cache(thumbsDir.string());
    ThreadPool pool(2);

    Importer importer(db, cache, &pool);

    std::atomic<int> progressCalls{0};
    auto progressCb = [&progressCalls](const ImportProgress& prog) {
        progressCalls++;
    };

    auto res = importer.importDirectory(photosDir.string(), true, progressCb);
    ASSERT_TRUE(res.isOk());

    auto prog = res.value();
    EXPECT_EQ(prog.total_files.load(), 2);
    EXPECT_EQ(prog.imported_files.load(), 2);
    EXPECT_EQ(prog.skipped_files.load(), 0);
    EXPECT_EQ(prog.failed_files.load(), 0);
    EXPECT_GT(progressCalls.load(), 0);

    // Verify media items in DB
    auto stats = db.getStats().value();
    EXPECT_EQ(stats.total_media, 2);

    // Run import again without changes -> all should be skipped
    auto res2 = importer.importDirectory(photosDir.string(), true, nullptr);
    ASSERT_TRUE(res2.isOk());
    auto prog2 = res2.value();
    EXPECT_EQ(prog2.total_files.load(), 2);
    EXPECT_EQ(prog2.imported_files.load(), 0);
    EXPECT_EQ(prog2.skipped_files.load(), 2);
    EXPECT_EQ(prog2.failed_files.load(), 0);

    db.close();
}

TEST_F(ImporterTest, CatalogFacadeE2E) {
    Catalog catalog(2);
    ASSERT_TRUE(catalog.open(dbPath, thumbsDir.string()).isOk());
    EXPECT_TRUE(catalog.isOpen());

    auto impRes = catalog.importDirectory(photosDir.string(), true, nullptr);
    ASSERT_TRUE(impRes.isOk());
    EXPECT_EQ(impRes.value().imported_files.load(), 2);

    // Query media
    QueryCriteria criteria;
    criteria.limit = 10;
    auto qRes = catalog.query(criteria);
    ASSERT_TRUE(qRes.isOk());
    EXPECT_EQ(qRes.value().total_count, 2);
    ASSERT_EQ(qRes.value().items.size(), 2);

    MediaId mid = qRes.value().items[0].id;

    // Rating & Flag
    EXPECT_TRUE(catalog.setRating(mid, 5).isOk());
    EXPECT_TRUE(catalog.setFlag(mid, FlagState::Pick).isOk());

    auto media = catalog.getMedia(mid).value();
    EXPECT_EQ(media.rating, 5);
    EXPECT_EQ(media.flag, FlagState::Pick);

    // Tagging
    EXPECT_TRUE(catalog.addTag(mid, "SummerVacation", "events").isOk());
    auto tags = catalog.getTags().value();
    EXPECT_EQ(tags.size(), 1);
    EXPECT_EQ(tags[0].name, "SummerVacation");

    // Stats
    auto stats = catalog.getStats().value();
    EXPECT_EQ(stats.total_media, 2);
    EXPECT_EQ(stats.total_tags, 1);

    catalog.close();
    EXPECT_FALSE(catalog.isOpen());
}
