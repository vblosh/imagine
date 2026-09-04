#include <gtest/gtest.h>
#include "imagine/metadata/exif_reader.hpp"
#include "imagine/metadata/hasher.hpp"
#include <filesystem>
#include <fstream>
#include <vector>

using namespace imagine;
using namespace imagine::metadata;

TEST(HasherTest, Sha256KnownVectors) {
    // Empty string SHA-256
    std::string emptyHash = Hasher::computeBytesSha256(reinterpret_cast<const uint8_t*>(""), 0);
    EXPECT_EQ(emptyHash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // "hello world" SHA-256
    std::string hello = "hello world";
    std::string helloHash = Hasher::computeBytesSha256(reinterpret_cast<const uint8_t*>(hello.data()), hello.size());
    EXPECT_EQ(helloHash, "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
}

TEST(HasherTest, FileSha256) {
    auto tmp = std::filesystem::temp_directory_path() / "test_sha.txt";
    {
        std::ofstream ofs(tmp);
        ofs << "hello world";
    }

    auto res = Hasher::computeFileSha256(tmp.string());
    ASSERT_TRUE(res.isOk());
    EXPECT_EQ(res.value(), "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");

    std::filesystem::remove(tmp);

    // Non-existent file error
    auto missingRes = Hasher::computeFileSha256((std::filesystem::temp_directory_path() / "non_existent_file.xyz").string());
    EXPECT_FALSE(missingRes.isOk());
    EXPECT_EQ(missingRes.status().code(), StatusCode::IoError);

    // Large file (> 64KB) to exercise multiple chunk iterations
    auto largeTmp = std::filesystem::temp_directory_path() / "large_file.bin";
    {
        std::ofstream ofs(largeTmp, std::ios::binary);
        std::vector<char> block(100 * 1024, 'A');
        ofs.write(block.data(), block.size());
    }
    auto largeRes = Hasher::computeFileSha256(largeTmp.string());
    ASSERT_TRUE(largeRes.isOk());
    EXPECT_FALSE(largeRes.value().empty());
    std::filesystem::remove(largeTmp);
}

TEST(ExifReaderTest, ParseExifDate) {
    int64_t ts = ExifReader::parseExifDate("2026:09:03 21:05:00");
    EXPECT_GT(ts, 0);

    // Invalid string
    EXPECT_EQ(ExifReader::parseExifDate("invalid-date"), 0);
    EXPECT_EQ(ExifReader::parseExifDate(""), 0);
    EXPECT_EQ(ExifReader::parseExifDate("short"), 0);
}

TEST(ExifReaderTest, NonExifImageGracefulFallback) {
    // A buffer with dummy non-exif data should return parseError, not crash
    std::vector<uint8_t> dummyData = { 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00 };
    auto res = ExifReader::readFromBuffer(dummyData.data(), dummyData.size());
    EXPECT_FALSE(res.isOk());
}

TEST(ExifReaderTest, ReadRealExifFileAndBuffer) {
    // Non-existent file
    auto nonExist = ExifReader::readFromFile("/nonexistent/file.jpg");
    EXPECT_FALSE(nonExist.isOk());
    EXPECT_EQ(nonExist.status().code(), StatusCode::IoError);

    // Empty file
    auto emptyTmp = std::filesystem::temp_directory_path() / "empty.jpg";
    {
        std::ofstream ofs(emptyTmp);
    }
    auto emptyRes = ExifReader::readFromFile(emptyTmp.string());
    EXPECT_FALSE(emptyRes.isOk());
    EXPECT_EQ(emptyRes.status().code(), StatusCode::IoError);
    std::filesystem::remove(emptyTmp);

    // Test real EXIF image
    std::string exifPath = "tests/fixtures/test_exif.jpg";
    if (std::filesystem::exists(exifPath)) {
        auto res = ExifReader::readFromFile(exifPath);
        ASSERT_TRUE(res.isOk());
        const auto& exif = res.value();
        EXPECT_EQ(exif.camera_make, "Apple");
        EXPECT_EQ(exif.camera_model, "iPhone 4S");
        EXPECT_EQ(exif.iso, 50);
        EXPECT_GT(exif.exposure_time, 0.0);
        EXPECT_GT(exif.f_number, 0.0);
        EXPECT_GT(exif.focal_length, 0.0);
        EXPECT_EQ(exif.orientation, 1);
        EXPECT_TRUE(exif.has_gps);
        EXPECT_NEAR(exif.latitude, 37.885, 0.01);
        EXPECT_NEAR(exif.longitude, -122.6225, 0.01);
        EXPECT_NEAR(exif.altitude, 122.0, 1.0);
        EXPECT_GT(exif.date_taken, 0);

        // Test truncated buffer (e.g. only first 13KB of image, containing APP1 but not EOF)
        std::ifstream file(exifPath, std::ios::binary);
        std::vector<uint8_t> truncatedBuf(13000);
        file.read(reinterpret_cast<char*>(truncatedBuf.data()), truncatedBuf.size());
        size_t bytesRead = file.gcount();
        auto truncRes = ExifReader::readFromBuffer(truncatedBuf.data(), bytesRead);
        ASSERT_TRUE(truncRes.isOk());
        EXPECT_EQ(truncRes.value().camera_make, "Apple");
        EXPECT_TRUE(truncRes.value().has_gps);
    }
}
