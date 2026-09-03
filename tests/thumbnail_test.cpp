#include <gtest/gtest.h>
#include "imagine/thumbnail/generator.hpp"
#include "imagine/thumbnail/cache.hpp"
#include <filesystem>

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

    auto loadRes = Generator::loadImage(testImgPath);
    ASSERT_TRUE(loadRes.isOk());
    const auto& img = loadRes.value();
    EXPECT_EQ(img.width, 400);
    EXPECT_EQ(img.height, 200);
    EXPECT_EQ(img.channels, 3);
}

TEST_F(ThumbnailTest, ResizePreservesAspect) {
    auto loadRes = Generator::loadImage(testImgPath);
    ASSERT_TRUE(loadRes.isOk());

    auto resizeRes = Generator::resize(loadRes.value(), 100);
    ASSERT_TRUE(resizeRes.isOk());
    const auto& resized = resizeRes.value();
    EXPECT_EQ(resized.width, 100);
    EXPECT_EQ(resized.height, 50);
}

TEST_F(ThumbnailTest, RotateImage) {
    auto loadRes = Generator::loadImage(testImgPath);
    ASSERT_TRUE(loadRes.isOk());

    // Orientation 6: 90 degrees CW -> 400x200 becomes 200x400
    auto rotated = Generator::rotate(loadRes.value(), 6);
    EXPECT_EQ(rotated.width, 200);
    EXPECT_EQ(rotated.height, 400);
}

TEST_F(ThumbnailTest, CacheGeneratesDualThumbnails) {
    std::string cacheDir = (testDir / "cache").string();
    Cache cache(cacheDir);

    std::string fakeHash = "1234567890abcdef1234567890abcdef";
    auto dualRes = cache.ensureDualThumbnails(testImgPath, fakeHash, 1);
    ASSERT_TRUE(dualRes.isOk());

    std::string smallPath = dualRes.value().first;
    std::string largePath = dualRes.value().second;

    EXPECT_TRUE(std::filesystem::exists(smallPath));
    EXPECT_TRUE(std::filesystem::exists(largePath));
    EXPECT_TRUE(cache.hasThumbnail(fakeHash, Cache::SmallSize));
    EXPECT_TRUE(cache.hasThumbnail(fakeHash, Cache::LargeSize));

    // Second call should return existing cached paths without error
    auto dualRes2 = cache.ensureDualThumbnails(testImgPath, fakeHash, 1);
    ASSERT_TRUE(dualRes2.isOk());
    EXPECT_EQ(dualRes2.value().first, smallPath);
    EXPECT_EQ(dualRes2.value().second, largePath);
}
