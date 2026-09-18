#include <gtest/gtest.h>

#include "imagine/core/event_suggestions.hpp"

using namespace imagine;
using namespace imagine::core;

namespace {

MediaItem photo(MediaId id, int64_t timestamp, double latitude = 0.0, double longitude = 0.0) {
    MediaItem item;
    item.id = id;
    item.media_type = "photo";
    item.date_taken = timestamp;
    item.file_name = "photo" + std::to_string(id) + ".jpg";
    item.file_path = "Trip/" + item.file_name;
    item.exif.has_gps = latitude != 0.0 || longitude != 0.0;
    item.exif.latitude = latitude;
    item.exif.longitude = longitude;
    return item;
}

} // namespace

TEST(EventSuggestionTest, SplitsLongGapsAndKeepsMidnightSessionTogether) {
    auto a = photo(1, 1704066900); // 2023-12-31 23:55 UTC
    auto b = photo(2, 1704074100); // 2024-01-01 01:55 UTC
    auto c = photo(3, 1704092101); // > 4 hours after b

    const auto result = EventSuggestionEngine::suggest({a, c, b});

    ASSERT_EQ(result.groups.size(), 2u);
    ASSERT_EQ(result.groups[0].photos.size(), 2u);
    EXPECT_EQ(result.groups[0].photos[0].id, 1);
    EXPECT_EQ(result.groups[0].photos[1].id, 2);
    EXPECT_EQ(result.groups[1].photos[0].id, 3);
}

TEST(EventSuggestionTest, SplitsDistantLocatedPhotos) {
    auto a = photo(1, 1700000000, 52.52, 13.405);
    auto b = photo(2, 1700000100, 48.8566, 2.3522);

    const auto result = EventSuggestionEngine::suggest({a, b});

    ASSERT_EQ(result.groups.size(), 2u);
}

TEST(EventSuggestionTest, UsesEventCaptionPlaceAndFolderPriority) {
    auto eventA = photo(1, 1700000000);
    auto eventB = photo(2, 1700000100);
    eventA.tags.push_back({10, "Conference", "events"});
    eventB.tags.push_back({10, "Conference", "events"});
    auto eventResult = EventSuggestionEngine::suggest({eventA, eventB});
    ASSERT_EQ(eventResult.groups[0].name_source, EventSuggestionNameSource::Event);
    EXPECT_EQ(eventResult.groups[0].existing_event_tag_id, 10);

    auto captionA = photo(3, 1700010000);
    auto captionB = photo(4, 1700010100);
    captionA.caption = "Family lunch";
    captionB.caption = "Family lunch";
    auto captionResult = EventSuggestionEngine::suggest({captionA, captionB});
    EXPECT_EQ(captionResult.groups[0].name_source, EventSuggestionNameSource::Caption);
    EXPECT_EQ(captionResult.groups[0].name_value, "Family lunch");
}

TEST(EventSuggestionTest, SkipsNonPhotosAndInvalidDates) {
    auto video = photo(1, 1700000000);
    video.media_type = "video";
    auto undated = photo(2, 0);

    const auto result = EventSuggestionEngine::suggest({video, undated});

    EXPECT_TRUE(result.groups.empty());
    ASSERT_EQ(result.skipped.size(), 2u);
    EXPECT_EQ(result.skipped[0].reason, "not_photo");
    EXPECT_EQ(result.skipped[1].reason, "invalid_date");
}
