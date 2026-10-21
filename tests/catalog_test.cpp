#include <gtest/gtest.h>
#include "imagine/core/catalog.hpp"
#include "imagine/core/deduplicator.hpp"
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
    // Renaming when source file does not exist on disk also fails consistently
    EXPECT_FALSE(cat.renameMedia(mid, "somewhere_else").isOk());
    auto mediaAfterMissing = cat.getMedia(mid);
    ASSERT_TRUE(mediaAfterMissing.isOk());
    EXPECT_EQ(mediaAfterMissing.value().file_path, "photo.jpg"); // untouched
}

TEST_F(CatalogClassTest, RenameMediaValidationAndCaseOnly) {
    Catalog cat(1);
    ASSERT_TRUE(cat.open(dbPath, thumbsDir, testDir.string()).isOk());

    auto impRes = cat.importFile(sampleImg);
    ASSERT_TRUE(impRes.isOk());
    MediaId mid = impRes.value().id;

    // Control characters and reserved symbols
    EXPECT_FALSE(cat.renameMedia(mid, "photo<bad>.jpg").isOk());
    EXPECT_FALSE(cat.renameMedia(mid, "photo>bad.jpg").isOk());
    EXPECT_FALSE(cat.renameMedia(mid, "photo|bad.jpg").isOk());
    EXPECT_FALSE(cat.renameMedia(mid, "photo?bad.jpg").isOk());
    EXPECT_FALSE(cat.renameMedia(mid, "photo*bad.jpg").isOk());
    EXPECT_FALSE(cat.renameMedia(mid, "photo\"bad.jpg").isOk());
    std::string ctrlName = "photo\x1f.jpg";
    EXPECT_FALSE(cat.renameMedia(mid, ctrlName).isOk());

    // Case-only rename should succeed on case-insensitive filesystems
    std::string newPath;
    std::string newName;
    EXPECT_TRUE(cat.renameMedia(mid, "PHOTO.jpg", &newPath, &newName).isOk());
    EXPECT_EQ(newName, "PHOTO.jpg");
    auto getRes = cat.getMedia(mid);
    ASSERT_TRUE(getRes.isOk());
    EXPECT_EQ(getRes.value().file_name, "PHOTO.jpg");
}

TEST_F(CatalogClassTest, Pre1970DateTakenFormatted) {
    Catalog cat(1);
    ASSERT_TRUE(cat.open(dbPath, thumbsDir, testDir.string()).isOk());

    auto impRes = cat.importFile(sampleImg);
    ASSERT_TRUE(impRes.isOk());
    MediaId mid = impRes.value().id;

    // 1935-01-01 00:00:00 UTC
    int64_t pre1970 = -1104537600;
    EXPECT_TRUE(cat.setDateTaken(mid, pre1970).isOk());

    auto getRes = cat.getMedia(mid);
    ASSERT_TRUE(getRes.isOk());
    EXPECT_EQ(getRes.value().date_taken, pre1970);
    EXPECT_EQ(getRes.value().exif.date_taken_str, "1935:01:01 00:00:00");
}

TEST_F(CatalogClassTest, ResolvePhotoPathSuffixPrecedence) {
    Catalog cat(1);
    ASSERT_TRUE(cat.open(dbPath, thumbsDir, testDir.string()).isOk());

    // Create root colliding file testDir/IMG_0001.JPG
    std::filesystem::path rootColliding = testDir / "IMG_0001.JPG";
    std::filesystem::copy_file(sampleImg, rootColliding);

    // Create subfolder file testDir/2026/trip/IMG_0001.JPG
    std::filesystem::path subDir = testDir / "2026" / "trip";
    std::filesystem::create_directories(subDir);
    std::filesystem::path targetSubFile = subDir / "IMG_0001.JPG";
    std::filesystem::copy_file(sampleImg, targetSubFile);

    // Resolving legacy path "/old_disk/photos/2026/trip/IMG_0001.JPG"
    // MUST resolve to testDir/2026/trip/IMG_0001.JPG, NOT the root testDir/IMG_0001.JPG!
    std::string resolved = cat.resolvePhotoPath("/old_disk/photos/2026/trip/IMG_0001.JPG");
    EXPECT_EQ(std::filesystem::path(resolved), targetSubFile);
}

TEST_F(CatalogClassTest, DeleteMediaGuardsUnsetPhotosDir) {
    // Open catalog without photosDir
    Catalog cat(1);
    ASSERT_TRUE(cat.open(dbPath, thumbsDir).isOk());

    // Insert an item with absolute path pointing to sampleImg
    MediaItem item;
    item.file_path = sampleImg;
    item.file_name = "photo.jpg";
    item.file_size = 1000;
    item.file_modified_time = 1000;
    item.content_hash = "h_delete_guard";
    item.date_taken = 1000;
    auto insRes = cat.db().insertMedia(item);
    ASSERT_TRUE(insRes.isOk());
    MediaId mid = insRes.value();

    // deleteMedia with deleteFromDisk=true must fail because photosDir is unset
    EXPECT_FALSE(cat.deleteMedia(mid, true).isOk());
    // File on disk must remain intact
    EXPECT_TRUE(std::filesystem::exists(sampleImg));
}

namespace imagine::core {
void setClipEvictionHook(std::function<void(MediaId)> hook);
}

TEST(DeduplicatorTest, KeeperRankingRules) {
    // 1. Flag priority: Pick (1) > Unflagged (0) > Reject (-1)
    {
        MediaItem a; a.id = 10; a.flag = FlagState::Pick;
        MediaItem b; b.id = 5; b.flag = FlagState::Unflagged;
        EXPECT_TRUE(isBetterKeeper(a, b));
        EXPECT_EQ(rankKeeper({a, b}), 10);

        MediaItem c; c.id = 2; c.flag = FlagState::Reject;
        EXPECT_TRUE(isBetterKeeper(b, c));
        EXPECT_EQ(rankKeeper({b, c}), 5);
    }

    // 2. Resolution: larger width * height wins
    {
        MediaItem a; a.id = 1; a.width = 4000; a.height = 3000; // 12MP
        MediaItem b; b.id = 2; b.width = 1920; b.height = 1080; // ~2MP
        EXPECT_TRUE(isBetterKeeper(a, b));
        EXPECT_FALSE(isBetterKeeper(b, a));
        EXPECT_EQ(rankKeeper({b, a}), 1);
    }

    // 3. File size: larger file_size wins
    {
        MediaItem a; a.id = 1; a.file_size = 5000000;
        MediaItem b; b.id = 2; b.file_size = 2000000;
        EXPECT_TRUE(isBetterKeeper(a, b));
        EXPECT_FALSE(isBetterKeeper(b, a));
        EXPECT_EQ(rankKeeper({b, a}), 1);
    }

    // 4. Metadata completeness: caption, GPS, tag count, date_taken != 0 bonus
    {
        MediaItem complete; complete.id = 1;
        complete.caption = "Sunset";
        complete.exif.has_gps = true;
        complete.tags = {Tag{1, "beach", "keyword"}, Tag{2, "summer", "keyword"}};
        complete.date_taken = 1700000000;

        MediaItem sparse; sparse.id = 2;
        sparse.date_taken = 0; // undated

        EXPECT_TRUE(isBetterKeeper(complete, sparse));
        EXPECT_EQ(rankKeeper({sparse, complete}), 1);

        // Pre-1970 negative dates earn the date_taken != 0 bonus
        MediaItem pre1970; pre1970.id = 3;
        pre1970.date_taken = -631152000; // 1950, != 0 bonus
        MediaItem undated; undated.id = 4;
        undated.date_taken = 0; // 0, no bonus
        EXPECT_TRUE(isBetterKeeper(pre1970, undated));
    }

    // 5. Path cleanliness: clean library path outranks messy one
    {
        MediaItem clean; clean.id = 1;
        clean.file_path = "photos/2026/vacation/beach.jpg";

        MediaItem copy; copy.id = 2;
        copy.file_path = "photos/2026/vacation/beach_copy.jpg";

        MediaItem exportItem; exportItem.id = 3;
        exportItem.file_path = "export/beach.jpg";

        MediaItem numItem; numItem.id = 4;
        numItem.file_path = "photos/beach (1).jpg";

        MediaItem urlItem; urlItem.id = 5;
        urlItem.file_path = "photos/beach%20photo.jpg";

        MediaItem copyrightItem; copyrightItem.id = 6;
        copyrightItem.file_path = "photos/copyright.jpg";

        MediaItem tempItem; tempItem.id = 7;
        tempItem.file_path = "photos/temperature.jpg";

        EXPECT_TRUE(isBetterKeeper(clean, copy));
        EXPECT_TRUE(isBetterKeeper(clean, exportItem));
        EXPECT_TRUE(isBetterKeeper(clean, numItem));
        EXPECT_TRUE(isBetterKeeper(clean, urlItem));
        // Path segment check: copyright.jpg and temperature.jpg should not be penalized by "copy" or "temp"
        EXPECT_TRUE(isBetterKeeper(copyrightItem, copy));
        EXPECT_TRUE(isBetterKeeper(tempItem, copy));
    }

    // 6. Earliest-id tiebreak: lowest id wins
    {
        MediaItem a; a.id = 3;
        MediaItem b; b.id = 7;
        EXPECT_TRUE(isBetterKeeper(a, b));
        EXPECT_FALSE(isBetterKeeper(b, a));
        EXPECT_EQ(rankKeeper({b, a}), 3);
    }

    // sortDuplicateGroup puts keeper at index 0
    {
        MediaItem a; a.id = 10; a.file_size = 100;
        MediaItem b; b.id = 20; b.file_size = 500;
        MediaItem c; c.id = 30; c.flag = FlagState::Pick; // wins
        std::vector<MediaItem> group = {a, b, c};
        sortDuplicateGroup(group);
        EXPECT_EQ(group[0].id, 30);
    }
}

TEST_F(CatalogClassTest, SharedThumbnailPreservedOnDelete) {
    Catalog cat(1);
    ASSERT_TRUE(cat.open(dbPath, thumbsDir, testDir.string()).isOk());

    std::string hashShared = "shared_thumb_hash_12345";

    // Item 1
    MediaItem item1;
    item1.file_path = sampleImg;
    item1.file_name = "photo.jpg";
    item1.file_size = 1000;
    item1.content_hash = hashShared;
    item1.date_taken = 1000;
    auto ins1 = cat.db().insertMedia(item1);
    ASSERT_TRUE(ins1.isOk());
    MediaId id1 = ins1.value();

    // Item 2 sharing the same content_hash
    std::string sampleImg2 = (testDir / "photo_dup.jpg").string();
    std::filesystem::copy_file(sampleImg, sampleImg2);
    MediaItem item2;
    item2.file_path = sampleImg2;
    item2.file_name = "photo_dup.jpg";
    item2.file_size = 1000;
    item2.content_hash = hashShared;
    item2.date_taken = 1000;
    auto ins2 = cat.db().insertMedia(item2);
    ASSERT_TRUE(ins2.isOk());
    MediaId id2 = ins2.value();

    // Create thumbnail in cache for hashShared
    ASSERT_TRUE(cat.cache().ensureDualThumbnails(sampleImg, hashShared).isOk());
    EXPECT_TRUE(cat.cache().hasThumbnail(hashShared, 256));

    // Delete item2: since item1 still shares hashShared, thumbnail must be PRESERVED
    ASSERT_TRUE(cat.deleteMedia(id2, false).isOk());
    EXPECT_TRUE(cat.cache().hasThumbnail(hashShared, 256));

    // Delete item1: no more items share hashShared, thumbnail should be CLEARED
    ASSERT_TRUE(cat.deleteMedia(id1, false).isOk());
    EXPECT_FALSE(cat.cache().hasThumbnail(hashShared, 256));
}

TEST_F(CatalogClassTest, HardlinkDeleteEntryOnly) {
    Catalog cat(1);
    ASSERT_TRUE(cat.open(dbPath, thumbsDir, testDir.string()).isOk());

    // Create a hardlink
    std::string hlImg = (testDir / "photo_hardlink.jpg").string();
    std::error_code ec;
    std::filesystem::create_hard_link(sampleImg, hlImg, ec);
    if (ec) {
        GTEST_SKIP() << "Hard links not supported: " << ec.message();
    }

    MediaItem item1;
    item1.file_path = sampleImg;
    item1.file_name = "photo.jpg";
    item1.file_size = 1000;
    item1.content_hash = "h_hl_1";
    auto id1 = cat.db().insertMedia(item1).value();

    MediaItem item2;
    item2.file_path = hlImg;
    item2.file_name = "photo_hardlink.jpg";
    item2.file_size = 1000;
    item2.content_hash = "h_hl_1";
    auto id2 = cat.db().insertMedia(item2).value();

    // Delete item2 from disk: directory entry for item2 is unlinked, item1 on disk stays intact
    ASSERT_TRUE(cat.deleteMedia(id2, true).isOk());
    EXPECT_FALSE(std::filesystem::exists(hlImg));
    EXPECT_TRUE(std::filesystem::exists(sampleImg));
    EXPECT_TRUE(cat.getMedia(id1).isOk());
}

TEST_F(CatalogClassTest, ClipEvictionHookCalledOnDelete) {
    Catalog cat(1);
    ASSERT_TRUE(cat.open(dbPath, thumbsDir, testDir.string()).isOk());

    MediaItem item;
    item.file_path = sampleImg;
    item.file_name = "photo.jpg";
    item.file_size = 1000;
    item.content_hash = "h_clip_evict";
    auto id = cat.db().insertMedia(item).value();

    MediaId evictedId = 0;
    setClipEvictionHook([&evictedId](MediaId mid) {
        evictedId = mid;
    });

    ASSERT_TRUE(cat.deleteMedia(id, false).isOk());
    EXPECT_EQ(evictedId, id);

    // Reset hook
    setClipEvictionHook(nullptr);
}
