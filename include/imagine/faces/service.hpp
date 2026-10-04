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
#include "imagine/thumbnail/generator.hpp"
#include "imagine/faces/engine.hpp"

namespace imagine::db { class CatalogDb; }

namespace imagine::faces {

class Service {
public:
    using PathResolver = std::function<std::string(const std::string&)>;
    using Analyzer = std::function<Result<std::vector<Detection>>(const thumbnail::ImageBuffer&)>;

    Service(db::CatalogDb& db, std::string cacheDirectory, PathResolver resolver, Analyzer analyzer = {});
    ~Service();

    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    nlohmann::json status() const;
    Status startJob(const std::string& scope, const std::vector<MediaId>& mediaIds,
                    bool force, int64_t& jobId);
    Result<nlohmann::json> getJob(int64_t jobId) const;
    Status cancelJob(int64_t jobId);
    Result<nlohmann::json> getMedia(MediaId mediaId) const;
    Result<nlohmann::json> getReview(int offset, int limit) const;
    Result<nlohmann::json> getGrid(std::optional<int64_t> jobId, int offset, int limit,
                                   bool includeDismissed) const;
    Result<nlohmann::json> setIdentity(int64_t faceId, int64_t revision,
                                       std::optional<TagId> tagId,
                                       const std::optional<std::string>& name);
    Result<nlohmann::json> acceptSuggestion(int64_t faceId, int64_t revision, TagId tagId);
    Result<nlohmann::json> rejectSuggestion(int64_t faceId, int64_t revision, TagId tagId);
    Result<nlohmann::json> setDismissed(int64_t faceId, int64_t revision, bool dismissed);
    Result<std::string> cropPath(int64_t faceId, int64_t revision);

private:
    void runJob(int64_t jobId, std::vector<MediaId> mediaIds, bool force);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace imagine::faces
