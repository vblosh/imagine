#include <gtest/gtest.h>
#include "imagine/core/catalog.hpp"
#include "imagine/thumbnail/generator.hpp"
#include <filesystem>

using namespace imagine;
using namespace imagine::core;
using namespace imagine::thumbnail;

class CatalogClassTest : public ::testing::Test {
protected:
    void SetUp() override {
        testDir = std::filesystem::temp_directory_path() / "imagine_catalog_class_test";
        std::filesystem::create_directories(testDir);
        dbPath = (testDir / "catalog.db").string();
        thumbsDir = (testDir / "thumbs").string();

        // Create sample image
        sampleImg = (testDir / "photo.jpg").string();
        ImageBuffer buf;
        buf.width = 200;
        buf.height = 100;
        buf.channels = 3;
        buf.data.resize(200 * 100 * 3, 150);
        ASSERT_TRUE(Generator::saveJpeg(buf, sampleImg).isOk());
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
    }

    std::filesystem::path testDir;
    std::string dbPath;
    std::string thumbsDir;
    std::string sampleImg;
};

TEST_F(CatalogClassTest, OperationsWhenClosedReturnError) {
    Catalog cat(1);
    EXPECT_FALSE(cat.isOpen());

    // All operations should return "Catalog is not open"
    EXPECT_FALSE(cat.importDirectory(testDir.string(), true).isOk());
    EXPECT_FALSE(cat.importFile(sampleImg).isOk());
    cat.cancelImport(); // no-op

    QueryCriteria qc;
    EXPECT_FALSE(cat.query(qc).isOk());
    EXPECT_FALSE(cat.getMedia(1).isOk());
    EXPECT_FALSE(cat.getMediaByPath(sampleImg).isOk());
    EXPECT_FALSE(cat.getMediaByHash("hash").isOk());
    EXPECT_FALSE(cat.setRating(1, 4).isOk());
    EXPECT_FALSE(cat.setFlag(1, FlagState::Pick).isOk());
    EXPECT_FALSE(cat.deleteMedia(1).isOk());
    EXPECT_FALSE(cat.addTag(1, "tag", "category").isOk());
    EXPECT_FALSE(cat.addTag(1, 10).isOk());
    EXPECT_FALSE(cat.removeTag(1, 10).isOk());
    EXPECT_FALSE(cat.deleteTag(10).isOk());
    EXPECT_FALSE(cat.createOrGetTag("tag", "cat").isOk());
    EXPECT_FALSE(cat.getTags().isOk());
    EXPECT_FALSE(cat.getTagsForMedia(1).isOk());
    EXPECT_FALSE(cat.getTagsForMediaBatch({1}).isOk());
    EXPECT_FALSE(cat.createAlbum("A", "D").isOk());
    EXPECT_FALSE(cat.getAlbums().isOk());
    EXPECT_FALSE(cat.getAlbum(1).isOk());
    EXPECT_FALSE(cat.deleteAlbum(1).isOk());
    EXPECT_FALSE(cat.addMediaToAlbum(1, 1).isOk());
    EXPECT_FALSE(cat.removeMediaFromAlbum(1, 1).isOk());
    EXPECT_FALSE(cat.getMediaInAlbum(1).isOk());
    EXPECT_FALSE(cat.getTimeline().isOk());
    EXPECT_FALSE(cat.getStats().isOk());

    // Accessors throw runtime_error when closed
    EXPECT_THROW(cat.db(), std::runtime_error);
    EXPECT_THROW(cat.cache(), std::runtime_error);
    EXPECT_THROW(cat.threadPool(), std::runtime_error);
    EXPECT_THROW(cat.importer(), std::runtime_error);

    const Catalog& constCat = cat;
    EXPECT_THROW(constCat.db(), std::runtime_error);
    EXPECT_THROW(constCat.cache(), std::runtime_error);
}

TEST_F(CatalogClassTest, OpenLifecycleAndFullOperations) {
    Catalog cat(2);
    ASSERT_TRUE(cat.open(dbPath, thumbsDir).isOk());
    EXPECT_TRUE(cat.isOpen());

    // Re-open when already open
    ASSERT_TRUE(cat.open(dbPath, thumbsDir).isOk());
    EXPECT_TRUE(cat.isOpen());

    // Accessors
    EXPECT_NO_THROW(cat.db());
    EXPECT_NO_THROW(cat.cache());
    EXPECT_NO_THROW(cat.threadPool());
    EXPECT_NO_THROW(cat.importer());
    const Catalog& constCat = cat;
    EXPECT_NO_THROW(constCat.db());
    EXPECT_NO_THROW(constCat.cache());

    // importFile
    auto impRes = cat.importFile(sampleImg);
    ASSERT_TRUE(impRes.isOk());
    MediaId mid = impRes.value().id;
    std::string hash = impRes.value().content_hash;

    // getMediaByPath and getMediaByHash
    auto byPath = cat.getMediaByPath(sampleImg);
    ASSERT_TRUE(byPath.isOk());
    EXPECT_EQ(byPath.value().id, mid);

    auto byHash = cat.getMediaByHash(hash);
    ASSERT_TRUE(byHash.isOk());
    EXPECT_EQ(byHash.value().id, mid);

    // Tagging operations
    auto tagIdRes = cat.createOrGetTag("Mountain", "nature");
    ASSERT_TRUE(tagIdRes.isOk());
    TagId tid = tagIdRes.value();

    EXPECT_TRUE(cat.addTag(mid, tid).isOk());

    auto tagsForMedia = cat.getTagsForMedia(mid);
    ASSERT_TRUE(tagsForMedia.isOk());
    EXPECT_EQ(tagsForMedia.value().size(), 1u);

    auto batchTags = cat.getTagsForMediaBatch({mid});
    ASSERT_TRUE(batchTags.isOk());
    EXPECT_EQ(batchTags.value()[mid].size(), 1u);

    EXPECT_TRUE(cat.removeTag(mid, tid).isOk());
    EXPECT_TRUE(cat.deleteTag(tid).isOk());

    // Album operations
    auto aidRes = cat.createAlbum("My Album", "Description");
    ASSERT_TRUE(aidRes.isOk());
    AlbumId aid = aidRes.value();

    auto albums = cat.getAlbums();
    ASSERT_TRUE(albums.isOk());
    EXPECT_EQ(albums.value().size(), 1u);

    auto singleAlbum = cat.getAlbum(aid);
    ASSERT_TRUE(singleAlbum.isOk());
    EXPECT_EQ(singleAlbum.value().name, "My Album");

    EXPECT_TRUE(cat.addMediaToAlbum(aid, mid, 0).isOk());
    auto inAlbum = cat.getMediaInAlbum(aid);
    ASSERT_TRUE(inAlbum.isOk());
    EXPECT_EQ(inAlbum.value().size(), 1u);

    EXPECT_TRUE(cat.removeMediaFromAlbum(aid, mid).isOk());
    EXPECT_TRUE(cat.deleteAlbum(aid).isOk());

    // Timeline
    auto tl = cat.getTimeline();
    ASSERT_TRUE(tl.isOk());

    // deleteMedia
    EXPECT_TRUE(cat.deleteMedia(mid).isOk());
    EXPECT_FALSE(cat.getMedia(mid).isOk());

    // cancelImport
    cat.cancelImport();

    // Close
    cat.close();
    EXPECT_FALSE(cat.isOpen());
    // Calling close again is safe
    cat.close();
}

TEST_F(CatalogClassTest, DeleteMediaWithDiskDeletion) {
    Catalog cat(1);
    ASSERT_TRUE(cat.open(dbPath, thumbsDir, testDir.string()).isOk());

    auto impRes = cat.importFile(sampleImg);
    ASSERT_TRUE(impRes.isOk());
    MediaId mid = impRes.value().id;

    std::string sampleImg2 = (testDir / "photo2.jpg").string();
    std::filesystem::copy_file(sampleImg, sampleImg2);
    auto impRes2 = cat.importFile(sampleImg2);
    ASSERT_TRUE(impRes2.isOk());
    MediaId mid2 = impRes2.value().id;

    // Delete mid2 without disk deletion
    EXPECT_TRUE(std::filesystem::exists(sampleImg2));
    EXPECT_TRUE(cat.deleteMedia(mid2, false).isOk());
    EXPECT_FALSE(cat.getMedia(mid2).isOk());
    EXPECT_TRUE(std::filesystem::exists(sampleImg2));

    // Delete mid with disk deletion
    EXPECT_TRUE(std::filesystem::exists(sampleImg));
    EXPECT_TRUE(cat.deleteMedia(mid, true).isOk());
    EXPECT_FALSE(cat.getMedia(mid).isOk());
    EXPECT_FALSE(std::filesystem::exists(sampleImg));
}

TEST_F(CatalogClassTest, RenameMediaAndDateTaken) {
    Catalog cat(1);
    ASSERT_TRUE(cat.open(dbPath, thumbsDir, testDir.string()).isOk());

    auto impRes = cat.importFile(sampleImg);
    ASSERT_TRUE(impRes.isOk());
    MediaId mid = impRes.value().id;

    // Test invalid rename
    EXPECT_FALSE(cat.renameMedia(mid, "").isOk());
    EXPECT_FALSE(cat.renameMedia(mid, "../escape.jpg").isOk());
    EXPECT_FALSE(cat.renameMedia(mid, "dir/name.jpg").isOk());

    // Successful rename without extension (preserves extension)
    std::string newPath;
    std::string newName;
    EXPECT_TRUE(cat.renameMedia(mid, "renamed_photo", &newPath, &newName).isOk());
    EXPECT_EQ(newName, "renamed_photo.jpg");

    // Verify on disk and in database
    auto mediaRes = cat.getMedia(mid);
    ASSERT_TRUE(mediaRes.isOk());
    EXPECT_EQ(mediaRes.value().file_name, "renamed_photo.jpg");
    EXPECT_FALSE(std::filesystem::exists(sampleImg));
    EXPECT_TRUE(std::filesystem::exists(testDir / "renamed_photo.jpg"));

    // Rename with explicit extension
    EXPECT_TRUE(cat.renameMedia(mid, "landscape.jpg", &newPath, &newName).isOk());
    EXPECT_EQ(newName, "landscape.jpg");
    EXPECT_TRUE(std::filesystem::exists(testDir / "landscape.jpg"));

    // Test setDateTaken
    int64_t newTs = 1718000000;
    EXPECT_TRUE(cat.setDateTaken(mid, newTs).isOk());
    auto mediaRes2 = cat.getMedia(mid);
    ASSERT_TRUE(mediaRes2.isOk());
    EXPECT_EQ(mediaRes2.value().date_taken, newTs);
    EXPECT_FALSE(mediaRes2.value().exif.date_taken_str.empty());
}

TEST_F(CatalogClassTest, MoveMediaToNewPath) {
    Catalog cat(1);
    ASSERT_TRUE(cat.open(dbPath, thumbsDir, testDir.string()).isOk());

    auto impRes = cat.importFile(sampleImg);
    ASSERT_TRUE(impRes.isOk());
    MediaId mid = impRes.value().id;

    // Invalid destination path
    EXPECT_FALSE(cat.moveMedia(mid, "").isOk());

    // Move to subfolder
    std::string newPath;
    EXPECT_TRUE(cat.moveMedia(mid, "vacation/summer", &newPath).isOk());
    EXPECT_EQ(newPath, "vacation/summer/photo.jpg");

    // Verify on disk
    EXPECT_FALSE(std::filesystem::exists(sampleImg));
    std::filesystem::path expectedDiskPath = testDir / "vacation" / "summer" / "photo.jpg";
    EXPECT_TRUE(std::filesystem::exists(expectedDiskPath));

    // Verify in catalog database
    auto mediaRes = cat.getMedia(mid);
    ASSERT_TRUE(mediaRes.isOk());
    EXPECT_EQ(mediaRes.value().file_path, "vacation/summer/photo.jpg");
    EXPECT_EQ(mediaRes.value().file_name, "photo.jpg");

    // Folders list should include the new folder
    auto foldersRes = cat.getFolders();
    ASSERT_TRUE(foldersRes.isOk());
    bool foundFolder = false;
    for (const auto& f : foldersRes.value()) {
        if (f == "vacation/summer") foundFolder = true;
    }
    EXPECT_TRUE(foundFolder);

    // Moving to same path is ok (no-op)
    EXPECT_TRUE(cat.moveMedia(mid, "vacation/summer", &newPath).isOk());
    EXPECT_EQ(newPath, "vacation/summer/photo.jpg");

    // Moving with leading slash should treat it as relative to photos root
    EXPECT_TRUE(cat.moveMedia(mid, "/winter", &newPath).isOk());
    EXPECT_EQ(newPath, "winter/photo.jpg");
    EXPECT_TRUE(std::filesystem::exists(testDir / "winter" / "photo.jpg"));

    // Moving with leading slash multi-level
    EXPECT_TRUE(cat.moveMedia(mid, "/spring/blossom", &newPath).isOk());
    EXPECT_EQ(newPath, "spring/blossom/photo.jpg");
    EXPECT_TRUE(std::filesystem::exists(testDir / "spring" / "blossom" / "photo.jpg"));

    // Moving with root slash "/" should move back to root
    EXPECT_TRUE(cat.moveMedia(mid, "/", &newPath).isOk());
    EXPECT_EQ(newPath, "photo.jpg");
    EXPECT_TRUE(std::filesystem::exists(testDir / "photo.jpg"));

    // Moving back to root folder via "."
    EXPECT_TRUE(cat.moveMedia(mid, ".", &newPath).isOk());
    EXPECT_EQ(newPath, "photo.jpg");
    EXPECT_TRUE(std::filesystem::exists(testDir / "photo.jpg"));

    // Moving outside photos directory is strictly rejected
    EXPECT_FALSE(cat.moveMedia(mid, "../outside_folder").isOk());
    EXPECT_FALSE(cat.moveMedia(mid, "../../outside_folder").isOk());
    std::filesystem::path outsideDir = testDir.parent_path() / "unauthorized_outside";
    EXPECT_FALSE(cat.moveMedia(mid, outsideDir.string()).isOk());

    // Moving when source file does not exist on disk fails and does NOT corrupt DB
    std::filesystem::remove(testDir / "photo.jpg");
    EXPECT_FALSE(cat.moveMedia(mid, "somewhere").isOk());
    auto mediaAfterMissing = cat.getMedia(mid);
    ASSERT_TRUE(mediaAfterMissing.isOk());
    EXPECT_EQ(mediaAfterMissing.value().file_path, "photo.jpg"); // untouched
}


