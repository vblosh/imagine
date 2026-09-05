#include "imagine/core/query.hpp"
#include <algorithm>

namespace imagine::core {

void to_json(nlohmann::json& j, const QueryCriteria& c) {
    j = nlohmann::json{
        {"camera_make", c.camera_make},
        {"camera_model", c.camera_model},
        {"date_from", c.date_from},
        {"date_to", c.date_to},
        {"search_text", c.search_text},
        {"sort_by", c.sort_by},
        {"sort_descending", c.sort_descending},
        {"limit", c.limit},
        {"offset", c.offset},
        {"tag_ids", c.tag_ids}
    };
    if (c.min_rating.has_value()) {
        j["min_rating"] = *c.min_rating;
    }
    if (c.max_rating.has_value()) {
        j["max_rating"] = *c.max_rating;
    }
    if (c.flag.has_value()) {
        j["flag"] = static_cast<int8_t>(*c.flag);
    }
    if (c.album_id.has_value()) {
        j["album_id"] = *c.album_id;
    }
}

void from_json(const nlohmann::json& j, QueryCriteria& c) {
    if (j.contains("camera_make") && j["camera_make"].is_string()) {
        c.camera_make = j["camera_make"].get<std::string>();
    }
    if (j.contains("camera_model") && j["camera_model"].is_string()) {
        c.camera_model = j["camera_model"].get<std::string>();
    }
    if (j.contains("date_from") && j["date_from"].is_number()) {
        c.date_from = j["date_from"].get<int64_t>();
    }
    if (j.contains("date_to") && j["date_to"].is_number()) {
        c.date_to = j["date_to"].get<int64_t>();
    }
    if (j.contains("search_text") && j["search_text"].is_string()) {
        c.search_text = j["search_text"].get<std::string>();
    }
    if (j.contains("sort_by") && j["sort_by"].is_string()) {
        c.sort_by = j["sort_by"].get<std::string>();
    }
    if (j.contains("sort_descending") && j["sort_descending"].is_boolean()) {
        c.sort_descending = j["sort_descending"].get<bool>();
    }
    if (j.contains("limit") && j["limit"].is_number()) {
        c.limit = j["limit"].get<int32_t>();
    }
    if (j.contains("offset") && j["offset"].is_number()) {
        c.offset = j["offset"].get<int32_t>();
    }
    if (j.contains("tag_ids") && j["tag_ids"].is_array()) {
        c.tag_ids = j["tag_ids"].get<std::vector<TagId>>();
    }
    if (j.contains("min_rating") && j["min_rating"].is_number()) {
        c.min_rating = j["min_rating"].get<int32_t>();
    }
    if (j.contains("max_rating") && j["max_rating"].is_number()) {
        c.max_rating = j["max_rating"].get<int32_t>();
    }
    if (j.contains("flag") && j["flag"].is_number()) {
        c.flag = static_cast<FlagState>(j["flag"].get<int8_t>());
    }
    if (j.contains("album_id") && j["album_id"].is_number()) {
        c.album_id = j["album_id"].get<AlbumId>();
    }
}

void to_json(nlohmann::json& j, const QueryResult& r) {
    j = nlohmann::json{
        {"items", r.items},
        {"total_count", r.total_count}
    };
}

void from_json(const nlohmann::json& j, QueryResult& r) {
    if (j.contains("items") && j["items"].is_array()) {
        r.items = j["items"].get<std::vector<MediaItem>>();
    }
    if (j.contains("total_count") && j["total_count"].is_number()) {
        r.total_count = j["total_count"].get<int64_t>();
    }
}

QueryBuilder::QueryBuilder(QueryCriteria criteria)
    : criteria_(std::move(criteria)) {}

QueryBuilder& QueryBuilder::setCriteria(QueryCriteria criteria) {
    criteria_ = std::move(criteria);
    return *this;
}

QueryBuilder& QueryBuilder::minRating(int32_t rating) {
    criteria_.min_rating = rating;
    return *this;
}

QueryBuilder& QueryBuilder::maxRating(int32_t rating) {
    criteria_.max_rating = rating;
    return *this;
}

QueryBuilder& QueryBuilder::flag(FlagState flag) {
    criteria_.flag = flag;
    return *this;
}

QueryBuilder& QueryBuilder::addTag(TagId tagId) {
    criteria_.tag_ids.push_back(tagId);
    return *this;
}

QueryBuilder& QueryBuilder::setTags(std::vector<TagId> tagIds) {
    criteria_.tag_ids = std::move(tagIds);
    return *this;
}

QueryBuilder& QueryBuilder::album(AlbumId albumId) {
    criteria_.album_id = albumId;
    return *this;
}

QueryBuilder& QueryBuilder::camera(std::string make, std::string model) {
    criteria_.camera_make = std::move(make);
    criteria_.camera_model = std::move(model);
    return *this;
}

QueryBuilder& QueryBuilder::dateRange(int64_t from, int64_t to) {
    criteria_.date_from = from;
    criteria_.date_to = to;
    return *this;
}

QueryBuilder& QueryBuilder::search(std::string text) {
    criteria_.search_text = std::move(text);
    return *this;
}

QueryBuilder& QueryBuilder::sort(std::string sortBy, bool descending) {
    criteria_.sort_by = std::move(sortBy);
    criteria_.sort_descending = descending;
    return *this;
}

QueryBuilder& QueryBuilder::paginate(int32_t limit, int32_t offset) {
    criteria_.limit = limit;
    criteria_.offset = offset;
    return *this;
}

std::pair<std::string, std::vector<std::string>> QueryBuilder::buildWhere() const {
    std::vector<std::string> clauses;
    std::vector<std::string> params;

    if (criteria_.min_rating.has_value()) {
        clauses.push_back("rating >= ?");
        params.push_back(std::to_string(*criteria_.min_rating));
    }

    if (criteria_.max_rating.has_value()) {
        clauses.push_back("rating <= ?");
        params.push_back(std::to_string(*criteria_.max_rating));
    }

    if (criteria_.flag.has_value()) {
        clauses.push_back("flag = ?");
        params.push_back(std::to_string(static_cast<int32_t>(*criteria_.flag)));
    }

    for (TagId tagId : criteria_.tag_ids) {
        clauses.push_back("id IN (SELECT media_id FROM media_tags WHERE tag_id = ?)");
        params.push_back(std::to_string(tagId));
    }

    if (criteria_.album_id.has_value()) {
        clauses.push_back("id IN (SELECT media_id FROM album_media WHERE album_id = ?)");
        params.push_back(std::to_string(*criteria_.album_id));
    }

    if (!criteria_.camera_make.empty()) {
        clauses.push_back("camera_make LIKE ?");
        params.push_back("%" + criteria_.camera_make + "%");
    }

    if (!criteria_.camera_model.empty()) {
        clauses.push_back("camera_model LIKE ?");
        params.push_back("%" + criteria_.camera_model + "%");
    }

    if (criteria_.date_from > 0) {
        clauses.push_back("date_taken >= ?");
        params.push_back(std::to_string(criteria_.date_from));
    }

    if (criteria_.date_to > 0) {
        clauses.push_back("date_taken <= ?");
        params.push_back(std::to_string(criteria_.date_to));
    }

    if (!criteria_.search_text.empty()) {
        clauses.push_back("(file_name LIKE ? OR file_path LIKE ? OR camera_make LIKE ? OR camera_model LIKE ? OR lens LIKE ? OR id IN (SELECT media_id FROM media_tags JOIN tags ON media_tags.tag_id = tags.id WHERE tags.name LIKE ?))");
        std::string pattern = "%" + criteria_.search_text + "%";
        params.push_back(pattern);
        params.push_back(pattern);
        params.push_back(pattern);
        params.push_back(pattern);
        params.push_back(pattern);
        params.push_back(pattern);
    }

    std::string whereClause;
    for (size_t i = 0; i < clauses.size(); ++i) {
        if (i > 0) {
            whereClause += " AND ";
        }
        whereClause += "(" + clauses[i] + ")";
    }

    return {whereClause, params};
}

std::string QueryBuilder::buildOrderBy() const {
    std::string column = "date_taken";
    if (criteria_.sort_by == "date_taken" || criteria_.sort_by == "rating" ||
        criteria_.sort_by == "file_name" || criteria_.sort_by == "file_size") {
        column = criteria_.sort_by;
    }
    std::string direction = criteria_.sort_descending ? "DESC" : "ASC";
    return column + " " + direction + ", id " + direction;
}

Result<QueryResult> QueryBuilder::execute(db::CatalogDb& db) const {
    auto [whereClause, params] = buildWhere();
    std::string orderBy = buildOrderBy();

    auto itemsRes = db.queryMedia(whereClause, params, orderBy, criteria_.limit, criteria_.offset);
    if (!itemsRes.isOk()) {
        return itemsRes.status();
    }

    QueryResult result;
    result.items = std::move(itemsRes.value());

    // Optimization: When offset is 0 and returned items count is less than limit,
    // total_count is known exactly without executing an extra full-table COUNT query.
    if (criteria_.offset == 0 && criteria_.limit > 0 &&
        static_cast<int>(result.items.size()) < criteria_.limit) {
        result.total_count = static_cast<int64_t>(result.items.size());
    } else {
        auto countRes = db.countMedia(whereClause, params);
        if (!countRes.isOk()) {
            return countRes.status();
        }
        result.total_count = countRes.value();
    }

    return result;
}

Result<std::vector<MediaItem>> QueryBuilder::executeQuery(db::CatalogDb& db) const {
    auto res = execute(db);
    if (!res.isOk()) {
        return res.status();
    }
    return std::move(res.value().items);
}

Result<int64_t> QueryBuilder::executeCount(db::CatalogDb& db) const {
    auto [whereClause, params] = buildWhere();
    return db.countMedia(whereClause, params);
}

Result<QueryResult> QueryBuilder::execute(db::CatalogDb& db, const QueryCriteria& criteria) {
    QueryBuilder qb(criteria);
    return qb.execute(db);
}

Result<std::vector<MediaItem>> QueryBuilder::executeQuery(db::CatalogDb& db, const QueryCriteria& criteria) {
    QueryBuilder qb(criteria);
    return qb.executeQuery(db);
}

Result<int64_t> QueryBuilder::executeCount(db::CatalogDb& db, const QueryCriteria& criteria) {
    QueryBuilder qb(criteria);
    return qb.executeCount(db);
}

} // namespace imagine::core
