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

TEST_F(ImporterTest, SupportedExtensionsAndProgressJson) {
    EXPECT_TRUE(Importer::isSupportedExtension("test.JPG"));
    EXPECT_TRUE(Importer::isSupportedExtension("test.jpeg"));
    EXPECT_TRUE(Importer::isSupportedExtension("test.PNG"));
    EXPECT_TRUE(Importer::isSupportedExtension("test.bmp"));
    EXPECT_TRUE(Importer::isSupportedExtension("test.webp"));
    EXPECT_TRUE(Importer::isSupportedExtension("test.tiff"));
    EXPECT_TRUE(Importer::isSupportedExtension("test.tif"));
    EXPECT_FALSE(Importer::isSupportedExtension("test.txt"));
    EXPECT_FALSE(Importer::isSupportedExtension("test.pdf"));

    ImportProgress prog;
    prog.total_files.store(10);
    prog.processed_files.store(7);
    prog.imported_files.store(5);
    prog.skipped_files.store(1);
    prog.failed_files.store(1);
    prog.current_file = "/photos/test.jpg";
    prog.is_running = true;

    nlohmann::json j = prog;
    EXPECT_EQ(j["total_files"], 10);
    EXPECT_EQ(j["processed_files"], 7);
    EXPECT_EQ(j["imported_files"], 5);
    EXPECT_EQ(j["skipped_files"], 1);
    EXPECT_EQ(j["failed_files"], 1);
    EXPECT_EQ(j["current_file"], "/photos/test.jpg");
    EXPECT_TRUE(j["is_running"]);

    ImportProgress d = j.get<ImportProgress>();
    EXPECT_EQ(d.total_files.load(), 10);
    EXPECT_EQ(d.processed_files.load(), 7);
    EXPECT_EQ(d.imported_files.load(), 5);
    EXPECT_EQ(d.skipped_files.load(), 1);
    EXPECT_EQ(d.failed_files.load(), 1);
    EXPECT_EQ(d.current_file, "/photos/test.jpg");
    EXPECT_TRUE(d.is_running);
}

TEST_F(ImporterTest, ImportSingleFileAndUpdate) {
    CatalogDb db;
    ASSERT_TRUE(db.open(dbPath).isOk());
    Cache cache(thumbsDir.string());
    Importer importer(db, cache, nullptr);

    // Unsupported extension
    auto badExt = importer.importFile("invalid.pdf");
    EXPECT_FALSE(badExt.isOk());
    EXPECT_EQ(badExt.status().code(), StatusCode::InvalidArgument);

    // Non-existent file
    auto missing = importer.importFile((photosDir / "missing.jpg").string());
    EXPECT_FALSE(missing.isOk());
    EXPECT_EQ(missing.status().code(), StatusCode::InternalError);

    // Valid single file import
    auto validRes = importer.importFile(img1Path);
    ASSERT_TRUE(validRes.isOk());
    EXPECT_EQ(validRes.value().file_name, "img1.jpg");
    EXPECT_EQ(validRes.value().width, 300);
    EXPECT_EQ(validRes.value().height, 200);

    // Re-importing after modifying file content triggers update
    {
        std::ofstream ofs(img1Path, std::ios::binary | std::ios::app);
        ofs << "extra_bytes";
    }
    auto updateRes = importer.importFile(img1Path);
    ASSERT_TRUE(updateRes.isOk());
    EXPECT_EQ(updateRes.value().id, validRes.value().id);
}

TEST_F(ImporterTest, SynchronousAndNonRecursiveAndCorrupted) {
    CatalogDb db;
    ASSERT_TRUE(db.open(dbPath).isOk());
    Cache cache(thumbsDir.string());
    // pool is nullptr -> exercises synchronous path in importDirectory
    Importer importer(db, cache, nullptr);

    EXPECT_FALSE(importer.isRunning());
    EXPECT_FALSE(importer.isCancelled());

    // Non-existent directory
    auto noDir = importer.importDirectory((testDir / "nonexistent").string(), true, nullptr);
    EXPECT_FALSE(noDir.isOk());
    EXPECT_EQ(noDir.status().code(), StatusCode::NotFound);

    // Add invalid/empty image to test failed_files counter
    std::string corruptPath = (photosDir / "corrupted.jpg").string();
    {
        std::ofstream ofs(corruptPath); // 0-byte file fails validation
    }

    // Subdirectory to test non-recursive mode
    auto subDir = photosDir / "sub";
    std::filesystem::create_directories(subDir);
    ImageBuffer buf;
    buf.width = 100;
    buf.height = 100;
    buf.channels = 3;
    buf.data.resize(100 * 100 * 3, 200);
    ASSERT_TRUE(Generator::saveJpeg(buf, (subDir / "sub.jpg").string()).isOk());

    // Non-recursive import (recursive = false)
    auto progRes = importer.importDirectory(photosDir.string(), false, nullptr);
    ASSERT_TRUE(progRes.isOk());
    auto prog = progRes.value();
    // In photosDir: img1.jpg, img2.jpg, corrupted.jpg (sub/sub.jpg skipped because recursive = false)
    EXPECT_EQ(prog.total_files.load(), 3);
    EXPECT_EQ(prog.imported_files.load(), 2);
    EXPECT_EQ(prog.failed_files.load(), 1);

    // Verify currentProgress
    auto current = importer.currentProgress();
    EXPECT_FALSE(current.is_running);

    // Cancellation
    importer.cancel();
    EXPECT_TRUE(importer.isCancelled());
}

TEST_F(ImporterTest, MoveSemantics) {
    static_assert(std::is_move_constructible_v<ImportProgress>, "ImportProgress must be move constructible");
    static_assert(std::is_move_assignable_v<ImportProgress>, "ImportProgress must be move assignable");

    ImportProgress orig;
    orig.total_files = 100;
    orig.processed_files = 80;
    orig.imported_files = 70;
    orig.skipped_files = 5;
    orig.failed_files = 3;
    orig.cancelled_files = 2;
    orig.current_file = "/photos/test.jpg";
    orig.is_running = true;

    // Move construct
    ImportProgress moved(std::move(orig));
    EXPECT_EQ(moved.total_files.load(), 100);
    EXPECT_EQ(moved.processed_files.load(), 80);
    EXPECT_EQ(moved.imported_files.load(), 70);
    EXPECT_EQ(moved.skipped_files.load(), 5);
    EXPECT_EQ(moved.failed_files.load(), 3);
    EXPECT_EQ(moved.cancelled_files.load(), 2);
    EXPECT_EQ(moved.current_file, "/photos/test.jpg");
    EXPECT_TRUE(moved.is_running);

    // Move assign
    ImportProgress assigned;
    assigned = std::move(moved);
    EXPECT_EQ(assigned.total_files.load(), 100);
    EXPECT_EQ(assigned.imported_files.load(), 70);
}

TEST_F(ImporterTest, CallbackNoDeadlockOnCurrentProgress) {
    CatalogDb db;
    ASSERT_TRUE(db.open(dbPath).isOk());
    Cache cache(thumbsDir.string());
    ThreadPool pool(2);
    Importer importer(db, cache, &pool);

    std::atomic<int> callbackInvocations{0};
    // The callback calls importer.currentProgress() inside, which would deadlock
    // if progress_mutex_ were held during callback execution.
    auto progressCb = [&importer, &callbackInvocations](const ImportProgress& prog) {
        callbackInvocations++;
        auto cur = importer.currentProgress();
        EXPECT_GE(cur.total_files.load(), 0);
    };

    auto res = importer.importDirectory(photosDir.string(), true, progressCb);
    ASSERT_TRUE(res.isOk());
    EXPECT_GT(callbackInvocations.load(), 0);
    EXPECT_EQ(res.value().imported_files.load(), 2);
    db.close();
}

TEST_F(ImporterTest, CancellationAccounting) {
    CatalogDb db;
    ASSERT_TRUE(db.open(dbPath).isOk());
    Cache cache(thumbsDir.string());
    Importer importer(db, cache, nullptr);

    // Create 10 dummy photos to test cancellation accounting
    for (int i = 3; i <= 10; ++i) {
        ImageBuffer buf;
        buf.width = 50;
        buf.height = 50;
        buf.channels = 3;
        buf.data.resize(50 * 50 * 3, static_cast<uint8_t>(i * 10));
        ASSERT_TRUE(Generator::saveJpeg(buf, (photosDir / ("img" + std::to_string(i) + ".jpg")).string()).isOk());
    }

    // Cancel during initial progress callback
    std::atomic<bool> cancelledOnce{false};
    auto progressCb = [&importer, &cancelledOnce](const ImportProgress& prog) {
        if (!cancelledOnce.exchange(true)) {
            importer.cancel();
        }
    };

    auto res = importer.importDirectory(photosDir.string(), true, progressCb);
    ASSERT_TRUE(res.isOk());
    auto prog = res.value();
    EXPECT_EQ(prog.total_files.load(), 10);
    EXPECT_EQ(prog.cancelled_files.load(), 10);
    EXPECT_EQ(prog.processed_files.load(), 10);
    EXPECT_EQ(prog.imported_files.load(), 0);
    EXPECT_EQ(prog.imported_files.load() + prog.skipped_files.load() +
              prog.failed_files.load() + prog.cancelled_files.load(),
              prog.total_files.load());
    db.close();
}

TEST_F(ImporterTest, PathNormalization) {
    std::string relPath = (photosDir / "." / "img1.jpg").string();
    std::string norm = Importer::normalizePath(relPath);
    EXPECT_NE(norm.find("img1.jpg"), std::string::npos);
    EXPECT_EQ(norm.find("/./"), std::string::npos);
}

TEST_F(ImporterTest, ThreadedCancellationMidwayAccounting) {
    CatalogDb db;
    ASSERT_TRUE(db.open(dbPath).isOk());
    Cache cache(thumbsDir.string());
    ThreadPool pool(2);
    Importer importer(db, cache, &pool);

    // Create 20 photos
    for (int i = 3; i <= 20; ++i) {
        ImageBuffer buf;
        buf.width = 50;
        buf.height = 50;
        buf.channels = 3;
        buf.data.resize(50 * 50 * 3, static_cast<uint8_t>(i * 5));
        ASSERT_TRUE(Generator::saveJpeg(buf, (photosDir / ("img" + std::to_string(i) + ".jpg")).string()).isOk());
    }

    // Deterministically trigger cancellation as soon as 2 tasks finish
    std::atomic<int> processedCount{0};
    importer.setFileHook([&importer, &processedCount](const std::string&) {
        if (++processedCount == 2) {
            importer.cancel();
        }
    });

    auto res = importer.importDirectory(photosDir.string(), true, nullptr);

    ASSERT_TRUE(res.isOk());
    auto prog = res.value();
    EXPECT_EQ(prog.total_files.load(), 20);
    EXPECT_EQ(prog.processed_files.load(), 20);
    EXPECT_GT(prog.cancelled_files.load(), 0);
    EXPECT_EQ(prog.imported_files.load() + prog.skipped_files.load() +
              prog.failed_files.load() + prog.cancelled_files.load(),
              prog.total_files.load());
    db.close();
}

TEST_F(ImporterTest, CommitToDbFalseAndBatchPersistence) {
    CatalogDb db;
    ASSERT_TRUE(db.open(dbPath).isOk());
    Cache cache(thumbsDir.string());
    Importer importer(db, cache, nullptr);

    // Import initial file
    auto res1 = importer.importFile(img1Path);
    ASSERT_TRUE(res1.isOk());
    MediaId origId = res1.value().id;

    // Modify img1 on disk
    {
        std::ofstream ofs(img1Path, std::ios::binary | std::ios::app);
        ofs << "modified_bytes_appended";
    }

    auto mediaBefore = db.getMediaById(origId).value();
    int64_t origSize = mediaBefore.file_size;

    // Import directory - verifies updates are batched and persisted properly
    auto impRes = importer.importDirectory(photosDir.string(), true, nullptr);
    ASSERT_TRUE(impRes.isOk());

    auto mediaAfter = db.getMediaById(origId).value();
    EXPECT_GT(mediaAfter.file_size, origSize);
    EXPECT_EQ(mediaAfter.id, origId);

    db.close();
}

TEST_F(ImporterTest, RootContainmentAndRelativePaths) {
    CatalogDb db;
    ASSERT_TRUE(db.open(dbPath).isOk());
    Cache cache(thumbsDir.string());
    Importer importer(db, cache, nullptr, photosDir.string());

    // 1. Create subfolder with image
    auto subDir = photosDir / "vacation" / "2026";
    std::filesystem::create_directories(subDir);
    std::string subImgPath = (subDir / "beach.jpg").string();
    {
        std::ofstream ofs(subImgPath, std::ios::binary);
        ofs << "RIFF"; // dummy content
    }
    // Make it a valid JPEG so importer accepts it
    ImageBuffer buf;
    buf.width = 100;
    buf.height = 100;
    buf.channels = 3;
    buf.data.resize(100 * 100 * 3, 200);
    ASSERT_TRUE(Generator::saveJpeg(buf, subImgPath).isOk());

    // 2. Create image outside photosDir
    std::string outsideImg = (testDir / "outside.jpg").string();
    ASSERT_TRUE(Generator::saveJpeg(buf, outsideImg).isOk());

    // 3. Attempting to import file outside photos root should be rejected
    auto badFileRes = importer.importFile(outsideImg);
    EXPECT_FALSE(badFileRes.isOk());

    // 4. Attempting to import directory outside photos root should be rejected
    auto badDirRes = importer.importDirectory(testDir.string(), true);
    EXPECT_FALSE(badDirRes.isOk());

    // 5. Import photosDir
    auto impRes = importer.importDirectory(photosDir.string(), true);
    ASSERT_TRUE(impRes.isOk());

    // 6. Verify stored paths in DB are relative to photosDir
    auto stats = db.getStats().value();
    EXPECT_EQ(stats.total_media, 3); // img1, img2, vacation/2026/beach.jpg

    auto beachRes = db.getMediaByPath("vacation/2026/beach.jpg");
    ASSERT_TRUE(beachRes.isOk());
    EXPECT_EQ(beachRes.value().file_path, "vacation/2026/beach.jpg");
    EXPECT_EQ(beachRes.value().file_name, "beach.jpg");
    EXPECT_EQ(beachRes.value().thumb_small.find("thumbs"), std::string::npos);
    EXPECT_NE(beachRes.value().thumb_small.find("_256.jpg"), std::string::npos);

    auto img1Res = db.getMediaByPath("img1.jpg");
    ASSERT_TRUE(img1Res.isOk());
    EXPECT_EQ(img1Res.value().file_path, "img1.jpg");

    db.close();
}

TEST_F(ImporterTest, WindowsRelativePathCompatibility) {
    std::filesystem::path root = "/photos/library";
    std::filesystem::path sub = "/photos/library/summer/2026/pic.jpg";
    std::filesystem::path outside = "/other/folder/pic.jpg";

    EXPECT_TRUE(Importer::isInsideRootDir(sub, root));
    EXPECT_FALSE(Importer::isInsideRootDir(outside, root));
    EXPECT_FALSE(Importer::isInsideRootDir(root, root));

    std::string rel = Importer::toRelativePath(sub, root);
    EXPECT_EQ(rel, "summer/2026/pic.jpg");
    EXPECT_EQ(rel.find('\\'), std::string::npos); // Guaranteed forward slashes

    std::string relOutside = Importer::toRelativePath(outside, root);
    EXPECT_EQ(relOutside, outside.generic_string());
}
