#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <optional>
#include "imagine/common/error.hpp"

namespace imagine::metadata {

struct MediaFileInfo {
    std::string media_type{"photo"}; // "photo", "video", "audio"
    int32_t width{0};
    int32_t height{0};
    double duration{0.0}; // in seconds
    int64_t date_taken{0}; // Unix timestamp in seconds
    std::string audio_artist;
    std::string audio_title;
    std::string audio_album;
    std::string audio_genre;
    std::string codec;
    int32_t bitrate{0};
    int32_t channels{0};
    int32_t sample_rate{0};
    int32_t orientation{1};
    std::vector<uint8_t> cover_art;
    std::string cover_mime;
    bool has_gps{false};
    double latitude{0.0};
    double longitude{0.0};
    double altitude{0.0};
};

class FfmpegHelper {
public:
    static bool isAvailable();
    static Result<std::string> extractVideoFrame(
        const std::string& videoPath,
        double timeSeconds,
        const std::string& outJpegPath
    );
};

class MediaReader {
public:
    static bool isSupportedExtension(const std::string& path);
    static bool isVideoExtension(const std::string& ext);
    static bool isAudioExtension(const std::string& ext);
    static bool isImageExtension(const std::string& ext);
    static std::string detectMediaType(const std::string& path);
    static bool parseIso6709(const std::string& str, double& outLat, double& outLon, double& outAlt);

    static Result<MediaFileInfo> readMetadata(const std::string& filePath);

    static Result<MediaFileInfo> readMp4(const std::string& filePath);
    static Result<MediaFileInfo> readWebm(const std::string& filePath);
    static Result<MediaFileInfo> readAvi(const std::string& filePath);
    static Result<MediaFileInfo> readMp3(const std::string& filePath);
    static Result<MediaFileInfo> readFlac(const std::string& filePath);
    static Result<MediaFileInfo> readWav(const std::string& filePath);
    static Result<MediaFileInfo> readOgg(const std::string& filePath);
};

} // namespace imagine::metadata
