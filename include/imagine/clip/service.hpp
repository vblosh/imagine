#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "imagine/common/error.hpp"
#include "imagine/common/types.hpp"
#include "imagine/clip/engine.hpp"

namespace imagine::db { class CatalogDb; }

namespace imagine::clip {

struct SearchHit {
    MediaId mediaId{0};
    float score{0.0f};
};

struct SearchRequest {
    std::string query;
    int limit{100};
    std::optional<int32_t> ratingMin;
    int64_t dateFrom{0};
    int64_t dateTo{0};
    std::optional<std::string> mediaType;
};

struct SearchResult {
    std::string modelId;
    double elapsedMs{0.0};
    std::vector<SearchHit> items;
};

inline void to_json(nlohmann::json& j, const SearchHit& hit) {
    j = nlohmann::json{
        {"mediaId", hit.mediaId},
        {"photoId", hit.mediaId},
        {"score", hit.score}
    };
}

inline void to_json(nlohmann::json& j, const SearchResult& res) {
    j = nlohmann::json{
        {"model", res.modelId},
        {"elapsedMs", res.elapsedMs},
        {"items", res.items}
    };
}

class Service {
public:
    using PathResolver = std::function<std::string(const std::string&)> ;

    Service(db::CatalogDb& db, std::string cacheDirectory, PathResolver resolver);
    ~Service();

    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    nlohmann::json status() const;

    Result<SearchResult> search(const SearchRequest& request);
    Result<SearchResult> findSimilar(MediaId mediaId, int limit = 20);

    Status startJob(const std::string& scope, const std::vector<MediaId>& mediaIds,
                    bool force, int64_t& jobId);
    Result<nlohmann::json> getJob(int64_t jobId) const;
    Status cancelJob(int64_t jobId);

private:
    void runJob(int64_t jobId, std::vector<MediaId> mediaIds, bool force);
    void loadIndex();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace imagine::clip
