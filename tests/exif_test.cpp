#include <gtest/gtest.h>
#include "imagine/metadata/exif_reader.hpp"
#include "imagine/metadata/hasher.hpp"
#include <filesystem>
#include <fstream>

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
}

TEST(ExifReaderTest, ParseExifDate) {
    int64_t ts = ExifReader::parseExifDate("2026:09:03 21:05:00");
    EXPECT_GT(ts, 0);

    // Invalid string
    EXPECT_EQ(ExifReader::parseExifDate("invalid-date"), 0);
    EXPECT_EQ(ExifReader::parseExifDate(""), 0);
}

TEST(ExifReaderTest, NonExifImageGracefulFallback) {
    // A buffer with dummy non-exif data should return parseError, not crash
    std::vector<uint8_t> dummyData = { 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00 };
    auto res = ExifReader::readFromBuffer(dummyData.data(), dummyData.size());
    EXPECT_FALSE(res.isOk());
}
