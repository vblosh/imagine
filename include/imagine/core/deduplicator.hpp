#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include "imagine/common/types.hpp"
#include "imagine/common/error.hpp"

namespace imagine::core {

enum class DuplicateMatchType {
    kExact,
    kVisual
};

struct DuplicateCandidateOptions {
    double visualThreshold = 0.97;       // cosine similarity floor
    int64_t dateTakenWindowSec = 86400; // max |dtA - dtB|; 0 = no window
    std::vector<MediaId> scopeIds;       // empty = whole catalog
};
using DeduplicatorOptions = DuplicateCandidateOptions;

struct DuplicateGroup {
    DuplicateMatchType type{DuplicateMatchType::kExact};
    std::vector<MediaId> memberIds; // [0] is the ranked keeper
    double minSimilarity{1.0};      // 1.0 for exact groups
};

struct DuplicateScanResult {
    std::vector<DuplicateGroup> groups;
    int mediaScanned{0};
};
using DeduplicatorScanResult = DuplicateScanResult;

namespace detail {

inline int flagPriority(FlagState flag) {
    switch (flag) {
        case FlagState::Pick:
            return 2;
        case FlagState::Unflagged:
            return 1;
        case FlagState::Reject:
            return 0;
    }
    return 1;
}

inline int metadataCompletenessScore(const MediaItem& item) {
    int score = 0;
    // In Imagine, user title/description is mapped to 'caption' for photos,
    // and 'audio_title' for audio items.
    if (!item.caption.empty() || !item.audio_title.empty()) {
        score += 1;
    }
    if (item.exif.has_gps) {
        score += 1;
    }
    // Pre-1970 negative dates are valid and earn the bonus; only 0 means undated.
    if (item.date_taken != 0) {
        score += 1;
    }
    score += static_cast<int>(item.tags.size());
    return score;
}

inline bool containsWordToken(std::string_view text, std::string_view word) {
    size_t pos = 0;
    while ((pos = text.find(word, pos)) != std::string_view::npos) {
        bool leftBound = (pos == 0) || !std::isalnum(static_cast<unsigned char>(text[pos - 1]));
        size_t endPos = pos + word.size();
        bool rightBound = (endPos == text.size()) || !std::isalnum(static_cast<unsigned char>(text[endPos]));
        if (leftBound && rightBound) {
            return true;
        }
        pos += word.size();
    }
    return false;
}

inline int pathCleanlinessPenalty(const std::string& rawPath) {
    std::string lower = rawPath;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    int penalty = 0;

    // Segment-based token matching (splitting on directory separators)
    size_t start = 0;
    while (start < lower.size()) {
        size_t end = lower.find_first_of("/\\", start);
        if (end == std::string_view::npos) {
            end = lower.size();
        }
        std::string_view segment(lower.data() + start, end - start);
        if (!segment.empty()) {
            // Whole-token matching for duplicate indicators so words like "copyright" are not penalized
            if (containsWordToken(segment, "copy")) {
                penalty += 10;
            }
            if (containsWordToken(segment, "export")) {
                penalty += 10;
            }
            if (containsWordToken(segment, "tmp") || containsWordToken(segment, "temp")) {
                penalty += 10;
            }
            if (segment.find("(1)") != std::string_view::npos ||
                segment.find("(2)") != std::string_view::npos ||
                segment.find("(3)") != std::string_view::npos) {
                penalty += 10;
            }
        }
        start = end + 1;
    }

    // Penalize percent-encoded sequences (%HEXHEX such as %20 or %2F)
    for (size_t i = 0; i + 2 < rawPath.size(); ++i) {
        if (rawPath[i] == '%' &&
            std::isxdigit(static_cast<unsigned char>(rawPath[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(rawPath[i + 2]))) {
            penalty += 1;
            i += 2;
        }
    }
    return penalty;
}

} // namespace detail

/// Compares two items according to the 6 keeper ranking criteria:
/// 1. Flag status (Pick > Unflagged > Reject)
/// 2. Resolution (width * height; 0 loses)
/// 3. File size (larger file_size wins)
/// 4. Metadata completeness (caption/title, GPS, tag count, date_taken != 0 bonus)
/// 5. Path cleanliness (clean library path outranks messy one; fewer penalties wins)
/// 6. Earliest MediaId tiebreak (lowest id wins)
inline bool isBetterKeeper(const MediaItem& a, const MediaItem& b) {
    // 1. Flag status (Pick > Unflagged > Reject)
    int flagA = detail::flagPriority(a.flag);
    int flagB = detail::flagPriority(b.flag);
    if (flagA != flagB) {
        return flagA > flagB;
    }

    // 2. Resolution (width * height; 0 loses)
    int64_t resA = static_cast<int64_t>(a.width) * a.height;
    int64_t resB = static_cast<int64_t>(b.width) * b.height;
    if (resA != resB) {
        return resA > resB;
    }

    // 3. File size (larger file_size wins)
    if (a.file_size != b.file_size) {
        return a.file_size > b.file_size;
    }

    // 4. Metadata completeness (caption/title, GPS, tag count, date_taken != 0 bonus)
    int metaA = detail::metadataCompletenessScore(a);
    int metaB = detail::metadataCompletenessScore(b);
    if (metaA != metaB) {
        return metaA > metaB;
    }

    // 5. Path cleanliness (clean library path outranks messy one; fewer penalties wins)
    int penA = detail::pathCleanlinessPenalty(a.file_path);
    int penB = detail::pathCleanlinessPenalty(b.file_path);
    if (penA != penB) {
        return penA < penB;
    }

    // 6. Earliest-id tiebreak (lowest MediaId wins)
    return a.id < b.id;
}

inline MediaId rankKeeper(const std::vector<MediaItem>& items) {
    if (items.empty()) {
        return 0;
    }
    const MediaItem* best = &items[0];
    for (size_t i = 1; i < items.size(); ++i) {
        if (isBetterKeeper(items[i], *best)) {
            best = &items[i];
        }
    }
    return best->id;
}

inline MediaId rankKeeper(const std::vector<MediaId>& memberIds,
                          const std::unordered_map<MediaId, MediaItem>& metadata) {
    if (memberIds.empty()) {
        return 0;
    }
    MediaId bestId = memberIds[0];
    for (size_t i = 1; i < memberIds.size(); ++i) {
        MediaId candId = memberIds[i];
        auto itCand = metadata.find(candId);
        auto itBest = metadata.find(bestId);
        if (itCand != metadata.end() && itBest != metadata.end()) {
            if (isBetterKeeper(itCand->second, itBest->second)) {
                bestId = candId;
            }
        } else if (itCand != metadata.end()) {
            bestId = candId;
        } else if (itBest == metadata.end()) {
            if (candId < bestId) {
                bestId = candId;
            }
        }
    }
    return bestId;
}

inline void sortDuplicateGroup(std::vector<MediaItem>& items) {
    std::stable_sort(items.begin(), items.end(), isBetterKeeper);
}

} // namespace imagine::core

namespace imagine {
using core::DuplicateMatchType;
using core::DuplicateCandidateOptions;
using core::DeduplicatorOptions;
using core::DuplicateGroup;
using core::DuplicateScanResult;
using core::DeduplicatorScanResult;
using core::isBetterKeeper;
using core::rankKeeper;
using core::sortDuplicateGroup;
} // namespace imagine
