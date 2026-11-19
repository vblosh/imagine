#include "imagine/metadata/exif_reader.hpp"
#include <exif.h>
#include <fstream>
#include <vector>
#include <ctime>
#include <sstream>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <array>
#include <cstring>
#include <zlib.h>

namespace imagine::metadata {

namespace {

constexpr std::array<uint8_t, 8> kPngSignature{0x89, 'P', 'N', 'G', 13, 10, 26, 10};
constexpr std::array<uint8_t, 6> kExifIdentifier{'E', 'x', 'i', 'f', 0, 0};

bool isPng(std::span<const uint8_t> data) {
    return data.size() >= kPngSignature.size() &&
        std::equal(kPngSignature.begin(), kPngSignature.end(), data.begin());
}

uint32_t readBig32(std::span<const uint8_t> data, size_t offset) {
    return (uint32_t{data[offset]} << 24) | (uint32_t{data[offset + 1]} << 16) |
           (uint32_t{data[offset + 2]} << 8) | data[offset + 3];
}

void appendBig32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<uint8_t>(value >> shift));
    }
}

struct ExifBlock {
    size_t offset{};
    size_t size{};
    std::span<const uint8_t> tiff;
};

ExifBlock findExifBlock(std::span<const uint8_t> data) {
    if (isPng(data)) {
        for (size_t pos = kPngSignature.size(); data.size() - pos >= 12;) {
            const size_t length = readBig32(data, pos);
            if (length > data.size() - pos - 12) break;
            if (std::memcmp(data.data() + pos + 4, "eXIf", 4) == 0) {
                return {pos, length + 12, data.subspan(pos + 8, length)};
            }
            pos += length + 12;
        }
    } else if (data.size() >= 2 && data[0] == 0xff && data[1] == 0xd8) {
        for (size_t pos = 2; data.size() - pos >= 4 && data[pos] == 0xff;) {
            if (data[pos + 1] == 0xff) {
                ++pos;
                continue;
            }
            const auto marker = data[pos + 1];
            if (marker == 0xda || marker == 0xd9) break;
            const size_t length = size_t{data[pos + 2]} * 256 + data[pos + 3];
            if (length < 2 || length > data.size() - pos - 2) break;
            const size_t identifierLength = kExifIdentifier.size();
            if (marker == 0xe1 && length >= 2 + identifierLength &&
                std::equal(kExifIdentifier.begin(), kExifIdentifier.end(), data.begin() + pos + 4)) {
                return {pos, length + 2, data.subspan(pos + 4 + identifierLength, length - 2 - identifierLength)};
            }
            pos += length + 2;
        }
    }
    return {};
}

Status normalizeExif(std::vector<uint8_t>& tiff, int width, int height) {
    const bool little = tiff.size() >= 8 && tiff[0] == 'I' && tiff[1] == 'I';
    if (!little && (tiff.size() < 8 || tiff[0] != 'M' || tiff[1] != 'M')) {
        return Status::parseError("Invalid EXIF byte order");
    }
    auto read = [&](size_t offset, size_t bytes) {
        uint32_t value = 0;
        for (size_t i = 0; i < bytes; ++i) {
            const size_t index = little ? bytes - i - 1 : i;
            value = (value << 8) | tiff[offset + index];
        }
        return value;
    };
    auto write = [&](size_t offset, size_t bytes, uint32_t value) {
        for (size_t i = 0; i < bytes; ++i) {
            const size_t index = little ? i : bytes - i - 1;
            tiff[offset + index] = static_cast<uint8_t>(value >> (i * 8));
        }
    };
    size_t exifOffset = 0;
    auto updateIfd = [&](size_t offset, bool primary) -> bool {
        if (offset < 8 || offset > tiff.size() || tiff.size() - offset < 6) return false;
        const size_t count = read(offset, 2);
        if (count > (tiff.size() - offset - 6) / 12) return false;
        for (size_t i = 0; i < count; ++i) {
            const size_t entry = offset + 2 + i * 12;
            const auto tag = read(entry, 2);
            const auto type = read(entry + 2, 2);
            if (read(entry + 4, 4) != 1 || (type != 3 && type != 4)) continue;
            const size_t bytes = type == 3 ? 2 : 4;
            if (primary && tag == 0x8769 && type == 4) exifOffset = read(entry + 8, 4);
            if (primary && tag == 0x0112) write(entry + 8, bytes, 1);
            if (tag == (primary ? 0x0100u : 0xa002u)) write(entry + 8, bytes, width);
            if (tag == (primary ? 0x0101u : 0xa003u)) write(entry + 8, bytes, height);
        }
        // The original image's embedded thumbnail no longer depicts the edited pixels.
        if (primary) write(offset + 2 + count * 12, 4, 0);
        return true;
    };
    if (read(2, 2) != 42 || !updateIfd(read(4, 4), true) ||
        (exifOffset != 0 && !updateIfd(exifOffset, false))) {
        return Status::parseError("Invalid EXIF directory");
    }
    return Status::ok();
}

} // namespace

Status ExifReader::preserveForEdit(std::span<const uint8_t> source, std::vector<uint8_t>& edited,
                                   int width, int height) {
    auto sourceBlock = findExifBlock(source);
    auto editedBlock = findExifBlock(edited);
    if (sourceBlock.tiff.empty()) sourceBlock = editedBlock;
    if (sourceBlock.tiff.empty()) return Status::ok();
    std::vector<uint8_t> tiff(sourceBlock.tiff.begin(), sourceBlock.tiff.end());
    auto normalized = normalizeExif(tiff, width, height);
    if (!normalized.isOk()) return normalized;

    std::vector<uint8_t> block;
    size_t insertAt = 2;
    if (isPng(edited)) {
        appendBig32(block, static_cast<uint32_t>(tiff.size()));
        block.insert(block.end(), {'e', 'X', 'I', 'f'});
        block.insert(block.end(), tiff.begin(), tiff.end());
        appendBig32(block, crc32(0, block.data() + 4, static_cast<uInt>(block.size() - 4)));
        insertAt = 33; // Immediately after the PNG signature and IHDR chunk.
        if (edited.size() < insertAt) return Status::parseError("Invalid PNG header");
    } else {
        const size_t length = tiff.size() + kExifIdentifier.size() + 2;
        if (length > 65535) return Status::invalidArgument("EXIF data too large for JPEG");
        block = {0xff, 0xe1, static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length)};
        block.insert(block.end(), kExifIdentifier.begin(), kExifIdentifier.end());
        block.insert(block.end(), tiff.begin(), tiff.end());
    }
    while (editedBlock.size > 0) {
        edited.erase(edited.begin() + editedBlock.offset, edited.begin() + editedBlock.offset + editedBlock.size);
        editedBlock = findExifBlock(edited);
    }
    edited.insert(edited.begin() + insertAt, block.begin(), block.end());
    return Status::ok();
}

int64_t ExifReader::parseExifDate(const std::string& dateStr) {
    if (dateStr.empty() || dateStr.size() < 19) {
        return 0;
    }

    // Format: "YYYY:MM:DD HH:MM:SS"
    std::istringstream ss(dateStr);
    char sep1, sep2, sep3, sep4;
    int year, month, day, hour, min, sec;
    if (ss >> year >> sep1 >> month >> sep2 >> day >> hour >> sep3 >> min >> sep4 >> sec) {
        if ((sep1 != ':' && sep1 != '-') || (sep2 != ':' && sep2 != '-') ||
            sep3 != ':' || sep4 != ':') {
            return 0;
        }
        if (hour < 0 || hour >= 24 || min < 0 || min >= 60 || sec < 0 || sec > 60) {
            return 0;
        }
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

    const auto block = findExifBlock({data, size});
    if (!block.tiff.empty()) {
        std::vector<uint8_t> segment(kExifIdentifier.begin(), kExifIdentifier.end());
        segment.insert(segment.end(), block.tiff.begin(), block.tiff.end());
        code = info.parseFromEXIFSegment(segment.data(), static_cast<unsigned int>(segment.size()));
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
    if (std::isfinite(info.GeoLocation.Latitude) && std::isfinite(info.GeoLocation.Longitude) &&
        info.GeoLocation.Latitude >= -90.0 && info.GeoLocation.Latitude <= 90.0 &&
        info.GeoLocation.Longitude >= -180.0 && info.GeoLocation.Longitude <= 180.0 &&
        (info.GeoLocation.Latitude != 0.0 || info.GeoLocation.Longitude != 0.0)) {
        ed.has_gps = true;
        ed.latitude = info.GeoLocation.Latitude;
        ed.longitude = info.GeoLocation.Longitude;
        ed.altitude = std::isfinite(info.GeoLocation.Altitude) ? info.GeoLocation.Altitude : 0.0;
    }

    return ed;
}

Result<ExifData> ExifReader::readFromFile(const std::string& filePath) {
    std::ifstream file(pathFromUtf8(filePath), std::ios::binary);
    if (!file.is_open()) {
        return Status::ioError("Unable to open image file: " + filePath);
    }

    // Read up to 512KB to accommodate JPEGs with large preceding ICC/XMP segments
    std::vector<uint8_t> buffer(512 * 1024);
    file.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
    size_t bytesRead = file.gcount();
    if (bytesRead == 0) {
        return Status::ioError("Empty image file: " + filePath);
    }

    return readFromBuffer(buffer.data(), bytesRead);
}

} // namespace imagine::metadata
