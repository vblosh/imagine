#include "imagine/metadata/media_reader.hpp"
#include "imagine/common/logger.hpp"
#include "imagine/common/types.hpp"
#include <fstream>
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <functional>

namespace imagine::metadata {

namespace {

// Helpers for reading big-endian / little-endian primitives
uint16_t readBe16(const uint8_t* p) {
    return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

uint32_t readBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           static_cast<uint32_t>(p[3]);
}

uint64_t readBe64(const uint8_t* p) {
    return (static_cast<uint64_t>(p[0]) << 56) |
           (static_cast<uint64_t>(p[1]) << 48) |
           (static_cast<uint64_t>(p[2]) << 40) |
           (static_cast<uint64_t>(p[3]) << 32) |
           (static_cast<uint64_t>(p[4]) << 24) |
           (static_cast<uint64_t>(p[5]) << 16) |
           (static_cast<uint64_t>(p[6]) << 8)  |
           static_cast<uint64_t>(p[7]);
}

uint16_t readLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

std::string getExtensionLower(const std::string& path) {
    std::filesystem::path p = pathFromUtf8(path);
    std::string ext = p.extension().string();
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

int64_t getFileModifiedTime(const std::string& filePath) {
    std::error_code ec;
    auto lwt = std::filesystem::last_write_time(pathFromUtf8(filePath), ec);
    if (!ec) {
        auto sctp = std::chrono::clock_cast<std::chrono::system_clock>(lwt);
        return std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count();
    }
    return 0;
}

} // anonymous namespace

// --- FfmpegHelper implementation ---

bool FfmpegHelper::isAvailable() {
    static int available = -1;
    if (available != -1) {
        return available == 1;
    }
    const char* customPath = std::getenv("IMAGINE_FFMPEG_PATH");
    std::string cmd = (customPath && *customPath) ? (std::string(customPath) + " -version >/dev/null 2>&1")
                                                  : "ffmpeg -version >/dev/null 2>&1";
    int ret = std::system(cmd.c_str());
    available = (ret == 0) ? 1 : 0;
    return available == 1;
}

Result<std::string> FfmpegHelper::extractVideoFrame(
    const std::string& videoPath,
    double timeSeconds,
    const std::string& outJpegPath
) {
    if (!isAvailable()) {
        return Status::internal("FFmpeg is not available on this system");
    }

    std::string bin = "ffmpeg";
    const char* customPath = std::getenv("IMAGINE_FFMPEG_PATH");
    if (customPath && *customPath) {
        bin = customPath;
    }

    std::ostringstream ss;
    ss << bin << " -v error -ss " << timeSeconds << " -i \"" << videoPath
       << "\" -vframes 1 -f image2 -y \"" << outJpegPath << "\" 2>/dev/null";

    int ret = std::system(ss.str().c_str());
    if (ret != 0) {
        return Status::internal("FFmpeg frame extraction failed with return code " + std::to_string(ret));
    }

    std::error_code ec;
    if (!std::filesystem::exists(outJpegPath, ec) || std::filesystem::file_size(outJpegPath, ec) == 0) {
        return Status::internal("FFmpeg did not produce a valid output image");
    }

    return outJpegPath;
}

// --- MediaReader implementation ---

bool MediaReader::isImageExtension(const std::string& ext) {
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" ||
           ext == ".bmp" || ext == ".webp" || ext == ".tiff" || ext == ".tif";
}

bool MediaReader::isVideoExtension(const std::string& ext) {
    return ext == ".mp4" || ext == ".m4v" || ext == ".mov" ||
           ext == ".webm" || ext == ".mkv" || ext == ".avi";
}

bool MediaReader::isAudioExtension(const std::string& ext) {
    return ext == ".mp3" || ext == ".wav" || ext == ".flac" ||
           ext == ".ogg" || ext == ".m4a" || ext == ".aac";
}

bool MediaReader::isSupportedExtension(const std::string& path) {
    std::string ext = getExtensionLower(path);
    return isImageExtension(ext) || isVideoExtension(ext) || isAudioExtension(ext);
}

std::string MediaReader::detectMediaType(const std::string& path) {
    std::string ext = getExtensionLower(path);
    if (isVideoExtension(ext)) return "video";
    if (isAudioExtension(ext)) return "audio";
    return "photo";
}

Result<MediaFileInfo> MediaReader::readMetadata(const std::string& filePath) {
    std::string ext = getExtensionLower(filePath);
    int64_t mtime = getFileModifiedTime(filePath);

    Result<MediaFileInfo> res = Status::invalidArgument("Unsupported media format");

    if (ext == ".mp4" || ext == ".m4v" || ext == ".mov" || ext == ".m4a") {
        res = readMp4(filePath);
    } else if (ext == ".webm" || ext == ".mkv") {
        res = readWebm(filePath);
    } else if (ext == ".avi") {
        res = readAvi(filePath);
    } else if (ext == ".mp3") {
        res = readMp3(filePath);
    } else if (ext == ".flac") {
        res = readFlac(filePath);
    } else if (ext == ".wav") {
        res = readWav(filePath);
    } else if (ext == ".ogg") {
        res = readOgg(filePath);
    }

    if (res.isOk()) {
        auto info = res.value();
        if (info.date_taken <= 0) {
            info.date_taken = mtime;
        }
        if (info.media_type.empty()) {
            info.media_type = detectMediaType(filePath);
        }
        return info;
    }

    // Fallback basic info if container parser encountered errors
    MediaFileInfo fallback;
    fallback.media_type = detectMediaType(filePath);
    fallback.date_taken = mtime;
    return fallback;
}

// --- MP4 / MOV / M4V / M4A Parser ---

Result<MediaFileInfo> MediaReader::readMp4(const std::string& filePath) {
    std::ifstream file(pathFromUtf8(filePath), std::ios::binary);
    if (!file.is_open()) {
        return Status::ioError("Unable to open MP4 file: " + filePath);
    }

    std::string ext = getExtensionLower(filePath);
    MediaFileInfo info;
    info.media_type = (ext == ".m4a") ? "audio" : "video";
    info.codec = (info.media_type == "audio") ? "aac" : "h264";

    file.seekg(0, std::ios::end);
    uint64_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    auto readBoxHeader = [&](uint64_t& outBoxSize, std::string& outType) -> bool {
        uint8_t hdr[8];
        if (!file.read(reinterpret_cast<char*>(hdr), 8)) return false;
        outBoxSize = readBe32(hdr);
        outType = std::string(reinterpret_cast<char*>(hdr + 4), 4);
        if (outBoxSize == 1) {
            uint8_t extHdr[8];
            if (!file.read(reinterpret_cast<char*>(extHdr), 8)) return false;
            outBoxSize = readBe64(extHdr);
        } else if (outBoxSize == 0) {
            outBoxSize = fileSize - static_cast<uint64_t>(file.tellg()) + 8;
        }
        return true;
    };

    std::function<void(uint64_t, uint64_t)> parseContainer;
    parseContainer = [&](uint64_t startPos, uint64_t endPos) {
        file.seekg(startPos, std::ios::beg);
        while (static_cast<uint64_t>(file.tellg()) < endPos && file.good()) {
            uint64_t currentBoxPos = file.tellg();
            uint64_t boxSize = 0;
            std::string boxType;
            if (!readBoxHeader(boxSize, boxType) || boxSize < 8) {
                break;
            }
            uint64_t dataPos = file.tellg();
            uint64_t nextBoxPos = currentBoxPos + boxSize;

            if (boxType == "moov" || boxType == "trak" || boxType == "mdia" ||
                boxType == "minf" || boxType == "stbl" || boxType == "udta") {
                parseContainer(dataPos, std::min(nextBoxPos, endPos));
            } else if (boxType == "mvhd") {
                uint8_t ver = file.get();
                file.seekg(3, std::ios::cur); // flags
                uint32_t timeScale = 0;
                uint64_t durationUnits = 0;
                int64_t creationTime = 0;
                if (ver == 1) {
                    uint8_t buf[28];
                    if (file.read(reinterpret_cast<char*>(buf), 28)) {
                        creationTime = static_cast<int64_t>(readBe64(buf));
                        timeScale = readBe32(buf + 16);
                        durationUnits = readBe64(buf + 20);
                    }
                } else {
                    uint8_t buf[16];
                    if (file.read(reinterpret_cast<char*>(buf), 16)) {
                        creationTime = static_cast<int64_t>(readBe32(buf));
                        timeScale = readBe32(buf + 8);
                        durationUnits = readBe32(buf + 12);
                    }
                }
                if (timeScale > 0) {
                    info.duration = static_cast<double>(durationUnits) / static_cast<double>(timeScale);
                }
                if (creationTime > 2082844800LL) {
                    info.date_taken = creationTime - 2082844800LL;
                }
            } else if (boxType == "tkhd") {
                uint8_t ver = file.get();
                file.seekg(3, std::ios::cur); // flags
                int skipToMatrix = (ver == 1) ? 48 : 36;
                file.seekg(skipToMatrix, std::ios::cur);

                // Matrix structure (36 bytes)
                uint8_t matrixBuf[36];
                if (file.read(reinterpret_cast<char*>(matrixBuf), 36)) {
                    int32_t b = static_cast<int32_t>(readBe32(matrixBuf + 4));
                    int32_t c = static_cast<int32_t>(readBe32(matrixBuf + 12));
                    if (b > 0 && c < 0) {
                        info.orientation = 6; // 90 CW
                    } else if (b < 0 && c > 0) {
                        info.orientation = 8; // 270 CW
                    }
                }

                uint8_t dimBuf[8];
                if (file.read(reinterpret_cast<char*>(dimBuf), 8)) {
                    uint32_t w = readBe32(dimBuf) >> 16;
                    uint32_t h = readBe32(dimBuf + 4) >> 16;
                    if (w > 0 && h > 0 && info.width == 0) {
                        info.width = static_cast<int32_t>(w);
                        info.height = static_cast<int32_t>(h);
                    }
                }
            } else if (boxType == "hdlr") {
                file.seekg(8, std::ios::cur); // version + flags + pre_defined
                char hType[4];
                if (file.read(hType, 4)) {
                    if (std::memcmp(hType, "vide", 4) == 0) {
                        info.media_type = "video";
                    } else if (std::memcmp(hType, "soun", 4) == 0 && ext == ".m4a") {
                        info.media_type = "audio";
                    }
                }
            } else if (boxType == "stsd") {
                file.seekg(8, std::ios::cur); // version + flags + entry count
                uint64_t entrySize = 0;
                std::string entryType;
                if (readBoxHeader(entrySize, entryType) && entrySize >= 16) {
                    if (entryType == "avc1") info.codec = "h264";
                    else if (entryType == "hvc1" || entryType == "hev1") info.codec = "h265";
                    else if (entryType == "vp09") info.codec = "vp9";
                    else if (entryType == "av01") info.codec = "av1";
                    else if (entryType == "mp4a") info.codec = "aac";

                    // Sample entry header
                    if (info.media_type == "video" && entrySize >= 36) {
                        file.seekg(16, std::ios::cur); // skip reserved, data_reference_index, pre_defined
                        uint8_t videoDimBuf[4];
                        if (file.read(reinterpret_cast<char*>(videoDimBuf), 4)) {
                            int w = readBe16(videoDimBuf);
                            int h = readBe16(videoDimBuf + 2);
                            if (w > 0 && h > 0) {
                                info.width = w;
                                info.height = h;
                            }
                        }
                    } else if (info.media_type == "audio" && entrySize >= 36) {
                        file.seekg(16, std::ios::cur);
                        uint8_t audioBuf[8];
                        if (file.read(reinterpret_cast<char*>(audioBuf), 8)) {
                            info.channels = readBe16(audioBuf);
                            info.sample_rate = readBe16(audioBuf + 6);
                        }
                    }
                }
            } else if (boxType == "meta") {
                file.seekg(4, std::ios::cur); // skip version & flags
                parseContainer(file.tellg(), std::min(nextBoxPos, endPos));
            } else if (boxType == "ilst") {
                // Parse iTunes style metadata atoms
                while (static_cast<uint64_t>(file.tellg()) < nextBoxPos && file.good()) {
                    uint64_t tagPos = file.tellg();
                    uint64_t tagSize = 0;
                    std::string tagType;
                    if (!readBoxHeader(tagSize, tagType) || tagSize < 8) break;
                    uint64_t nextTagPos = tagPos + tagSize;

                    // Search for inner "data" atom
                    uint64_t dataAtomPos = file.tellg();
                    while (dataAtomPos + 8 <= nextTagPos) {
                        file.seekg(dataAtomPos, std::ios::beg);
                        uint64_t dataSize = 0;
                        std::string dType;
                        if (!readBoxHeader(dataSize, dType) || dataSize < 16) break;
                        if (dType == "data") {
                            uint8_t typeFlag[4];
                            file.read(reinterpret_cast<char*>(typeFlag), 4);
                            file.seekg(4, std::ios::cur); // locale
                            uint32_t valLen = static_cast<uint32_t>(dataSize - 16);

                            if (tagType == "\xa9" "nam" || tagType == "titl") {
                                std::string val(valLen, '\0');
                                file.read(val.data(), valLen);
                                info.audio_title = val;
                            } else if (tagType == "\xa9" "ART" || tagType == "aART") {
                                std::string val(valLen, '\0');
                                file.read(val.data(), valLen);
                                info.audio_artist = val;
                            } else if (tagType == "\xa9" "alb") {
                                std::string val(valLen, '\0');
                                file.read(val.data(), valLen);
                                info.audio_album = val;
                            } else if (tagType == "\xa9" "gen" || tagType == "gnre") {
                                std::string val(valLen, '\0');
                                file.read(val.data(), valLen);
                                info.audio_genre = val;
                            } else if (tagType == "covr" && valLen > 0 && valLen < 20 * 1024 * 1024) {
                                info.cover_art.resize(valLen);
                                file.read(reinterpret_cast<char*>(info.cover_art.data()), valLen);
                                uint32_t flagVal = readBe32(typeFlag);
                                info.cover_mime = (flagVal == 14) ? "image/png" : "image/jpeg";
                            }
                            break;
                        }
                        dataAtomPos += dataSize;
                    }

                    file.seekg(nextTagPos, std::ios::beg);
                }
            }

            file.seekg(nextBoxPos, std::ios::beg);
        }
    };

    parseContainer(0, fileSize);

    if (fileSize > 0 && info.duration > 0) {
        info.bitrate = static_cast<int32_t>((fileSize * 8) / info.duration);
    }

    return info;
}

// --- WebM / MKV Parser ---

Result<MediaFileInfo> MediaReader::readWebm(const std::string& filePath) {
    std::ifstream file(pathFromUtf8(filePath), std::ios::binary);
    if (!file.is_open()) {
        return Status::ioError("Unable to open WebM/MKV file: " + filePath);
    }

    uint8_t magic[4];
    if (!file.read(reinterpret_cast<char*>(magic), 4) || readBe32(magic) != 0x1A45DFA3) {
        return Status::parseError("Not a valid EBML file: " + filePath);
    }

    file.seekg(0, std::ios::end);
    uint64_t fileSize = file.tellg();
    file.seekg(4, std::ios::beg);

    MediaFileInfo info;
    info.media_type = "video";
    info.codec = "vp9";

    auto readVInt = [&](uint64_t& outId, uint32_t& outLen) -> bool {
        int first = file.get();
        if (first == EOF) return false;
        uint8_t f = static_cast<uint8_t>(first);
        int mask = 0x80;
        outLen = 1;
        while (mask && !(f & mask)) {
            mask >>= 1;
            outLen++;
        }
        if (outLen > 8) return false;

        outId = f;
        for (uint32_t i = 1; i < outLen; ++i) {
            int nextB = file.get();
            if (nextB == EOF) return false;
            outId = (outId << 8) | static_cast<uint8_t>(nextB);
        }
        return true;
    };

    auto readVIntSize = [&](uint64_t& outSize) -> bool {
        uint64_t raw = 0;
        uint32_t len = 0;
        if (!readVInt(raw, len)) return false;
        uint64_t clearMask = (1ULL << (7 * len)) - 1;
        outSize = raw & clearMask;
        return true;
    };

    uint64_t timecodeScale = 1000000; // default 1ms
    double durationUnits = 0.0;

    // Scan top-level elements up to first 2MB
    uint64_t maxScan = std::min<uint64_t>(fileSize, 2ULL * 1024 * 1024);
    while (static_cast<uint64_t>(file.tellg()) < maxScan && file.good()) {
        uint64_t elemId = 0;
        uint32_t idLen = 0;
        if (!readVInt(elemId, idLen)) break;
        uint64_t elemSize = 0;
        if (!readVIntSize(elemSize)) break;

        uint64_t elemDataPos = file.tellg();

        if (elemId == 0x18538067) { // Segment
            continue; // enter segment
        } else if (elemId == 0x1549A966) { // Info
            uint64_t infoEnd = elemDataPos + elemSize;
            while (static_cast<uint64_t>(file.tellg()) < infoEnd && file.good()) {
                uint64_t subId = 0;
                uint32_t subIdLen = 0;
                if (!readVInt(subId, subIdLen)) break;
                uint64_t subSize = 0;
                if (!readVIntSize(subSize)) break;

                if (subId == 0x2AD7B1 && subSize <= 8) { // TimecodeScale
                    uint64_t tScale = 0;
                    for (uint64_t i = 0; i < subSize; ++i) tScale = (tScale << 8) | file.get();
                    if (tScale > 0) timecodeScale = tScale;
                } else if (subId == 0x4489 && (subSize == 4 || subSize == 8)) { // Duration
                    if (subSize == 4) {
                        uint8_t buf[4];
                        file.read(reinterpret_cast<char*>(buf), 4);
                        uint32_t raw = readBe32(buf);
                        float fval = 0.0f;
                        std::memcpy(&fval, &raw, 4);
                        durationUnits = fval;
                    } else {
                        uint8_t buf[8];
                        file.read(reinterpret_cast<char*>(buf), 8);
                        uint64_t raw = readBe64(buf);
                        double dval = 0.0;
                        std::memcpy(&dval, &raw, 8);
                        durationUnits = dval;
                    }
                } else {
                    file.seekg(subSize, std::ios::cur);
                }
            }
        } else if (elemId == 0x1654AE6B) { // Tracks
            uint64_t tracksEnd = elemDataPos + elemSize;
            while (static_cast<uint64_t>(file.tellg()) < tracksEnd && file.good()) {
                uint64_t trackId = 0;
                uint32_t tIdLen = 0;
                if (!readVInt(trackId, tIdLen)) break;
                uint64_t tSize = 0;
                if (!readVIntSize(tSize)) break;

                if (trackId == 0xAE) { // TrackEntry
                    uint64_t entryEnd = static_cast<uint64_t>(file.tellg()) + tSize;
                    while (static_cast<uint64_t>(file.tellg()) < entryEnd && file.good()) {
                        uint64_t fieldId = 0;
                        uint32_t fLen = 0;
                        if (!readVInt(fieldId, fLen)) break;
                        uint64_t fSize = 0;
                        if (!readVIntSize(fSize)) break;

                        if (fieldId == 0x86) { // CodecID
                            std::string c(fSize, '\0');
                            file.read(c.data(), fSize);
                            if (c.rfind("V_VP8", 0) == 0) info.codec = "vp8";
                            else if (c.rfind("V_VP9", 0) == 0) info.codec = "vp9";
                            else if (c.rfind("V_AV1", 0) == 0) info.codec = "av1";
                            else if (c.rfind("A_OPUS", 0) == 0) info.codec = "opus";
                            else if (c.rfind("A_VORBIS", 0) == 0) info.codec = "vorbis";
                        } else if (fieldId == 0xE0) { // Video Settings
                            uint64_t vEnd = static_cast<uint64_t>(file.tellg()) + fSize;
                            while (static_cast<uint64_t>(file.tellg()) < vEnd && file.good()) {
                                uint64_t vid = 0; uint32_t vl = 0;
                                if (!readVInt(vid, vl)) break;
                                uint64_t vs = 0;
                                if (!readVIntSize(vs)) break;
                                if (vid == 0xB0 && vs <= 4) { // PixelWidth
                                    uint32_t w = 0;
                                    for (uint64_t i = 0; i < vs; ++i) w = (w << 8) | file.get();
                                    info.width = static_cast<int32_t>(w);
                                } else if (vid == 0xBA && vs <= 4) { // PixelHeight
                                    uint32_t h = 0;
                                    for (uint64_t i = 0; i < vs; ++i) h = (h << 8) | file.get();
                                    info.height = static_cast<int32_t>(h);
                                } else {
                                    file.seekg(vs, std::ios::cur);
                                }
                            }
                        } else {
                            file.seekg(fSize, std::ios::cur);
                        }
                    }
                } else {
                    file.seekg(tSize, std::ios::cur);
                }
            }
        } else {
            file.seekg(elemDataPos + elemSize, std::ios::beg);
        }
    }

    if (durationUnits > 0.0 && timecodeScale > 0) {
        info.duration = (durationUnits * static_cast<double>(timecodeScale)) / 1e9;
    }

    if (fileSize > 0 && info.duration > 0) {
        info.bitrate = static_cast<int32_t>((fileSize * 8) / info.duration);
    }

    return info;
}

// --- AVI Parser ---

Result<MediaFileInfo> MediaReader::readAvi(const std::string& filePath) {
    std::ifstream file(pathFromUtf8(filePath), std::ios::binary);
    if (!file.is_open()) {
        return Status::ioError("Unable to open AVI file: " + filePath);
    }

    uint8_t hdr[12];
    if (!file.read(reinterpret_cast<char*>(hdr), 12) ||
        std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "AVI ", 4) != 0) {
        return Status::parseError("Not a valid AVI file");
    }

    MediaFileInfo info;
    info.media_type = "video";
    info.codec = "avi";

    // Read headers chunk
    uint8_t chunkHdr[8];
    while (file.read(reinterpret_cast<char*>(chunkHdr), 8)) {
        std::string chunkId(reinterpret_cast<char*>(chunkHdr), 4);
        uint32_t chunkSize = readLe32(chunkHdr + 4);
        uint64_t nextChunk = static_cast<uint64_t>(file.tellg()) + chunkSize;

        if (chunkId == "avih" && chunkSize >= 40) {
            uint8_t avih[40];
            if (file.read(reinterpret_cast<char*>(avih), 40)) {
                uint32_t microSecPerFrame = readLe32(avih);
                uint32_t totalFrames = readLe32(avih + 16);
                info.width = static_cast<int32_t>(readLe32(avih + 32));
                info.height = static_cast<int32_t>(readLe32(avih + 36));
                if (microSecPerFrame > 0 && totalFrames > 0) {
                    info.duration = (static_cast<double>(totalFrames) * static_cast<double>(microSecPerFrame)) / 1e6;
                }
            }
            break;
        }

        if (chunkId == "LIST") {
            char listType[4];
            file.read(listType, 4);
            continue;
        }

        file.seekg(nextChunk, std::ios::beg);
    }

    return info;
}

// --- MP3 Parser (ID3v2 & MPEG audio frame headers) ---

Result<MediaFileInfo> MediaReader::readMp3(const std::string& filePath) {
    std::ifstream file(pathFromUtf8(filePath), std::ios::binary);
    if (!file.is_open()) {
        return Status::ioError("Unable to open MP3 file: " + filePath);
    }

    file.seekg(0, std::ios::end);
    uint64_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    MediaFileInfo info;
    info.media_type = "audio";
    info.codec = "mp3";

    uint64_t audioStartPos = 0;
    uint8_t id3Hdr[10];
    if (file.read(reinterpret_cast<char*>(id3Hdr), 10) && std::memcmp(id3Hdr, "ID3", 3) == 0) {
        int ver = id3Hdr[3];
        uint32_t id3Size = (static_cast<uint32_t>(id3Hdr[6] & 0x7F) << 21) |
                           (static_cast<uint32_t>(id3Hdr[7] & 0x7F) << 14) |
                           (static_cast<uint32_t>(id3Hdr[8] & 0x7F) << 7)  |
                           static_cast<uint32_t>(id3Hdr[9] & 0x7F);

        audioStartPos = 10 + id3Size;
        uint64_t id3End = std::min<uint64_t>(audioStartPos, fileSize);

        // Parse ID3v2 frames
        while (static_cast<uint64_t>(file.tellg()) + 10 < id3End && file.good()) {
            uint8_t frameHdr[10];
            if (!file.read(reinterpret_cast<char*>(frameHdr), 10)) break;
            if (frameHdr[0] == 0) break; // padding reached

            std::string frameId(reinterpret_cast<char*>(frameHdr), 4);
            uint32_t frameSize = 0;
            if (ver == 4) { // synchsafe size in ID3v2.4
                frameSize = (static_cast<uint32_t>(frameHdr[4] & 0x7F) << 21) |
                            (static_cast<uint32_t>(frameHdr[5] & 0x7F) << 14) |
                            (static_cast<uint32_t>(frameHdr[6] & 0x7F) << 7)  |
                            static_cast<uint32_t>(frameHdr[7] & 0x7F);
            } else {
                frameSize = readBe32(frameHdr + 4);
            }

            if (frameSize == 0 || static_cast<uint64_t>(file.tellg()) + frameSize > id3End) {
                break;
            }

            uint64_t nextFrame = static_cast<uint64_t>(file.tellg()) + frameSize;

            if (frameId == "TIT2") {
                file.get(); // skip encoding
                std::string val(frameSize - 1, '\0');
                file.read(val.data(), frameSize - 1);
                info.audio_title = val;
            } else if (frameId == "TPE1") {
                file.get();
                std::string val(frameSize - 1, '\0');
                file.read(val.data(), frameSize - 1);
                info.audio_artist = val;
            } else if (frameId == "TALB") {
                file.get();
                std::string val(frameSize - 1, '\0');
                file.read(val.data(), frameSize - 1);
                info.audio_album = val;
            } else if (frameId == "TCON") {
                file.get();
                std::string val(frameSize - 1, '\0');
                file.read(val.data(), frameSize - 1);
                info.audio_genre = val;
            } else if (frameId == "APIC" && frameSize > 12 && frameSize < 20 * 1024 * 1024) {
                uint8_t enc = file.get();
                (void)enc;
                std::string mime;
                char c;
                while (file.get(c) && c != '\0') mime += c;
                uint8_t picType = file.get();
                (void)picType;
                // skip description
                while (file.get(c) && c != '\0') {}

                uint64_t cur = file.tellg();
                if (cur < nextFrame) {
                    uint32_t imgLen = static_cast<uint32_t>(nextFrame - cur);
                    info.cover_art.resize(imgLen);
                    file.read(reinterpret_cast<char*>(info.cover_art.data()), imgLen);
                    info.cover_mime = mime.empty() ? "image/jpeg" : mime;
                }
            }

            file.seekg(nextFrame, std::ios::beg);
        }
    }

    // Seek to audio frames to parse MPEG header & duration
    file.seekg(audioStartPos, std::ios::beg);
    uint8_t scanBuf[4096];
    size_t scanRead = file.read(reinterpret_cast<char*>(scanBuf), sizeof(scanBuf)).gcount();

    for (size_t i = 0; i + 4 < scanRead; ++i) {
        if (scanBuf[i] == 0xFF && (scanBuf[i + 1] & 0xE0) == 0xE0) { // Sync word
            uint8_t b1 = scanBuf[i + 1];
            uint8_t b2 = scanBuf[i + 2];
            uint8_t b3 = scanBuf[i + 3];

            int mpegVer = (b1 >> 3) & 0x03; // 3 = MPEG 1, 2 = MPEG 2
            int layer = (b1 >> 1) & 0x03;   // 1 = Layer III
            int bitrateIdx = (b2 >> 4) & 0x0F;
            int srateIdx = (b2 >> 2) & 0x03;
            int channelMode = (b3 >> 6) & 0x03;

            if (mpegVer == 3 && layer == 1 && bitrateIdx > 0 && bitrateIdx < 15 && srateIdx < 3) {
                static const int srateTable[] = {44100, 48000, 32000};
                static const int bitrateTable[] = {
                    0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320
                };
                info.sample_rate = srateTable[srateIdx];
                info.bitrate = bitrateTable[bitrateIdx] * 1000;
                info.channels = (channelMode == 3) ? 1 : 2;

                // Check for Xing / Info header
                size_t xingOffset = i + 4 + ((channelMode == 3) ? 17 : 32);
                if (xingOffset + 12 < scanRead) {
                    if (std::memcmp(scanBuf + xingOffset, "Xing", 4) == 0 ||
                        std::memcmp(scanBuf + xingOffset, "Info", 4) == 0) {
                        uint32_t flags = readBe32(scanBuf + xingOffset + 4);
                        if (flags & 1) { // total frames present
                            uint32_t frames = readBe32(scanBuf + xingOffset + 8);
                            if (info.sample_rate > 0) {
                                info.duration = (static_cast<double>(frames) * 1152.0) / static_cast<double>(info.sample_rate);
                            }
                        }
                    }
                }

                // Fallback duration calculation
                if (info.duration <= 0 && info.bitrate > 0 && fileSize > audioStartPos) {
                    info.duration = static_cast<double>((fileSize - audioStartPos) * 8) / static_cast<double>(info.bitrate);
                }
                break;
            }
        }
    }

    return info;
}

// --- FLAC Parser ---

Result<MediaFileInfo> MediaReader::readFlac(const std::string& filePath) {
    std::ifstream file(pathFromUtf8(filePath), std::ios::binary);
    if (!file.is_open()) {
        return Status::ioError("Unable to open FLAC file: " + filePath);
    }

    uint8_t magic[4];
    if (!file.read(reinterpret_cast<char*>(magic), 4) || std::memcmp(magic, "fLaC", 4) != 0) {
        return Status::parseError("Not a valid FLAC file");
    }

    MediaFileInfo info;
    info.media_type = "audio";
    info.codec = "flac";

    bool isLast = false;
    while (!isLast && file.good()) {
        uint8_t hdr[4];
        if (!file.read(reinterpret_cast<char*>(hdr), 4)) break;
        isLast = (hdr[0] & 0x80) != 0;
        int blockType = hdr[0] & 0x7F;
        uint32_t blockLen = (static_cast<uint32_t>(hdr[1]) << 16) |
                            (static_cast<uint32_t>(hdr[2]) << 8)  |
                            static_cast<uint32_t>(hdr[3]);

        uint64_t nextBlock = static_cast<uint64_t>(file.tellg()) + blockLen;

        if (blockType == 0 && blockLen >= 18) { // STREAMINFO
            uint8_t infoBuf[18];
            file.seekg(10, std::ios::cur); // skip min/max blocksize and framesize
            if (file.read(reinterpret_cast<char*>(infoBuf), 8)) {
                uint32_t srate = (static_cast<uint32_t>(infoBuf[0]) << 12) |
                                 (static_cast<uint32_t>(infoBuf[1]) << 4)  |
                                 (static_cast<uint32_t>(infoBuf[2]) >> 4);
                uint32_t channels = ((infoBuf[2] >> 1) & 0x07) + 1;
                uint64_t totalSamples = (static_cast<uint64_t>(infoBuf[3] & 0x0F) << 32) |
                                        (static_cast<uint64_t>(infoBuf[4]) << 24) |
                                        (static_cast<uint64_t>(infoBuf[5]) << 16) |
                                        (static_cast<uint64_t>(infoBuf[6]) << 8)  |
                                        static_cast<uint64_t>(infoBuf[7]);
                info.sample_rate = static_cast<int32_t>(srate);
                info.channels = static_cast<int32_t>(channels);
                if (srate > 0) {
                    info.duration = static_cast<double>(totalSamples) / static_cast<double>(srate);
                }
            }
        } else if (blockType == 4 && blockLen >= 8) { // VORBIS_COMMENT
            uint8_t vBuf[4];
            if (file.read(reinterpret_cast<char*>(vBuf), 4)) {
                uint32_t vendorLen = readLe32(vBuf);
                file.seekg(vendorLen, std::ios::cur);
                if (file.read(reinterpret_cast<char*>(vBuf), 4)) {
                    uint32_t numComments = readLe32(vBuf);
                    for (uint32_t c = 0; c < numComments && c < 200; ++c) {
                        uint8_t cLenBuf[4];
                        if (!file.read(reinterpret_cast<char*>(cLenBuf), 4)) break;
                        uint32_t cLen = readLe32(cLenBuf);
                        if (cLen > 4096) { file.seekg(cLen, std::ios::cur); continue; }
                        std::string comment(cLen, '\0');
                        file.read(comment.data(), cLen);

                        auto eq = comment.find('=');
                        if (eq != std::string::npos) {
                            std::string key = comment.substr(0, eq);
                            std::string val = comment.substr(eq + 1);
                            for (char& ch : key) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                            if (key == "TITLE") info.audio_title = val;
                            else if (key == "ARTIST") info.audio_artist = val;
                            else if (key == "ALBUM") info.audio_album = val;
                            else if (key == "GENRE") info.audio_genre = val;
                        }
                    }
                }
            }
        } else if (blockType == 6 && blockLen > 32 && blockLen < 20 * 1024 * 1024) { // PICTURE
            uint8_t pBuf[8];
            if (file.read(reinterpret_cast<char*>(pBuf), 8)) {
                uint32_t mimeLen = readBe32(pBuf + 4);
                std::string mime(mimeLen, '\0');
                file.read(mime.data(), mimeLen);
                uint8_t dBuf[4];
                file.read(reinterpret_cast<char*>(dBuf), 4);
                uint32_t descLen = readBe32(dBuf);
                file.seekg(descLen + 16, std::ios::cur); // skip description, width, height, depth, colors
                uint8_t lenBuf[4];
                if (file.read(reinterpret_cast<char*>(lenBuf), 4)) {
                    uint32_t dataLen = readBe32(lenBuf);
                    if (dataLen > 0 && dataLen < 20 * 1024 * 1024) {
                        info.cover_art.resize(dataLen);
                        file.read(reinterpret_cast<char*>(info.cover_art.data()), dataLen);
                        info.cover_mime = mime.empty() ? "image/jpeg" : mime;
                    }
                }
            }
        }

        file.seekg(nextBlock, std::ios::beg);
    }

    return info;
}

// --- WAV Parser ---

Result<MediaFileInfo> MediaReader::readWav(const std::string& filePath) {
    std::ifstream file(pathFromUtf8(filePath), std::ios::binary);
    if (!file.is_open()) {
        return Status::ioError("Unable to open WAV file: " + filePath);
    }

    uint8_t hdr[12];
    if (!file.read(reinterpret_cast<char*>(hdr), 12) ||
        std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
        return Status::parseError("Not a valid WAV file");
    }

    MediaFileInfo info;
    info.media_type = "audio";
    info.codec = "pcm";

    uint32_t byteRate = 0;
    uint32_t dataSize = 0;

    uint8_t chunkHdr[8];
    while (file.read(reinterpret_cast<char*>(chunkHdr), 8)) {
        std::string chunkId(reinterpret_cast<char*>(chunkHdr), 4);
        uint32_t chunkSize = readLe32(chunkHdr + 4);
        uint64_t nextChunk = static_cast<uint64_t>(file.tellg()) + chunkSize;

        if (chunkId == "fmt " && chunkSize >= 16) {
            uint8_t fmt[16];
            if (file.read(reinterpret_cast<char*>(fmt), 16)) {
                info.channels = readLe16(fmt + 2);
                info.sample_rate = static_cast<int32_t>(readLe32(fmt + 4));
                byteRate = readLe32(fmt + 8);
                if (byteRate > 0) {
                    info.bitrate = static_cast<int32_t>(byteRate * 8);
                }
            }
        } else if (chunkId == "data") {
            dataSize = chunkSize;
        }

        file.seekg(nextChunk, std::ios::beg);
    }

    if (byteRate > 0 && dataSize > 0) {
        info.duration = static_cast<double>(dataSize) / static_cast<double>(byteRate);
    }

    return info;
}

// --- OGG Parser ---

Result<MediaFileInfo> MediaReader::readOgg(const std::string& filePath) {
    std::ifstream file(pathFromUtf8(filePath), std::ios::binary);
    if (!file.is_open()) {
        return Status::ioError("Unable to open OGG file: " + filePath);
    }

    uint8_t magic[4];
    if (!file.read(reinterpret_cast<char*>(magic), 4) || std::memcmp(magic, "OggS", 4) != 0) {
        return Status::parseError("Not a valid OGG file");
    }

    MediaFileInfo info;
    info.media_type = "audio";
    info.codec = "vorbis";

    file.seekg(0, std::ios::end);
    uint64_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    // Read first page header
    uint8_t oggHdr[28];
    if (file.read(reinterpret_cast<char*>(oggHdr), 28)) {
        uint8_t numSegments = oggHdr[26];
        uint32_t payloadSize = 0;
        for (int s = 0; s < numSegments; ++s) payloadSize += file.get();

        std::vector<uint8_t> payload(payloadSize);
        if (file.read(reinterpret_cast<char*>(payload.data()), payloadSize)) {
            if (payload.size() >= 16 && std::memcmp(payload.data(), "\x01vorbis", 7) == 0) {
                info.channels = payload[11];
                info.sample_rate = static_cast<int32_t>(readLe32(payload.data() + 12));
                info.codec = "vorbis";
            } else if (payload.size() >= 19 && std::memcmp(payload.data(), "OpusHead", 8) == 0) {
                info.channels = payload[9];
                info.sample_rate = static_cast<int32_t>(readLe32(payload.data() + 12));
                info.codec = "opus";
            }
        }
    }

    // Rough duration estimation based on bitrate and file size if available
    if (info.sample_rate > 0 && fileSize > 0) {
        info.bitrate = 128000; // default Vorbis 128 kbps
        info.duration = static_cast<double>(fileSize * 8) / 128000.0;
    }

    return info;
}

} // namespace imagine::metadata
