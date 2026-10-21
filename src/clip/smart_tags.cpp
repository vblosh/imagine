#include "imagine/clip/smart_tags.hpp"
#include "imagine/common/types.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace imagine::clip {

namespace {

uint64_t fnv1a64(const void* data, size_t len) {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        hash ^= ptr[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string toLower(std::string_view sv) {
    std::string res;
    res.reserve(sv.size());
    for (char c : sv) {
        res.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return res;
}

std::string computeTaxonomyVersion(const std::vector<TagLabel>& labels) {
    std::vector<const TagLabel*> sortedLabels;
    sortedLabels.reserve(labels.size());
    for (const auto& lbl : labels) {
        sortedLabels.push_back(&lbl);
    }
    std::sort(sortedLabels.begin(), sortedLabels.end(), [](const TagLabel* a, const TagLabel* b) {
        return a->id < b->id;
    });

    std::ostringstream canonical;
    canonical.imbue(std::locale::classic());
    for (const auto* l : sortedLabels) {
        canonical << l->id.size() << ':' << l->id << ';'
                  << l->name.size() << ':' << l->name << ';'
                  << l->group.size() << ':' << l->group << ';'
                  << std::fixed << std::setprecision(6) << l->threshold << ';'
                  << l->prompts.size() << '[';
        for (const auto& p : l->prompts) {
            canonical << p.size() << ':' << p << ',';
        }
        canonical << "]\n";
    }
    std::string canonStr = canonical.str();
    uint64_t h = fnv1a64(canonStr.data(), canonStr.size());
    char hexBuf[17];
    std::snprintf(hexBuf, sizeof(hexBuf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(hexBuf);
}

const char* kDefaultTaxonomyJson = R"json({
  "version": 1,
  "labels": [
    {
      "id": "beach",
      "name": "Beach",
      "group": "Nature",
      "threshold": 0.24,
      "prompts": [
        "a photo of a beach",
        "sand and ocean waves",
        "people relaxing on a sandy beach"
      ]
    },
    {
      "id": "mountains",
      "name": "Mountains",
      "group": "Nature",
      "threshold": 0.24,
      "prompts": [
        "a photo of mountains",
        "mountain range landscape",
        "snowy mountain peaks"
      ]
    },
    {
      "id": "forest",
      "name": "Forest",
      "group": "Nature",
      "threshold": 0.24,
      "prompts": [
        "a photo of a forest",
        "trees in a woodland",
        "lush green forest"
      ]
    },
    {
      "id": "lake",
      "name": "Lake",
      "group": "Nature",
      "threshold": 0.24,
      "prompts": [
        "a photo of a lake",
        "calm lake water",
        "scenic lake with reflections"
      ]
    },
    {
      "id": "river",
      "name": "River",
      "group": "Nature",
      "threshold": 0.24,
      "prompts": [
        "a photo of a river",
        "flowing river water",
        "river flowing through nature"
      ]
    },
    {
      "id": "ocean",
      "name": "Ocean",
      "group": "Nature",
      "threshold": 0.24,
      "prompts": [
        "a photo of the ocean",
        "deep blue ocean water",
        "waves crashing in the sea"
      ]
    },
    {
      "id": "sunset",
      "name": "Sunset",
      "group": "Nature",
      "threshold": 0.24,
      "prompts": [
        "a photo of a sunset",
        "colorful sunset sky",
        "golden hour evening sun"
      ]
    },
    {
      "id": "sunrise",
      "name": "Sunrise",
      "group": "Nature",
      "threshold": 0.24,
      "prompts": [
        "a photo of a sunrise",
        "early morning sunrise",
        "dawn light on the horizon"
      ]
    },
    {
      "id": "snow",
      "name": "Snow",
      "group": "Nature",
      "threshold": 0.24,
      "prompts": [
        "a photo of snow",
        "winter snow landscape",
        "fresh white snow covering the ground"
      ]
    },
    {
      "id": "flowers",
      "name": "Flowers",
      "group": "Nature",
      "threshold": 0.24,
      "prompts": [
        "a photo of flowers",
        "blooming colorful flowers",
        "close up of floral blossoms"
      ]
    },
    {
      "id": "sky",
      "name": "Sky",
      "group": "Nature",
      "threshold": 0.23,
      "prompts": [
        "a photo of the sky",
        "wide open blue sky",
        "bright clear sky"
      ]
    },
    {
      "id": "clouds",
      "name": "Clouds",
      "group": "Nature",
      "threshold": 0.24,
      "prompts": [
        "a photo of clouds",
        "fluffy white clouds",
        "cloudy sky overhead"
      ]
    },
    {
      "id": "dog",
      "name": "Dog",
      "group": "Animals",
      "threshold": 0.25,
      "prompts": [
        "a photo of a dog",
        "a pet dog",
        "a cute dog playing"
      ]
    },
    {
      "id": "cat",
      "name": "Cat",
      "group": "Animals",
      "threshold": 0.25,
      "prompts": [
        "a photo of a cat",
        "a pet cat",
        "a cute feline kitten"
      ]
    },
    {
      "id": "bird",
      "name": "Bird",
      "group": "Animals",
      "threshold": 0.24,
      "prompts": [
        "a photo of a bird",
        "a bird perched on a branch",
        "flying wild bird"
      ]
    },
    {
      "id": "horse",
      "name": "Horse",
      "group": "Animals",
      "threshold": 0.25,
      "prompts": [
        "a photo of a horse",
        "a horse in a pasture",
        "equine running in a field"
      ]
    },
    {
      "id": "wildlife",
      "name": "Wildlife",
      "group": "Animals",
      "threshold": 0.24,
      "prompts": [
        "a photo of wildlife",
        "wild animals in nature",
        "safari animal outdoors"
      ]
    },
    {
      "id": "fish",
      "name": "Fish",
      "group": "Animals",
      "threshold": 0.25,
      "prompts": [
        "a photo of fish",
        "fish swimming in water",
        "aquarium fish or marine life"
      ]
    },
    {
      "id": "insect",
      "name": "Insect",
      "group": "Animals",
      "threshold": 0.25,
      "prompts": [
        "a photo of an insect",
        "butterfly or bee on a flower",
        "small bug or insect"
      ]
    },
    {
      "id": "food",
      "name": "Food",
      "group": "Food & Drink",
      "threshold": 0.23,
      "prompts": [
        "a photo of food",
        "a delicious meal on a plate",
        "tasty cooked dish"
      ]
    },
    {
      "id": "coffee",
      "name": "Coffee",
      "group": "Food & Drink",
      "threshold": 0.25,
      "prompts": [
        "a cup of coffee",
        "hot espresso or latte art",
        "coffee mug on a table"
      ]
    },
    {
      "id": "wine",
      "name": "Wine",
      "group": "Food & Drink",
      "threshold": 0.25,
      "prompts": [
        "a glass of wine",
        "red or white wine bottle and glass",
        "tasting wine"
      ]
    },
    {
      "id": "beer",
      "name": "Beer",
      "group": "Food & Drink",
      "threshold": 0.25,
      "prompts": [
        "a glass of beer",
        "a cold pint of draft beer",
        "craft beer in a mug"
      ]
    },
    {
      "id": "cake",
      "name": "Cake",
      "group": "Food & Drink",
      "threshold": 0.25,
      "prompts": [
        "a photo of a cake",
        "sweet dessert cake or pastry",
        "decorated birthday cake"
      ]
    },
    {
      "id": "fruit",
      "name": "Fruit",
      "group": "Food & Drink",
      "threshold": 0.24,
      "prompts": [
        "fresh fruit bowl",
        "colorful ripe fruits",
        "apples, oranges and berries"
      ]
    },
    {
      "id": "wedding",
      "name": "Wedding",
      "group": "People & Events",
      "threshold": 0.25,
      "prompts": [
        "a wedding ceremony",
        "bride and groom at wedding",
        "wedding celebration reception"
      ]
    },
    {
      "id": "birthday",
      "name": "Birthday",
      "group": "People & Events",
      "threshold": 0.25,
      "prompts": [
        "a birthday party celebration",
        "birthday candles and decorations",
        "celebrating someone's birthday"
      ]
    },
    {
      "id": "concert",
      "name": "Concert",
      "group": "People & Events",
      "threshold": 0.25,
      "prompts": [
        "a live music concert",
        "band performing on stage",
        "crowd at a concert with stage lights"
      ]
    },
    {
      "id": "portrait",
      "name": "Portrait",
      "group": "People & Events",
      "threshold": 0.24,
      "prompts": [
        "a portrait of a person",
        "close up face portrait",
        "headshot photo of someone"
      ]
    },
    {
      "id": "party",
      "name": "Party",
      "group": "People & Events",
      "threshold": 0.24,
      "prompts": [
        "a party celebration",
        "people having fun at a party",
        "social festive gathering"
      ]
    },
    {
      "id": "baby",
      "name": "Baby",
      "group": "People & Events",
      "threshold": 0.25,
      "prompts": [
        "a photo of a baby",
        "cute infant or newborn baby",
        "little toddler child"
      ]
    },
    {
      "id": "city",
      "name": "City",
      "group": "Places & Travel",
      "threshold": 0.24,
      "prompts": [
        "a photo of a city",
        "urban cityscape skyline",
        "downtown buildings in a city"
      ]
    },
    {
      "id": "architecture",
      "name": "Architecture",
      "group": "Places & Travel",
      "threshold": 0.24,
      "prompts": [
        "modern or historic architecture",
        "architectural building facade",
        "striking structure and design"
      ]
    },
    {
      "id": "bridge",
      "name": "Bridge",
      "group": "Places & Travel",
      "threshold": 0.25,
      "prompts": [
        "a photo of a bridge",
        "large suspension bridge over water",
        "scenic architectural bridge"
      ]
    },
    {
      "id": "street",
      "name": "Street",
      "group": "Places & Travel",
      "threshold": 0.24,
      "prompts": [
        "a city street view",
        "urban roadway with sidewalks and shops",
        "busy street scene"
      ]
    },
    {
      "id": "airplane",
      "name": "Airplane",
      "group": "Places & Travel",
      "threshold": 0.25,
      "prompts": [
        "an airplane in flight",
        "commercial aircraft at an airport",
        "passenger plane in the sky"
      ]
    },
    {
      "id": "car",
      "name": "Car",
      "group": "Places & Travel",
      "threshold": 0.24,
      "prompts": [
        "a photo of a car",
        "automobile driving on a road",
        "parked motor vehicle"
      ]
    },
    {
      "id": "bicycle",
      "name": "Bicycle",
      "group": "Places & Travel",
      "threshold": 0.25,
      "prompts": [
        "a bicycle parked or ridden",
        "cycling bike on a path",
        "two-wheeled bicycle outdoors"
      ]
    },
    {
      "id": "boat",
      "name": "Boat",
      "group": "Places & Travel",
      "threshold": 0.25,
      "prompts": [
        "a boat on the water",
        "sailboat or yacht sailing",
        "small boat floating in a lake or sea"
      ]
    },
    {
      "id": "train",
      "name": "Train",
      "group": "Places & Travel",
      "threshold": 0.25,
      "prompts": [
        "a photo of a train",
        "railway locomotive on tracks",
        "passenger train traveling"
      ]
    },
    {
      "id": "camping",
      "name": "Camping",
      "group": "Activities",
      "threshold": 0.25,
      "prompts": [
        "a camping site with tent",
        "camping outdoors in the wilderness",
        "campfire and tents"
      ]
    },
    {
      "id": "hiking",
      "name": "Hiking",
      "group": "Activities",
      "threshold": 0.24,
      "prompts": [
        "hiking in the mountains or woods",
        "person hiking on a nature trail",
        "outdoor trekking with backpacks"
      ]
    },
    {
      "id": "swimming",
      "name": "Swimming",
      "group": "Activities",
      "threshold": 0.25,
      "prompts": [
        "person swimming in water",
        "swimming in a pool or sea",
        "swimmer splashing in water"
      ]
    },
    {
      "id": "skiing",
      "name": "Skiing",
      "group": "Activities",
      "threshold": 0.25,
      "prompts": [
        "person skiing down snowy slope",
        "snow skiing in the winter mountains",
        "skier or snowboarder in the snow"
      ]
    },
    {
      "id": "running",
      "name": "Running",
      "group": "Activities",
      "threshold": 0.25,
      "prompts": [
        "person running outdoors",
        "jogging or sprinting athlete",
        "runner on a road or trail"
      ]
    },
    {
      "id": "cycling",
      "name": "Cycling",
      "group": "Activities",
      "threshold": 0.25,
      "prompts": [
        "person riding a bicycle",
        "cyclist riding on a road",
        "biking ride through outdoors"
      ]
    },
    {
      "id": "book",
      "name": "Book",
      "group": "Objects & Home",
      "threshold": 0.25,
      "prompts": [
        "a photo of a book",
        "open book or stack of books",
        "reading a novel or textbook"
      ]
    },
    {
      "id": "instrument",
      "name": "Instrument",
      "group": "Objects & Home",
      "threshold": 0.25,
      "prompts": [
        "a musical instrument",
        "acoustic guitar or piano",
        "playing a musical instrument"
      ]
    },
    {
      "id": "art",
      "name": "Art",
      "group": "Objects & Home",
      "threshold": 0.24,
      "prompts": [
        "a piece of art or painting",
        "sculpture or gallery artwork",
        "creative artistic visual work"
      ]
    },
    {
      "id": "garden",
      "name": "Garden",
      "group": "Objects & Home",
      "threshold": 0.24,
      "prompts": [
        "a beautiful garden",
        "flower garden or backyard plants",
        "landscaped green garden"
      ]
    },
    {
      "id": "interior",
      "name": "Interior",
      "group": "Objects & Home",
      "threshold": 0.24,
      "prompts": [
        "an indoor room interior",
        "living room or home interior design",
        "furnished indoor space"
      ]
    },
    {
      "id": "furniture",
      "name": "Furniture",
      "group": "Objects & Home",
      "threshold": 0.25,
      "prompts": [
        "modern home furniture",
        "chair, sofa or dining table",
        "interior furniture arrangement"
      ]
    },
    {
      "id": "night",
      "name": "Night",
      "group": "Mood & Weather",
      "threshold": 0.24,
      "prompts": [
        "a photo taken at night",
        "dark night scene with lights",
        "nighttime landscape or skyline"
      ]
    },
    {
      "id": "autumn",
      "name": "Autumn",
      "group": "Mood & Weather",
      "threshold": 0.24,
      "prompts": [
        "autumn season landscape",
        "fall foliage with yellow and red leaves",
        "crisp autumn day"
      ]
    },
    {
      "id": "spring",
      "name": "Spring",
      "group": "Mood & Weather",
      "threshold": 0.24,
      "prompts": [
        "spring season blooming",
        "fresh green spring nature",
        "spring flowers and budding trees"
      ]
    },
    {
      "id": "summer",
      "name": "Summer",
      "group": "Mood & Weather",
      "threshold": 0.24,
      "prompts": [
        "sunny summer day outdoors",
        "bright sunny warm summer scene",
        "summer vacation vibe"
      ]
    },
    {
      "id": "winter",
      "name": "Winter",
      "group": "Mood & Weather",
      "threshold": 0.24,
      "prompts": [
        "cold winter landscape",
        "frosty winter day scenery",
        "winter season atmosphere"
      ]
    },
    {
      "id": "rain",
      "name": "Rain",
      "group": "Mood & Weather",
      "threshold": 0.25,
      "prompts": [
        "rain falling down",
        "rainy day with water droplets",
        "wet rainy street with umbrellas"
      ]
    }
  ]
})json";

} // namespace

const char* getDefaultTaxonomyJson() {
    return kDefaultTaxonomyJson;
}

Result<TagTaxonomy> parseTaxonomyJson(const std::string& jsonStr) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(jsonStr);
    } catch (const std::exception& e) {
        return Result<TagTaxonomy>::failure(
            Status::parseError(std::string("Taxonomy JSON parse error: ") + e.what()));
    }

    if (!j.is_object()) {
        return Result<TagTaxonomy>::failure(
            Status::invalidArgument("Taxonomy root must be a JSON object"));
    }

    if (!j.contains("labels") || !j["labels"].is_array()) {
        return Result<TagTaxonomy>::failure(
            Status::invalidArgument("Taxonomy JSON must contain a 'labels' array"));
    }

    const auto& labelsArr = j["labels"];
    if (labelsArr.empty()) {
        return Result<TagTaxonomy>::failure(
            Status::invalidArgument("Taxonomy 'labels' array must not be empty"));
    }

    TagTaxonomy taxonomy;
    taxonomy.labels.reserve(labelsArr.size());

    std::unordered_set<std::string> seenIds;
    std::unordered_set<std::string> seenNames;

    for (size_t i = 0; i < labelsArr.size(); ++i) {
        const auto& item = labelsArr[i];
        if (!item.is_object()) {
            return Result<TagTaxonomy>::failure(
                Status::invalidArgument("Label at index " + std::to_string(i) + " must be a JSON object"));
        }

        // Validate id
        if (!item.contains("id") || !item["id"].is_string()) {
            return Result<TagTaxonomy>::failure(
                Status::invalidArgument("Label at index " + std::to_string(i) + " is missing required string 'id'"));
        }
        std::string id = item["id"].get<std::string>();
        if (trimWhitespace(id).empty()) {
            return Result<TagTaxonomy>::failure(
                Status::invalidArgument("Label at index " + std::to_string(i) + " has empty 'id'"));
        }
        std::string lowerId = toLower(id);
        if (seenIds.find(lowerId) != seenIds.end()) {
            return Result<TagTaxonomy>::failure(
                Status::invalidArgument("Duplicate label id '" + id + "'"));
        }
        seenIds.insert(lowerId);

        // Validate name
        if (!item.contains("name") || !item["name"].is_string()) {
            return Result<TagTaxonomy>::failure(
                Status::invalidArgument("Label '" + id + "' is missing required string 'name'"));
        }
        std::string name = item["name"].get<std::string>();
        if (trimWhitespace(name).empty()) {
            return Result<TagTaxonomy>::failure(
                Status::invalidArgument("Label '" + id + "' has empty 'name'"));
        }
        std::string lowerName = toLower(name);
        if (seenNames.find(lowerName) != seenNames.end()) {
            return Result<TagTaxonomy>::failure(
                Status::invalidArgument("Duplicate label name '" + name + "'"));
        }
        seenNames.insert(lowerName);

        // Validate group
        if (!item.contains("group") || !item["group"].is_string()) {
            return Result<TagTaxonomy>::failure(
                Status::invalidArgument("Label '" + id + "' is missing required string 'group'"));
        }
        std::string group = item["group"].get<std::string>();
        if (trimWhitespace(group).empty()) {
            return Result<TagTaxonomy>::failure(
                Status::invalidArgument("Label '" + id + "' has empty 'group'"));
        }

        // Validate threshold
        float threshold = 0.24f;
        if (item.contains("threshold")) {
            if (!item["threshold"].is_number()) {
                return Result<TagTaxonomy>::failure(
                    Status::invalidArgument("Label '" + id + "' threshold must be a number"));
            }
            threshold = item["threshold"].get<float>();
        }
        if (std::isnan(threshold) || threshold <= 0.0f || threshold > 1.0f) {
            return Result<TagTaxonomy>::failure(
                Status::invalidArgument("Label '" + id + "' threshold must be in range (0.0, 1.0]"));
        }

        // Validate prompts
        if (!item.contains("prompts") || !item["prompts"].is_array()) {
            return Result<TagTaxonomy>::failure(
                Status::invalidArgument("Label '" + id + "' is missing required array 'prompts'"));
        }
        const auto& promptsArr = item["prompts"];
        if (promptsArr.empty()) {
            return Result<TagTaxonomy>::failure(
                Status::invalidArgument("Label '" + id + "' 'prompts' array must not be empty"));
        }
        std::vector<std::string> prompts;
        prompts.reserve(promptsArr.size());
        for (size_t pIdx = 0; pIdx < promptsArr.size(); ++pIdx) {
            const auto& p = promptsArr[pIdx];
            if (!p.is_string()) {
                return Result<TagTaxonomy>::failure(
                    Status::invalidArgument("Label '" + id + "' prompt at index " + std::to_string(pIdx) + " must be a string"));
            }
            std::string pStr = p.get<std::string>();
            if (trimWhitespace(pStr).empty()) {
                return Result<TagTaxonomy>::failure(
                    Status::invalidArgument("Label '" + id + "' prompt at index " + std::to_string(pIdx) + " must not be empty"));
            }
            prompts.push_back(std::move(pStr));
        }

        TagLabel lbl;
        lbl.id = std::move(id);
        lbl.name = std::move(name);
        lbl.group = std::move(group);
        lbl.threshold = threshold;
        lbl.prompts = std::move(prompts);
        taxonomy.labels.push_back(std::move(lbl));
    }

    taxonomy.version = computeTaxonomyVersion(taxonomy.labels);
    return taxonomy;
}

Result<TagTaxonomy> loadTaxonomy(const std::optional<std::string>& customPath) {
    std::string path;
    if (customPath.has_value() && !customPath->empty()) {
        path = *customPath;
    } else {
        const char* envPath = std::getenv("IMAGINE_SMART_TAGS_TAXONOMY");
        if (envPath && *envPath) {
            path = envPath;
        }
    }

    if (!path.empty()) {
        std::ifstream file(pathFromUtf8(path), std::ios::binary);
        if (!file.is_open()) {
            return Result<TagTaxonomy>::failure(
                Status::ioError("Failed to open taxonomy file: " + path));
        }
        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
        return parseTaxonomyJson(content);
    }

    return parseTaxonomyJson(getDefaultTaxonomyJson());
}

std::vector<float> meanPoolAndNormalize(const std::vector<std::vector<float>>& promptEmbeddings) {
    if (promptEmbeddings.empty()) {
        return {};
    }
    const size_t dim = promptEmbeddings[0].size();
    if (dim == 0) {
        return {};
    }
    for (const auto& v : promptEmbeddings) {
        if (v.size() != dim) {
            return {};
        }
    }

    std::vector<double> mean(dim, 0.0);
    for (const auto& v : promptEmbeddings) {
        for (size_t i = 0; i < dim; ++i) {
            mean[i] += static_cast<double>(v[i]);
        }
    }

    const double n = static_cast<double>(promptEmbeddings.size());
    double sumSq = 0.0;
    for (size_t i = 0; i < dim; ++i) {
        mean[i] /= n;
        sumSq += mean[i] * mean[i];
    }

    const double norm = std::sqrt(sumSq);
    std::vector<float> result(dim, 0.0f);
    if (norm > 1e-12) {
        for (size_t i = 0; i < dim; ++i) {
            result[i] = static_cast<float>(mean[i] / norm);
        }
    }
    return result;
}

std::vector<LabelScore> selectSmartLabels(
    const std::vector<float>& imageEmbedding,
    const std::vector<std::vector<float>>& labelEmbeddings,
    const std::vector<TagLabel>& labels,
    const SmartTagOptions& options) {

    if (imageEmbedding.empty() || labelEmbeddings.empty() || labels.empty()) {
        return {};
    }
    if (options.maxLabels <= 0 || options.maxPerGroup <= 0) {
        return {};
    }

    const size_t count = std::min(labelEmbeddings.size(), labels.size());
    std::vector<LabelScore> candidates;
    candidates.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        const auto& label = labels[i];
        const auto& labelVec = labelEmbeddings[i];
        if (labelVec.size() != imageEmbedding.size()) {
            continue;
        }

        double dot = 0.0;
        for (size_t d = 0; d < imageEmbedding.size(); ++d) {
            dot += static_cast<double>(imageEmbedding[d]) * static_cast<double>(labelVec[d]);
        }
        const float score = static_cast<float>(dot);
        const float requiredThreshold = std::max(options.minScore, label.threshold);
        if (score >= requiredThreshold) {
            candidates.push_back(LabelScore{label.id, label.name, label.group, score});
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const LabelScore& a, const LabelScore& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.id < b.id;
    });

    std::vector<LabelScore> selected;
    std::unordered_map<std::string, int> groupCounts;
    for (const auto& candidate : candidates) {
        if (static_cast<int>(selected.size()) >= options.maxLabels) {
            break;
        }
        int& groupCount = groupCounts[candidate.group];
        if (groupCount < options.maxPerGroup) {
            selected.push_back(candidate);
            ++groupCount;
        }
    }

    return selected;
}

SmartTagOptions loadSmartTagOptionsFromEnv() {
    SmartTagOptions options;

    const char* minScoreEnv = std::getenv("IMAGINE_SMART_TAGS_MIN_SCORE");
    if (minScoreEnv && *minScoreEnv) {
        try {
            float val = std::stof(minScoreEnv);
            if (!std::isnan(val)) {
                options.minScore = std::clamp(val, 0.0f, 1.0f);
            }
        } catch (...) {
        }
    }

    const char* maxLabelsEnv = std::getenv("IMAGINE_SMART_TAGS_MAX_LABELS");
    if (maxLabelsEnv && *maxLabelsEnv) {
        try {
            int val = std::stoi(maxLabelsEnv);
            options.maxLabels = std::clamp(val, 1, 20);
        } catch (...) {
        }
    }

    const char* maxPerGroupEnv = std::getenv("IMAGINE_SMART_TAGS_MAX_PER_GROUP");
    if (maxPerGroupEnv && *maxPerGroupEnv) {
        try {
            int val = std::stoi(maxPerGroupEnv);
            options.maxPerGroup = std::clamp(val, 1, 10);
        } catch (...) {
        }
    }

    return options;
}

} // namespace imagine::clip
