#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <utility>
#include <nlohmann/json.hpp>
#include "imagine/common/types.hpp"
#include "imagine/common/error.hpp"
#include "imagine/db/catalog_db.hpp"

namespace imagine::core {

struct QueryCriteria {
    std::optional<int32_t> min_rating{std::nullopt};
    std::optional<int32_t> max_rating{std::nullopt};
    std::optional<FlagState> flag{std::nullopt};
    std::vector<TagId> tag_ids;
    std::optional<AlbumId> album_id{std::nullopt};
    std::string camera_make;
    std::string camera_model;
    int64_t date_from{0};
    int64_t date_to{0};
    std::string search_text;
    std::string sort_by{"date_taken"};
    bool sort_descending{true};
    int32_t limit{100};
    int32_t offset{0};
};

void to_json(nlohmann::json& j, const QueryCriteria& c);
void from_json(const nlohmann::json& j, QueryCriteria& c);

struct QueryResult {
    std::vector<MediaItem> items;
    int64_t total_count{0};

    operator const std::vector<MediaItem>&() const noexcept { return items; }
};

void to_json(nlohmann::json& j, const QueryResult& r);
void from_json(const nlohmann::json& j, QueryResult& r);

class QueryBuilder {
public:
    QueryBuilder() = default;
    explicit QueryBuilder(QueryCriteria criteria);

    QueryBuilder& setCriteria(QueryCriteria criteria);
    const QueryCriteria& criteria() const noexcept { return criteria_; }
    QueryCriteria& criteria() noexcept { return criteria_; }

    QueryBuilder& minRating(int32_t rating);
    QueryBuilder& maxRating(int32_t rating);
    QueryBuilder& flag(FlagState flag);
    QueryBuilder& addTag(TagId tagId);
    QueryBuilder& setTags(std::vector<TagId> tagIds);
    QueryBuilder& album(AlbumId albumId);
    QueryBuilder& camera(std::string make, std::string model = "");
    QueryBuilder& dateRange(int64_t from, int64_t to);
    QueryBuilder& search(std::string text);
    QueryBuilder& sort(std::string sortBy, bool descending = true);
    QueryBuilder& paginate(int32_t limit, int32_t offset = 0);

    std::pair<std::string, std::vector<std::string>> buildWhere() const;
    std::string buildOrderBy() const;

    Result<QueryResult> execute(db::CatalogDb& db) const;
    Result<std::vector<MediaItem>> executeQuery(db::CatalogDb& db) const;
    Result<int64_t> executeCount(db::CatalogDb& db) const;

    static Result<QueryResult> execute(db::CatalogDb& db, const QueryCriteria& criteria);
    static Result<std::vector<MediaItem>> executeQuery(db::CatalogDb& db, const QueryCriteria& criteria);
    static Result<int64_t> executeCount(db::CatalogDb& db, const QueryCriteria& criteria);

private:
    QueryCriteria criteria_;
};

} // namespace imagine::core

namespace imagine {
using core::QueryCriteria;
using core::QueryResult;
using core::QueryBuilder;
}
