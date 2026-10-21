#pragma once

#include <string>
#include <vector>
#include <optional>
#include "imagine/common/error.hpp"

namespace imagine::clip {

struct TagLabel {
    std::string id;          // stable slug, e.g. "beach"
    std::string name;        // display tag name, e.g. "Beach"
    std::string group;       // "Nature", "Animals", "Food & Drink", etc.
    float threshold{0.24f};
    std::vector<std::string> prompts;
};

struct TagTaxonomy {
    std::string version;     // 64-bit hex hash (FNV-1a or SHA-256) of canonical label definitions
    std::vector<TagLabel> labels;
};

struct SmartTagOptions {
    float minScore{0.23f};
    int maxLabels{3};
    int maxPerGroup{2};
};

struct LabelScore {
    std::string id;
    std::string name;
    std::string group;
    float score{0.0f};
};

const char* getDefaultTaxonomyJson();
Result<TagTaxonomy> parseTaxonomyJson(const std::string& jsonStr);
Result<TagTaxonomy> loadTaxonomy(const std::optional<std::string>& customPath = std::nullopt);
std::vector<float> meanPoolAndNormalize(const std::vector<std::vector<float>>& promptEmbeddings);
std::vector<LabelScore> selectSmartLabels(
    const std::vector<float>& imageEmbedding,
    const std::vector<std::vector<float>>& labelEmbeddings,
    const std::vector<TagLabel>& labels,
    const SmartTagOptions& options);
SmartTagOptions loadSmartTagOptionsFromEnv();

} // namespace imagine::clip
