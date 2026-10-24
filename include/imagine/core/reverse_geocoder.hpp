#pragma once

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

namespace imagine::core {

struct Place {
    std::string city;
    std::string admin1;
    std::string countryCode;
    std::string countryName;
    double distanceKm{0.0};
};

class ReverseGeocoder {
public:
    static constexpr double kMaxDistanceKm = 100.0;

    // Loads a GeoNames-style CSV with the header
    // name,admin1,country_code,country_name,latitude,longitude,population.
    // Returns false when the file cannot be opened or the header is invalid,
    // when no valid rows are parsed, or when reading fails; in those cases
    // previously loaded data is left untouched.
    //
    // Thread safety: lookup() may be called concurrently once a load has
    // completed. load() is not meant to run concurrently with lookup(); if it
    // does, the last successful load wins.
    bool load(const std::string& path);

    // Returns the nearest known city within kMaxDistanceKm, or std::nullopt
    // when the coordinates are invalid or no city is close enough.
    std::optional<Place> lookup(double latitude, double longitude) const;

private:
    struct Entry {
        std::string city;
        std::string admin1;
        std::string countryCode;
        std::string countryName;
        double latitude{0.0};
        double longitude{0.0};
        std::int64_t population{0};
    };

    std::vector<Entry> entries_;
    mutable std::shared_mutex mutex_;
};

} // namespace imagine::core
