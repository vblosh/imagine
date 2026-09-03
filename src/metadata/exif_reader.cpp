#include "imagine/metadata/exif_reader.hpp"
#include <exif.h>
#include <fstream>
#include <vector>
#include <ctime>
#include <sstream>
#include <iomanip>

namespace imagine::metadata {

int64_t ExifReader::parseExifDate(const std::string& dateStr) {
    if (dateStr.empty() || dateStr.size() < 19) {
        return 0;
    }

    // Format: "YYYY:MM:DD HH:MM:SS"
    std::tm tm{};
    std::istringstream ss(dateStr);
    char sep;
    int year, month, day, hour, min, sec;
    if (ss >> year >> sep >> month >> sep >> day >> hour >> sep >> min >> sep >> sec) {
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = min;
        tm.tm_sec = sec;
        tm.tm_isdst = -1;
        time_t t = timegm(&tm);
        if (t != -1) {
            return static_cast<int64_t>(t);
        }
    }
    return 0;
}

Result<ExifData> ExifReader::readFromBuffer(const uint8_t* data, size_t size) {
    easyexif::EXIFInfo info;
    int code = info.parseFrom(data, static_cast<unsigned int>(size));
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
