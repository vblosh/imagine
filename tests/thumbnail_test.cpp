#include <gtest/gtest.h>
#include "imagine/thumbnail/generator.hpp"
#include "imagine/thumbnail/cache.hpp"
#include <filesystem>
#include <fstream>

using namespace imagine;
using namespace imagine::thumbnail;

class ThumbnailTest : public ::testing::Test {
protected:
    void SetUp() override {
        testDir = std::filesystem::temp_directory_path() / "imagine_thumb_test";
        std::filesystem::create_directories(testDir);
        testImgPath = (testDir / "sample.jpg").string();

        // Generate synthetic 400x200 RGB image
        ImageBuffer buf;
        buf.width = 400;
        buf.height = 200;
        buf.channels = 3;
        buf.data.resize(400 * 200 * 3, 128); // Gray

        ASSERT_TRUE(Generator::saveJpeg(buf, testImgPath).isOk());
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
    }

    std::filesystem::path testDir;
    std::string testImgPath;
};

TEST_F(ThumbnailTest, LoadAndCheckDimensions) {
    auto dimsRes = Generator::getImageDimensions(testImgPath);
    ASSERT_TRUE(dimsRes.isOk());
    EXPECT_EQ(dimsRes.value().first, 400);
    EXPECT_EQ(dimsRes.value().second, 200);

    // Non-existent image dimension check
    auto badDims = Generator::getImageDimensions((testDir / "missing.jpg").string());
    EXPECT_FALSE(badDims.isOk());
    EXPECT_EQ(badDims.status().code(), StatusCode::ParseError);

    auto loadRes = Generator::loadImage(testImgPath);
    ASSERT_TRUE(loadRes.isOk());
    const auto& img = loadRes.value();
    EXPECT_EQ(img.width, 400);
    EXPECT_EQ(img.height, 200);
    EXPECT_EQ(img.channels, 3);

    // Non-existent image load check
    auto badLoad = Generator::loadImage((testDir / "missing.jpg").string());
    EXPECT_FALSE(badLoad.isOk());
    EXPECT_EQ(badLoad.status().code(), StatusCode::IoError);
}

TEST_F(ThumbnailTest, LoadImageFromMemory) {
    std::ifstream ifs(testImgPath, std::ios::binary);
    ASSERT_TRUE(ifs.is_open());
    std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

    auto memRes = Generator::loadImageFromMemory(buffer.data(), buffer.size());
    ASSERT_TRUE(memRes.isOk());
    EXPECT_EQ(memRes.value().width, 400);
    EXPECT_EQ(memRes.value().height, 200);

    // Invalid memory buffer
    std::vector<uint8_t> junk = {0x00, 0x11, 0x22};
    auto badMem = Generator::loadImageFromMemory(junk.data(), junk.size());
    EXPECT_FALSE(badMem.isOk());
    EXPECT_EQ(badMem.status().code(), StatusCode::ParseError);
}

TEST_F(ThumbnailTest, ResizeAspectAndBoundaries) {
    auto loadRes = Generator::loadImage(testImgPath);
    ASSERT_TRUE(loadRes.isOk());

    // Landscape resize
    auto resizeRes = Generator::resize(loadRes.value(), 100);
    ASSERT_TRUE(resizeRes.isOk());
    const auto& resized = resizeRes.value();
    EXPECT_EQ(resized.width, 100);
    EXPECT_EQ(resized.height, 50);

    // Portrait image resize
    ImageBuffer portrait;
    portrait.width = 200;
    portrait.height = 400;
    portrait.channels = 3;
    portrait.data.resize(200 * 400 * 3, 200);
    auto portraitRes = Generator::resize(portrait, 100);
    ASSERT_TRUE(portraitRes.isOk());
    EXPECT_EQ(portraitRes.value().height, 100);
    EXPECT_EQ(portraitRes.value().width, 50);

    // Image already smaller than maxDimension: no upscale, return original
    auto smallRes = Generator::resize(loadRes.value(), 800);
    ASSERT_TRUE(smallRes.isOk());
    EXPECT_EQ(smallRes.value().width, 400);
    EXPECT_EQ(smallRes.value().height, 200);

    // Empty or invalid image resize
    ImageBuffer emptyBuf;
    auto errResize = Generator::resize(emptyBuf, 100);
    EXPECT_FALSE(errResize.isOk());
    EXPECT_EQ(errResize.status().code(), StatusCode::InvalidArgument);
}

TEST_F(ThumbnailTest, RotateAllOrientations) {
    auto loadRes = Generator::loadImage(testImgPath);
    ASSERT_TRUE(loadRes.isOk());
    const auto& img = loadRes.value();

    // Orientation <= 1: unchanged
    EXPECT_EQ(Generator::rotate(img, 1).width, 400);
    EXPECT_EQ(Generator::rotate(img, 0).width, 400);

    // Orientation > 8: unchanged
    EXPECT_EQ(Generator::rotate(img, 9).width, 400);

    // Empty buffer: unchanged
    ImageBuffer emptyBuf;
    EXPECT_TRUE(Generator::rotate(emptyBuf, 6).data.empty());

    // Orientation 3: 180 degrees CW -> dimensions unchanged (400x200)
    auto rot3 = Generator::rotate(img, 3);
    EXPECT_EQ(rot3.width, 400);
    EXPECT_EQ(rot3.height, 200);

    // Orientation 6: 90 degrees CW -> 400x200 becomes 200x400
    auto rot6 = Generator::rotate(img, 6);
    EXPECT_EQ(rot6.width, 200);
    EXPECT_EQ(rot6.height, 400);

    // Orientation 8: 270 degrees CW (90 CCW) -> 400x200 becomes 200x400
    auto rot8 = Generator::rotate(img, 8);
    EXPECT_EQ(rot8.width, 200);
    EXPECT_EQ(rot8.height, 400);

    // Other unhandled orientations (e.g. 2, 4, 5, 7): returns src
    auto rot2 = Generator::rotate(img, 2);
    EXPECT_EQ(rot2.width, 400);
    EXPECT_EQ(rot2.height, 200);
}

TEST_F(ThumbnailTest, SaveJpegAndPng) {
    auto loadRes = Generator::loadImage(testImgPath);
    ASSERT_TRUE(loadRes.isOk());

    std::string pngPath = (testDir / "sample.png").string();
    auto pngStatus = Generator::savePng(loadRes.value(), pngPath);
    EXPECT_TRUE(pngStatus.isOk());
    EXPECT_TRUE(std::filesystem::exists(pngPath));

    // Check saved PNG can be read back
    auto pngDims = Generator::getImageDimensions(pngPath);
    ASSERT_TRUE(pngDims.isOk());
    EXPECT_EQ(pngDims.value().first, 400);
    EXPECT_EQ(pngDims.value().second, 200);

    // Save to invalid path should return ioError
    auto badJpeg = Generator::saveJpeg(loadRes.value(), "/proc/invalid_dir/test.jpg");
    EXPECT_FALSE(badJpeg.isOk());
    EXPECT_EQ(badJpeg.code(), StatusCode::IoError);

    auto badPng = Generator::savePng(loadRes.value(), "/proc/invalid_dir/test.png");
    EXPECT_FALSE(badPng.isOk());
    EXPECT_EQ(badPng.code(), StatusCode::IoError);
}

TEST_F(ThumbnailTest, CacheOperationsAndSingleThumbnail) {
    std::string cacheDir = (testDir / "cache").string();
    Cache cache(cacheDir);

    // setCacheDir
    std::string newCacheDir = (testDir / "cache2").string();
    cache.setCacheDir(newCacheDir);

    // Hash < 4 characters
    std::string shortHash = "ab";
    std::string shortPath = cache.getThumbnailPath(shortHash, 256);
    EXPECT_NE(shortPath.find("misc_ab_256.jpg"), std::string::npos);

    // Default Cache constructor with empty string uses defaultCacheDir
    Cache defaultCache("");
    std::string defPath = defaultCache.getThumbnailPath("123456", 256);
    EXPECT_FALSE(defPath.empty());

    // Single ensureThumbnail
    std::string fakeHash = "abcdef0123456789abcdef0123456789";
    auto singleRes = cache.ensureThumbnail(testImgPath, fakeHash, 150, 6);
    ASSERT_TRUE(singleRes.isOk());
    EXPECT_TRUE(std::filesystem::exists(singleRes.value()));
    EXPECT_TRUE(cache.hasThumbnail(fakeHash, 150));

    // Calling ensureThumbnail again should return cached path immediately
    auto singleResCached = cache.ensureThumbnail(testImgPath, fakeHash, 150, 6);
    ASSERT_TRUE(singleResCached.isOk());
    EXPECT_EQ(singleResCached.value(), singleRes.value());

    // ensureThumbnail with non-existent file
    auto badRes = cache.ensureThumbnail((testDir / "nonexistent.jpg").string(), "failhash", 150, 1);
    EXPECT_FALSE(badRes.isOk());

    // ensureDualThumbnails with small image (width <= 1024 and height <= 1024)
    auto dualRes = cache.ensureDualThumbnails(testImgPath, fakeHash, 1);
    ASSERT_TRUE(dualRes.isOk());
    EXPECT_TRUE(std::filesystem::exists(dualRes.value().first));
    EXPECT_TRUE(std::filesystem::exists(dualRes.value().second));

    // ensureDualThumbnails with non-existent file
    auto dualBad = cache.ensureDualThumbnails((testDir / "nonexistent.jpg").string(), "failhash2", 1);
    EXPECT_FALSE(dualBad.isOk());
}

TEST_F(ThumbnailTest, CacheLargeImageDownscale) {
    std::string cacheDir = (testDir / "large_cache").string();
    Cache cache(cacheDir);

    // Create large image (1200x800 > 1024)
    std::string largeImgPath = (testDir / "large_sample.jpg").string();
    ImageBuffer buf;
    buf.width = 1200;
    buf.height = 800;
    buf.channels = 3;
    buf.data.resize(1200 * 800 * 3, 100);
    ASSERT_TRUE(Generator::saveJpeg(buf, largeImgPath).isOk());

    std::string hash = "9999999999abcdef9999999999abcdef";
    auto dualRes = cache.ensureDualThumbnails(largeImgPath, hash, 1);
    ASSERT_TRUE(dualRes.isOk());

    // Second call triggers (hasSmall && hasLarge) branch
    auto dualRes2 = cache.ensureDualThumbnails(largeImgPath, hash, 1);
    ASSERT_TRUE(dualRes2.isOk());
    EXPECT_EQ(dualRes.value().first, dualRes2.value().first);
}
