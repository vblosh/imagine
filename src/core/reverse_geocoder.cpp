#include "imagine/core/reverse_geocoder.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <mutex>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace imagine::core {

namespace {

constexpr double kEarthRadiusKm = 6371.0088;
constexpr double kPi = 3.14159265358979323846;

std::string trim(std::string_view value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(begin, end - begin + 1));
}

std::string lowerAscii(std::string_view value) {
    std::string lower;
    lower.reserve(value.size());
    for (char ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
        lower.push_back(ch);
    }
    return lower;
}

std::vector<std::string> splitCsvLine(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    bool inQuotes = false;

    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (inQuotes) {
            if (ch == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    current.push_back('"');
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                current.push_back(ch);
            }
        } else if (ch == '"') {
            inQuotes = true;
        } else if (ch == ',') {
            fields.push_back(trim(current));
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    fields.push_back(trim(current));
    return fields;
}

bool parseDouble(const std::string& text, double& value) {
    if (text.empty()) {
        return false;
    }
    const char* first = text.data();
    const char* last = first + text.size();
    if (*first == '+') {
        ++first;
    }
    if (first == last) {
        return false;
    }
    double parsed = 0.0;
    const auto result = std::from_chars(first, last, parsed, std::chars_format::general);
    if (result.ec != std::errc{} || result.ptr != last || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

bool parsePopulation(const std::string& text, std::int64_t& value) {
    if (text.empty()) {
        return false;
    }
    const char* first = text.data();
    const char* last = first + text.size();
    if (*first == '+') {
        ++first;
    }
    if (first == last) {
        return false;
    }
    std::int64_t parsed = 0;
    const auto result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last) {
        return false;
    }
    value = parsed;
    return true;
}

double degreesToRadians(double degrees) {
    return degrees * kPi / 180.0;
}

double haversineKm(double lat1, double lon1, double lat2, double lon2) {
    const double phi1 = degreesToRadians(lat1);
    const double phi2 = degreesToRadians(lat2);
    const double deltaPhi = degreesToRadians(lat2 - lat1);
    const double deltaLambda = degreesToRadians(lon2 - lon1);
    const double sinLat = std::sin(deltaPhi / 2.0);
    const double sinLon = std::sin(deltaLambda / 2.0);
    double h = sinLat * sinLat + std::cos(phi1) * std::cos(phi2) * sinLon * sinLon;
    h = std::clamp(h, 0.0, 1.0);
    return 2.0 * kEarthRadiusKm * std::asin(std::sqrt(h));
}

bool validCoordinates(double latitude, double longitude) {
    return std::isfinite(latitude) && std::isfinite(longitude) &&
           latitude >= -90.0 && latitude <= 90.0 &&
           longitude >= -180.0 && longitude <= 180.0;
}

} // namespace

bool ReverseGeocoder::load(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }

    std::string headerLine;
    if (!std::getline(input, headerLine)) {
        return false;
    }
    if (headerLine.size() >= 3 &&
        static_cast<unsigned char>(headerLine[0]) == 0xEF &&
        static_cast<unsigned char>(headerLine[1]) == 0xBB &&
        static_cast<unsigned char>(headerLine[2]) == 0xBF) {
        headerLine.erase(0, 3);
    }

    std::unordered_map<std::string, size_t> columns;
    const auto headerFields = splitCsvLine(headerLine);
    for (size_t i = 0; i < headerFields.size(); ++i) {
        columns.emplace(lowerAscii(headerFields[i]), i);
    }

    const auto nameIt = columns.find("name");
    const auto latitudeIt = columns.find("latitude");
    const auto longitudeIt = columns.find("longitude");
    if (nameIt == columns.end() || latitudeIt == columns.end() ||
        longitudeIt == columns.end()) {
        return false;
    }

    const auto optionalColumn = [&columns](const char* name) -> std::optional<size_t> {
        const auto it = columns.find(name);
        if (it == columns.end()) {
            return std::nullopt;
        }
        return it->second;
    };
    const auto admin1Column = optionalColumn("admin1");
    const auto countryCodeColumn = optionalColumn("country_code");
    const auto countryNameColumn = optionalColumn("country_name");
    const auto populationColumn = optionalColumn("population");

    const auto field = [](const std::vector<std::string>& fields,
                          size_t index) -> const std::string& {
        static const std::string kEmpty;
        return index < fields.size() ? fields[index] : kEmpty;
    };

    std::vector<Entry> entries;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        const auto fields = splitCsvLine(line);
        if (fields.size() <= std::max({nameIt->second, latitudeIt->second,
                                       longitudeIt->second})) {
            continue;
        }

        double latitude = 0.0;
        double longitude = 0.0;
        if (!parseDouble(fields[latitudeIt->second], latitude) ||
            !parseDouble(fields[longitudeIt->second], longitude) ||
            !validCoordinates(latitude, longitude)) {
            continue;
        }

        const std::string& name = fields[nameIt->second];
        if (name.empty()) {
            continue;
        }

        Entry entry;
        entry.city = name;
        entry.admin1 = admin1Column ? field(fields, *admin1Column) : std::string{};
        entry.countryCode = countryCodeColumn ? field(fields, *countryCodeColumn) : std::string{};
        entry.countryName = countryNameColumn ? field(fields, *countryNameColumn) : std::string{};
        entry.latitude = latitude;
        entry.longitude = longitude;
        if (populationColumn) {
            std::int64_t population = 0;
            if (parsePopulation(field(fields, *populationColumn), population)) {
                entry.population = population;
            }
        }
        entries.push_back(std::move(entry));
    }

    if (input.bad() || entries.empty()) {
        return false;
    }

    {
        std::unique_lock lock(mutex_);
        entries_.swap(entries);
    }
    return true;
}

std::optional<Place> ReverseGeocoder::lookup(double latitude, double longitude) const {
    if (!validCoordinates(latitude, longitude)) {
        return std::nullopt;
    }

    std::shared_lock lock(mutex_);
    const Entry* nearest = nullptr;
    double nearestDistance = std::numeric_limits<double>::infinity();

    for (const Entry& entry : entries_) {
        const double distance = haversineKm(latitude, longitude,
                                            entry.latitude, entry.longitude);
        if (distance <= kMaxDistanceKm && distance < nearestDistance) {
            nearest = &entry;
            nearestDistance = distance;
        }
    }

    if (nearest == nullptr) {
        return std::nullopt;
    }

    Place place;
    place.city = nearest->city;
    place.admin1 = nearest->admin1;
    place.countryCode = nearest->countryCode;
    place.countryName = nearest->countryName;
    place.distanceKm = nearestDistance;
    return place;
}

} // namespace imagine::core
