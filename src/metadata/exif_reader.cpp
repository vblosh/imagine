#include "imagine/metadata/exif_reader.hpp"
#include <exif.h>
#include <fstream>
#include <vector>
#include <ctime>
#include <sstream>
#include <chrono>

namespace imagine::metadata {

int64_t ExifReader::parseExifDate(const std::string& dateStr) {
    if (dateStr.empty() || dateStr.size() < 19) {
        return 0;
    }

    // Format: "YYYY:MM:DD HH:MM:SS"
    std::istringstream ss(dateStr);
    char sep;
    int year, month, day, hour, min, sec;
    if (ss >> year >> sep >> month >> sep >> day >> hour >> sep >> min >> sep >> sec) {
        std::chrono::year_month_day ymd{
            std::chrono::year{year},
            std::chrono::month{static_cast<unsigned>(month)},
            std::chrono::day{static_cast<unsigned>(day)}
        };
        if (!ymd.ok()) return 0;
        std::chrono::sys_days sd = ymd;
        auto tp = sd + std::chrono::hours{hour} + std::chrono::minutes{min} + std::chrono::seconds{sec};
        return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
    }
    return 0;
}

Result<ExifData> ExifReader::readFromBuffer(const uint8_t* data, size_t size) {
    if (!data || size < 4) {
        return Status::parseError("No EXIF data found or failed to parse");
    }

    easyexif::EXIFInfo info;
    int code = PARSE_EXIF_ERROR_NO_EXIF;

    // If data starts with JPEG SOI, find and parse the APP1 (0xFF 0xE1) Exif segment directly.
    // This avoids easyexif's parseFrom() bug when given a buffer slice (e.g. 128KB):
    // parseFrom() scans backwards from buffer end for 0xFF 0xD9 (JPEG EOI). In images with
    // an embedded thumbnail, it encounters the thumbnail's 0xFF 0xD9, truncates 'len' to that,
    // and wrongly rejects the APP1 section as corrupt (code 1985) when padding bytes follow.
    if (data[0] == 0xFF && data[1] == 0xD8) {
        size_t offs = 2;
        while (offs + 4 <= size) {
            if (data[offs] != 0xFF) {
                offs++;
                continue;
            }
            uint8_t marker = data[offs + 1];
            if (marker == 0xFF) {
                offs++;
                continue;
            }
            if (marker == 0xD8 || marker == 0xD9 || (marker >= 0xD0 && marker <= 0xD7)) {
                offs += 2;
                continue;
            }
            if (marker == 0xDA) { // SOS (Start of Scan) - compressed image data begins
                break;
            }
            uint16_t sectionLen = (static_cast<uint16_t>(data[offs + 2]) << 8) | data[offs + 3];
            if (sectionLen < 2) break;

            if (marker == 0xE1) { // APP1
                size_t payloadOffs = offs + 4;
                size_t payloadLen = sectionLen - 2;
                if (payloadOffs + 6 <= size && std::equal(data + payloadOffs, data + payloadOffs + 6, "Exif\0\0")) {
                    size_t availableLen = std::min(payloadLen, size - payloadOffs);
                    code = info.parseFromEXIFSegment(data + payloadOffs, static_cast<unsigned int>(availableLen));
                    if (code == PARSE_EXIF_SUCCESS) {
                        break;
                    }
                }
            }
            offs += 2 + sectionLen;
        }
    }

    // Fallback to standard easyexif parseFrom
    if (code != PARSE_EXIF_SUCCESS) {
        code = info.parseFrom(data, static_cast<unsigned int>(size));
    }

    if (code != PARSE_EXIF_SUCCESS) {
        return Status::parseError("No EXIF data found or failed to parse");
    }

    ExifData ed;
    ed.camera_make = info.Make;
    ed.camera_model = info.Model;
    ed.lens = info.LensInfo.Model;
    ed.date_taken_str = !info.DateTimeOriginal.empty() ? info.DateTimeOriginal : info.DateTime;
    ed.date_taken = parseExifDate(ed.date_taken_str);
    ed.exposure_time = info.ExposureTime;
    ed.f_number = info.FNumber;
    ed.iso = static_cast<int32_t>(info.ISOSpeedRatings);
    ed.focal_length = info.FocalLength;
    ed.orientation = static_cast<int32_t>(info.Orientation);
    if (ed.orientation < 1 || ed.orientation > 8) {
        ed.orientation = 1;
    }

    // GPS
    if (info.GeoLocation.Latitude != 0.0 || info.GeoLocation.Longitude != 0.0) {
        ed.has_gps = true;
        ed.latitude = info.GeoLocation.Latitude;
        ed.longitude = info.GeoLocation.Longitude;
        ed.altitude = info.GeoLocation.Altitude;
    }

    return ed;
}

Result<ExifData> ExifReader::readFromFile(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        return Status::ioError("Unable to open image file: " + filePath);
    }

    // EXIF header is within first 64KB - 128KB of JPEG
    std::vector<uint8_t> buffer(128 * 1024);
    file.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
    size_t bytesRead = file.gcount();
    if (bytesRead == 0) {
        return Status::ioError("Empty image file: " + filePath);
    }

    return readFromBuffer(buffer.data(), bytesRead);
}

} // namespace imagine::metadata
