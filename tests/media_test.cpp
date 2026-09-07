#include <gtest/gtest.h>
#include "imagine/metadata/media_reader.hpp"
#include "imagine/thumbnail/generator.hpp"
#include "imagine/thumbnail/cache.hpp"
#include "imagine/db/catalog_db.hpp"
#include "imagine/core/catalog.hpp"
#include "imagine/core/query.hpp"
#include "imagine/core/importer.hpp"
#include "imagine/server/web_server.hpp"
#include <fstream>
#include <filesystem>
#include <vector>
#include <chrono>
#include <httplib.h>
#include <nlohmann/json.hpp>

using namespace imagine;
using namespace imagine::metadata;
using namespace imagine::thumbnail;
using namespace imagine::db;
using namespace imagine::core;

namespace {

// Helper to create a temporary test directory
class TempDir {
public:
    TempDir() {
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() / ("imagine_media_test_" + std::to_string(now));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }
    std::string string() const { return path_.string(); }
private:
    std::filesystem::path path_;
};

void writeBe16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

void writeBe32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

void writeLe32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void writeLe16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void writeString(std::vector<uint8_t>& buf, const std::string& s) {
    buf.insert(buf.end(), s.begin(), s.end());
}

} // anonymous namespace

// 1. Test Extension and Media Type Detection
TEST(MediaTest, ExtensionAndTypeDetection) {
    EXPECT_TRUE(MediaReader::isSupportedExtension("test.jpg"));
    EXPECT_TRUE(MediaReader::isSupportedExtension("test.PNG"));
    EXPECT_TRUE(MediaReader::isSupportedExtension("movie.mp4"));
    EXPECT_TRUE(MediaReader::isSupportedExtension("movie.MOV"));
    EXPECT_TRUE(MediaReader::isSupportedExtension("clip.webm"));
    EXPECT_TRUE(MediaReader::isSupportedExtension("clip.mkv"));
    EXPECT_TRUE(MediaReader::isSupportedExtension("audio.mp3"));
    EXPECT_TRUE(MediaReader::isSupportedExtension("track.FLAC"));
    EXPECT_TRUE(MediaReader::isSupportedExtension("sound.wav"));
    EXPECT_TRUE(MediaReader::isSupportedExtension("voice.ogg"));
    EXPECT_TRUE(MediaReader::isSupportedExtension("clip.mts"));
    EXPECT_TRUE(MediaReader::isSupportedExtension("movie.mpg"));
    EXPECT_TRUE(MediaReader::isSupportedExtension("stream.m2t"));
    EXPECT_TRUE(MediaReader::isSupportedExtension("clip.wmv"));
    EXPECT_TRUE(MediaReader::isSupportedExtension("video.flv"));
    EXPECT_TRUE(MediaReader::isSupportedExtension("old.3gp"));
    EXPECT_TRUE(MediaReader::isSupportedExtension("song.wma"));
    EXPECT_FALSE(MediaReader::isSupportedExtension("document.pdf"));
    EXPECT_FALSE(MediaReader::isSupportedExtension("archive.zip"));

    EXPECT_EQ(MediaReader::detectMediaType("photo.jpg"), "photo");
    EXPECT_EQ(MediaReader::detectMediaType("photo.webp"), "photo");
    EXPECT_EQ(MediaReader::detectMediaType("photo.tiff"), "photo");
    EXPECT_EQ(MediaReader::detectMediaType("video.mp4"), "video");
    EXPECT_EQ(MediaReader::detectMediaType("video.mov"), "video");
    EXPECT_EQ(MediaReader::detectMediaType("video.webm"), "video");
    EXPECT_EQ(MediaReader::detectMediaType("video.avi"), "video");
    EXPECT_EQ(MediaReader::detectMediaType("video.mts"), "video");
    EXPECT_EQ(MediaReader::detectMediaType("video.mpg"), "video");
    EXPECT_EQ(MediaReader::detectMediaType("music.mp3"), "audio");
    EXPECT_EQ(MediaReader::detectMediaType("music.flac"), "audio");
    EXPECT_EQ(MediaReader::detectMediaType("sound.wav"), "audio");
    EXPECT_EQ(MediaReader::detectMediaType("sound.ogg"), "audio");
    EXPECT_EQ(MediaReader::detectMediaType("audio.wma"), "audio");
}

// 2. Test MP4 Container Parser
TEST(MediaTest, Mp4Parser) {
    TempDir tmp;
    std::string mp4Path = (tmp.path() / "test.mp4").string();

    std::vector<uint8_t> data;
    // ftyp box
    writeBe32(data, 20);
    writeString(data, "ftyp");
    writeString(data, "isom");
    writeBe32(data, 512); // minor version
    writeString(data, "isom"); // compatible brand

    // moov box container
    size_t moovStart = data.size();
    writeBe32(data, 0); // placeholder for moov size
    writeString(data, "moov");

    // mvhd box
    size_t mvhdStart = data.size();
    writeBe32(data, 108);
    writeString(data, "mvhd");
    data.push_back(0); // version 0
    data.push_back(0); data.push_back(0); data.push_back(0); // flags
    writeBe32(data, 0); // creation time
    writeBe32(data, 0); // modification time
    writeBe32(data, 1000); // timescale = 1000 Hz
    writeBe32(data, 15500); // duration = 15500 / 1000 = 15.5s
    // rate, volume, reserved, matrix, pre_defined, next_track_id (rest of 108 bytes)
    data.resize(mvhdStart + 108, 0);

    // trak box container
    size_t trakStart = data.size();
    writeBe32(data, 0); // placeholder for trak size
    writeString(data, "trak");

    // tkhd box
    size_t tkhdStart = data.size();
    writeBe32(data, 92);
    writeString(data, "tkhd");
    data.push_back(0); // version 0
    data.push_back(0); data.push_back(0); data.push_back(0); // flags
    data.resize(tkhdStart + 84, 0); // up to width
    writeBe32(data, 1920 << 16); // width 1920 in 16.16 fixed point
    writeBe32(data, 1080 << 16); // height 1080 in 16.16 fixed point

    // Patch trak size
    uint32_t trakSize = static_cast<uint32_t>(data.size() - trakStart);
    data[trakStart] = (trakSize >> 24) & 0xFF;
    data[trakStart + 1] = (trakSize >> 16) & 0xFF;
    data[trakStart + 2] = (trakSize >> 8) & 0xFF;
    data[trakStart + 3] = trakSize & 0xFF;

    // Patch moov size
    uint32_t moovSize = static_cast<uint32_t>(data.size() - moovStart);
    data[moovStart] = (moovSize >> 24) & 0xFF;
    data[moovStart + 1] = (moovSize >> 16) & 0xFF;
    data[moovStart + 2] = (moovSize >> 8) & 0xFF;
    data[moovStart + 3] = moovSize & 0xFF;

    // Write file
    {
        std::ofstream ofs(mp4Path, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
    }

    auto res = MediaReader::readMetadata(mp4Path);
    ASSERT_TRUE(res.isOk()) << res.status().message();
    auto info = res.value();
    EXPECT_EQ(info.media_type, "video");
    EXPECT_NEAR(info.duration, 15.5, 0.1);
    EXPECT_EQ(info.width, 1920);
    EXPECT_EQ(info.height, 1080);
}

// Test MP4 Multi-Track Container (Video + Audio tracks)
TEST(MediaTest, Mp4MultiTrackParser) {
    TempDir tmp;
    std::string mp4Path = (tmp.path() / "multitrack.mp4").string();

    std::vector<uint8_t> data;
    // ftyp box
    writeBe32(data, 20);
    writeString(data, "ftyp");
    writeString(data, "isom");
    writeBe32(data, 512);
    writeString(data, "isom");

    // moov box container
    size_t moovStart = data.size();
    writeBe32(data, 0);
    writeString(data, "moov");

    // mvhd box
    size_t mvhdStart = data.size();
    writeBe32(data, 108);
    writeString(data, "mvhd");
    data.push_back(0); // version 0
    data.push_back(0); data.push_back(0); data.push_back(0); // flags
    writeBe32(data, 0); // creation time
    writeBe32(data, 0); // modification time
    writeBe32(data, 1000); // timescale = 1000 Hz
    writeBe32(data, 10000); // duration = 10.0s
    data.resize(mvhdStart + 108, 0);

    // Track 1: Video Track
    size_t trak1Start = data.size();
    writeBe32(data, 0);
    writeString(data, "trak");

    // tkhd 1
    size_t tkhd1Start = data.size();
    writeBe32(data, 92);
    writeString(data, "tkhd");
    data.push_back(0);
    data.push_back(0); data.push_back(0); data.push_back(0);
    data.resize(tkhd1Start + 84, 0);
    writeBe32(data, 1920 << 16); // width 1920
    writeBe32(data, 1080 << 16); // height 1080

    // mdia 1
    size_t mdia1Start = data.size();
    writeBe32(data, 0);
    writeString(data, "mdia");

    // hdlr 1 (vide)
    writeBe32(data, 32);
    writeString(data, "hdlr");
    writeBe32(data, 0); // version + flags
    writeBe32(data, 0); // pre_defined
    writeString(data, "vide");
    data.resize(data.size() + 12, 0);

    // minf 1 -> stbl 1 -> stsd 1
    size_t minf1Start = data.size();
    writeBe32(data, 0);
    writeString(data, "minf");

    size_t stbl1Start = data.size();
    writeBe32(data, 0);
    writeString(data, "stbl");

    // stsd 1 with avc1
    size_t stsd1Start = data.size();
    writeBe32(data, 0);
    writeString(data, "stsd");
    writeBe32(data, 0); // version + flags
    writeBe32(data, 1); // entry count

    size_t avc1Start = data.size();
    writeBe32(data, 0);
    writeString(data, "avc1");
    data.resize(avc1Start + 40, 0); // sample entry padding

    uint32_t avc1Size = static_cast<uint32_t>(data.size() - avc1Start);
    data[avc1Start] = (avc1Size >> 24) & 0xFF;
    data[avc1Start + 1] = (avc1Size >> 16) & 0xFF;
    data[avc1Start + 2] = (avc1Size >> 8) & 0xFF;
    data[avc1Start + 3] = avc1Size & 0xFF;

    uint32_t stsd1Size = static_cast<uint32_t>(data.size() - stsd1Start);
    data[stsd1Start] = (stsd1Size >> 24) & 0xFF;
    data[stsd1Start + 1] = (stsd1Size >> 16) & 0xFF;
    data[stsd1Start + 2] = (stsd1Size >> 8) & 0xFF;
    data[stsd1Start + 3] = stsd1Size & 0xFF;

    uint32_t stbl1Size = static_cast<uint32_t>(data.size() - stbl1Start);
    data[stbl1Start] = (stbl1Size >> 24) & 0xFF;
    data[stbl1Start + 1] = (stbl1Size >> 16) & 0xFF;
    data[stbl1Start + 2] = (stbl1Size >> 8) & 0xFF;
    data[stbl1Start + 3] = stbl1Size & 0xFF;

    uint32_t minf1Size = static_cast<uint32_t>(data.size() - minf1Start);
    data[minf1Start] = (minf1Size >> 24) & 0xFF;
    data[minf1Start + 1] = (minf1Size >> 16) & 0xFF;
    data[minf1Start + 2] = (minf1Size >> 8) & 0xFF;
    data[minf1Start + 3] = minf1Size & 0xFF;

    uint32_t mdia1Size = static_cast<uint32_t>(data.size() - mdia1Start);
    data[mdia1Start] = (mdia1Size >> 24) & 0xFF;
    data[mdia1Start + 1] = (mdia1Size >> 16) & 0xFF;
    data[mdia1Start + 2] = (mdia1Size >> 8) & 0xFF;
    data[mdia1Start + 3] = mdia1Size & 0xFF;

    uint32_t trak1Size = static_cast<uint32_t>(data.size() - trak1Start);
    data[trak1Start] = (trak1Size >> 24) & 0xFF;
    data[trak1Start + 1] = (trak1Size >> 16) & 0xFF;
    data[trak1Start + 2] = (trak1Size >> 8) & 0xFF;
    data[trak1Start + 3] = trak1Size & 0xFF;

    // Track 2: Audio Track (soun, mp4a)
    size_t trak2Start = data.size();
    writeBe32(data, 0);
    writeString(data, "trak");

    // tkhd 2 (audio has 0 width and height)
    size_t tkhd2Start = data.size();
    writeBe32(data, 92);
    writeString(data, "tkhd");
    data.push_back(0);
    data.push_back(0); data.push_back(0); data.push_back(0);
    data.resize(tkhd2Start + 84, 0);
    writeBe32(data, 0); // width 0
    writeBe32(data, 0); // height 0

    // mdia 2
    size_t mdia2Start = data.size();
    writeBe32(data, 0);
    writeString(data, "mdia");

    // hdlr 2 (soun)
    writeBe32(data, 32);
    writeString(data, "hdlr");
    writeBe32(data, 0);
    writeBe32(data, 0);
    writeString(data, "soun");
    data.resize(data.size() + 12, 0);

    // minf 2 -> stbl 2 -> stsd 2 with mp4a
    size_t minf2Start = data.size();
    writeBe32(data, 0);
    writeString(data, "minf");

    size_t stbl2Start = data.size();
    writeBe32(data, 0);
    writeString(data, "stbl");

    size_t stsd2Start = data.size();
    writeBe32(data, 0);
    writeString(data, "stsd");
    writeBe32(data, 0);
    writeBe32(data, 1);

    // mp4a sample entry
    size_t mp4aStart = data.size();
    writeBe32(data, 36);
    writeString(data, "mp4a");
    writeBe32(data, 0); // reserved(6) + data_ref_idx(2) high part
    writeBe32(data, 0);
    writeBe32(data, 0); // reserved(8)
    writeBe32(data, 0);
    writeBe16(data, 2); // channelcount = 2
    writeBe16(data, 16); // samplesize = 16
    writeBe16(data, 0); // pre_defined
    writeBe16(data, 0); // reserved
    writeBe16(data, 44100); // sample_rate (high 16 bits)
    writeBe16(data, 0);

    uint32_t stsd2Size = static_cast<uint32_t>(data.size() - stsd2Start);
    data[stsd2Start] = (stsd2Size >> 24) & 0xFF;
    data[stsd2Start + 1] = (stsd2Size >> 16) & 0xFF;
    data[stsd2Start + 2] = (stsd2Size >> 8) & 0xFF;
    data[stsd2Start + 3] = stsd2Size & 0xFF;

    uint32_t stbl2Size = static_cast<uint32_t>(data.size() - stbl2Start);
    data[stbl2Start] = (stbl2Size >> 24) & 0xFF;
    data[stbl2Start + 1] = (stbl2Size >> 16) & 0xFF;
    data[stbl2Start + 2] = (stbl2Size >> 8) & 0xFF;
    data[stbl2Start + 3] = stbl2Size & 0xFF;

    uint32_t minf2Size = static_cast<uint32_t>(data.size() - minf2Start);
    data[minf2Start] = (minf2Size >> 24) & 0xFF;
    data[minf2Start + 1] = (minf2Size >> 16) & 0xFF;
    data[minf2Start + 2] = (minf2Size >> 8) & 0xFF;
    data[minf2Start + 3] = minf2Size & 0xFF;

    uint32_t mdia2Size = static_cast<uint32_t>(data.size() - mdia2Start);
    data[mdia2Start] = (mdia2Size >> 24) & 0xFF;
    data[mdia2Start + 1] = (mdia2Size >> 16) & 0xFF;
    data[mdia2Start + 2] = (mdia2Size >> 8) & 0xFF;
    data[mdia2Start + 3] = mdia2Size & 0xFF;

    uint32_t trak2Size = static_cast<uint32_t>(data.size() - trak2Start);
    data[trak2Start] = (trak2Size >> 24) & 0xFF;
    data[trak2Start + 1] = (trak2Size >> 16) & 0xFF;
    data[trak2Start + 2] = (trak2Size >> 8) & 0xFF;
    data[trak2Start + 3] = trak2Size & 0xFF;

    // Patch moov size
    uint32_t moovSize = static_cast<uint32_t>(data.size() - moovStart);
    data[moovStart] = (moovSize >> 24) & 0xFF;
    data[moovStart + 1] = (moovSize >> 16) & 0xFF;
    data[moovStart + 2] = (moovSize >> 8) & 0xFF;
    data[moovStart + 3] = moovSize & 0xFF;

    {
        std::ofstream ofs(mp4Path, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
    }

    auto res = MediaReader::readMetadata(mp4Path);
    ASSERT_TRUE(res.isOk()) << res.status().message();
    auto info = res.value();
    EXPECT_EQ(info.media_type, "video");
    EXPECT_NEAR(info.duration, 10.0, 0.1);
    EXPECT_EQ(info.width, 1920);
    EXPECT_EQ(info.height, 1080);
    EXPECT_EQ(info.codec, "h264"); // Must NOT be overwritten with "aac"!
    EXPECT_EQ(info.channels, 2);
}

// 3. Test MP3 with ID3v2 Tags
TEST(MediaTest, Mp3Id3v2Parser) {
    TempDir tmp;
    std::string mp3Path = (tmp.path() / "test.mp3").string();

    std::vector<uint8_t> data;
    // ID3v2.3 header: "ID3", ver 3.0, flags 0, synchsafe size
    writeString(data, "ID3");
    data.push_back(3); // major
    data.push_back(0); // revision
    data.push_back(0); // flags

    std::vector<uint8_t> tagData;
    auto addFrame = [&](const std::string& tag, const std::string& val) {
        writeString(tagData, tag);
        uint32_t fsize = static_cast<uint32_t>(val.size() + 1);
        writeBe32(tagData, fsize);
        writeBe16(tagData, 0); // flags
        tagData.push_back(0); // ISO-8859-1 encoding
        writeString(tagData, val);
    };

    addFrame("TPE1", "Imagine Artist");
    addFrame("TIT2", "Imagine Title");
    addFrame("TALB", "Imagine Album");
    addFrame("TCON", "Electronic");

    // Write synchsafe size of tagData
    uint32_t tagSize = static_cast<uint32_t>(tagData.size());
    data.push_back((tagSize >> 21) & 0x7F);
    data.push_back((tagSize >> 14) & 0x7F);
    data.push_back((tagSize >> 7) & 0x7F);
    data.push_back(tagSize & 0x7F);

    data.insert(data.end(), tagData.begin(), tagData.end());

    // Append a fake MPEG audio sync frame
    // 0xFF, 0xFB (MPEG 1 Layer 3, 128 kbps, 44100 Hz, stereo)
    data.push_back(0xFF);
    data.push_back(0xFB);
    data.push_back(0x90); // 128 kbps, 44100 Hz
    data.push_back(0x00);
    data.resize(data.size() + 417, 0); // 1 frame length ~ 418 bytes

    {
        std::ofstream ofs(mp3Path, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
    }

    auto res = MediaReader::readMetadata(mp3Path);
    ASSERT_TRUE(res.isOk()) << res.status().message();
    auto info = res.value();
    EXPECT_EQ(info.media_type, "audio");
    EXPECT_EQ(info.audio_artist, "Imagine Artist");
    EXPECT_EQ(info.audio_title, "Imagine Title");
    EXPECT_EQ(info.audio_album, "Imagine Album");
    EXPECT_EQ(info.audio_genre, "Electronic");
    EXPECT_EQ(info.sample_rate, 44100);
    EXPECT_EQ(info.bitrate, 128000);
    EXPECT_EQ(info.channels, 2);
}

// 4. Test WAV RIFF Parser
TEST(MediaTest, WavParser) {
    TempDir tmp;
    std::string wavPath = (tmp.path() / "test.wav").string();

    std::vector<uint8_t> data;
    writeString(data, "RIFF");
    writeLe32(data, 44 - 8 + 176400); // chunk size
    writeString(data, "WAVE");

    // "fmt " chunk
    writeString(data, "fmt ");
    writeLe32(data, 16); // subchunk1 size
    writeLe16(data, 1);  // PCM format
    writeLe16(data, 2);  // 2 channels (stereo)
    writeLe32(data, 44100); // 44100 Hz sample rate
    writeLe32(data, 44100 * 2 * 2); // byte rate: 176400 B/s
    writeLe16(data, 4);  // block align
    writeLe16(data, 16); // bits per sample: 16

    // "data" chunk (1 second of audio = 176400 bytes)
    writeString(data, "data");
    writeLe32(data, 176400);
    data.resize(data.size() + 176400, 0);

    {
        std::ofstream ofs(wavPath, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
    }

    auto res = MediaReader::readMetadata(wavPath);
    ASSERT_TRUE(res.isOk()) << res.status().message();
    auto info = res.value();
    EXPECT_EQ(info.media_type, "audio");
    EXPECT_EQ(info.channels, 2);
    EXPECT_EQ(info.sample_rate, 44100);
    EXPECT_NEAR(info.duration, 1.0, 0.05);
    EXPECT_EQ(info.codec, "pcm");
}

// 5. Test AVI RIFF Parser
TEST(MediaTest, AviParser) {
    TempDir tmp;
    std::string aviPath = (tmp.path() / "test.avi").string();

    std::vector<uint8_t> data;
    writeString(data, "RIFF");
    writeLe32(data, 128); // placeholder size
    writeString(data, "AVI ");

    // LIST hdrl chunk
    writeString(data, "LIST");
    writeLe32(data, 80);
    writeString(data, "hdrl");

    // avih chunk
    writeString(data, "avih");
    writeLe32(data, 56);
    writeLe32(data, 33333); // microsec per frame (~30 fps)
    writeLe32(data, 1000000); // max bytes per sec
    writeLe32(data, 0); // padding
    writeLe32(data, 0); // flags
    writeLe32(data, 300); // 300 total frames -> 300 * 33333 us = ~10.0 sec
    writeLe32(data, 0); // initial frames
    writeLe32(data, 1); // streams
    writeLe32(data, 0); // buffer size
    writeLe32(data, 1280); // width 1280
    writeLe32(data, 720);  // height 720
    data.resize(data.size() + 16, 0); // reserved

    {
        std::ofstream ofs(aviPath, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
    }

    auto res = MediaReader::readMetadata(aviPath);
    ASSERT_TRUE(res.isOk()) << res.status().message();
    auto info = res.value();
    EXPECT_EQ(info.media_type, "video");
    EXPECT_EQ(info.width, 1280);
    EXPECT_EQ(info.height, 720);
    EXPECT_NEAR(info.duration, 10.0, 0.1);
}

// 6. Test FLAC Parser with STREAMINFO and Vorbis Comments
TEST(MediaTest, FlacParser) {
    TempDir tmp;
    std::string flacPath = (tmp.path() / "test.flac").string();

    std::vector<uint8_t> data;
    writeString(data, "fLaC");

    // Block 0: STREAMINFO (size 34)
    data.push_back(0); // not last block, type 0
    data.push_back(0); data.push_back(0); data.push_back(34); // size 34
    // min/max block size (4 bytes), min/max frame size (6 bytes)
    for (int i = 0; i < 10; ++i) data.push_back(0);
    // sample_rate (20 bits): 48000 = 0x0BB80
    // channels - 1 (3 bits): 1 = stereo (2 ch)
    // bits_per_sample - 1 (5 bits): 15 = 16 bits
    // total_samples (36 bits): 480000 = 10 seconds
    data.push_back((0x0BB80 >> 12) & 0xFF);
    data.push_back((0x0BB80 >> 4) & 0xFF);
    data.push_back(((0x0BB80 & 0x0F) << 4) | (1 << 1) | 0);
    data.push_back((15 << 4) | 0);
    // total samples 480000 = 0x00075300
    data.push_back(0);
    data.push_back(0x07);
    data.push_back(0x53);
    data.push_back(0x00);
    // MD5 signature (16 bytes)
    for (int i = 0; i < 16; ++i) data.push_back(0);

    // Block 4: VORBIS_COMMENT (last block)
    std::vector<uint8_t> vcomments;
    // vendor string length + vendor
    writeLe32(vcomments, 9);
    writeString(vcomments, "reference");
    // user comment count
    writeLe32(vcomments, 3);
    auto addComment = [&](const std::string& c) {
        writeLe32(vcomments, static_cast<uint32_t>(c.size()));
        writeString(vcomments, c);
    };
    addComment("ARTIST=FLAC Maestro");
    addComment("TITLE=Lossless Symphony");
    addComment("ALBUM=Hi-Fi Dreams");

    data.push_back(0x80 | 4); // last block (0x80), type 4
    uint32_t vcLen = static_cast<uint32_t>(vcomments.size());
    data.push_back((vcLen >> 16) & 0xFF);
    data.push_back((vcLen >> 8) & 0xFF);
    data.push_back(vcLen & 0xFF);
    data.insert(data.end(), vcomments.begin(), vcomments.end());

    {
        std::ofstream ofs(flacPath, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
    }

    auto res = MediaReader::readMetadata(flacPath);
    ASSERT_TRUE(res.isOk()) << res.status().message();
    auto info = res.value();
    EXPECT_EQ(info.media_type, "audio");
    EXPECT_EQ(info.sample_rate, 48000);
    EXPECT_EQ(info.channels, 2);
    EXPECT_NEAR(info.duration, 10.0, 0.1);
    EXPECT_EQ(info.audio_artist, "FLAC Maestro");
    EXPECT_EQ(info.audio_title, "Lossless Symphony");
    EXPECT_EQ(info.audio_album, "Hi-Fi Dreams");
}

// 7. Test Procedural Thumbnail Generation
TEST(MediaTest, ProceduralThumbnailGeneration) {
    TempDir tmp;
    Cache cache(tmp.string());

    auto videoDualRes = cache.ensureProceduralThumbnails("fakevideohash123", "video");
    ASSERT_TRUE(videoDualRes.isOk()) << videoDualRes.status().message();
    auto [vSmall, vLarge] = videoDualRes.value();
    EXPECT_TRUE(std::filesystem::exists(vSmall));
    EXPECT_TRUE(std::filesystem::exists(vLarge));
    EXPECT_GT(std::filesystem::file_size(vSmall), 0u);
    EXPECT_GT(std::filesystem::file_size(vLarge), 0u);

    auto audioDualRes = cache.ensureProceduralThumbnails("fakeaudiohash456", "audio", "Beethoven - Moonlight");
    ASSERT_TRUE(audioDualRes.isOk()) << audioDualRes.status().message();
    auto [aSmall, aLarge] = audioDualRes.value();
    EXPECT_TRUE(std::filesystem::exists(aSmall));
    EXPECT_TRUE(std::filesystem::exists(aLarge));
    EXPECT_GT(std::filesystem::file_size(aSmall), 0u);
    EXPECT_GT(std::filesystem::file_size(aLarge), 0u);

    // Calling again returns cached files
    auto cachedRes = cache.ensureProceduralThumbnails("fakevideohash123", "video");
    ASSERT_TRUE(cachedRes.isOk());
    EXPECT_EQ(cachedRes.value().first, vSmall);
}

// 8. Test Database Schema V2 and SQLite CRUD with video and audio fields
TEST(MediaTest, DatabaseV2CrudAndStats) {
    CatalogDb db;
    ASSERT_TRUE(db.open(":memory:").isOk());

    MediaItem photo;
    photo.file_path = "photos/pic.jpg";
    photo.file_name = "pic.jpg";
    photo.file_size = 1000;
    photo.media_type = "photo";
    photo.content_hash = "hash_photo";
    photo.date_taken = 1000;
    auto pIdRes = db.insertMedia(photo);
    ASSERT_TRUE(pIdRes.isOk());

    MediaItem video;
    video.file_path = "videos/clip.mp4";
    video.file_name = "clip.mp4";
    video.file_size = 5000000;
    video.media_type = "video";
    video.content_hash = "hash_video";
    video.duration = 45.5;
    video.width = 1920;
    video.height = 1080;
    video.codec = "h264";
    video.bitrate = 2500000;
    video.date_taken = 2000;
    auto vIdRes = db.insertMedia(video);
    ASSERT_TRUE(vIdRes.isOk());
    int64_t vId = vIdRes.value();

    MediaItem audio;
    audio.file_path = "music/song.mp3";
    audio.file_name = "song.mp3";
    audio.file_size = 3000000;
    audio.media_type = "audio";
    audio.content_hash = "hash_audio";
    audio.duration = 180.0;
    audio.audio_artist = "Hans Zimmer";
    audio.audio_title = "Time";
    audio.audio_album = "Inception";
    audio.audio_genre = "Soundtrack";
    audio.codec = "mp3";
    audio.channels = 2;
    audio.sample_rate = 44100;
    audio.bitrate = 320000;
    audio.date_taken = 3000;
    auto aIdRes = db.insertMedia(audio);
    ASSERT_TRUE(aIdRes.isOk());

    // Verify fetching by ID preserves all new fields
    auto fetchVideo = db.getMediaById(vId);
    ASSERT_TRUE(fetchVideo.isOk());
    EXPECT_EQ(fetchVideo.value().media_type, "video");
    EXPECT_NEAR(fetchVideo.value().duration, 45.5, 0.01);
    EXPECT_EQ(fetchVideo.value().codec, "h264");
    EXPECT_EQ(fetchVideo.value().bitrate, 2500000);

    // Verify CatalogStats aggregates breakdown
    auto statsRes = db.getStats();
    ASSERT_TRUE(statsRes.isOk());
    const auto& st = statsRes.value();
    EXPECT_EQ(st.total_media, 3);
    EXPECT_EQ(st.total_photos, 1);
    EXPECT_EQ(st.total_videos, 1);
    EXPECT_EQ(st.total_audio, 1);
    EXPECT_EQ(st.total_picks, 0);
    EXPECT_EQ(st.total_rejects, 0);
    EXPECT_EQ(st.total_not_rejects, 3);
    EXPECT_NEAR(st.total_duration, 225.5, 0.1);
}

// 9. Test Query Engine filtering by media_type and audio tag search
TEST(MediaTest, QueryEngineMediaTypeAndSearch) {
    CatalogDb db;
    ASSERT_TRUE(db.open(":memory:").isOk());

    MediaItem photo;
    photo.file_path = "pic.jpg";
    photo.file_name = "pic.jpg";
    photo.media_type = "photo";
    photo.content_hash = "h1";
    photo.date_taken = 100;
    db.insertMedia(photo);

    MediaItem video1;
    video1.file_path = "vid1.mp4";
    video1.file_name = "vid1.mp4";
    video1.media_type = "video";
    video1.duration = 30.0;
    video1.content_hash = "h2";
    video1.date_taken = 200;
    db.insertMedia(video1);

    MediaItem video2;
    video2.file_path = "vid2.mp4";
    video2.file_name = "vid2.mp4";
    video2.media_type = "video";
    video2.duration = 120.0;
    video2.content_hash = "h3";
    video2.date_taken = 300;
    db.insertMedia(video2);

    MediaItem audio;
    audio.file_path = "audio.mp3";
    audio.file_name = "audio.mp3";
    audio.media_type = "audio";
    audio.duration = 200.0;
    audio.audio_artist = "Daft Punk";
    audio.audio_title = "Get Lucky";
    audio.audio_album = "RAM";
    audio.content_hash = "h4";
    audio.date_taken = 400;
    db.insertMedia(audio);

    // 1. Filter media_type = "video"
    QueryCriteria critVideo;
    critVideo.media_type = "video";
    auto qVideo = QueryBuilder::execute(db, critVideo);
    ASSERT_TRUE(qVideo.isOk());
    EXPECT_EQ(qVideo.value().total_count, 2);
    for (const auto& it : qVideo.value().items) {
        EXPECT_EQ(it.media_type, "video");
    }

    // 2. Filter media_type = "audio"
    QueryCriteria critAudio;
    critAudio.media_type = "audio";
    auto qAudio = QueryBuilder::execute(db, critAudio);
    ASSERT_TRUE(qAudio.isOk());
    EXPECT_EQ(qAudio.value().total_count, 1);
    EXPECT_EQ(qAudio.value().items[0].audio_artist, "Daft Punk");

    // 3. Sort by duration descending
    QueryCriteria critSort;
    critSort.sort_by = "duration";
    critSort.sort_descending = true;
    auto qSort = QueryBuilder::execute(db, critSort);
    ASSERT_TRUE(qSort.isOk());
    ASSERT_EQ(qSort.value().items.size(), 4u);
    EXPECT_EQ(qSort.value().items[0].content_hash, "h4"); // 200s
    EXPECT_EQ(qSort.value().items[1].content_hash, "h3"); // 120s
    EXPECT_EQ(qSort.value().items[2].content_hash, "h2"); // 30s
    EXPECT_EQ(qSort.value().items[3].content_hash, "h1"); // 0s

    // 4. Search text matches audio artist
    QueryCriteria critSearch;
    critSearch.search_text = "Daft Punk";
    auto qSearch = QueryBuilder::execute(db, critSearch);
    ASSERT_TRUE(qSearch.isOk());
    EXPECT_EQ(qSearch.value().total_count, 1);
    EXPECT_EQ(qSearch.value().items[0].audio_title, "Get Lucky");
}

// 10. Test Importer End-to-End with Video and Audio Files
TEST(MediaTest, ImporterVideoAndAudio) {
    TempDir tmpPhotos;
    TempDir tmpCache;

    // Create a dummy WAV file
    std::string wavFile = (tmpPhotos.path() / "test.wav").string();
    {
        std::vector<uint8_t> data;
        writeString(data, "RIFF");
        writeLe32(data, 44 - 8 + 88200);
        writeString(data, "WAVE");
        writeString(data, "fmt ");
        writeLe32(data, 16);
        writeLe16(data, 1);  // PCM
        writeLe16(data, 1);  // mono
        writeLe32(data, 44100);
        writeLe32(data, 88200);
        writeLe16(data, 2);
        writeLe16(data, 16);
        writeString(data, "data");
        writeLe32(data, 88200);
        data.resize(data.size() + 88200, 0);

        std::ofstream ofs(wavFile, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
    }

    CatalogDb db;
    ASSERT_TRUE(db.open(":memory:").isOk());
    Cache cache(tmpCache.string());

    Importer importer(db, cache, nullptr, tmpPhotos.string());
    auto impRes = importer.importDirectory(tmpPhotos.string(), false);
    ASSERT_TRUE(impRes.isOk()) << impRes.status().message();
    EXPECT_EQ(impRes.value().imported_files.load(), 1);

    auto itemRes = db.getMediaByPath("test.wav");
    ASSERT_TRUE(itemRes.isOk());
    const auto& item = itemRes.value();
    EXPECT_EQ(item.media_type, "audio");
    EXPECT_EQ(item.channels, 1);
    EXPECT_EQ(item.sample_rate, 44100);
    EXPECT_NEAR(item.duration, 1.0, 0.1);
    EXPECT_FALSE(item.content_hash.empty());
    EXPECT_TRUE(cache.hasThumbnail(item.content_hash, Cache::SmallSize));
    EXPECT_TRUE(cache.hasThumbnail(item.content_hash, Cache::LargeSize));
}

// 11. Test Server Routes: media_type filter, /api/media/:id/file, and thumbnail upload
TEST(MediaTest, ServerMediaEndpoints) {
    TempDir tmpDir;
    std::string dbPath = (tmpDir.path() / "test_catalog.db").string();
    std::string cacheDir = (tmpDir.path() / "thumbs").string();
    std::string photosDir = (tmpDir.path() / "photos").string();
    std::filesystem::create_directories(photosDir);

    // Create a dummy WAV file
    std::string wavName = "soundtrack.wav";
    std::string wavPath = (std::filesystem::path(photosDir) / wavName).string();
    {
        std::vector<uint8_t> data;
        writeString(data, "RIFF");
        writeLe32(data, 44 - 8 + 44100);
        writeString(data, "WAVE");
        writeString(data, "fmt ");
        writeLe32(data, 16);
        writeLe16(data, 1);
        writeLe16(data, 1);
        writeLe32(data, 22050);
        writeLe32(data, 44100);
        writeLe16(data, 2);
        writeLe16(data, 16);
        writeString(data, "data");
        writeLe32(data, 44100);
        data.resize(data.size() + 44100, 0);

        std::ofstream ofs(wavPath, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
    }

    Catalog catalog(2);
    ASSERT_TRUE(catalog.open(dbPath, cacheDir, photosDir).isOk());
    auto impRes = catalog.importFile(wavPath);
    ASSERT_TRUE(impRes.isOk()) << impRes.status().message();
    int64_t mediaId = impRes.value().id;

    std::string webDir = (tmpDir.path() / "web").string();
    std::filesystem::create_directories(webDir);
    int port = 18000 + (std::rand() % 5000);

    server::WebServer server(catalog, webDir);
    ASSERT_TRUE(server.start("127.0.0.1", port, webDir).isOk());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    httplib::Client client("127.0.0.1", port);

    // 1. GET /api/media?media_type=audio
    auto resAudio = client.Get("/api/media?media_type=audio");
    ASSERT_TRUE(resAudio != nullptr);
    EXPECT_EQ(resAudio->status, 200);
    auto jAudio = nlohmann::json::parse(resAudio->body);
    EXPECT_EQ(jAudio["total"], 1);
    EXPECT_EQ(jAudio["items"][0]["media_type"], "audio");

    // 2. GET /api/media?media_type=photo should return 0 items
    auto resPhoto = client.Get("/api/media?media_type=photo");
    ASSERT_TRUE(resPhoto != nullptr);
    EXPECT_EQ(resPhoto->status, 200);
    auto jPhoto = nlohmann::json::parse(resPhoto->body);
    EXPECT_EQ(jPhoto["total"], 0);

    // 3. GET /api/media/:id/file stream endpoint
    auto resFile = client.Get(("/api/media/" + std::to_string(mediaId) + "/file").c_str());
    ASSERT_TRUE(resFile != nullptr);
    EXPECT_EQ(resFile->status, 200);
    EXPECT_EQ(resFile->get_header_value("Content-Type"), "audio/wav");
    EXPECT_EQ(resFile->get_header_value("Accept-Ranges"), "bytes");

    // 4. POST /api/media/:id/thumbnail with generated image
    ImageBuffer dummyImg{64, 64, 3, std::vector<uint8_t>(64 * 64 * 3, 128)};
    for (size_t i = 0; i < dummyImg.data.size(); ++i) {
        dummyImg.data[i] = static_cast<uint8_t>(i % 256);
    }
    std::string thumbJpg = (tmpDir.path() / "test_thumb.jpg").string();
    ASSERT_TRUE(Generator::saveJpeg(dummyImg, thumbJpg).isOk());
    std::vector<uint8_t> jpgBytes;
    {
        std::ifstream ifs(thumbJpg, std::ios::binary);
        jpgBytes.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }

    auto postRes = client.Post(
        ("/api/media/" + std::to_string(mediaId) + "/thumbnail").c_str(),
        reinterpret_cast<const char*>(jpgBytes.data()),
        jpgBytes.size(),
        "image/jpeg"
    );
    ASSERT_TRUE(postRes != nullptr);
    EXPECT_EQ(postRes->status, 200);
    auto postJson = nlohmann::json::parse(postRes->body);
    EXPECT_EQ(postJson["status"], "ok");

    server.stop();
    catalog.close();
}

TEST(MediaTest, Mp4QuickTimeGpsParser) {
    // 1. Direct ISO 6709 parser testing
    double lat = 0.0, lon = 0.0, alt = 0.0;
    EXPECT_TRUE(MediaReader::parseIso6709("+49.4792+010.9830+295.730/", lat, lon, alt));
    EXPECT_NEAR(lat, 49.4792, 0.0001);
    EXPECT_NEAR(lon, 10.9830, 0.0001);
    EXPECT_NEAR(alt, 295.730, 0.001);

    EXPECT_TRUE(MediaReader::parseIso6709("-28.4142-016.5577/", lat, lon, alt));
    EXPECT_NEAR(lat, -28.4142, 0.0001);
    EXPECT_NEAR(lon, -16.5577, 0.0001);
    EXPECT_NEAR(alt, 0.0, 0.001);

    EXPECT_FALSE(MediaReader::parseIso6709("", lat, lon, alt));
    EXPECT_FALSE(MediaReader::parseIso6709("invalid_coords", lat, lon, alt));
    EXPECT_FALSE(MediaReader::parseIso6709("+00.0000+000.0000/", lat, lon, alt));
    EXPECT_FALSE(MediaReader::parseIso6709("+95.0000+010.0000/", lat, lon, alt));

    // 2. Synthesize QuickTime MP4 with meta -> keys + ilst
    TempDir tmp;
    std::string movPath = (tmp.path() / "test_gps.mov").string();
    std::vector<uint8_t> data;

    // ftyp
    writeBe32(data, 20);
    writeString(data, "ftyp");
    writeString(data, "qt  ");
    writeBe32(data, 512);
    writeString(data, "qt  ");

    // moov
    size_t moovStart = data.size();
    writeBe32(data, 0); // placeholder
    writeString(data, "moov");

    // mvhd
    size_t mvhdStart = data.size();
    writeBe32(data, 108);
    writeString(data, "mvhd");
    data.push_back(0); // version
    data.push_back(0); data.push_back(0); data.push_back(0); // flags
    writeBe32(data, 0); // creation
    writeBe32(data, 0); // mod
    writeBe32(data, 600); // timescale
    writeBe32(data, 6000); // 10s
    data.resize(mvhdStart + 108, 0);

    // meta box container
    size_t metaStart = data.size();
    writeBe32(data, 0); // placeholder
    writeString(data, "meta");
    writeBe32(data, 0); // version + flags

    // hdlr box
    size_t hdlrStart = data.size();
    writeBe32(data, 0); // placeholder
    writeString(data, "hdlr");
    writeBe32(data, 0); // version + flags
    writeBe32(data, 0); // pre_defined
    writeString(data, "mdta");
    data.resize(data.size() + 17, 0);
    uint32_t hdlrSize = static_cast<uint32_t>(data.size() - hdlrStart);
    data[hdlrStart] = (hdlrSize >> 24) & 0xFF;
    data[hdlrStart + 1] = (hdlrSize >> 16) & 0xFF;
    data[hdlrStart + 2] = (hdlrSize >> 8) & 0xFF;
    data[hdlrStart + 3] = hdlrSize & 0xFF;

    // keys box
    size_t keysStart = data.size();
    writeBe32(data, 0); // placeholder
    writeString(data, "keys");
    writeBe32(data, 0); // version + flags
    writeBe32(data, 1); // 1 key
    std::string keyName = "com.apple.quicktime.location.ISO6709";
    writeBe32(data, static_cast<uint32_t>(8 + keyName.size()));
    writeString(data, "mdta");
    writeString(data, keyName);
    uint32_t keysSize = static_cast<uint32_t>(data.size() - keysStart);
    data[keysStart] = (keysSize >> 24) & 0xFF;
    data[keysStart + 1] = (keysSize >> 16) & 0xFF;
    data[keysStart + 2] = (keysSize >> 8) & 0xFF;
    data[keysStart + 3] = keysSize & 0xFF;

    // ilst box
    size_t ilstStart = data.size();
    writeBe32(data, 0); // placeholder
    writeString(data, "ilst");

    // item 1 (key index 1)
    size_t item1Start = data.size();
    writeBe32(data, 0); // placeholder
    writeBe32(data, 1); // tag type = 1

    // data atom inside item 1
    std::string coordStr = "+49.4792+010.9830+295.730/";
    writeBe32(data, static_cast<uint32_t>(16 + coordStr.size()));
    writeString(data, "data");
    writeBe32(data, 1); // type = UTF-8 text
    writeBe32(data, 0); // locale
    writeString(data, coordStr);

    uint32_t item1Size = static_cast<uint32_t>(data.size() - item1Start);
    data[item1Start] = (item1Size >> 24) & 0xFF;
    data[item1Start + 1] = (item1Size >> 16) & 0xFF;
    data[item1Start + 2] = (item1Size >> 8) & 0xFF;
    data[item1Start + 3] = item1Size & 0xFF;

    uint32_t ilstSize = static_cast<uint32_t>(data.size() - ilstStart);
    data[ilstStart] = (ilstSize >> 24) & 0xFF;
    data[ilstStart + 1] = (ilstSize >> 16) & 0xFF;
    data[ilstStart + 2] = (ilstSize >> 8) & 0xFF;
    data[ilstStart + 3] = ilstSize & 0xFF;

    // Patch meta size
    uint32_t metaSize = static_cast<uint32_t>(data.size() - metaStart);
    data[metaStart] = (metaSize >> 24) & 0xFF;
    data[metaStart + 1] = (metaSize >> 16) & 0xFF;
    data[metaStart + 2] = (metaSize >> 8) & 0xFF;
    data[metaStart + 3] = metaSize & 0xFF;

    // Patch moov size
    uint32_t moovSize = static_cast<uint32_t>(data.size() - moovStart);
    data[moovStart] = (moovSize >> 24) & 0xFF;
    data[moovStart + 1] = (moovSize >> 16) & 0xFF;
    data[moovStart + 2] = (moovSize >> 8) & 0xFF;
    data[moovStart + 3] = moovSize & 0xFF;

    // Write file
    {
        std::ofstream ofs(movPath, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
    }

    auto res = MediaReader::readMetadata(movPath);
    ASSERT_TRUE(res.isOk()) << res.status().message();
    auto info = res.value();
    EXPECT_TRUE(info.has_gps);
    EXPECT_NEAR(info.latitude, 49.4792, 0.0001);
    EXPECT_NEAR(info.longitude, 10.9830, 0.0001);
    EXPECT_NEAR(info.altitude, 295.730, 0.001);

    // 3. Test with real camera video if available
    std::string realMov = "D:/Foto/Camera Roll/IMG_0001.MOV";
    if (std::filesystem::exists(realMov)) {
        auto realRes = MediaReader::readMetadata(realMov);
        ASSERT_TRUE(realRes.isOk()) << realRes.status().message();
        auto realInfo = realRes.value();
        EXPECT_TRUE(realInfo.has_gps);
        EXPECT_NEAR(realInfo.latitude, 49.4792, 0.0001);
        EXPECT_NEAR(realInfo.longitude, 10.9830, 0.0001);
        EXPECT_NEAR(realInfo.altitude, 295.730, 0.001);
    }
}

