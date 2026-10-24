#include <gtest/gtest.h>

#include "imagine/core/reverse_geocoder.hpp"
#include "imagine/db/catalog_db.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

using imagine::Tag;
using imagine::db::CatalogDb;
using imagine::core::ReverseGeocoder;

std::filesystem::path uniqueTempPath(const std::string& suffix) {
    static std::atomic<unsigned long long> counter{0};
#if defined(_WIN32)
    const auto pid = static_cast<unsigned long long>(GetCurrentProcessId());
#else
    const auto pid = static_cast<unsigned long long>(getpid());
#endif
    return std::filesystem::temp_directory_path() /
        ("imagine_geo_" + std::to_string(pid) + "_" +
         std::to_string(counter.fetch_add(1)) + suffix);
}

bool writeFile(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out << contents;
    return out.good();
}

std::optional<std::string> envValue(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
}

bool setEnvValue(const char* name, const std::string& value) {
#if defined(_WIN32)
    return _putenv_s(name, value.c_str()) == 0;
#else
    return setenv(name, value.c_str(), 1) == 0;
#endif
}

bool clearEnvValue(const char* name) {
#if defined(_WIN32)
    return _putenv_s(name, "") == 0;
#else
    return unsetenv(name) == 0;
#endif
}

const Tag* findTag(const std::vector<Tag>& tags, const std::string& name,
                   const std::string& category = "places") {
    const auto it = std::find_if(tags.begin(), tags.end(), [&](const Tag& tag) {
        return tag.name == name && tag.category == category;
    });
    return it == tags.end() ? nullptr : &*it;
}

class ReverseGeocoderTest : public ::testing::Test {
protected:
    void SetUp() override {
        csvPath_ = uniqueTempPath(".csv");
        WriteCsv(
            "name,admin1,country_code,country_name,latitude,longitude,population\n"
            "Berlin,Berlin,DE,Germany,52.5200,13.4050,3644826\n"
            "Potsdam,Brandenburg,DE,Germany,52.3906,13.0645,183431\n");
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove(csvPath_, ec);
    }

    void WriteCsv(const std::string& contents) {
        ASSERT_TRUE(writeFile(csvPath_, contents));
    }

    std::filesystem::path csvPath_;
};

TEST_F(ReverseGeocoderTest, LoadsCsvAndResolvesBerlin) {
    ReverseGeocoder geocoder;
    ASSERT_TRUE(geocoder.load(csvPath_.string()));

    const auto place = geocoder.lookup(52.5200, 13.4050);
    ASSERT_TRUE(place.has_value());
    EXPECT_EQ(place->city, "Berlin");
    EXPECT_EQ(place->admin1, "Berlin");
    EXPECT_EQ(place->countryCode, "DE");
    EXPECT_EQ(place->countryName, "Germany");
}

TEST_F(ReverseGeocoderTest, LoadsBomCrlfAndQuotedFields) {
    WriteCsv(
        "\xEF\xBB\xBF" "name,admin1,country_code,country_name,latitude,longitude,population\r\n"
        "\"Bern, Stadt\",\"Bern\",\"CH\",\"Switzerland\",46.9480,7.4474,133883\r\n");

    ReverseGeocoder geocoder;
    ASSERT_TRUE(geocoder.load(csvPath_.string()));

    const auto place = geocoder.lookup(46.9480, 7.4474);
    ASSERT_TRUE(place.has_value());
    EXPECT_EQ(place->city, "Bern, Stadt");
    EXPECT_EQ(place->admin1, "Bern");
    EXPECT_EQ(place->countryCode, "CH");
    EXPECT_EQ(place->countryName, "Switzerland");
}

TEST_F(ReverseGeocoderTest, SkipsMixedInvalidRows) {
    WriteCsv(
        "name,admin1,country_code,country_name,latitude,longitude,population\n"
        "BadLat,Bad,DE,Germany,not-a-number,13.40,1000\n"
        "BadLon,Bad,DE,Germany,52.50,999.0,1000\n"
        ",Empty,DE,Germany,52.50,13.40,1000\n"
        "TooFew,Only,DE\n"
        "Berlin,Berlin,DE,Germany,52.5200,13.4050,3644826\n");

    ReverseGeocoder geocoder;
    ASSERT_TRUE(geocoder.load(csvPath_.string()));

    const auto place = geocoder.lookup(52.5200, 13.4050);
    ASSERT_TRUE(place.has_value());
    EXPECT_EQ(place->city, "Berlin");
}

TEST_F(ReverseGeocoderTest, ReturnsNulloptForOceanPoint) {
    ReverseGeocoder geocoder;
    ASSERT_TRUE(geocoder.load(csvPath_.string()));

    EXPECT_FALSE(geocoder.lookup(0.0, -150.0).has_value());
}

TEST_F(ReverseGeocoderTest, ReturnsNulloptForInvalidCoordinates) {
    ReverseGeocoder geocoder;
    ASSERT_TRUE(geocoder.load(csvPath_.string()));

    EXPECT_FALSE(geocoder.lookup(200.0, 0.0).has_value());
    EXPECT_FALSE(geocoder.lookup(0.0, 200.0).has_value());
}

TEST_F(ReverseGeocoderTest, RespectsMaxDistanceCutoff) {
    EXPECT_DOUBLE_EQ(ReverseGeocoder::kMaxDistanceKm, 100.0);

    ReverseGeocoder geocoder;
    ASSERT_TRUE(geocoder.load(csvPath_.string()));

    const auto nearby = geocoder.lookup(52.97, 13.40);
    ASSERT_TRUE(nearby.has_value());
    EXPECT_EQ(nearby->city, "Berlin");
    EXPECT_LE(nearby->distanceKm, ReverseGeocoder::kMaxDistanceKm);

    EXPECT_FALSE(geocoder.lookup(54.0, 13.40).has_value());
}

TEST_F(ReverseGeocoderTest, EmptyDataLoadReturnsFalse) {
    WriteCsv(
        "name,admin1,country_code,country_name,latitude,longitude,population\n");

    ReverseGeocoder geocoder;
    EXPECT_FALSE(geocoder.load(csvPath_.string()));

    WriteCsv("");
    EXPECT_FALSE(geocoder.load(csvPath_.string()));
}

TEST_F(ReverseGeocoderTest, FailedReloadPreservesPriorData) {
    ReverseGeocoder geocoder;
    ASSERT_TRUE(geocoder.load(csvPath_.string()));
    ASSERT_TRUE(geocoder.lookup(52.5200, 13.4050).has_value());

    WriteCsv("city,lat,lon\nBerlin,52.52,13.40\n");
    EXPECT_FALSE(geocoder.load(csvPath_.string()));

    const auto place = geocoder.lookup(52.5200, 13.4050);
    ASSERT_TRUE(place.has_value());
    EXPECT_EQ(place->city, "Berlin");
}

TEST_F(ReverseGeocoderTest, RejectsMissingFile) {
    ReverseGeocoder geocoder;
    EXPECT_FALSE(
        geocoder.load((csvPath_.parent_path() / "imagine-no-such-file.csv").string()));
}

class GpsPlaceTagIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        csvPath_ = uniqueTempPath(".csv");
        dbPath_ = uniqueTempPath(".db");
        ASSERT_TRUE(writeFile(
            csvPath_,
            "name,admin1,country_code,country_name,latitude,longitude,population\n"
            "Berlin,Berlin,DE,Germany,52.5200,13.4050,3644826\n"
            "Potsdam,Brandenburg,DE,Germany,52.3906,13.0645,183431\n"));

        originalEnv_ = envValue("IMAGINE_GEONAMES_CSV");
        ASSERT_TRUE(setEnvValue("IMAGINE_GEONAMES_CSV", csvPath_.string()));
        ASSERT_TRUE(db_.open(dbPath_.string()).isOk());
    }

    void TearDown() override {
        db_.close();
        if (originalEnv_.has_value()) {
            setEnvValue("IMAGINE_GEONAMES_CSV", *originalEnv_);
        } else {
            clearEnvValue("IMAGINE_GEONAMES_CSV");
        }
        std::error_code ec;
        std::filesystem::remove(csvPath_, ec);
        std::filesystem::remove(dbPath_, ec);
        std::filesystem::remove(dbPath_.string() + "-wal", ec);
        std::filesystem::remove(dbPath_.string() + "-shm", ec);
    }

    imagine::MediaId insertPhoto(const std::string& hash) {
        imagine::MediaItem item;
        item.file_path = "geo/" + hash + ".jpg";
        item.file_name = hash + ".jpg";
        item.file_size = 1000;
        item.content_hash = hash;
        const auto result = db_.insertMedia(item);
        EXPECT_TRUE(result.isOk());
        return result.isOk() ? result.value() : 0;
    }

    std::vector<Tag> tagsFor(imagine::MediaId id) {
        const auto result = db_.getTagsForMedia(id);
        EXPECT_TRUE(result.isOk());
        return result.isOk() ? result.value() : std::vector<Tag>{};
    }

    void expectPlaceSource(imagine::MediaId id, const std::string& name,
                           const std::string& expectedSource) {
        const auto tags = tagsFor(id);
        const Tag* tag = findTag(tags, name);
        ASSERT_NE(tag, nullptr) << "missing places tag: " << name;
        EXPECT_EQ(tag->source, expectedSource) << "tag: " << name;
    }

    void expectNoPlaceTags(imagine::MediaId id) {
        for (const Tag& tag : tagsFor(id)) {
            EXPECT_NE(tag.category, "places") << "unexpected places tag: " << tag.name;
        }
    }

    CatalogDb db_;
    std::filesystem::path csvPath_;
    std::filesystem::path dbPath_;
    std::optional<std::string> originalEnv_;
};

TEST_F(GpsPlaceTagIntegrationTest, BerlinCreatesGpsPlaceTags) {
    const auto mediaId = insertPhoto("geo-berlin");
    ASSERT_GT(mediaId, 0);

    ASSERT_TRUE(
        db_.applyGpsPlaceTags(mediaId, true, 52.5200, 13.4050).isOk());

    expectPlaceSource(mediaId, "Berlin", "gps");
    expectPlaceSource(mediaId, "Germany", "gps");
}

TEST_F(GpsPlaceTagIntegrationTest, GpsChangeReplacesStaleTags) {
    const auto mediaId = insertPhoto("geo-change");
    ASSERT_GT(mediaId, 0);

    ASSERT_TRUE(
        db_.applyGpsPlaceTags(mediaId, true, 52.5200, 13.4050).isOk());
    expectPlaceSource(mediaId, "Berlin", "gps");

    ASSERT_TRUE(
        db_.applyGpsPlaceTags(mediaId, true, 52.3906, 13.0645).isOk());

    expectPlaceSource(mediaId, "Potsdam", "gps");
    expectPlaceSource(mediaId, "Brandenburg", "gps");
    expectPlaceSource(mediaId, "Germany", "gps");
    const auto tags = tagsFor(mediaId);
    EXPECT_EQ(findTag(tags, "Berlin"), nullptr);
}

TEST_F(GpsPlaceTagIntegrationTest, ClearingGpsRemovesGpsTags) {
    const auto mediaId = insertPhoto("geo-clear");
    ASSERT_GT(mediaId, 0);

    ASSERT_TRUE(
        db_.applyGpsPlaceTags(mediaId, true, 52.5200, 13.4050).isOk());
    expectPlaceSource(mediaId, "Berlin", "gps");

    ASSERT_TRUE(db_.applyGpsPlaceTags(mediaId, false, 0.0, 0.0).isOk());

    expectNoPlaceTags(mediaId);
}

TEST_F(GpsPlaceTagIntegrationTest, ManualSameNameTagSurvivesRefreshAndClear) {
    const auto mediaId = insertPhoto("geo-manual");
    ASSERT_GT(mediaId, 0);

    const auto manualTag = db_.createOrGetTag("Berlin", "places");
    ASSERT_TRUE(manualTag.isOk());
    ASSERT_TRUE(db_.addTagToMedia(mediaId, manualTag.value()).isOk());
    expectPlaceSource(mediaId, "Berlin", "manual");

    ASSERT_TRUE(
        db_.applyGpsPlaceTags(mediaId, true, 52.5200, 13.4050).isOk());
    expectPlaceSource(mediaId, "Berlin", "manual");
    expectPlaceSource(mediaId, "Germany", "gps");

    ASSERT_TRUE(db_.applyGpsPlaceTags(mediaId, false, 0.0, 0.0).isOk());
    expectPlaceSource(mediaId, "Berlin", "manual");
    const auto tags = tagsFor(mediaId);
    EXPECT_EQ(findTag(tags, "Germany"), nullptr);
}

} // namespace
