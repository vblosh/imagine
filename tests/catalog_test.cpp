#include <gtest/gtest.h>
#include "imagine/core/catalog.hpp"
#include "imagine/core/deduplicator.hpp"
#include "imagine/thumbnail/generator.hpp"
#include <filesystem>
#include <fstream>

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

TEST_F(CatalogClassTest, DeletePreservesAnOriginalReferencedByAnAbsoluteAlias) {
    Catalog cat(1);
    ASSERT_TRUE(cat.open(dbPath, thumbsDir, testDir.string()).isOk());
    const auto imported = cat.importFile(sampleImg);
    ASSERT_TRUE(imported.isOk());

    auto legacy = imported.value();
    legacy.file_path = sampleImg;
    ASSERT_TRUE(cat.db().insertMedia(legacy).isOk());

    auto deleted = cat.deleteMediaWithResult(imported.value().id, true);
    ASSERT_TRUE(deleted.isOk()) << deleted.status().message();
    EXPECT_FALSE(deleted.value().deleted_from_disk);
    EXPECT_TRUE(deleted.value().disk_error.empty());
    EXPECT_TRUE(cat.getMedia(legacy.id).isOk());
    EXPECT_TRUE(std::filesystem::exists(sampleImg));
}

TEST_F(CatalogClassTest, DeletePreservesAnOriginalReferencedWithDifferentPathCasing) {
    const auto uppercase = testDir / "PHOTO.JPG";
    if (!std::filesystem::exists(uppercase)) {
        GTEST_SKIP() << "This test needs a case-insensitive photo directory";
    }
    Catalog cat(1);
    ASSERT_TRUE(cat.open(dbPath, thumbsDir, testDir.string()).isOk());
    const auto imported = cat.importFile(sampleImg);
    ASSERT_TRUE(imported.isOk());

    auto alias = imported.value();
    alias.file_path = uppercase.filename().string();
    ASSERT_TRUE(cat.db().insertMedia(alias).isOk());

    auto deleted = cat.deleteMediaWithResult(imported.value().id, true);
    ASSERT_TRUE(deleted.isOk()) << deleted.status().message();
    EXPECT_FALSE(deleted.value().deleted_from_disk);
    EXPECT_TRUE(cat.getMedia(alias.id).isOk());
    EXPECT_TRUE(std::filesystem::exists(sampleImg));
}

TEST_F(CatalogClassTest, DeleteRefreshesOriginalReferencesAfterAnotherConnectionAddsAnAlias) {
    Catalog cat(1);
    ASSERT_TRUE(cat.open(dbPath, thumbsDir, testDir.string()).isOk());
    const auto imported = cat.importFile(sampleImg);
    ASSERT_TRUE(imported.isOk());
    const auto warmupPath = testDir / "warmup.jpg";
    std::filesystem::copy_file(sampleImg, warmupPath);
    const auto warmup = cat.importFile(warmupPath.string());
    ASSERT_TRUE(warmup.isOk());
    ASSERT_TRUE(cat.deleteMedia(warmup.value().id, true).isOk());

    db::CatalogDb writer;
    ASSERT_TRUE(writer.open(dbPath).isOk());
    auto alias = imported.value();
    alias.file_path = sampleImg;
    ASSERT_TRUE(writer.insertMedia(alias).isOk());

    const auto deleted = cat.deleteMediaWithResult(imported.value().id, true);
    ASSERT_TRUE(deleted.isOk()) << deleted.status().message();
    EXPECT_FALSE(deleted.value().deleted_from_disk);
    EXPECT_TRUE(cat.getMedia(alias.id).isOk());
    EXPECT_TRUE(std::filesystem::exists(sampleImg));
}

TEST_F(CatalogClassTest, DeleteChecksForFilesystemAliasesCreatedAfterIndexing) {
    Catalog cat(1);
    ASSERT_TRUE(cat.open(dbPath, thumbsDir, testDir.string()).isOk());
    const auto imported = cat.importFile(sampleImg);
    ASSERT_TRUE(imported.isOk());
    const auto aliasDir = testDir / "aliases";
    std::filesystem::create_directory(aliasDir);
    const auto aliasPath = aliasDir / "reference.jpg";
    auto alias = imported.value();
    alias.file_path = "aliases/reference.jpg";
    ASSERT_TRUE(cat.db().insertMedia(alias).isOk());
    const auto warmupPath = testDir / "warmup.jpg";
    std::filesystem::copy_file(sampleImg, warmupPath);
    const auto warmup = cat.importFile(warmupPath.string());
    ASSERT_TRUE(warmup.isOk());
    ASSERT_TRUE(cat.deleteMedia(warmup.value().id, true).isOk());

    std::error_code ec;
    std::filesystem::create_symlink(sampleImg, aliasPath, ec);
    if (ec) GTEST_SKIP() << "Cannot create a file symlink: " << ec.message();

    const auto deleted = cat.deleteMediaWithResult(imported.value().id, true);
    ASSERT_TRUE(deleted.isOk()) << deleted.status().message();
    EXPECT_FALSE(deleted.value().deleted_from_disk);
    EXPECT_TRUE(cat.getMedia(alias.id).isOk());
    EXPECT_TRUE(std::filesystem::exists(aliasPath));
}

TEST_F(CatalogClassTest, DeleteDoesNotGuessAnOfflineOriginalsLocation) {
    Catalog cat(1);
    ASSERT_TRUE(cat.open(dbPath, thumbsDir, testDir.string()).isOk());
    const auto imported = cat.importFile(sampleImg);
    ASSERT_TRUE(imported.isOk());

    auto legacy = imported.value();
    legacy.file_path = (testDir / "offline" / "photo.jpg").string();
    ASSERT_TRUE(cat.db().insertMedia(legacy).isOk());
    ASSERT_EQ(pathFromUtf8(cat.resolvePhotoPath(legacy.file_path)), pathFromUtf8(sampleImg));

    auto deleted = cat.deleteMediaWithResult(legacy.id, true);
    EXPECT_FALSE(deleted.isOk());
    EXPECT_TRUE(cat.getMedia(legacy.id).isOk());
    EXPECT_TRUE(std::filesystem::exists(sampleImg));
}

#ifdef _WIN32
TEST_F(CatalogClassTest, DeleteKeepsTheCatalogRowWhenAnOriginalIsReadOnly) {
    Catalog cat(1);
    ASSERT_TRUE(cat.open(dbPath, thumbsDir, testDir.string()).isOk());
    const auto imported = cat.importFile(sampleImg);
    ASSERT_TRUE(imported.isOk());
    const auto writeBits = std::filesystem::perms::owner_write | std::filesystem::perms::group_write |
                           std::filesystem::perms::others_write;
    std::error_code ec;
    std::filesystem::permissions(sampleImg, writeBits, std::filesystem::perm_options::remove, ec);
    ASSERT_FALSE(ec);

    auto deleted = cat.deleteMediaWithResult(imported.value().id, true);
    EXPECT_EQ(deleted.status().code(), StatusCode::IoError);
    EXPECT_TRUE(cat.getMedia(imported.value().id).isOk());
    EXPECT_TRUE(std::filesystem::exists(sampleImg));
    std::filesystem::permissions(sampleImg, writeBits, std::filesystem::perm_options::add, ec);
    EXPECT_FALSE(ec);
}
#endif

TEST_F(CatalogClassTest, DuplicateGroupRestoresStagedFilesWhenDatabaseDeleteFails) {
    Catalog cat(1);
    ASSERT_TRUE(cat.open(dbPath, thumbsDir, testDir.string()).isOk());
    const auto firstPath = testDir / "first.jpg";
    const auto secondPath = testDir / "second.jpg";
    std::filesystem::copy_file(sampleImg, firstPath);
    std::filesystem::copy_file(sampleImg, secondPath);
    const auto keeper = cat.importFile(sampleImg);
    const auto first = cat.importFile(firstPath.string());
    const auto second = cat.importFile(secondPath.string());
    ASSERT_TRUE(keeper.isOk());
    ASSERT_TRUE(first.isOk());
    ASSERT_TRUE(second.isOk());
    ASSERT_TRUE(cat.setRating(first.value().id, 4).isOk());
    ASSERT_TRUE(cat.db().connection().execute(
        "CREATE TRIGGER fail_delete BEFORE DELETE ON media_items WHEN OLD.id = " +
        std::to_string(second.value().id) + " BEGIN SELECT RAISE(ABORT, 'forced failure'); END;").isOk());

    const ReviewedMedia reviewedKeeper{keeper.value().id, keeper.value().content_hash};
    const std::vector<ReviewedMedia> removals{
        {first.value().id, first.value().content_hash},
        {second.value().id, second.value().content_hash}
    };
    const auto failed = cat.resolveDuplicateGroup(reviewedKeeper, removals, true, true);
    ASSERT_FALSE(failed.isOk());
    EXPECT_NE(failed.status().message().find("forced failure"), std::string::npos);
    EXPECT_EQ(cat.getMedia(reviewedKeeper.id).value().rating, 0);
    EXPECT_TRUE(cat.getMedia(removals[0].id).isOk());
    EXPECT_TRUE(cat.getMedia(removals[1].id).isOk());
    EXPECT_TRUE(std::filesystem::exists(firstPath));
    EXPECT_TRUE(std::filesystem::exists(secondPath));

    ASSERT_TRUE(cat.db().connection().execute("DROP TRIGGER fail_delete;").isOk());
    const auto resolved = cat.resolveDuplicateGroup(reviewedKeeper, removals, true, true);
    ASSERT_TRUE(resolved.isOk()) << resolved.status().message();
    ASSERT_EQ(resolved.value().size(), 2);
    EXPECT_TRUE(resolved.value()[0].deleted_from_disk);
    EXPECT_TRUE(resolved.value()[1].deleted_from_disk);
    EXPECT_EQ(resolved.value()[0].freed_bytes, first.value().file_size);
    EXPECT_EQ(cat.getMedia(reviewedKeeper.id).value().rating, 4);
    EXPECT_FALSE(std::filesystem::exists(firstPath));
    EXPECT_FALSE(std::filesystem::exists(secondPath));
    EXPECT_TRUE(std::filesystem::exists(sampleImg));
}

TEST_F(CatalogClassTest, DuplicateGroupRejectsAnOriginalChangedOutsideTheCatalog) {
    Catalog cat(1);
    ASSERT_TRUE(cat.open(dbPath, thumbsDir, testDir.string()).isOk());
    const auto copyPath = testDir / "copy.jpg";
    std::filesystem::copy_file(sampleImg, copyPath);
    const auto keeper = cat.importFile(sampleImg);
    const auto copy = cat.importFile(copyPath.string());
    ASSERT_TRUE(keeper.isOk());
    ASSERT_TRUE(copy.isOk());
    {
        std::ofstream file(copyPath, std::ios::binary | std::ios::app);
        file << "new photo bytes";
    }

    const auto result = cat.resolveDuplicateGroup(
        {keeper.value().id, keeper.value().content_hash}, {{copy.value().id, copy.value().content_hash}},
        true, true);
    EXPECT_EQ(result.status().code(), StatusCode::AlreadyExists);
    EXPECT_TRUE(cat.getMedia(copy.value().id).isOk());
    EXPECT_GT(std::filesystem::file_size(copyPath), static_cast<uintmax_t>(copy.value().file_size));
}

TEST_F(CatalogClassTest, DuplicateMergePreservesAutomaticTagSourcesAndPromotesManualOwnership) {
    Catalog cat(1);
    ASSERT_TRUE(cat.open(dbPath, thumbsDir, testDir.string()).isOk());
    const auto imported = cat.importFile(sampleImg);
    ASSERT_TRUE(imported.isOk());
    const auto keeper = imported.value().id;
    auto duplicate = imported.value();
    duplicate.file_path = "another.jpg";
    ASSERT_TRUE(cat.db().insertMedia(duplicate).isOk());

    const auto gpsTag = cat.createOrGetTag("GPS place", "places");
    const auto clipTag = cat.createOrGetTag("CLIP only", "keyword");
    const auto promotedTag = cat.createOrGetTag("Manual and CLIP", "keyword");
    ASSERT_TRUE(gpsTag.isOk());
    ASSERT_TRUE(clipTag.isOk());
    ASSERT_TRUE(promotedTag.isOk());
    ASSERT_TRUE(cat.addTag(duplicate.id, gpsTag.value()).isOk());
    ASSERT_TRUE(cat.addTag(duplicate.id, clipTag.value()).isOk());
    ASSERT_TRUE(cat.addTag(duplicate.id, promotedTag.value()).isOk());
    ASSERT_TRUE(cat.addTag(keeper, promotedTag.value()).isOk());

    const auto setSource = [&](MediaId id, TagId tag, std::string source) {
        return cat.db().connection().execute(
            "UPDATE media_tag_provenance SET manual = 0, source = '" + source +
            "' WHERE media_id = " + std::to_string(id) + " AND tag_id = " + std::to_string(tag) + ";");
    };
    ASSERT_TRUE(setSource(duplicate.id, gpsTag.value(), "gps").isOk());
    ASSERT_TRUE(setSource(duplicate.id, clipTag.value(), "clip").isOk());
    ASSERT_TRUE(setSource(keeper, promotedTag.value(), "clip").isOk());

    const auto merged = cat.resolveDuplicateGroup({keeper, {}}, {{duplicate.id, {}}}, false, true);
    ASSERT_TRUE(merged.isOk()) << merged.status().message();
    const auto tags = cat.getTagsForMedia(keeper);
    ASSERT_TRUE(tags.isOk());
    std::unordered_map<TagId, std::string> sources;
    for (const auto& tag : tags.value()) sources.emplace(tag.id, tag.source);
    EXPECT_EQ(sources[gpsTag.value()], "gps");
    EXPECT_EQ(sources[clipTag.value()], "clip");
    EXPECT_EQ(sources[promotedTag.value()], "manual");

    ASSERT_TRUE(cat.db().clearSmartTags().isOk());
    const auto retained = cat.getTagsForMedia(keeper);
    ASSERT_TRUE(retained.isOk());
    ASSERT_EQ(retained.value().size(), 2);
    EXPECT_EQ(retained.value()[0].id, gpsTag.value());
    EXPECT_EQ(retained.value()[1].id, promotedTag.value());
}

TEST_F(CatalogClassTest, IndependentImportsStoreDistinctAbsolutePathsWithoutARoot) {
    const auto firstDir = testDir / "first";
    const auto secondDir = testDir / "second";
    std::filesystem::create_directory(firstDir);
    std::filesystem::create_directory(secondDir);
    const auto firstPath = firstDir / "photo.jpg";
    const auto secondPath = secondDir / "photo.jpg";
    std::filesystem::copy_file(sampleImg, firstPath);
    std::filesystem::copy_file(sampleImg, secondPath);

    Catalog cat(1);
    ASSERT_TRUE(cat.open(dbPath, thumbsDir).isOk());
    ASSERT_TRUE(cat.importDirectory(firstDir.string(), true, nullptr, false).isOk());
    cat.close();
    ASSERT_TRUE(cat.open(dbPath, thumbsDir).isOk());
    ASSERT_TRUE(cat.importDirectory(secondDir.string(), true, nullptr, false).isOk());

    const auto first = cat.getMediaByPath(Importer::normalizePath(firstPath.string()));
    const auto second = cat.getMediaByPath(Importer::normalizePath(secondPath.string()));
    ASSERT_TRUE(first.isOk());
    ASSERT_TRUE(second.isOk());
    EXPECT_NE(first.value().id, second.value().id);
}

TEST_F(CatalogClassTest, CancellingAFolderImportDoesNotCancelTheNextSingleFileImport) {
    Catalog cat(1);
    ASSERT_TRUE(cat.open(dbPath, thumbsDir, testDir.string()).isOk());
    auto cancelled = cat.importDirectory(testDir.string(), true, [&](const ImportProgress& progress) {
        if (progress.is_running) cat.cancelImport();
    });
    ASSERT_TRUE(cancelled.isOk());
    EXPECT_EQ(cancelled.value().cancelled_files.load(), 1);
    EXPECT_TRUE(cat.importFile(sampleImg).isOk());
}

TEST_F(CatalogClassTest, RescanPreservesCatalogEditsMadeAfterReading) {
    Catalog cat(1);
    ASSERT_TRUE(cat.open(dbPath, thumbsDir, testDir.string()).isOk());
    auto original = cat.importFile(sampleImg);
    ASSERT_TRUE(original.isOk());
    const auto id = original.value().id;
    {
        std::ofstream file(sampleImg, std::ios::binary | std::ios::app);
        file << "changed original";
    }
    cat.importer().setFileHook([&](const std::string&) {
        EXPECT_TRUE(cat.setRating(id, 5).isOk());
        EXPECT_TRUE(cat.setCaption(id, "Edited during rescan").isOk());
        EXPECT_TRUE(cat.setDateTaken(id, 1234567890).isOk());
    });
    auto rescanned = cat.importDirectory(testDir.string());
    ASSERT_TRUE(rescanned.isOk());
    EXPECT_EQ(rescanned.value().imported_files.load(), 1);
    const auto item = cat.getMedia(id).value();
    EXPECT_EQ(item.rating, 5);
    EXPECT_EQ(item.caption, "Edited during rescan");
    EXPECT_EQ(item.date_taken, 1234567890);
    EXPECT_NE(item.content_hash, original.value().content_hash);
}

TEST_F(CatalogClassTest, RescanDoesNotRestoreAPathRenamedAfterReading) {
    Catalog cat(1);
    ASSERT_TRUE(cat.open(dbPath, thumbsDir, testDir.string()).isOk());
    auto original = cat.importFile(sampleImg);
    ASSERT_TRUE(original.isOk());
    const auto id = original.value().id;
    {
        std::ofstream file(sampleImg, std::ios::binary | std::ios::app);
        file << "changed original";
    }
    cat.importer().setFileHook([&](const std::string&) {
        EXPECT_TRUE(cat.renameMedia(id, "renamed.jpg").isOk());
    });
    auto rescanned = cat.importDirectory(testDir.string());
    ASSERT_TRUE(rescanned.isOk());
    EXPECT_EQ(rescanned.value().failed_files.load(), 1);
    EXPECT_EQ(cat.getMedia(id).value().file_path, "renamed.jpg");
    EXPECT_TRUE(std::filesystem::exists(testDir / "renamed.jpg"));
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
