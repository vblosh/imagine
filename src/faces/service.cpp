#include "imagine/faces/service.hpp"

#include "imagine/db/catalog_db.hpp"
#include "imagine/faces/engine.hpp"
#include "imagine/thumbnail/generator.hpp"
#include "imagine/metadata/hasher.hpp"
#include "imagine/common/types.hpp"
#include "imagine/common/logger.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <condition_variable>
#include <future>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if defined(_MSC_VER)
#include <intrin.h>
#include <immintrin.h>
#elif defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#include <immintrin.h>
#endif

namespace imagine::faces {
namespace {

using nlohmann::json;
using db::Connection;
using db::Statement;
using db::StepResult;
constexpr size_t kEmbeddingDimensions = 512;

int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string envString(const char* key, const std::string& fallback) {
    const char* value = std::getenv(key);
    return value ? std::string(value) : fallback;
}

float envFloat(const char* key, float fallback) {
    const char* value = std::getenv(key);
    if (!value) return fallback;
    try {
        size_t used = 0;
        float parsed = std::stof(value, &used);
        if (used == std::strlen(value)) return parsed;
    } catch (...) {}
    return fallback;
}

Status dbError(Connection& conn, const std::string& context) {
    return Status::databaseError(context + ": " + conn.lastErrorMessage());
}

inline float dotProduct512Scalar(const float* a, const float* b) {
    double dot = 0;
    for (size_t i = 0; i < kEmbeddingDimensions; ++i) {
        dot += static_cast<double>(a[i]) * b[i];
    }
    return static_cast<float>(dot);
}

#if (defined(_M_X64) || defined(__x86_64__))
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2,fma")))
#endif
inline float dotProduct512Avx2(const float* a, const float* b) {
    __m256d sum0 = _mm256_setzero_pd();
    __m256d sum1 = _mm256_setzero_pd();
    __m256d sum2 = _mm256_setzero_pd();
    __m256d sum3 = _mm256_setzero_pd();
    for (size_t i = 0; i < kEmbeddingDimensions; i += 16) {
        __m128 a0 = _mm_loadu_ps(a + i);
        __m128 b0 = _mm_loadu_ps(b + i);
        __m128 a1 = _mm_loadu_ps(a + i + 4);
        __m128 b1 = _mm_loadu_ps(b + i + 4);
        __m128 a2 = _mm_loadu_ps(a + i + 8);
        __m128 b2 = _mm_loadu_ps(b + i + 8);
        __m128 a3 = _mm_loadu_ps(a + i + 12);
        __m128 b3 = _mm_loadu_ps(b + i + 12);

        sum0 = _mm256_fmadd_pd(_mm256_cvtps_pd(a0), _mm256_cvtps_pd(b0), sum0);
        sum1 = _mm256_fmadd_pd(_mm256_cvtps_pd(a1), _mm256_cvtps_pd(b1), sum1);
        sum2 = _mm256_fmadd_pd(_mm256_cvtps_pd(a2), _mm256_cvtps_pd(b2), sum2);
        sum3 = _mm256_fmadd_pd(_mm256_cvtps_pd(a3), _mm256_cvtps_pd(b3), sum3);
    }
    __m256d sum = _mm256_add_pd(_mm256_add_pd(sum0, sum1), _mm256_add_pd(sum2, sum3));
    alignas(32) double vals[4];
    _mm256_store_pd(vals, sum);
    return static_cast<float>(vals[0] + vals[1] + vals[2] + vals[3]);
}

inline bool checkAvx2FmaSupport() {
#if defined(_MSC_VER)
    int cpuInfo[4];
    __cpuid(cpuInfo, 0);
    if (cpuInfo[0] < 7) return false;
    __cpuid(cpuInfo, 1);
    bool osxsave = (cpuInfo[2] & (1 << 27)) != 0;
    bool fma = (cpuInfo[2] & (1 << 12)) != 0;
    bool avx = (cpuInfo[2] & (1 << 28)) != 0;
    if (!osxsave || !fma || !avx) return false;
    unsigned long long xcrFeatureMask = _xgetbv(0);
    if ((xcrFeatureMask & 0x6) != 0x6) return false;
    __cpuidex(cpuInfo, 7, 0);
    return (cpuInfo[1] & (1 << 5)) != 0;
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#else
    return false;
#endif
}

inline bool hasAvx2Fma() {
    static const bool supported = checkAvx2FmaSupport();
    return supported;
}
#endif

inline float dotProduct512(const float* a, const float* b) {
#if (defined(_M_X64) || defined(__x86_64__))
    if (hasAvx2Fma()) {
        return dotProduct512Avx2(a, b);
    }
#endif
    return dotProduct512Scalar(a, b);
}

struct ReviewStamp {
    int64_t totalChanges{0};
    int64_t dataVersion{0};

    bool operator==(const ReviewStamp& other) const noexcept {
        return totalChanges == other.totalChanges && dataVersion == other.dataVersion;
    }
};

Result<ReviewStamp> readReviewStamp(Connection& conn) {
    if (!conn.raw()) return Status::databaseError("Catalog database is not open");
    auto versionRes = conn.prepare("PRAGMA data_version;");
    if (!versionRes.isOk()) return versionRes.status();
    auto version = std::move(versionRes.value());
    if (version.step() != StepResult::Row) return dbError(conn, "Failed to read catalog data version");
    return ReviewStamp{sqlite3_total_changes64(conn.raw()), version.getInt64(0)};
}

std::string newReviewToken() {
    unsigned char bytes[16];
    if (RAND_bytes(bytes, static_cast<int>(sizeof(bytes))) != 1) return {};
    static constexpr char kHex[] = "0123456789abcdef";
    std::string token;
    token.resize(sizeof(bytes) * 2);
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        token[i * 2] = kHex[bytes[i] >> 4];
        token[i * 2 + 1] = kHex[bytes[i] & 0x0f];
    }
    return token;
}

json jobJson(Connection& conn, int64_t id) {
    auto stmtRes = conn.prepare(
        "SELECT id,state,total,processed,skipped,failed,remaining,error FROM face_analysis_jobs WHERE id=?;");
    if (!stmtRes.isOk()) return json();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, id);
    if (stmt.step() != StepResult::Row) return json();
    return json{{"id", stmt.getInt64(0)}, {"state", stmt.getString(1)},
                {"total", stmt.getInt64(2)}, {"processed", stmt.getInt64(3)},
                {"skipped", stmt.getInt64(4)}, {"failed", stmt.getInt64(5)},
                {"remaining", stmt.getInt64(6)}, {"error", stmt.getString(7)}};
}

bool isPeopleTag(Connection& conn, TagId tagId, std::string* name = nullptr) {
    auto stmtRes = conn.prepare("SELECT name FROM tags WHERE id=? AND category='people';");
    if (!stmtRes.isOk()) return false;
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, tagId);
    if (stmt.step() != StepResult::Row) return false;
    if (name) *name = stmt.getString(0);
    return true;
}

struct Suggestion {
    TagId tagId{0};
    std::string name;
    float score{0};
};

struct Exemplar {
    int64_t faceId{0};
    TagId tagId{0};
    std::string name;
    std::vector<float> embedding;
};

using ExemplarCache = std::unordered_map<std::string, std::vector<Exemplar>>;
using RejectionCache = std::unordered_map<int64_t, std::unordered_set<TagId>>;

struct FaceMatchInfo {
    std::vector<float> embedding;
    std::optional<TagId> personTagId;
    std::string recognizerChecksum;
    std::string pipelineVersion;
    std::string embeddingError;
    std::string analysisState;
    bool dismissed{false};
};

std::vector<float> readVector(const Statement& stmt, int col, int expectedSize) {
    sqlite3_stmt* raw = stmt.raw();
    if (!raw || sqlite3_column_type(raw, col) != SQLITE_BLOB) return {};
    int bytes = sqlite3_column_bytes(raw, col);
    const void* blob = sqlite3_column_blob(raw, col);
    if (!blob || bytes <= 0 || bytes % static_cast<int>(sizeof(float)) != 0) return {};
    int count = bytes / static_cast<int>(sizeof(float));
    if (expectedSize > 0 && count != expectedSize) return {};
    std::vector<float> out(static_cast<size_t>(count));
    std::memcpy(out.data(), blob, static_cast<size_t>(bytes));
    return out;
}

std::vector<Exemplar> loadExemplars(Connection& conn, const std::string& recognizerChecksum,
                                    const std::string& pipelineVersion) {
    std::vector<Exemplar> exemplars;
    exemplars.reserve(1024);
    auto stmtRes = conn.prepare(R"SQL(
        SELECT f.id,t.id,t.name,f.embedding,f.embedding_size
        FROM faces f
        JOIN tags t ON t.id=f.person_tag_id AND t.category='people'
        JOIN face_media_analysis a ON a.media_id=f.media_id
        WHERE f.dismissed=0 AND f.embedding IS NOT NULL AND f.embedding_error=''
          AND f.embedding_size=512 AND a.recognizer_checksum=? AND a.pipeline_version=?
          AND a.state='complete'
    )SQL");
    if (!stmtRes.isOk()) return exemplars;
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, recognizerChecksum);
    stmt.bind(2, pipelineVersion);
    while (stmt.step() == StepResult::Row) {
        auto embedding = readVector(stmt, 3, 512);
        if (embedding.size() == 512) {
            exemplars.push_back({stmt.getInt64(0), stmt.getInt64(1), stmt.getString(2), std::move(embedding)});
        }
    }
    return exemplars;
}

std::vector<Suggestion> suggestionsFromExemplars(
    int64_t faceId, const std::vector<float>& query,
    const std::vector<Exemplar>& exemplars, float threshold,
    const std::unordered_set<TagId>& excludedTagIds) {
    std::vector<Suggestion> result;
    if (query.size() != kEmbeddingDimensions) return result;

    std::unordered_map<TagId, Suggestion> best;
    const bool hasExclusions = !excludedTagIds.empty();
    const float* queryData = query.data();
    for (const auto& exemplar : exemplars) {
        if (exemplar.faceId == faceId || (hasExclusions && excludedTagIds.contains(exemplar.tagId)) ||
            exemplar.embedding.size() != kEmbeddingDimensions) continue;
        float score = dotProduct512(queryData, exemplar.embedding.data());
        if (!std::isfinite(score) || score < threshold) continue;
        score = std::clamp(score, -1.0f, 1.0f);
        auto it = best.find(exemplar.tagId);
        if (it == best.end() || score > it->second.score) {
            best[exemplar.tagId] = Suggestion{exemplar.tagId, exemplar.name, score};
        }
    }
    for (auto& [id, suggestion] : best) result.push_back(std::move(suggestion));
    std::sort(result.begin(), result.end(), [](const Suggestion& a, const Suggestion& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.name < b.name;
    });
    if (result.size() > 3) result.resize(3);
    return result;
}

Status loadRejections(Connection& conn, const std::vector<int64_t>& faceIds,
                      RejectionCache& cache) {
    if (faceIds.empty()) return Status::ok();
    constexpr size_t kChunkSize = 900;
    for (size_t start = 0; start < faceIds.size(); start += kChunkSize) {
        const size_t count = std::min(kChunkSize, faceIds.size() - start);
        std::string sql = "SELECT face_id,tag_id FROM face_rejections WHERE face_id IN (";
        for (size_t i = 0; i < count; ++i) {
            if (i) sql += ',';
            sql += '?';
        }
        sql += ");";
        auto stmtRes = conn.prepare(sql);
        if (!stmtRes.isOk()) return stmtRes.status();
        auto stmt = std::move(stmtRes.value());
        for (size_t i = 0; i < count; ++i) {
            Status bound = stmt.bind(static_cast<int>(i + 1), faceIds[start + i]);
            if (!bound.isOk()) return bound;
        }
        StepResult step;
        while ((step = stmt.step()) == StepResult::Row) {
            cache[stmt.getInt64(0)].insert(stmt.getInt64(1));
        }
        if (step == StepResult::Error) return dbError(conn, "Failed to load face suggestion rejections");
    }
    return Status::ok();
}

std::vector<Suggestion> suggestionsFor(Connection& conn, int64_t faceId,
                                       const std::vector<float>& query,
                                       const std::string& recognizerChecksum,
                                       const std::string& pipelineVersion,
                                       float threshold, ExemplarCache* cache = nullptr,
                                       const std::unordered_set<TagId>* rejectedTags = nullptr) {
    std::vector<Suggestion> result;
    if (query.size() != kEmbeddingDimensions || recognizerChecksum.empty() || pipelineVersion.empty()) return result;

    std::vector<Exemplar> local;
    const std::vector<Exemplar>* exemplars = nullptr;
    if (cache) {
        const std::string key = recognizerChecksum + "\x1f" + pipelineVersion;
        auto it = cache->find(key);
        if (it == cache->end()) it = cache->emplace(key, loadExemplars(conn, recognizerChecksum, pipelineVersion)).first;
        exemplars = &it->second;
    } else {
        local = loadExemplars(conn, recognizerChecksum, pipelineVersion);
        exemplars = &local;
    }

    std::unordered_set<TagId> loadedRejectedTags;
    if (!rejectedTags) {
        auto rejectedRes = conn.prepare("SELECT tag_id FROM face_rejections WHERE face_id=?;");
        if (!rejectedRes.isOk()) return result;
        auto rejectedStmt = std::move(rejectedRes.value()); rejectedStmt.bind(1, faceId);
        while (rejectedStmt.step() == StepResult::Row) loadedRejectedTags.insert(rejectedStmt.getInt64(0));
    }
    const auto& excludedTagIds = rejectedTags ? *rejectedTags : loadedRejectedTags;

    return suggestionsFromExemplars(faceId, query, *exemplars, threshold, excludedTagIds);
}

Result<json> faceJson(Connection& conn, int64_t faceId, float matchThreshold,
                      ExemplarCache* exemplarCache = nullptr,
                      const RejectionCache* rejectionCache = nullptr,
                      bool suppressReviewedSuggestions = false,
                      FaceMatchInfo* matchInfo = nullptr,
                      bool includeSuggestions = true) {
    auto stmtRes = conn.prepare(R"SQL(
        SELECT f.id,f.media_id,f.revision,f.x,f.y,f.width,f.height,f.score,f.landmarks,
               f.person_tag_id,t.name,f.dismissed,f.embedding_error,f.embedding,f.embedding_size,
               a.recognizer_checksum,a.pipeline_version,a.state
        FROM faces f
        LEFT JOIN tags t ON t.id=f.person_tag_id
        LEFT JOIN face_media_analysis a ON a.media_id=f.media_id
        WHERE f.id=?;
    )SQL");
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, faceId);
    if (stmt.step() != StepResult::Row) return Status::notFound("Face not found");

    FaceMatchInfo info;
    if (!stmt.isNull(9)) info.personTagId = stmt.getInt64(9);
    info.dismissed = stmt.getInt(11) != 0;
    info.embeddingError = stmt.getString(12);
    info.recognizerChecksum = stmt.getString(15);
    info.pipelineVersion = stmt.getString(16);
    info.analysisState = stmt.getString(17);
    if (!stmt.isNull(13) && stmt.getInt(14) == 512) info.embedding = readVector(stmt, 13, 512);

    json landmarks = json::array();
    try { landmarks = json::parse(stmt.getString(8)); } catch (...) {}
    json out = {{"id", stmt.getInt64(0)}, {"media_id", stmt.getInt64(1)},
                {"revision", stmt.getInt64(2)}, {"x", stmt.getDouble(3)},
                {"y", stmt.getDouble(4)}, {"width", stmt.getDouble(5)},
                {"height", stmt.getDouble(6)}, {"score", stmt.getDouble(7)},
                {"landmarks", std::move(landmarks)},
                {"person_tag_id", stmt.isNull(9) ? json(nullptr) : json(stmt.getInt64(9))},
                {"person_name", stmt.isNull(9) ? json(nullptr) : json(stmt.getString(10))},
                {"dismissed", stmt.getInt(11) != 0}, {"embedding_error", stmt.getString(12)},
                {"suggestions", json::array()}, {"crop_url", "/api/faces/" + std::to_string(faceId) + "/crop?revision=" + std::to_string(stmt.getInt64(2))}};
    const bool suggestionTarget = !suppressReviewedSuggestions ||
        (!info.personTagId.has_value() && !info.dismissed);
    if (includeSuggestions && suggestionTarget && info.analysisState == "complete" &&
        info.embeddingError.empty() && info.embedding.size() == 512) {
        const std::unordered_set<TagId>* rejected = nullptr;
        if (rejectionCache) {
            static const std::unordered_set<TagId> kNoRejections;
            auto it = rejectionCache->find(faceId);
            rejected = it == rejectionCache->end() ? &kNoRejections : &it->second;
        }
        auto suggestions = suggestionsFor(conn, faceId, info.embedding,
                                          info.recognizerChecksum, info.pipelineVersion,
                                          matchThreshold, exemplarCache, rejected);
        for (const auto& s : suggestions) {
            out["suggestions"].push_back({{"tag_id", s.tagId}, {"name", s.name}, {"score", s.score}});
        }
    }
    if (matchInfo) *matchInfo = std::move(info);
    return out;
}

void updateExemplarCache(ExemplarCache& cache, const FaceMatchInfo& info,
                         int64_t faceId, TagId tagId, const std::string& tagName) {
    for (auto& [key, exemplars] : cache) {
        exemplars.erase(std::remove_if(exemplars.begin(), exemplars.end(),
            [faceId](const Exemplar& exemplar) { return exemplar.faceId == faceId; }), exemplars.end());
    }
    if (info.analysisState == "complete" && !info.dismissed && info.embeddingError.empty() &&
        info.embedding.size() == 512 && !info.recognizerChecksum.empty() && !info.pipelineVersion.empty()) {
        const std::string key = info.recognizerChecksum + "\x1f" + info.pipelineVersion;
        auto it = cache.find(key);
        if (it != cache.end()) it->second.push_back({faceId, tagId, tagName, info.embedding});
    }
}

Status execDone(Connection& conn, Statement& stmt, const std::string& context) {
    if (stmt.step() != StepResult::Done) return dbError(conn, context);
    return Status::ok();
}

Status syncFaceTagOwnership(Connection& conn, MediaId mediaId, TagId tagId) {
    auto countRes = conn.prepare(
        "SELECT COUNT(*) FROM faces INDEXED BY idx_faces_media "
        "WHERE media_id=? AND person_tag_id=? AND dismissed=0;");
    if (!countRes.isOk()) return countRes.status();
    auto count = std::move(countRes.value());
    count.bind(1, mediaId);
    count.bind(2, tagId);
    int64_t assigned = count.step() == StepResult::Row ? count.getInt64(0) : 0;

    auto provenanceRes = conn.prepare("SELECT manual FROM media_tag_provenance WHERE media_id=? AND tag_id=?;");
    if (!provenanceRes.isOk()) return provenanceRes.status();
    auto provenance = std::move(provenanceRes.value());
    provenance.bind(1, mediaId);
    provenance.bind(2, tagId);
    bool hasProvenance = provenance.step() == StepResult::Row;
    bool manual = hasProvenance && provenance.getInt(0) != 0;

    if (assigned > 0) {
        auto linkRes = conn.prepare("INSERT OR IGNORE INTO media_tags(media_id,tag_id) VALUES(?,?);");
        if (!linkRes.isOk()) return linkRes.status();
        auto link = std::move(linkRes.value());
        link.bind(1, mediaId); link.bind(2, tagId);
        Status s = execDone(conn, link, "Failed to attach face people tag");
        if (!s.isOk()) return s;
        if (!hasProvenance) {
            auto provRes = conn.prepare("INSERT INTO media_tag_provenance(media_id,tag_id,manual) VALUES(?,?,?);");
            if (!provRes.isOk()) return provRes.status();
            auto prov = std::move(provRes.value());
            prov.bind(1, mediaId); prov.bind(2, tagId);
            prov.bind(3, conn.changes() > 0 ? 0 : 1);
            return execDone(conn, prov, "Failed to record face tag ownership");
        }
        return Status::ok();
    }

    if (hasProvenance && !manual) {
        auto deleteLinkRes = conn.prepare("DELETE FROM media_tags WHERE media_id=? AND tag_id=?;");
        if (!deleteLinkRes.isOk()) return deleteLinkRes.status();
        auto deleteLink = std::move(deleteLinkRes.value());
        deleteLink.bind(1, mediaId); deleteLink.bind(2, tagId);
        Status s = execDone(conn, deleteLink, "Failed to remove face-owned people tag");
        if (!s.isOk()) return s;
        auto deleteProvRes = conn.prepare("DELETE FROM media_tag_provenance WHERE media_id=? AND tag_id=?;");
        if (!deleteProvRes.isOk()) return deleteProvRes.status();
        auto deleteProv = std::move(deleteProvRes.value());
        deleteProv.bind(1, mediaId); deleteProv.bind(2, tagId);
        return execDone(conn, deleteProv, "Failed to clear face tag ownership");
    }
    return Status::ok();
}

Status assignExistingPersonTag(Connection& conn, int64_t faceId, int64_t revision,
                               TagId tagId, int64_t expectedDataVersion) {
    if (faceId <= 0 || revision <= 0 || tagId <= 0) {
        return Status::invalidArgument("face ID, revision, and tag_id must be positive");
    }
    if (!isPeopleTag(conn, tagId)) return Status::invalidArgument("tag_id must identify a people tag");

    db::Transaction tx(conn);
    auto getRes = conn.prepare("SELECT media_id,person_tag_id,revision,dismissed FROM faces WHERE id=?;");
    if (!getRes.isOk()) return getRes.status();
    auto get = std::move(getRes.value());
    get.bind(1, faceId);
    if (get.step() != StepResult::Row) return Status::notFound("Face not found");
    const MediaId mediaId = get.getInt64(0);
    const bool alreadyAssigned = !get.isNull(1);
    if (get.getInt64(2) != revision) {
        return Status::alreadyExists("Face review is stale; reload before changing its identity");
    }
    if (alreadyAssigned || get.getInt(3) != 0) {
        return Status::alreadyExists("Face is no longer an active unnamed suggestion");
    }

    auto stamp = readReviewStamp(conn);
    if (!stamp.isOk()) return stamp.status();
    if (stamp.value().dataVersion != expectedDataVersion) {
        return Status::alreadyExists("Face group snapshot is stale; reload the groups");
    }

    auto updateRes = conn.prepare(
        "UPDATE faces SET person_tag_id=?,revision=revision+1,updated_at=? WHERE id=? AND revision=? AND person_tag_id IS NULL AND dismissed=0;");
    if (!updateRes.isOk()) return updateRes.status();
    auto update = std::move(updateRes.value());
    update.bind(1, tagId);
    update.bind(2, nowSeconds());
    update.bind(3, faceId);
    update.bind(4, revision);
    if (update.step() != StepResult::Done) {
        const int code = conn.lastErrorCode() & 0xff;
        if (code == SQLITE_BUSY || code == SQLITE_LOCKED) {
            return Status::alreadyExists("Face group snapshot is stale; reload the groups");
        }
        return dbError(conn, "Failed to assign face identity");
    }
    if (conn.changes() != 1) {
        return Status::alreadyExists("Face review is stale; reload before changing its identity");
    }
    auto rejectRes = conn.prepare("DELETE FROM face_rejections WHERE face_id=? AND tag_id=?;");
    if (!rejectRes.isOk()) return rejectRes.status();
    auto reject = std::move(rejectRes.value());
    reject.bind(1, faceId);
    reject.bind(2, tagId);
    Status status = execDone(conn, reject, "Failed to clear identity rejection");
    if (!status.isOk()) return status;
    status = syncFaceTagOwnership(conn, mediaId, tagId);
    if (!status.isOk()) return status;
    return tx.commit();
}

void bindEmbedding(Statement& stmt, int index, const std::vector<float>& embedding) {
    sqlite3_stmt* raw = stmt.raw();
    if (embedding.empty()) {
        sqlite3_bind_null(raw, index);
    } else {
        sqlite3_bind_blob(raw, index, embedding.data(),
                          static_cast<int>(embedding.size() * sizeof(float)), SQLITE_TRANSIENT);
    }
}

json pointsJson(const std::array<Point, 5>& points) {
    json result = json::array();
    for (const auto& p : points) result.push_back({{"x", p.x}, {"y", p.y}});
    return result;
}

struct OrientedRgb {
    const thumbnail::ImageBuffer* source{nullptr};
    thumbnail::ImageBuffer rotated;

    const thumbnail::ImageBuffer& image() const {
        return source ? *source : rotated;
    }
};

OrientedRgb orientRgb(const thumbnail::ImageBuffer& src, int orientation) {
    if (orientation == 1) return {&src, {}};
    return {nullptr, thumbnail::Generator::rotate(src, orientation)};
}

struct EncodedSource {
    std::vector<uint8_t> bytes;
    std::string hash;
};

Result<EncodedSource> readEncodedSource(const std::string& path) {
    auto bytes = thumbnail::Generator::loadImageBytes(path);
    if (!bytes.isOk()) return bytes.status();
    auto hash = metadata::Hasher::computeBytesSha256(bytes.value().data(), bytes.value().size());
    if (hash.empty()) return Status::internal("Failed to compute SHA-256 digest");
    return EncodedSource{std::move(bytes.value()), std::move(hash)};
}

class SourcePrefetcher {
public:
    SourcePrefetcher() = default;
    SourcePrefetcher(const SourcePrefetcher&) = delete;
    SourcePrefetcher& operator=(const SourcePrefetcher&) = delete;

    ~SourcePrefetcher() { stop(); }

    std::optional<std::future<Result<EncodedSource>>> schedule(std::string path) {
        std::promise<Result<EncodedSource>> promise;
        auto future = promise.get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ || pending_ || busy_) return std::nullopt;
            if (!worker_.joinable()) {
                try {
                    worker_ = std::thread([this] { run(); });
                } catch (const std::system_error&) {
                    return std::nullopt;
                }
            }
            pending_.emplace(Task{std::move(path), std::move(promise)});
        }
        ready_.notify_one();
        return std::optional<std::future<Result<EncodedSource>>>(std::move(future));
    }

private:
    struct Task {
        std::string path;
        std::promise<Result<EncodedSource>> promise;
    };

    void run() {
        for (;;) {
            std::optional<Task> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [&] { return stopping_ || pending_.has_value(); });
                if (stopping_ && !pending_) return;
                task.emplace(std::move(*pending_));
                pending_.reset();
                busy_ = true;
            }

            std::optional<Result<EncodedSource>> result;
            std::exception_ptr failure;
            try {
                result.emplace(readEncodedSource(task->path));
            } catch (...) {
                failure = std::current_exception();
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                busy_ = false;
            }
            try {
                if (failure) task->promise.set_exception(failure);
                else task->promise.set_value(std::move(*result));
            } catch (...) {}
        }
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            pending_.reset();
        }
        ready_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::optional<Task> pending_;
    bool busy_{false};
    bool stopping_{false};
    std::thread worker_;
};

std::vector<float> checkedEmbedding(const Detection& d, std::string& error) {
    if (!d.embeddingError.empty()) { error = d.embeddingError; return {}; }
    if (d.embedding.size() != 512) { error = "Recognition model returned an invalid embedding size"; return {}; }
    double norm2 = 0;
    for (float value : d.embedding) {
        if (!std::isfinite(value)) { error = "Recognition model returned a non-finite embedding"; return {}; }
        norm2 += static_cast<double>(value) * value;
    }
    if (!std::isfinite(norm2) || norm2 <= 1e-20) {
        error = "Recognition model returned a zero-norm embedding";
        return {};
    }
    float norm = static_cast<float>(std::sqrt(norm2));
    std::vector<float> normalized = d.embedding;
    for (float& value : normalized) value /= norm;
    return normalized;
}

Status normalizeCandidateGeometry(std::vector<Detection>& detections, int width, int height) {
    std::vector<Detection> clipped;
    clipped.reserve(detections.size());
    for (auto detection : detections) {
        if (!std::isfinite(detection.x) || !std::isfinite(detection.y) ||
            !std::isfinite(detection.width) || !std::isfinite(detection.height) ||
            !std::isfinite(detection.score) || detection.width <= 0 || detection.height <= 0 ||
            detection.score < 0 || detection.score > 1) {
            return Status::internal("Face detector returned invalid geometry or score");
        }
        double right = detection.x + detection.width;
        double bottom = detection.y + detection.height;
        if (!std::isfinite(right) || !std::isfinite(bottom)) return Status::internal("Face detector returned invalid geometry");
        double left = std::clamp<double>(detection.x, 0, width);
        double top = std::clamp<double>(detection.y, 0, height);
        right = std::clamp<double>(right, 0, width);
        bottom = std::clamp<double>(bottom, 0, height);
        if (right <= left || bottom <= top) continue;
        detection.x = static_cast<float>(left);
        detection.y = static_cast<float>(top);
        detection.width = static_cast<float>(right - left);
        detection.height = static_cast<float>(bottom - top);
        for (auto& point : detection.landmarks) {
            if (!std::isfinite(point.x) || !std::isfinite(point.y)) return Status::internal("Face detector returned an invalid landmark");
            point.x = std::clamp(point.x, 0.0f, static_cast<float>(width));
            point.y = std::clamp(point.y, 0.0f, static_cast<float>(height));
        }
        clipped.push_back(std::move(detection));
    }
    detections = std::move(clipped);
    return Status::ok();
}

Status saveAnalysisFailure(Connection& conn, MediaId mediaId, const MediaItem& media,
                           const MediaItem& catalogSnapshot,
                           const RuntimeInfo& runtime, const Config& config,
                           const std::string& error) {
    auto currentRes = conn.prepare(
        "SELECT content_hash,width,height,orientation,media_type FROM media_items WHERE id=?;");
    if (!currentRes.isOk()) return currentRes.status();
    auto current = std::move(currentRes.value()); current.bind(1, mediaId);
    if (current.step() != StepResult::Row ||
        current.getString(0) != catalogSnapshot.content_hash ||
        current.getInt(1) != catalogSnapshot.width || current.getInt(2) != catalogSnapshot.height ||
        current.getInt(3) != catalogSnapshot.exif.orientation ||
        current.getString(4) != catalogSnapshot.media_type) {
        return Status::alreadyExists("Photo changed while face analysis was running");
    }
    // A failed refresh must not relabel or hide a previously committed success.
    // Retain its metadata, embeddings, and review state until a replacement
    // succeeds, even if this attempt observed another source/model version.
    auto priorRes = conn.prepare("SELECT state FROM face_media_analysis WHERE media_id=?;");
    if (!priorRes.isOk()) return priorRes.status();
    auto prior = std::move(priorRes.value());
    prior.bind(1, mediaId);
    const StepResult priorStep = prior.step();
    if (priorStep == StepResult::Error) return dbError(conn, "Failed to inspect previous face analysis");
    if (priorStep == StepResult::Row && prior.getString(0) == "complete") {
        return Status::ok();
    }
    if (priorStep == StepResult::Row) {
        // Retain any prior review rows and their model provenance on failure.
        // Only a successful replacement may publish new metadata/embeddings.
        auto stmtRes = conn.prepare(
            "UPDATE face_media_analysis SET state='failed',error=?,analyzed_at=? WHERE media_id=?;");
        if (!stmtRes.isOk()) return stmtRes.status();
        auto stmt = std::move(stmtRes.value());
        stmt.bind(1, error); stmt.bind(2, nowSeconds()); stmt.bind(3, mediaId);
        return execDone(conn, stmt, "Failed to persist face analysis error");
    }

    auto insertRes = conn.prepare(R"SQL(
        INSERT INTO face_media_analysis(media_id,state,error,source_hash,width,height,detector_checksum,
            recognizer_checksum,pipeline_version,detector_provider,recognizer_provider,device,runtime_version,
            fallback_reason,confidence,nms_threshold,analyzed_at)
        VALUES(?,'failed',?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);
    )SQL");
    if (!insertRes.isOk()) return insertRes.status();
    auto insert = std::move(insertRes.value());
    insert.bind(1, mediaId); insert.bind(2, error); insert.bind(3, media.content_hash);
    insert.bind(4, media.width); insert.bind(5, media.height); insert.bind(6, runtime.detectorChecksum);
    insert.bind(7, runtime.recognizerChecksum); insert.bind(8, runtime.pipelineVersion);
    insert.bind(9, runtime.detectorProvider); insert.bind(10, runtime.recognizerProvider);
    insert.bind(11, config.device); insert.bind(12, runtime.runtimeVersion);
    insert.bind(13, runtime.fallbackReason); insert.bind(14, config.confidence);
    insert.bind(15, config.nmsThreshold); insert.bind(16, nowSeconds());
    return execDone(conn, insert, "Failed to create face analysis error");
}

bool analysisReusable(Connection& conn, const MediaItem& media, const RuntimeInfo& runtime,
                      const Config& config) {
    auto stmtRes = conn.prepare(R"SQL(
        SELECT state,source_hash,detector_checksum,recognizer_checksum,pipeline_version,confidence,nms_threshold
        FROM face_media_analysis WHERE media_id=?;
    )SQL");
    if (!stmtRes.isOk()) return false;
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, media.id);
    return stmt.step() == StepResult::Row && stmt.getString(0) == "complete" &&
           stmt.getString(1) == media.content_hash && stmt.getString(2) == runtime.detectorChecksum &&
           stmt.getString(3) == runtime.recognizerChecksum && stmt.getString(4) == runtime.pipelineVersion &&
           std::abs(stmt.getDouble(5) - config.confidence) < 1e-6 &&
           std::abs(stmt.getDouble(6) - config.nmsThreshold) < 1e-6;
}

bool recognizerRefreshNeeded(Connection& conn, const MediaItem& media, const RuntimeInfo& runtime,
                             const Config& config) {
    auto stmtRes = conn.prepare(R"SQL(
        SELECT state,source_hash,detector_checksum,recognizer_checksum,pipeline_version,confidence,nms_threshold
        FROM face_media_analysis WHERE media_id=?;
    )SQL");
    if (!stmtRes.isOk()) return false;
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, media.id);
    return stmt.step() == StepResult::Row && stmt.getString(0) == "complete" &&
           stmt.getString(1) == media.content_hash && stmt.getString(2) == runtime.detectorChecksum &&
           stmt.getString(3) != runtime.recognizerChecksum && stmt.getString(4) == runtime.pipelineVersion &&
           std::abs(stmt.getDouble(5) - config.confidence) < 1e-6 &&
           std::abs(stmt.getDouble(6) - config.nmsThreshold) < 1e-6;
}

bool detectorRefreshRequiresForce(Connection& conn, const MediaItem& media, const RuntimeInfo& runtime,
                                  const Config& config) {
    auto stmtRes = conn.prepare(R"SQL(
        SELECT state,source_hash,detector_checksum,pipeline_version,confidence,nms_threshold
        FROM face_media_analysis WHERE media_id=?;
    )SQL");
    if (!stmtRes.isOk()) return false;
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, media.id);
    if (stmt.step() != StepResult::Row || stmt.getString(0) != "complete" ||
        stmt.getString(1) != media.content_hash) return false;
    const bool detectorCompatible = stmt.getString(2) == runtime.detectorChecksum &&
        stmt.getString(3) == runtime.pipelineVersion &&
        std::abs(stmt.getDouble(4) - config.confidence) < 1e-6 &&
        std::abs(stmt.getDouble(5) - config.nmsThreshold) < 1e-6;
    return !detectorCompatible;
}

Status persistAnalysis(Connection& conn, MediaId mediaId, const MediaItem& media,
                       const MediaItem& catalogSnapshot,
                       const RuntimeInfo& runtime, const Config& config,
                       const std::vector<Detection>& detections, bool preserveIdentities) {
    for (const auto& detection : detections) {
        if (!std::isfinite(detection.x) || !std::isfinite(detection.y) ||
            !std::isfinite(detection.width) || !std::isfinite(detection.height) ||
            !std::isfinite(detection.score) || detection.width <= 0 || detection.height <= 0 ||
            detection.score < 0 || detection.score > 1 || detection.x < 0 || detection.y < 0 ||
            detection.x + detection.width > media.width + 1e-3 ||
            detection.y + detection.height > media.height + 1e-3) {
            return Status::internal("Face detector returned invalid geometry or score");
        }
        for (const auto& point : detection.landmarks) {
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0 || point.y < 0 ||
                point.x > media.width || point.y > media.height) {
                return Status::internal("Face detector returned an invalid landmark");
            }
        }
    }
    auto currentMediaRes = conn.prepare(
        "SELECT content_hash,width,height,orientation,media_type FROM media_items WHERE id=?;");
    if (!currentMediaRes.isOk()) return currentMediaRes.status();
    auto currentMedia = std::move(currentMediaRes.value());
    currentMedia.bind(1, mediaId);
    if (currentMedia.step() != StepResult::Row ||
        currentMedia.getString(0) != catalogSnapshot.content_hash ||
        currentMedia.getInt(1) != catalogSnapshot.width ||
        currentMedia.getInt(2) != catalogSnapshot.height ||
        currentMedia.getInt(3) != catalogSnapshot.exif.orientation ||
        currentMedia.getString(4) != catalogSnapshot.media_type) {
        return Status::alreadyExists("Photo changed while face analysis was running");
    }

    db::Transaction tx(conn);
    if (preserveIdentities) {
        auto oldFacesRes = conn.prepare("SELECT id,x,y,width,height FROM faces WHERE media_id=?;");
        if (!oldFacesRes.isOk()) return oldFacesRes.status();
        auto oldFaces = std::move(oldFacesRes.value());
        oldFaces.bind(1, mediaId);
        struct OldFace { int64_t id; double x,y,w,h; bool used{false}; };
        std::vector<OldFace> previous;
        while (oldFaces.step() == StepResult::Row) previous.push_back({oldFaces.getInt64(0),oldFaces.getDouble(1),oldFaces.getDouble(2),oldFaces.getDouble(3),oldFaces.getDouble(4),false});
        for (const auto& detection : detections) {
            double bestIou = 0;
            OldFace* match = nullptr;
            for (auto& old : previous) {
                if (old.used) continue;
                double left = std::max<double>(detection.x, old.x);
                double top = std::max<double>(detection.y, old.y);
                double right = std::min<double>(detection.x + detection.width, old.x + old.w);
                double bottom = std::min<double>(detection.y + detection.height, old.y + old.h);
                double inter = std::max(0.0, right-left) * std::max(0.0, bottom-top);
                double uni = detection.width*detection.height + old.w*old.h - inter;
                double iou = uni > 0 ? inter/uni : 0;
                if (iou > bestIou) { bestIou = iou; match = &old; }
            }
            std::string embedError;
            auto embedding = checkedEmbedding(detection, embedError);
            if (match && bestIou >= 0.5) {
                match->used = true;
                auto upRes = conn.prepare("UPDATE faces SET embedding=?,embedding_size=?,embedding_error=?,updated_at=? WHERE id=?;");
                if (!upRes.isOk()) return upRes.status();
                auto up = std::move(upRes.value());
                bindEmbedding(up, 1, embedding); up.bind(2, static_cast<int32_t>(embedding.size()));
                up.bind(3, embedError); up.bind(4, nowSeconds()); up.bind(5, match->id);
                Status s = execDone(conn, up, "Failed to refresh face embedding");
                if (!s.isOk()) return s;
            } else {
                auto insRes = conn.prepare(R"SQL(
                    INSERT INTO faces(media_id,x,y,width,height,score,landmarks,embedding,embedding_size,embedding_error,created_at,updated_at)
                    VALUES(?,?,?,?,?,?,?,?,?,?,?,?);
                )SQL");
                if (!insRes.isOk()) return insRes.status();
                auto ins = std::move(insRes.value());
                ins.bind(1, mediaId); ins.bind(2, detection.x); ins.bind(3, detection.y);
                ins.bind(4, detection.width); ins.bind(5, detection.height); ins.bind(6, detection.score);
                ins.bind(7, pointsJson(detection.landmarks).dump()); bindEmbedding(ins, 8, embedding);
                ins.bind(9, static_cast<int32_t>(embedding.size())); ins.bind(10, embedError);
                ins.bind(11, nowSeconds()); ins.bind(12, nowSeconds());
                Status s = execDone(conn, ins, "Failed to insert refreshed face");
                if (!s.isOk()) return s;
            }
        }
        for (const auto& old : previous) {
            if (old.used) continue;
            auto clearRes = conn.prepare(
                "UPDATE faces SET embedding=NULL,embedding_size=0,embedding_error=?,updated_at=? WHERE id=?;");
            if (!clearRes.isOk()) return clearRes.status();
            auto clear = std::move(clearRes.value());
            clear.bind(1, "No compatible refreshed embedding was produced");
            clear.bind(2, nowSeconds()); clear.bind(3, old.id);
            Status s = execDone(conn, clear, "Failed to invalidate unmatched old face embedding");
            if (!s.isOk()) return s;
        }
    } else {
        auto oldTagsRes = conn.prepare("SELECT DISTINCT person_tag_id FROM faces WHERE media_id=? AND person_tag_id IS NOT NULL;");
        if (!oldTagsRes.isOk()) return oldTagsRes.status();
        auto oldTagsStmt = std::move(oldTagsRes.value());
        oldTagsStmt.bind(1, mediaId);
        std::vector<TagId> oldTags;
        while (oldTagsStmt.step() == StepResult::Row) oldTags.push_back(oldTagsStmt.getInt64(0));
        auto delRes = conn.prepare("DELETE FROM faces WHERE media_id=?;");
        if (!delRes.isOk()) return delRes.status();
        auto del = std::move(delRes.value()); del.bind(1, mediaId);
        Status s = execDone(conn, del, "Failed to replace old faces");
        if (!s.isOk()) return s;
        for (TagId tagId : oldTags) {
            s = syncFaceTagOwnership(conn, mediaId, tagId);
            if (!s.isOk()) return s;
        }
        for (const auto& detection : detections) {
            std::string embedError;
            auto embedding = checkedEmbedding(detection, embedError);
            auto insRes = conn.prepare(R"SQL(
                INSERT INTO faces(media_id,x,y,width,height,score,landmarks,embedding,embedding_size,embedding_error,created_at,updated_at)
                VALUES(?,?,?,?,?,?,?,?,?,?,?,?);
            )SQL");
            if (!insRes.isOk()) return insRes.status();
            auto ins = std::move(insRes.value());
            ins.bind(1, mediaId); ins.bind(2, detection.x); ins.bind(3, detection.y);
            ins.bind(4, detection.width); ins.bind(5, detection.height); ins.bind(6, detection.score);
            ins.bind(7, pointsJson(detection.landmarks).dump()); bindEmbedding(ins, 8, embedding);
            ins.bind(9, static_cast<int32_t>(embedding.size())); ins.bind(10, embedError);
            ins.bind(11, nowSeconds()); ins.bind(12, nowSeconds());
            s = execDone(conn, ins, "Failed to persist detected face");
            if (!s.isOk()) return s;
        }
    }

    auto analysisRes = conn.prepare(R"SQL(
        INSERT INTO face_media_analysis(media_id,state,error,source_hash,width,height,detector_checksum,
            recognizer_checksum,pipeline_version,detector_provider,recognizer_provider,device,runtime_version,
            fallback_reason,confidence,nms_threshold,analyzed_at)
        VALUES(?, 'complete','', ?,?,?,?,?,?,?,?,?,?,?,?,?,?)
        ON CONFLICT(media_id) DO UPDATE SET state='complete',error='',source_hash=excluded.source_hash,
            width=excluded.width,height=excluded.height,detector_checksum=excluded.detector_checksum,
            recognizer_checksum=excluded.recognizer_checksum,pipeline_version=excluded.pipeline_version,
            detector_provider=excluded.detector_provider,recognizer_provider=excluded.recognizer_provider,
            device=excluded.device,runtime_version=excluded.runtime_version,fallback_reason=excluded.fallback_reason,
            confidence=excluded.confidence,nms_threshold=excluded.nms_threshold,
            analyzed_at=excluded.analyzed_at;
    )SQL");
    if (!analysisRes.isOk()) return analysisRes.status();
    auto analysis = std::move(analysisRes.value());
    analysis.bind(1, mediaId); analysis.bind(2, media.content_hash); analysis.bind(3, media.width); analysis.bind(4, media.height);
    analysis.bind(5, runtime.detectorChecksum); analysis.bind(6, runtime.recognizerChecksum);
    analysis.bind(7, runtime.pipelineVersion); analysis.bind(8, runtime.detectorProvider);
    analysis.bind(9, runtime.recognizerProvider); analysis.bind(10, config.device);
    analysis.bind(11, runtime.runtimeVersion); analysis.bind(12, runtime.fallbackReason);
    analysis.bind(13, config.confidence); analysis.bind(14, config.nmsThreshold); analysis.bind(15, nowSeconds());
    Status s = execDone(conn, analysis, "Failed to save face analysis metadata");
    if (!s.isOk()) return s;
    return tx.commit();
}

struct GroupReviewFace {
    int64_t id{0};
    MediaId mediaId{0};
    int64_t revision{0};
    std::optional<TagId> personTagId;
    std::string personName;
    bool dismissed{false};
    bool queryEligible{false};
    std::vector<float> embedding;
    std::string recognizerChecksum;
    std::string pipelineVersion;
    std::unordered_set<TagId> rejectedTags;
    struct RankedPerson {
        Suggestion suggestion;
        int64_t bestExemplarId{0};
        int64_t secondExemplarId{0};
        float secondScore{0};
        bool hasSecond{false};
        bool secondExact{true};
    };
    std::vector<RankedPerson> ranked;
    std::optional<RankedPerson> omittedUpperBound;
    bool rankingComplete{true};
    bool frontierExact{true};
    bool omittedBoundaryTie{false};
    std::vector<Suggestion> suggestions;
};

struct ExemplarDescriptor {
    TagId tagId{0};
    std::string name;
    std::array<unsigned char, SHA256_DIGEST_LENGTH> embeddingHash{};
};

struct GroupReviewIndex {
    std::optional<int64_t> jobId;
    ReviewStamp stamp;
    std::vector<GroupReviewFace> faces;
    std::unordered_map<int64_t, size_t> facePositions;
    std::unordered_map<std::string, std::unordered_map<int64_t, ExemplarDescriptor>> exemplarDescriptors;
};

struct PendingGroupQuery {
    size_t faceIndex{0};
    std::vector<float> embedding;
    std::string recognizerChecksum;
    std::string pipelineVersion;
};

struct GroupReviewBuild {
    std::shared_ptr<GroupReviewIndex> index;
    ExemplarCache exemplars;
    RejectionCache rejections;
    std::vector<PendingGroupQuery> queries;
};

using RankedPerson = GroupReviewFace::RankedPerson;

std::array<unsigned char, SHA256_DIGEST_LENGTH> embeddingDigest(const std::vector<float>& embedding) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    const auto* bytes = reinterpret_cast<const unsigned char*>(embedding.data());
    SHA256(bytes, embedding.size() * sizeof(float), digest.data());
    return digest;
}

bool rankedBefore(const RankedPerson& left, const RankedPerson& right) {
    if (left.suggestion.score != right.suggestion.score) {
        return left.suggestion.score > right.suggestion.score;
    }
    return left.suggestion.name < right.suggestion.name;
}

bool ranksBeforeBoundary(const RankedPerson& candidate, const RankedPerson& boundary) {
    if (candidate.suggestion.score != boundary.suggestion.score) {
        return candidate.suggestion.score > boundary.suggestion.score;
    }
    return candidate.suggestion.name <= boundary.suggestion.name;
}

void applyRankedSuggestions(GroupReviewFace& face) {
    face.suggestions.clear();
    const size_t count = std::min<size_t>(3, face.ranked.size());
    face.suggestions.reserve(count);
    for (size_t i = 0; i < count; ++i) face.suggestions.push_back(face.ranked[i].suggestion);
}

std::vector<RankedPerson> rankExemplars(
    int64_t faceId, const std::vector<float>& query,
    const std::vector<Exemplar>& exemplars, float threshold,
    const std::unordered_set<TagId>& excludedTagIds,
    bool& rankingComplete, bool& omittedBoundaryTie,
    std::optional<RankedPerson>& omittedUpperBound,
    const float* packedEmbeddings = nullptr) {
    std::unordered_map<TagId, RankedPerson> best;
    if (query.size() != kEmbeddingDimensions) {
        rankingComplete = true;
        omittedBoundaryTie = false;
        omittedUpperBound.reset();
        return {};
    }
    const size_t numExemplars = exemplars.size();
    const bool hasExclusions = !excludedTagIds.empty();
    const float* queryData = query.data();

    for (size_t i = 0; i < numExemplars; ++i) {
        const auto& exemplar = exemplars[i];
        if (exemplar.faceId == faceId || (hasExclusions && excludedTagIds.contains(exemplar.tagId)) ||
            exemplar.embedding.size() != kEmbeddingDimensions) continue;
        const float* exemplarData = packedEmbeddings ? (packedEmbeddings + i * kEmbeddingDimensions) : exemplar.embedding.data();
        float score = dotProduct512(queryData, exemplarData);
        if (!std::isfinite(score) || score < threshold) continue;
        score = std::clamp(score, -1.0f, 1.0f);
        auto found = best.find(exemplar.tagId);
        RankedPerson candidate;
        candidate.suggestion = Suggestion{exemplar.tagId, exemplar.name, score};
        candidate.bestExemplarId = exemplar.faceId;
        if (found == best.end()) {
            best.emplace(exemplar.tagId, std::move(candidate));
            continue;
        }
        auto& current = found->second;
        const bool better = score > current.suggestion.score ||
            (score == current.suggestion.score && exemplar.faceId < current.bestExemplarId);
        if (better) {
            candidate.secondExemplarId = current.bestExemplarId;
            candidate.secondScore = current.suggestion.score;
            candidate.hasSecond = true;
            if (current.hasSecond && (current.secondScore > candidate.secondScore ||
                (current.secondScore == candidate.secondScore && current.secondExemplarId < candidate.secondExemplarId))) {
                candidate.secondExemplarId = current.secondExemplarId;
                candidate.secondScore = current.secondScore;
            }
            current = std::move(candidate);
        } else {
            const bool betterSecond = !current.hasSecond || score > current.secondScore ||
                (score == current.secondScore && exemplar.faceId < current.secondExemplarId);
            if (betterSecond) {
                current.secondExemplarId = exemplar.faceId;
                current.secondScore = score;
                current.hasSecond = true;
            }
        }
    }

    std::vector<RankedPerson> all;
    all.reserve(best.size());
    for (auto& [tagId, person] : best) all.push_back(std::move(person));
    std::sort(all.begin(), all.end(), rankedBefore);
    rankingComplete = all.size() <= 4;
    omittedBoundaryTie = all.size() > 4 &&
        all[4].suggestion.score == all[3].suggestion.score;
    omittedUpperBound = all.size() > 4 ? std::optional<RankedPerson>(all[3]) : std::nullopt;
    if (all.size() > 4) all.resize(4);
    return all;
}

void exactRankFace(GroupReviewFace& face, const std::vector<Exemplar>& exemplars,
                   float threshold, const float* packedEmbeddings = nullptr) {
    face.ranked = rankExemplars(face.id, face.embedding, exemplars, threshold,
                                face.rejectedTags, face.rankingComplete,
                                face.omittedBoundaryTie, face.omittedUpperBound,
                                packedEmbeddings);
    applyRankedSuggestions(face);
}

struct GroupReviewGroup {
    std::string key;
    std::string type;
    std::optional<TagId> tagId;
    std::string name;
    int64_t facesCount{0};
    int64_t photosCount{0};
    int64_t suggestionFacesCount{0};
    int64_t suggestionPhotosCount{0};
    std::vector<size_t> members;
    std::vector<size_t> suggestionMembers;
};

struct GroupReviewSnapshot {
    std::string token;
    std::shared_ptr<GroupReviewIndex> index;
    std::optional<int64_t> jobId;
    bool showNamed{true};
    bool showUnnamed{true};
    bool includeDismissed{false};
    std::optional<TagId> personTagId;
    std::vector<GroupReviewGroup> groups;
    std::chrono::steady_clock::time_point createdAt;
};

std::string exemplarKey(const std::string& checksum, const std::string& pipeline) {
    return checksum + "\x1f" + pipeline;
}

Result<GroupReviewBuild> buildGroupReviewIndex(
    Connection& conn, std::optional<int64_t> jobId, const ReviewStamp& stamp) {
    GroupReviewBuild built;
    built.index = std::make_shared<GroupReviewIndex>();
    auto& index = built.index;
    index->jobId = jobId;
    index->stamp = stamp;

    const std::string jobJoin = jobId
        ? " JOIN face_analysis_job_media jm ON jm.media_id=f.media_id AND jm.job_id=? "
        : " ";
    const std::string analysisJoin = " FROM faces f JOIN face_media_analysis a ON a.media_id=f.media_id "
        "LEFT JOIN tags t ON t.id=f.person_tag_id " + jobJoin;
    const std::string completeScope = " WHERE a.state='complete' ";

    std::unordered_set<std::string> exemplarKeys;
    auto keyRes = conn.prepare(
        "SELECT DISTINCT a.recognizer_checksum,a.pipeline_version" + analysisJoin +
        completeScope + " AND f.person_tag_id IS NULL AND f.dismissed=0 AND f.embedding_error='' "
        "AND f.embedding IS NOT NULL AND f.embedding_size=512 AND a.recognizer_checksum<>'' AND a.pipeline_version<>'';");
    if (!keyRes.isOk()) return keyRes.status();
    auto keys = std::move(keyRes.value());
    if (jobId) keys.bind(1, *jobId);
    StepResult keyStep;
    while ((keyStep = keys.step()) == StepResult::Row) {
        const std::string checksum = keys.getString(0);
        const std::string pipeline = keys.getString(1);
        exemplarKeys.insert(exemplarKey(checksum, pipeline));
    }
    if (keyStep == StepResult::Error) return dbError(conn, "Failed to enumerate face-recognition pipelines");

    for (const auto& key : exemplarKeys) {
        const size_t separator = key.find('\x1f');
        if (separator == std::string::npos) continue;
        const std::string checksum = key.substr(0, separator);
        const std::string pipeline = key.substr(separator + 1);
        built.exemplars.emplace(key, loadExemplars(conn, checksum, pipeline));
    }

    auto rejectionRes = conn.prepare(
        "SELECT r.face_id,r.tag_id FROM face_rejections r JOIN faces f ON f.id=r.face_id "
        "JOIN face_media_analysis a ON a.media_id=f.media_id " + jobJoin +
        "WHERE a.state='complete' AND f.person_tag_id IS NULL AND f.dismissed=0;");
    if (!rejectionRes.isOk()) return rejectionRes.status();
    auto rejectionStmt = std::move(rejectionRes.value());
    if (jobId) rejectionStmt.bind(1, *jobId);
    StepResult rejectionStep;
    while ((rejectionStep = rejectionStmt.step()) == StepResult::Row) {
        built.rejections[rejectionStmt.getInt64(0)].insert(rejectionStmt.getInt64(1));
    }
    if (rejectionStep == StepResult::Error) return dbError(conn, "Failed to load face suggestion rejections");

    auto countRes = conn.prepare("SELECT count(*)" + analysisJoin + completeScope + ";");
    if (countRes.isOk()) {
        auto countStmt = std::move(countRes.value());
        if (jobId) countStmt.bind(1, *jobId);
        if (countStmt.step() == StepResult::Row) {
            int64_t count = countStmt.getInt64(0);
            if (count > 0) {
                index->faces.reserve(static_cast<size_t>(count));
                index->facePositions.reserve(static_cast<size_t>(count));
            }
        }
    }

    auto facesRes = conn.prepare(
        "SELECT f.id,f.media_id,f.revision,f.person_tag_id,t.name,f.dismissed,f.embedding_error,"
        "CASE WHEN f.person_tag_id IS NULL AND f.dismissed=0 AND f.embedding_error='' AND f.embedding_size=512 "
        "THEN f.embedding ELSE NULL END,f.embedding_size,a.recognizer_checksum,a.pipeline_version " +
        analysisJoin + completeScope + " ORDER BY f.media_id,f.id;");
    if (!facesRes.isOk()) return facesRes.status();
    auto faces = std::move(facesRes.value());
    if (jobId) faces.bind(1, *jobId);
    StepResult faceStep;
    while ((faceStep = faces.step()) == StepResult::Row) {
        GroupReviewFace indexed;
        indexed.id = faces.getInt64(0);
        indexed.mediaId = faces.getInt64(1);
        indexed.revision = faces.getInt64(2);
        if (!faces.isNull(3)) indexed.personTagId = faces.getInt64(3);
        if (!faces.isNull(4)) indexed.personName = faces.getString(4);
        indexed.dismissed = faces.getInt(5) != 0;
        const size_t faceIndex = index->faces.size();
        const bool queryEligible = !indexed.personTagId && !indexed.dismissed && faces.getString(6).empty() &&
            !faces.isNull(7) && faces.getInt(8) == 512;
        if (queryEligible) {
            auto embedding = readVector(faces, 7, 512);
            if (embedding.size() == 512) {
                indexed.queryEligible = true;
                indexed.embedding = std::move(embedding);
                indexed.recognizerChecksum = faces.getString(9);
                indexed.pipelineVersion = faces.getString(10);
            }
        }
        index->facePositions.emplace(indexed.id, faceIndex);
        index->faces.push_back(std::move(indexed));
    }
    if (faceStep == StepResult::Error) return dbError(conn, "Failed to build face review index");
    return built;
}

std::unordered_map<std::string, std::unordered_map<int64_t, ExemplarDescriptor>>
describeExemplars(const ExemplarCache& exemplars) {
    std::unordered_map<std::string, std::unordered_map<int64_t, ExemplarDescriptor>> described;
    for (const auto& [key, rows] : exemplars) {
        auto& output = described[key];
        output.reserve(rows.size());
        if (rows.size() < 128) {
            for (const auto& exemplar : rows) {
                output.emplace(exemplar.faceId, ExemplarDescriptor{
                    exemplar.tagId, exemplar.name, embeddingDigest(exemplar.embedding)});
            }
        } else {
            std::vector<std::array<unsigned char, SHA256_DIGEST_LENGTH>> digests(rows.size());
            unsigned int hw = std::thread::hardware_concurrency();
            if (hw == 0) hw = 4;
            const size_t numThreads = std::min<size_t>(hw, (rows.size() + 127) / 128);
            const size_t chunkSize = (rows.size() + numThreads - 1) / numThreads;
            std::vector<std::jthread> threads;
            threads.reserve(numThreads - 1);
            for (size_t t = 1; t < numThreads; ++t) {
                const size_t start = t * chunkSize;
                const size_t end = std::min(start + chunkSize, rows.size());
                if (start < end) {
                    threads.emplace_back([&rows, &digests, start, end]() {
                        for (size_t i = start; i < end; ++i) {
                            digests[i] = embeddingDigest(rows[i].embedding);
                        }
                    });
                }
            }
            const size_t chunk0End = std::min(chunkSize, rows.size());
            for (size_t i = 0; i < chunk0End; ++i) {
                digests[i] = embeddingDigest(rows[i].embedding);
            }
            threads.clear();
            for (size_t i = 0; i < rows.size(); ++i) {
                output.emplace(rows[i].faceId, ExemplarDescriptor{
                    rows[i].tagId, rows[i].name, digests[i]});
            }
        }
    }
    return described;
}

struct ExemplarChanges {
    std::vector<std::pair<int64_t, TagId>> removed;
    std::unordered_map<TagId, std::vector<const Exemplar*>> addedByTag;
    std::unordered_map<TagId, std::string> currentNames;
    std::unordered_set<TagId> renamedTags;
    std::unordered_set<TagId> newlyIntroducedTags;
    bool anyNameChanged{false};
};

ExemplarChanges diffExemplars(
    const GroupReviewIndex* previous, const GroupReviewIndex& current,
    const ExemplarCache& exemplars, const std::string& key) {
    ExemplarChanges changes;
    auto vectors = exemplars.find(key);
    auto descriptors = current.exemplarDescriptors.find(key);
    if (descriptors != current.exemplarDescriptors.end()) {
        for (const auto& [id, descriptor] : descriptors->second) {
            changes.currentNames[descriptor.tagId] = descriptor.name;
        }
    }
    const std::unordered_map<int64_t, ExemplarDescriptor>* old = nullptr;
    if (previous) {
        auto found = previous->exemplarDescriptors.find(key);
        if (found != previous->exemplarDescriptors.end()) old = &found->second;
    }
    auto currentDescriptors = descriptors == current.exemplarDescriptors.end()
        ? nullptr : &descriptors->second;
    std::unordered_map<int64_t, const Exemplar*> currentVectors;
    if (vectors != exemplars.end()) {
        currentVectors.reserve(vectors->second.size());
        for (const auto& exemplar : vectors->second) currentVectors.emplace(exemplar.faceId, &exemplar);
    }

    if (old) {
        for (const auto& [id, prior] : *old) {
            auto now = currentDescriptors ? currentDescriptors->find(id) :
                std::unordered_map<int64_t, ExemplarDescriptor>::const_iterator{};
            const bool exists = currentDescriptors && now != currentDescriptors->end();
            if (!exists || now->second.tagId != prior.tagId ||
                now->second.embeddingHash != prior.embeddingHash) {
                changes.removed.emplace_back(id, prior.tagId);
            }
            if (exists && now->second.name != prior.name) {
                changes.anyNameChanged = true;
                changes.renamedTags.insert(prior.tagId);
                changes.renamedTags.insert(now->second.tagId);
            }
        }
    }
    if (currentDescriptors) {
        std::unordered_set<TagId> oldTags;
        if (old) {
            oldTags.reserve(old->size());
            for (const auto& [id, descriptor] : *old) oldTags.insert(descriptor.tagId);
        }
        for (const auto& [id, descriptor] : *currentDescriptors) {
            if (!oldTags.contains(descriptor.tagId)) changes.newlyIntroducedTags.insert(descriptor.tagId);
        }
        for (const auto& [id, descriptor] : *currentDescriptors) {
            auto prior = old ? old->find(id) :
                std::unordered_map<int64_t, ExemplarDescriptor>::const_iterator{};
            const bool existed = old && prior != old->end();
            if (!existed || prior->second.tagId != descriptor.tagId ||
                prior->second.embeddingHash != descriptor.embeddingHash) {
                auto vector = currentVectors.find(id);
                if (vector != currentVectors.end()) changes.addedByTag[descriptor.tagId].push_back(vector->second);
            }
        }
    }
    return changes;
}

float exemplarScore(const std::vector<float>& query, const Exemplar& exemplar) {
    if (query.size() != kEmbeddingDimensions || exemplar.embedding.size() != kEmbeddingDimensions) {
        return -std::numeric_limits<float>::infinity();
    }
    float score = dotProduct512(query.data(), exemplar.embedding.data());
    return std::isfinite(score) ? score : -std::numeric_limits<float>::infinity();
}

bool incrementallyUpdateFace(GroupReviewFace& face, const ExemplarChanges& changes,
                             const std::vector<Exemplar>& exemplars, float threshold) {
    const std::vector<RankedPerson> oldRanked = face.ranked;
    const bool oldComplete = face.rankingComplete;
    const bool oldBoundaryTie = face.omittedBoundaryTie;
    if (!oldComplete && changes.anyNameChanged) return false;
    std::optional<RankedPerson> oldBoundary;
    if (!oldComplete) oldBoundary = face.omittedUpperBound;

    std::unordered_map<TagId, RankedPerson> current;
    for (const auto& person : oldRanked) current.emplace(person.suggestion.tagId, person);
    std::unordered_map<TagId, std::unordered_set<int64_t>> removedByTag;
    for (const auto& [faceId, tagId] : changes.removed) removedByTag[tagId].insert(faceId);

    for (const auto& [tagId, removedIds] : removedByTag) {
        auto found = current.find(tagId);
        if (found == current.end() || face.rejectedTags.contains(tagId)) continue;
        auto& person = found->second;
        const bool removeBest = removedIds.contains(person.bestExemplarId);
        const bool removeSecond = person.hasSecond && removedIds.contains(person.secondExemplarId);
        if (removeBest) {
            if (person.hasSecond && !removeSecond && person.secondExact) {
                person.bestExemplarId = person.secondExemplarId;
                person.suggestion.score = person.secondScore;
                person.hasSecond = false;
                person.secondExact = false;
            } else if (!person.hasSecond && person.secondExact) {
                current.erase(found);
                continue;
            } else {
                const bool tagStillExists = std::any_of(exemplars.begin(), exemplars.end(),
                    [&](const Exemplar& item) { return item.tagId == tagId; });
                if (tagStillExists) return false;
                current.erase(found);
                continue;
            }
        } else if (removeSecond) {
            person.hasSecond = false;
            person.secondExact = false;
        }
    }

    for (const auto& [tagId, additions] : changes.addedByTag) {
        if (face.rejectedTags.contains(tagId)) continue;
        auto found = current.find(tagId);
        if (found == current.end()) {
            RankedPerson added;
            added.suggestion.tagId = tagId;
            auto name = changes.currentNames.find(tagId);
            if (name != changes.currentNames.end()) added.suggestion.name = name->second;
            std::vector<std::pair<int64_t, float>> scored;
            for (const auto* exemplar : additions) {
                if (exemplar->faceId == face.id) continue;
                const float rawScore = exemplarScore(face.embedding, *exemplar);
                if (rawScore >= threshold) scored.emplace_back(exemplar->faceId, std::clamp(rawScore, -1.0f, 1.0f));
            }
            std::sort(scored.begin(), scored.end(), [](const auto& left, const auto& right) {
                return left.second != right.second ? left.second > right.second : left.first < right.first;
            });
            if (scored.empty()) continue;
            added.suggestion.score = scored[0].second;
            added.bestExemplarId = scored[0].first;
            if (scored.size() > 1) {
                added.hasSecond = true;
                added.secondExemplarId = scored[1].first;
                added.secondScore = scored[1].second;
            }
            if (!oldComplete && !changes.newlyIntroducedTags.contains(tagId)) {
                if (!oldBoundary) return false;
                // Earlier examples for an omitted person are below the old
                // frontier. If this new score stays below it, the person stays
                // omitted as well. Otherwise compute an exact face ranking.
                if (ranksBeforeBoundary(added, *oldBoundary)) return false;
                continue;
            }
            if (!oldComplete && oldBoundary && !ranksBeforeBoundary(added, *oldBoundary)) continue;
            current.emplace(tagId, std::move(added));
            continue;
        }

        auto& person = found->second;
        if (!person.secondExact) {
            std::vector<std::pair<int64_t, float>> addedScores;
            for (const auto* exemplar : additions) {
                if (exemplar->faceId == face.id) continue;
                const float rawScore = exemplarScore(face.embedding, *exemplar);
                if (rawScore >= threshold) {
                    addedScores.emplace_back(exemplar->faceId, std::clamp(rawScore, -1.0f, 1.0f));
                }
            }
            std::sort(addedScores.begin(), addedScores.end(), [](const auto& left, const auto& right) {
                return left.second != right.second ? left.second > right.second : left.first < right.first;
            });
            if (!addedScores.empty()) {
                const auto oldBest = std::make_pair(person.bestExemplarId, person.suggestion.score);
                const auto addedIsBetter = [&](const auto& candidate, const auto& other) {
                    return candidate.second > other.second ||
                        (candidate.second == other.second && candidate.first < other.first);
                };
                if (addedIsBetter(addedScores.front(), oldBest)) {
                    person.bestExemplarId = addedScores.front().first;
                    person.suggestion.score = addedScores.front().second;
                    if (addedScores.size() > 1 && addedIsBetter(addedScores[1], oldBest)) {
                        person.hasSecond = true;
                        person.secondExemplarId = addedScores[1].first;
                        person.secondScore = addedScores[1].second;
                    } else {
                        person.hasSecond = true;
                        person.secondExemplarId = oldBest.first;
                        person.secondScore = oldBest.second;
                    }
                    person.secondExact = true;
                }
            }
            continue;
        }
        std::vector<std::pair<int64_t, float>> scored;
        scored.emplace_back(person.bestExemplarId, person.suggestion.score);
        if (person.hasSecond) scored.emplace_back(person.secondExemplarId, person.secondScore);
        for (const auto* exemplar : additions) {
            if (exemplar->faceId == face.id) continue;
            const float rawScore = exemplarScore(face.embedding, *exemplar);
            if (rawScore >= threshold) scored.emplace_back(exemplar->faceId, std::clamp(rawScore, -1.0f, 1.0f));
        }
        std::sort(scored.begin(), scored.end(), [](const auto& left, const auto& right) {
            return left.second != right.second ? left.second > right.second : left.first < right.first;
        });
        scored.erase(std::unique(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
            return a.first == b.first;
        }), scored.end());
        if (scored.empty()) return false;
        person.bestExemplarId = scored[0].first;
        person.suggestion.score = scored[0].second;
        person.hasSecond = scored.size() > 1;
        person.secondExact = true;
        if (person.hasSecond) {
            person.secondExemplarId = scored[1].first;
            person.secondScore = scored[1].second;
        }
        auto name = changes.currentNames.find(tagId);
        if (name != changes.currentNames.end()) person.suggestion.name = name->second;
    }

    for (const auto& [tagId, name] : changes.currentNames) {
        auto found = current.find(tagId);
        if (found != current.end()) found->second.suggestion.name = name;
    }
    if (changes.anyNameChanged && oldBoundaryTie) return false;

    std::vector<RankedPerson> ranked;
    ranked.reserve(current.size());
    for (auto& [tagId, person] : current) ranked.push_back(std::move(person));
    std::sort(ranked.begin(), ranked.end(), rankedBefore);
    if (oldComplete) {
        face.rankingComplete = ranked.size() <= 4;
        face.frontierExact = true;
        face.omittedBoundaryTie = ranked.size() > 4 && ranked[4].suggestion.score == ranked[3].suggestion.score;
        face.omittedUpperBound = ranked.size() > 4 ? std::optional<RankedPerson>(ranked[3]) : std::nullopt;
        if (ranked.size() > 4) ranked.resize(4);
    } else {
        face.rankingComplete = false;
        if (!oldBoundary) return false;
        const size_t ahead = static_cast<size_t>(std::count_if(ranked.begin(), ranked.end(),
            [&](const RankedPerson& candidate) { return ranksBeforeBoundary(candidate, *oldBoundary); }));
        if (ahead < 3) return false;
        face.frontierExact = ahead >= 4;
        face.omittedBoundaryTie = oldBoundaryTie && !changes.anyNameChanged;
        if (face.frontierExact) {
            face.omittedUpperBound = ranked.size() > 4 ? std::optional<RankedPerson>(ranked[3]) : oldBoundary;
            if (ranked.size() > 4) ranked.resize(4);
        } else {
            face.omittedUpperBound = oldBoundary;
            if (ranked.size() > 3) ranked.resize(3);
        }
    }

    // The unknown previous fifth-place person can only stay below the old
    // boundary. An uncached added person at or above that boundary takes the
    // exact path above; otherwise the old top-four prefix remains sufficient.
    face.ranked = std::move(ranked);
    applyRankedSuggestions(face);
    return true;
}

void finishGroupReviewBuild(GroupReviewBuild& built, float matchThreshold,
                            const GroupReviewIndex* previous = nullptr) {
    auto& index = *built.index;
    index.exemplarDescriptors = describeExemplars(built.exemplars);

    std::unordered_map<std::string, ExemplarChanges> changesByKey;
    if (previous) {
        for (const auto& face : index.faces) {
            if (face.queryEligible) {
                const std::string key = exemplarKey(face.recognizerChecksum, face.pipelineVersion);
                if (!changesByKey.contains(key)) {
                    changesByKey.emplace(key, diffExemplars(previous, index, built.exemplars, key));
                }
            }
        }
    }

    for (const auto& [faceId, rejected] : built.rejections) {
        auto position = index.facePositions.find(faceId);
        if (position != index.facePositions.end()) index.faces[position->second].rejectedTags = rejected;
    }

    struct PackedExemplars {
        const std::vector<Exemplar>* exemplars{nullptr};
        std::vector<float> embeddings;
    };

    std::unordered_map<std::string, PackedExemplars> packedByKey;
    packedByKey.reserve(built.exemplars.size());
    for (const auto& [key, exemplars] : built.exemplars) {
        PackedExemplars packed;
        packed.exemplars = &exemplars;
        packed.embeddings.resize(exemplars.size() * kEmbeddingDimensions);
        for (size_t i = 0; i < exemplars.size(); ++i) {
            if (exemplars[i].embedding.size() == kEmbeddingDimensions) {
                std::memcpy(packed.embeddings.data() + i * kEmbeddingDimensions,
                            exemplars[i].embedding.data(),
                            kEmbeddingDimensions * sizeof(float));
            }
        }
        packedByKey.emplace(key, std::move(packed));
    }

    const size_t totalFaces = index.faces.size();
    unsigned int hardwareThreads = std::thread::hardware_concurrency();
    if (hardwareThreads == 0) hardwareThreads = 4;
    const size_t numThreads = (totalFaces < 32) ? 1 : std::min<size_t>(hardwareThreads, (totalFaces + 31) / 32);

    auto processFaceRange = [&](size_t start, size_t end) {
        static const std::vector<Exemplar> kNoExemplars;
        static const ExemplarChanges kNoChanges;

        for (size_t idx = start; idx < end; ++idx) {
            auto& face = index.faces[idx];
            if (!face.queryEligible) continue;
            bool reused = false;
            if (previous) {
                auto oldPosition = previous->facePositions.find(face.id);
                if (oldPosition != previous->facePositions.end()) {
                    const auto& old = previous->faces[oldPosition->second];
                    if (old.queryEligible && old.embedding == face.embedding &&
                        old.recognizerChecksum == face.recognizerChecksum &&
                        old.pipelineVersion == face.pipelineVersion && old.rejectedTags == face.rejectedTags) {
                        face.ranked = old.ranked;
                        face.omittedUpperBound = old.omittedUpperBound;
                        face.rankingComplete = old.rankingComplete;
                        face.frontierExact = old.frontierExact;
                        face.omittedBoundaryTie = old.omittedBoundaryTie;
                        face.suggestions = old.suggestions;
                        reused = true;
                    }
                }
            }

            const std::string key = exemplarKey(face.recognizerChecksum, face.pipelineVersion);
            auto packedIt = packedByKey.find(key);
            const std::vector<Exemplar>& currentExemplars = (packedIt != packedByKey.end() && packedIt->second.exemplars)
                ? *packedIt->second.exemplars : kNoExemplars;
            const float* packedEmbs = (packedIt != packedByKey.end() && !packedIt->second.embeddings.empty())
                ? packedIt->second.embeddings.data() : nullptr;

            if (!reused) {
                exactRankFace(face, currentExemplars, matchThreshold, packedEmbs);
                continue;
            }

            const auto changes = changesByKey.find(key);
            const ExemplarChanges& currentChanges = changes == changesByKey.end() ? kNoChanges : changes->second;
            const bool updated = incrementallyUpdateFace(face, currentChanges, currentExemplars, matchThreshold);
            if (!updated) {
                exactRankFace(face, currentExemplars, matchThreshold, packedEmbs);
            }
        }
    };

    if (numThreads <= 1) {
        processFaceRange(0, totalFaces);
    } else {
        std::vector<std::jthread> threads;
        threads.reserve(numThreads - 1);
        const size_t chunkSize = (totalFaces + numThreads - 1) / numThreads;
        for (size_t t = 1; t < numThreads; ++t) {
            const size_t start = t * chunkSize;
            const size_t end = std::min(start + chunkSize, totalFaces);
            if (start < end) {
                threads.emplace_back(processFaceRange, start, end);
            }
        }
        const size_t chunk0End = std::min(chunkSize, totalFaces);
        processFaceRange(0, chunk0End);
    }

    // Exemplar vectors and temporary query/rejection maps are large. Keep only
    // compact per-exemplar digests and the active unmatched query vectors.
    built.queries.clear();
    built.queries.shrink_to_fit();
    built.exemplars.clear();
    built.rejections.clear();
}

struct GroupAccumulator {
    GroupReviewGroup group;
    std::unordered_set<MediaId> photos;
    std::unordered_set<MediaId> suggestionPhotos;
};

std::shared_ptr<GroupReviewSnapshot> makeGroupReviewSnapshot(
    std::shared_ptr<GroupReviewIndex> index, std::string token,
    bool showNamed, bool showUnnamed, bool includeDismissed,
    std::optional<TagId> personTagId) {
    auto snapshot = std::make_shared<GroupReviewSnapshot>();
    snapshot->token = std::move(token);
    snapshot->index = std::move(index);
    snapshot->jobId = snapshot->index->jobId;
    snapshot->showNamed = showNamed;
    snapshot->showUnnamed = showUnnamed;
    snapshot->includeDismissed = includeDismissed;
    snapshot->personTagId = personTagId;
    snapshot->createdAt = std::chrono::steady_clock::now();

    std::unordered_map<std::string, GroupAccumulator> accumulators;
    auto addFace = [&](std::string key, std::string type, std::optional<TagId> tagId,
                       const std::string& name, size_t faceIndex, bool suggested) {
        auto& accumulator = accumulators[key];
        if (accumulator.group.key.empty()) {
            accumulator.group.key = key;
            accumulator.group.type = std::move(type);
            accumulator.group.tagId = tagId;
            accumulator.group.name = name;
        }
        const auto& face = snapshot->index->faces[faceIndex];
        accumulator.group.members.push_back(faceIndex);
        accumulator.photos.insert(face.mediaId);
        if (suggested) {
            accumulator.group.suggestionMembers.push_back(faceIndex);
            accumulator.suggestionPhotos.insert(face.mediaId);
        }
    };

    for (size_t i = 0; i < snapshot->index->faces.size(); ++i) {
        const auto& face = snapshot->index->faces[i];
        if (face.dismissed) {
            if (includeDismissed &&
                (!personTagId || (face.personTagId && *face.personTagId == *personTagId))) {
                addFace("dismissed", "dismissed", std::nullopt, "", i, false);
            }
            continue;
        }
        if (face.personTagId) {
            if (showNamed && (!personTagId || *face.personTagId == *personTagId)) {
                addFace("person:" + std::to_string(*face.personTagId), "person",
                        face.personTagId, face.personName, i, false);
            }
            continue;
        }
        if (!showUnnamed || personTagId) continue;
        if (!face.suggestions.empty()) {
            const auto& suggestion = face.suggestions.front();
            addFace("person:" + std::to_string(suggestion.tagId), "person",
                    suggestion.tagId, suggestion.name, i, true);
        } else {
            addFace("unnamed", "unnamed", std::nullopt, "", i, false);
        }
    }

    for (auto& [key, accumulator] : accumulators) {
        accumulator.group.facesCount = static_cast<int64_t>(accumulator.group.members.size());
        accumulator.group.photosCount = static_cast<int64_t>(accumulator.photos.size());
        accumulator.group.suggestionFacesCount = static_cast<int64_t>(accumulator.group.suggestionMembers.size());
        accumulator.group.suggestionPhotosCount = static_cast<int64_t>(accumulator.suggestionPhotos.size());
        snapshot->groups.push_back(std::move(accumulator.group));
    }
    std::sort(snapshot->groups.begin(), snapshot->groups.end(), [](const auto& a, const auto& b) {
        if (a.type != b.type) {
            auto order = [](const std::string& type) {
                if (type == "person") return 0;
                if (type == "unnamed") return 1;
                return 2;
            };
            return order(a.type) < order(b.type);
        }
        if (a.name != b.name) return a.name < b.name;
        return a.tagId.value_or(0) < b.tagId.value_or(0);
    });
    return snapshot;
}

nlohmann::json groupSummaryJson(const GroupReviewGroup& group) {
    return {{"key", group.key}, {"type", group.type},
            {"tag_id", group.tagId ? nlohmann::json(*group.tagId) : nlohmann::json(nullptr)},
            {"name", group.type == "person" ? nlohmann::json(group.name) : nlohmann::json(nullptr)},
            {"faces_count", group.facesCount}, {"photos_count", group.photosCount},
            {"suggestion_faces_count", group.suggestionFacesCount},
            {"suggestion_photos_count", group.suggestionPhotosCount}};
}

} // namespace

struct Service::Impl {
    db::CatalogDb& db;
    std::string cacheDirectory;
    PathResolver resolver;
    Analyzer analyzer;
    Config config;
    Engine engine;
    RuntimeInfo runtime;
    mutable std::mutex workerMutex;
    std::jthread worker;
    std::atomic<bool> cancelRequested{false};
    Status engineStatus;
    mutable std::mutex reviewMutex;
    mutable std::shared_ptr<GroupReviewIndex> reviewIndex;
    // Kept independently from short-lived review snapshots so a stale page or
    // mutation token cannot discard the exact-match baseline used for deltas.
    mutable std::shared_ptr<GroupReviewIndex> matcherIndex;
    mutable std::vector<std::shared_ptr<GroupReviewSnapshot>> reviewSnapshots;

    Impl(db::CatalogDb& dbRef, std::string cacheDir, PathResolver resolve, Analyzer analyze)
        : db(dbRef), cacheDirectory(std::move(cacheDir)), resolver(std::move(resolve)), analyzer(std::move(analyze)),
          config{envString("IMAGINE_FACE_MODELS", ""), envString("IMAGINE_FACE_DEVICE", "cpu"),
                 0.5f, 0.4f, envFloat("IMAGINE_FACE_MATCH_THRESHOLD", 0.5f), 4},
          engine(config) {
        if (analyzer) {
            engineStatus = Status::ok();
            runtime.built = true;
            runtime.ready = true;
            runtime.detectorChecksum = "test-detector";
            runtime.recognizerChecksum = "test-recognizer";
            runtime.runtimeVersion = "test-runtime";
            runtime.detectorProvider = "test";
            runtime.recognizerProvider = "test";
            runtime.fallbackReason = "test-fallback";
        } else {
            engineStatus = engine.initialize();
            runtime = engine.info();
        }
        if (!std::isfinite(config.matchThreshold) || config.matchThreshold < -1 || config.matchThreshold > 1) {
            engineStatus = Status::invalidArgument("IMAGINE_FACE_MATCH_THRESHOLD must be finite and between -1 and 1");
        }
        if (analyzer) {
            runtime.error.clear();
        }
    }
};

Service::Service(db::CatalogDb& db, std::string cacheDirectory, PathResolver resolver, Analyzer analyzer)
    : impl_(std::make_unique<Impl>(db, std::move(cacheDirectory), std::move(resolver), std::move(analyzer))) {
    std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
    auto recover = impl_->db.conn_.prepare(
        "UPDATE face_analysis_jobs SET state='interrupted',error='Process restarted before scan completed',updated_at=? WHERE state IN ('running','cancelling');");
    if (recover.isOk()) {
        auto stmt = std::move(recover.value());
        stmt.bind(1, nowSeconds());
        stmt.step();
    }
}

Service::~Service() {
    if (!impl_) return;
    impl_->cancelRequested.store(true);
    std::lock_guard<std::mutex> lock(impl_->workerMutex);
    if (impl_->worker.joinable()) {
        impl_->worker.request_stop();
        impl_->worker.join();
    }
}

json Service::status() const {
    const auto& info = impl_->runtime;
    json result = {
        {"built", info.built}, {"ready", info.ready && impl_->engineStatus.isOk()},
        {"error", impl_->engineStatus.isOk() ? info.error : impl_->engineStatus.message()},
        {"config", {{"device", impl_->config.device}, {"confidence", impl_->config.confidence},
                    {"nms_threshold", impl_->config.nmsThreshold}, {"match_threshold", impl_->config.matchThreshold}}},
        {"runtime", {{"detector_checksum", info.detectorChecksum}, {"recognizer_checksum", info.recognizerChecksum},
                     {"runtime_version", info.runtimeVersion}, {"detector_provider", info.detectorProvider},
                     {"recognizer_provider", info.recognizerProvider}, {"fallback_reason", info.fallbackReason},
                     {"pipeline_version", info.pipelineVersion}}},
        {"job", nullptr}
    };
    std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
    auto jobRes = impl_->db.conn_.prepare(
        "SELECT id,state,failed FROM face_analysis_jobs ORDER BY id DESC LIMIT 1;");
    if (jobRes.isOk()) {
        auto stmt = std::move(jobRes.value());
        if (stmt.step() == StepResult::Row) {
            const std::string state = stmt.getString(1);
            const bool recoverable = state == "running" || state == "cancelling" || state == "interrupted" ||
                state == "failed" || (state == "completed" && stmt.getInt64(2) > 0);
            if (recoverable) result["job"] = jobJson(impl_->db.conn_, stmt.getInt64(0));
        }
    }
    return result;
}

Status Service::startJob(const std::string& scope, const std::vector<MediaId>& requestedIds,
                         bool force, int64_t& jobId) {
    if (scope != "selected" && scope != "catalog") return Status::invalidArgument("scope must be selected or catalog");
    if (scope == "catalog" && !requestedIds.empty()) return Status::invalidArgument("media_ids are only valid with selected scope");
    if (scope == "selected" && (requestedIds.empty() || requestedIds.size() > 1000)) {
        return Status::invalidArgument("Selected scans require between 1 and 1000 media IDs");
    }
    std::unordered_set<MediaId> seen;
    for (MediaId id : requestedIds) {
        if (id <= 0 || !seen.insert(id).second) return Status::invalidArgument("media_ids must be unique positive IDs");
    }

    std::unique_lock<std::mutex> workerLock(impl_->workerMutex);
    std::vector<MediaId> ids;
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
        auto activeRes = impl_->db.conn_.prepare("SELECT id FROM face_analysis_jobs WHERE state IN ('running','cancelling') LIMIT 1;");
        if (!activeRes.isOk()) return activeRes.status();
        auto active = std::move(activeRes.value());
        if (active.step() == StepResult::Row) return Status::alreadyExists("A face-analysis scan is already active");

        if (scope == "catalog") {
            // Keep this eligibility predicate aligned with analysisReusable().
            auto mediaRes = impl_->db.conn_.prepare(R"SQL(
                SELECT m.id
                FROM media_items m
                WHERE m.media_type='photo'
                  AND (?=1 OR NOT EXISTS (
                    SELECT 1 FROM face_media_analysis a
                    WHERE a.media_id=m.id AND a.state='complete'
                      AND a.source_hash=m.content_hash
                      AND a.detector_checksum=? AND a.recognizer_checksum=?
                      AND a.pipeline_version=?
                      AND ABS(a.confidence-?)<0.000001
                      AND ABS(a.nms_threshold-?)<0.000001
                  ))
                ORDER BY m.id;
            )SQL");
            if (!mediaRes.isOk()) return mediaRes.status();
            auto media = std::move(mediaRes.value());
            media.bind(1, force ? 1 : 0);
            media.bind(2, impl_->runtime.detectorChecksum);
            media.bind(3, impl_->runtime.recognizerChecksum);
            media.bind(4, impl_->runtime.pipelineVersion);
            media.bind(5, impl_->config.confidence);
            media.bind(6, impl_->config.nmsThreshold);
            while (media.step() == StepResult::Row) ids.push_back(media.getInt64(0));
        } else {
            ids = requestedIds;
            for (MediaId id : ids) {
                auto mediaRes = impl_->db.getMediaById(id);
                if (!mediaRes.isOk()) return mediaRes.status();
                if (mediaRes.value().media_type != "photo") return Status::invalidArgument("Face analysis only supports photos");
            }
        }

        if (!impl_->runtime.ready || !impl_->engineStatus.isOk()) {
            return Status::ioError(impl_->engineStatus.isOk() ? impl_->runtime.error : impl_->engineStatus.message());
        }

        db::Transaction tx(impl_->db.conn_);
        int64_t now = nowSeconds();
        auto insertRes = impl_->db.conn_.prepare(
            "INSERT INTO face_analysis_jobs(state,scope,total,remaining,force,created_at,updated_at) VALUES('running',?,?,?,?,?,?);");
        if (!insertRes.isOk()) return insertRes.status();
        auto insert = std::move(insertRes.value());
        insert.bind(1, scope); insert.bind(2, static_cast<int64_t>(ids.size()));
        insert.bind(3, static_cast<int64_t>(ids.size())); insert.bind(4, force ? 1 : 0);
        insert.bind(5, now); insert.bind(6, now);
        if (insert.step() != StepResult::Done) {
            if ((impl_->db.conn_.lastErrorCode() & 0xff) == SQLITE_CONSTRAINT) {
                return Status::alreadyExists("A face-analysis scan is already active");
            }
            return dbError(impl_->db.conn_, "Failed to create face-analysis job");
        }
        jobId = impl_->db.conn_.lastInsertRowId();
        auto memberRes = impl_->db.conn_.prepare(
            "INSERT INTO face_analysis_job_media(job_id,media_id) VALUES(?,?);");
        if (!memberRes.isOk()) return memberRes.status();
        auto member = std::move(memberRes.value());
        for (MediaId id : ids) {
            member.bind(1, jobId); member.bind(2, id);
            if (member.step() != StepResult::Done) return dbError(impl_->db.conn_, "Failed to record face-analysis job membership");
            member.reset();
        }
        Status committed = tx.commit();
        if (!committed.isOk()) return committed;
    }

    IMAGINE_LOG_INFO("Created face-analysis job " + std::to_string(jobId) +
                     " (scope: " + scope + ", items: " + std::to_string(ids.size()) +
                     ", force: " + (force ? "true" : "false") + ")");

    impl_->cancelRequested.store(false);
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->worker = std::jthread([this, jobId, ids = std::move(ids), force](std::stop_token) mutable {
        runJob(jobId, std::move(ids), force);
    });
    return Status::ok();
}

Result<json> Service::getJob(int64_t jobId) const {
    if (jobId <= 0) return Status::invalidArgument("job ID must be positive");
    std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
    json job = jobJson(impl_->db.conn_, jobId);
    if (job.is_null()) return Status::notFound("Face-analysis job not found");
    return job;
}

Status Service::cancelJob(int64_t jobId) {
    if (jobId <= 0) return Status::invalidArgument("job ID must be positive");
    std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
    auto stateRes = impl_->db.conn_.prepare("SELECT state FROM face_analysis_jobs WHERE id=?;");
    if (!stateRes.isOk()) return stateRes.status();
    auto state = std::move(stateRes.value()); state.bind(1, jobId);
    if (state.step() != StepResult::Row) return Status::notFound("Face-analysis job not found");
    std::string value = state.getString(0);
    if (value == "cancelling") return Status::ok();
    if (value != "running") return Status::alreadyExists("Face-analysis job is no longer running");
    auto updateRes = impl_->db.conn_.prepare("UPDATE face_analysis_jobs SET state='cancelling',updated_at=? WHERE id=? AND state='running';");
    if (!updateRes.isOk()) return updateRes.status();
    auto update = std::move(updateRes.value()); update.bind(1, nowSeconds()); update.bind(2, jobId);
    Status s = execDone(impl_->db.conn_, update, "Failed to cancel face-analysis job");
    if (!s.isOk()) return s;
    impl_->cancelRequested.store(true);
    IMAGINE_LOG_INFO("Requested cancellation for face-analysis job " + std::to_string(jobId));
    return Status::ok();
}

Result<json> Service::getMedia(MediaId mediaId) const {
    if (mediaId <= 0) return Status::invalidArgument("media ID must be positive");
    std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
    auto mediaRes = impl_->db.conn_.prepare(R"SQL(
        SELECT m.id,m.media_type,a.state,a.error,a.width,a.height
        FROM media_items m LEFT JOIN face_media_analysis a ON a.media_id=m.id WHERE m.id=?;
    )SQL");
    if (!mediaRes.isOk()) return mediaRes.status();
    auto media = std::move(mediaRes.value()); media.bind(1, mediaId);
    if (media.step() != StepResult::Row) return Status::notFound("Media not found");
    if (media.getString(1) != "photo") return Status::invalidArgument("Face analysis only supports photos");
    std::string state = media.isNull(2) ? "unscanned" : media.getString(2);
    json out = {{"media_id", mediaId}, {"state", state},
                {"error", media.isNull(3) ? "" : media.getString(3)},
                {"width", media.isNull(4) ? 0 : media.getInt(4)},
                {"height", media.isNull(5) ? 0 : media.getInt(5)}, {"faces", json::array()}};
    auto facesRes = impl_->db.conn_.prepare("SELECT id FROM faces WHERE media_id=? ORDER BY id;");
    if (!facesRes.isOk()) return facesRes.status();
    auto faces = std::move(facesRes.value()); faces.bind(1, mediaId);
    std::vector<int64_t> faceIds;
    while (faces.step() == StepResult::Row) faceIds.push_back(faces.getInt64(0));
    RejectionCache rejectionCache;
    Status rejectionStatus = loadRejections(impl_->db.conn_, faceIds, rejectionCache);
    if (!rejectionStatus.isOk()) return rejectionStatus;
    ExemplarCache exemplarCache;
    for (int64_t faceId : faceIds) {
        auto face = faceJson(impl_->db.conn_, faceId, impl_->config.matchThreshold,
                             &exemplarCache, &rejectionCache);
        if (face.isOk()) out["faces"].push_back(std::move(face.value()));
    }
    return out;
}

Result<json> Service::getReview(int offset, int limit) const {
    if (offset < 0 || limit < 1 || limit > 200) return Status::invalidArgument("offset must be nonnegative and limit must be between 1 and 200");
    std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
    auto countRes = impl_->db.conn_.prepare(
        "SELECT COUNT(*) FROM faces f JOIN face_media_analysis a ON a.media_id=f.media_id "
        "WHERE f.person_tag_id IS NULL AND f.dismissed=0 AND a.state='complete';");
    if (!countRes.isOk()) return countRes.status();
    auto count = std::move(countRes.value());
    int64_t total = count.step() == StepResult::Row ? count.getInt64(0) : 0;
    auto stmtRes = impl_->db.conn_.prepare(
        "SELECT f.id FROM faces f JOIN face_media_analysis a ON a.media_id=f.media_id "
        "WHERE f.person_tag_id IS NULL AND f.dismissed=0 AND a.state='complete' ORDER BY f.id LIMIT ? OFFSET ?;");
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value()); stmt.bind(1, limit); stmt.bind(2, offset);
    std::vector<int64_t> faceIds;
    while (stmt.step() == StepResult::Row) faceIds.push_back(stmt.getInt64(0));
    RejectionCache rejectionCache;
    Status rejectionStatus = loadRejections(impl_->db.conn_, faceIds, rejectionCache);
    if (!rejectionStatus.isOk()) return rejectionStatus;
    ExemplarCache exemplarCache;
    json items = json::array();
    for (int64_t faceId : faceIds) {
        auto face = faceJson(impl_->db.conn_, faceId, impl_->config.matchThreshold,
                             &exemplarCache, &rejectionCache);
        if (face.isOk()) items.push_back(std::move(face.value()));
    }
    return json{{"items", std::move(items)}, {"total", total}};
}

Result<json> Service::getGrid(std::optional<int64_t> jobId, int offset, int limit,
                              bool includeDismissed) const {
    if ((jobId && *jobId <= 0) || offset < 0 || limit < 1 || limit > 1000) {
        return Status::invalidArgument("job_id must be positive, offset nonnegative, and limit between 1 and 1000");
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
    Connection& conn = impl_->db.conn_;
    if (jobId) {
        auto jobRes = conn.prepare("SELECT 1 FROM face_analysis_jobs WHERE id=?;");
        if (!jobRes.isOk()) return jobRes.status();
        auto job = std::move(jobRes.value()); job.bind(1, *jobId);
        if (job.step() != StepResult::Row) return Status::notFound("Face-analysis job not found");
    }

    std::string from = " FROM faces f JOIN face_media_analysis a ON a.media_id=f.media_id";
    if (jobId) from += " JOIN face_analysis_job_media jm ON jm.media_id=f.media_id AND jm.job_id=?";
    std::string where = " WHERE a.state='complete'";
    if (!includeDismissed) where += " AND f.dismissed=0";

    auto countRes = conn.prepare("SELECT COUNT(*)" + from + where + ";");
    if (!countRes.isOk()) return countRes.status();
    auto countStmt = std::move(countRes.value());
    if (jobId) countStmt.bind(1, *jobId);
    int64_t total = countStmt.step() == StepResult::Row ? countStmt.getInt64(0) : 0;

    auto itemsRes = conn.prepare("SELECT f.id" + from + where + " ORDER BY f.id LIMIT ? OFFSET ?;");
    if (!itemsRes.isOk()) return itemsRes.status();
    auto itemStmt = std::move(itemsRes.value());
    int param = 1;
    if (jobId) itemStmt.bind(param++, *jobId);
    itemStmt.bind(param++, limit); itemStmt.bind(param, offset);
    std::vector<int64_t> faceIds;
    while (itemStmt.step() == StepResult::Row) faceIds.push_back(itemStmt.getInt64(0));
    RejectionCache rejectionCache;
    Status rejectionStatus = loadRejections(conn, faceIds, rejectionCache);
    if (!rejectionStatus.isOk()) return rejectionStatus;
    json items = json::array();
    ExemplarCache exemplarCache;
    for (int64_t faceId : faceIds) {
        auto face = faceJson(conn, faceId, impl_->config.matchThreshold,
                             &exemplarCache, &rejectionCache, true);
        if (face.isOk()) items.push_back(std::move(face.value()));
    }
    return json{{"items", std::move(items)}, {"total", total}};
}

Result<json> Service::getFace(int64_t faceId) const {
    if (faceId <= 0) return Status::invalidArgument("face ID must be positive");
    std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
    return faceJson(impl_->db.conn_, faceId, impl_->config.matchThreshold);
}

Result<std::vector<FaceLookupOutcome>> Service::lookupFaces(
    const std::vector<int64_t>& faceIds) const {
    if (faceIds.empty() || faceIds.size() > 200) {
        return Status::invalidArgument("Face lookup requires between 1 and 200 IDs");
    }
    std::unordered_set<int64_t> uniqueIds;
    uniqueIds.reserve(faceIds.size());
    for (int64_t faceId : faceIds) {
        if (faceId <= 0 || !uniqueIds.insert(faceId).second) {
            return Status::invalidArgument("Face lookup IDs must be unique positive integers");
        }
    }

    std::lock_guard<std::mutex> reviewLock(impl_->reviewMutex);
    std::lock_guard<std::recursive_mutex> dbLock(impl_->db.mutex_);
    Connection& conn = impl_->db.conn_;
    auto stampRes = readReviewStamp(conn);
    if (!stampRes.isOk()) return stampRes.status();
    const ReviewStamp stamp = stampRes.value();

    std::shared_ptr<GroupReviewIndex> currentIndex = impl_->reviewIndex;
    if (currentIndex && !(currentIndex->stamp == stamp)) {
        impl_->reviewIndex.reset();
        impl_->reviewSnapshots.clear();
        currentIndex.reset();
    }

    std::vector<int64_t> uncachedIds;
    uncachedIds.reserve(faceIds.size());
    for (int64_t faceId : faceIds) {
        if (!currentIndex || !currentIndex->facePositions.contains(faceId)) uncachedIds.push_back(faceId);
    }
    RejectionCache rejectionCache;
    Status rejectionStatus = loadRejections(conn, uncachedIds, rejectionCache);
    if (!rejectionStatus.isOk()) return rejectionStatus;

    ExemplarCache exemplarCache;
    std::vector<FaceLookupOutcome> outcomes;
    outcomes.reserve(faceIds.size());
    for (int64_t faceId : faceIds) {
        FaceLookupOutcome outcome;
        outcome.id = faceId;
        if (currentIndex) {
            auto position = currentIndex->facePositions.find(faceId);
            if (position != currentIndex->facePositions.end()) {
                const auto& indexed = currentIndex->faces[position->second];
                auto face = faceJson(conn, faceId, impl_->config.matchThreshold,
                                     nullptr, nullptr, true, nullptr, false);
                if (!face.isOk()) {
                    outcome.status = face.status();
                } else if (face.value()["revision"].get<int64_t>() != indexed.revision) {
                    impl_->reviewIndex.reset();
                    impl_->reviewSnapshots.clear();
                    return Status::alreadyExists("Face group snapshot is stale; retry the face lookup");
                } else {
                    if (!indexed.dismissed && !indexed.personTagId) {
                        for (const auto& suggestion : indexed.suggestions) {
                            face.value()["suggestions"].push_back({{"tag_id", suggestion.tagId},
                                {"name", suggestion.name}, {"score", suggestion.score}});
                        }
                    }
                    outcome.face = std::move(face.value());
                }
                outcomes.push_back(std::move(outcome));
                continue;
            }
        }

        auto face = faceJson(conn, faceId, impl_->config.matchThreshold,
                             &exemplarCache, &rejectionCache, true);
        if (!face.isOk()) outcome.status = face.status();
        else outcome.face = std::move(face.value());
        outcomes.push_back(std::move(outcome));
    }

    auto finalStamp = readReviewStamp(conn);
    if (!finalStamp.isOk()) return finalStamp.status();
    if (!(finalStamp.value() == stamp)) {
        impl_->reviewIndex.reset();
        impl_->reviewSnapshots.clear();
        return Status::alreadyExists("Catalog changed during face lookup; retry the request");
    }
    return outcomes;
}

Result<json> Service::getGroups(std::optional<int64_t> jobId, bool showNamed,
                                bool showUnnamed, bool includeDismissed,
                                std::optional<TagId> personTagId) const {
    if ((jobId && *jobId <= 0) || (personTagId && *personTagId <= 0)) {
        return Status::invalidArgument("job_id and person_tag_id must be positive");
    }
    std::unique_lock<std::mutex> reviewLock(impl_->reviewMutex);
    ReviewStamp currentStamp;
    {
        std::lock_guard<std::recursive_mutex> dbLock(impl_->db.mutex_);
        auto stamp = readReviewStamp(impl_->db.conn_);
        if (!stamp.isOk()) return stamp.status();
        currentStamp = stamp.value();
        if (personTagId && !isPeopleTag(impl_->db.conn_, *personTagId)) {
            return Status::invalidArgument("person_tag_id must identify a people tag");
        }
        if (jobId) {
            auto jobRes = impl_->db.conn_.prepare("SELECT 1 FROM face_analysis_jobs WHERE id=?;");
            if (!jobRes.isOk()) return jobRes.status();
            auto job = std::move(jobRes.value());
            job.bind(1, *jobId);
            if (job.step() != StepResult::Row) return Status::notFound("Face-analysis job not found");
        }
    }

    const auto sameScope = [&](const std::shared_ptr<GroupReviewIndex>& index) {
        return index && index->jobId == jobId;
    };
    if (!sameScope(impl_->reviewIndex) || !(impl_->reviewIndex->stamp == currentStamp)) {
        GroupReviewBuild pendingBuild;
        ReviewStamp buildStamp;
        const GroupReviewIndex* previousIndex = impl_->matcherIndex &&
            impl_->matcherIndex->jobId == jobId ? impl_->matcherIndex.get() : nullptr;
        {
            std::lock_guard<std::recursive_mutex> dbLock(impl_->db.mutex_);
            db::Transaction readTransaction(impl_->db.conn_);
            auto startStamp = readReviewStamp(impl_->db.conn_);
            if (!startStamp.isOk()) return startStamp.status();
            if (!(startStamp.value() == currentStamp)) {
                return Status::alreadyExists("Catalog changed before face groups could be built; reload the groups");
            }
            if (jobId) {
                auto jobRes = impl_->db.conn_.prepare("SELECT 1 FROM face_analysis_jobs WHERE id=?;");
                if (!jobRes.isOk()) return jobRes.status();
                auto job = std::move(jobRes.value());
                job.bind(1, *jobId);
                if (job.step() != StepResult::Row) return Status::notFound("Face-analysis job not found");
            }
            auto build = buildGroupReviewIndex(impl_->db.conn_, jobId, startStamp.value());
            if (!build.isOk()) return build.status();
            pendingBuild = std::move(build.value());
            Status committed = readTransaction.commit();
            if (!committed.isOk()) return committed;
            auto endStamp = readReviewStamp(impl_->db.conn_);
            if (!endStamp.isOk()) return endStamp.status();
            if (!(endStamp.value() == startStamp.value())) {
                return Status::alreadyExists("Catalog changed while building face groups; reload the groups");
            }
            buildStamp = endStamp.value();
        }

        // Exact cosine ranking is CPU-bound, so run it after releasing the
        // catalog mutex and read transaction. Only the final publication check
        // reacquires the DB lock.
        finishGroupReviewBuild(pendingBuild, impl_->config.matchThreshold, previousIndex);
        pendingBuild.index->stamp = buildStamp;
        {
            std::lock_guard<std::recursive_mutex> dbLock(impl_->db.mutex_);
            auto afterMatching = readReviewStamp(impl_->db.conn_);
            if (!afterMatching.isOk()) return afterMatching.status();
            if (!(afterMatching.value() == buildStamp)) {
                return Status::alreadyExists("Catalog changed while matching face groups; reload the groups");
            }
        }
        impl_->matcherIndex = pendingBuild.index;
        impl_->reviewIndex = std::move(pendingBuild.index);
        impl_->reviewSnapshots.clear();
    }

    constexpr auto kSnapshotTtl = std::chrono::minutes(10);
    auto now = std::chrono::steady_clock::now();
    for (auto it = impl_->reviewSnapshots.begin(); it != impl_->reviewSnapshots.end();) {
        auto& snapshot = *it;
        if (now - snapshot->createdAt >= kSnapshotTtl || snapshot->index != impl_->reviewIndex) {
            it = impl_->reviewSnapshots.erase(it);
            continue;
        }
        if (snapshot->jobId == jobId && snapshot->showNamed == showNamed &&
            snapshot->showUnnamed == showUnnamed && snapshot->includeDismissed == includeDismissed &&
            snapshot->personTagId == personTagId) {
            auto stable = snapshot;
            impl_->reviewSnapshots.erase(it);
            impl_->reviewSnapshots.push_back(stable);
            nlohmann::json groups = nlohmann::json::array();
            int64_t totalFaces = 0;
            for (const auto& group : stable->groups) {
                groups.push_back(groupSummaryJson(group));
                totalFaces += group.facesCount;
            }
            {
                std::lock_guard<std::recursive_mutex> dbLock(impl_->db.mutex_);
                auto after = readReviewStamp(impl_->db.conn_);
                if (!after.isOk()) return after.status();
                if (!(after.value() == stable->index->stamp)) {
                    impl_->reviewIndex.reset();
                    impl_->reviewSnapshots.clear();
                    return Status::alreadyExists("Face group snapshot is stale; reload the groups");
                }
            }
            return nlohmann::json{{"snapshot", stable->token}, {"total", totalFaces},
                                  {"group_count", stable->groups.size()}, {"groups", std::move(groups)}};
        }
        ++it;
    }

    std::string token = newReviewToken();
    if (token.empty()) return Status::internal("Failed to create face-group snapshot token");
    auto snapshot = makeGroupReviewSnapshot(impl_->reviewIndex, std::move(token),
        showNamed, showUnnamed, includeDismissed, personTagId);
    impl_->reviewSnapshots.push_back(snapshot);
    constexpr size_t kMaxCachedSnapshots = 4;
    if (impl_->reviewSnapshots.size() > kMaxCachedSnapshots) {
        impl_->reviewSnapshots.erase(impl_->reviewSnapshots.begin());
    }

    nlohmann::json groups = nlohmann::json::array();
    int64_t totalFaces = 0;
    for (const auto& group : snapshot->groups) {
        groups.push_back(groupSummaryJson(group));
        totalFaces += group.facesCount;
    }
    {
        std::lock_guard<std::recursive_mutex> dbLock(impl_->db.mutex_);
        auto after = readReviewStamp(impl_->db.conn_);
        if (!after.isOk()) return after.status();
        if (!(after.value() == snapshot->index->stamp)) {
            impl_->reviewIndex.reset();
            impl_->reviewSnapshots.clear();
            return Status::alreadyExists("Face group snapshot is stale; reload the groups");
        }
    }
    return nlohmann::json{{"snapshot", snapshot->token}, {"total", totalFaces},
                          {"group_count", snapshot->groups.size()}, {"groups", std::move(groups)}};
}

Result<json> Service::getGroupPage(const std::string& snapshotToken, const std::string& key,
                                   int offset, int limit) const {
    if (snapshotToken.empty() || key.empty() || offset < 0 || limit < 1 || limit > 200) {
        return Status::invalidArgument("snapshot and key are required; offset must be nonnegative and limit between 1 and 200");
    }
    std::lock_guard<std::mutex> reviewLock(impl_->reviewMutex);
    const auto now = std::chrono::steady_clock::now();
    auto snapshotIt = std::find_if(impl_->reviewSnapshots.begin(), impl_->reviewSnapshots.end(),
        [&](const auto& candidate) { return candidate->token == snapshotToken; });
    if (snapshotIt == impl_->reviewSnapshots.end() || now - (*snapshotIt)->createdAt >= std::chrono::minutes(10)) {
        return Status::alreadyExists("Face group snapshot expired or was evicted; reload the groups");
    }
    auto snapshot = *snapshotIt;
    auto groupIt = std::find_if(snapshot->groups.begin(), snapshot->groups.end(),
        [&](const auto& group) { return group.key == key; });
    if (groupIt == snapshot->groups.end()) return Status::notFound("Face group not found");

    std::lock_guard<std::recursive_mutex> dbLock(impl_->db.mutex_);
    auto before = readReviewStamp(impl_->db.conn_);
    if (!before.isOk()) return before.status();
    if (!(before.value() == snapshot->index->stamp)) {
        impl_->reviewIndex.reset();
        impl_->reviewSnapshots.clear();
        return Status::alreadyExists("Face group snapshot is stale; reload the groups");
    }

    json items = json::array();
    const size_t start = std::min(static_cast<size_t>(offset), groupIt->members.size());
    const size_t end = std::min(groupIt->members.size(), start + static_cast<size_t>(limit));
    for (size_t i = start; i < end; ++i) {
        const auto& indexed = snapshot->index->faces[groupIt->members[i]];
        auto face = faceJson(impl_->db.conn_, indexed.id, impl_->config.matchThreshold,
                             nullptr, nullptr, true, nullptr, false);
        if (!face.isOk()) return face.status();
        if (face.value()["revision"].get<int64_t>() != indexed.revision) {
            impl_->reviewIndex.reset();
            impl_->reviewSnapshots.clear();
            return Status::alreadyExists("Face group snapshot is stale; reload the groups");
        }
        if (!indexed.dismissed && !indexed.personTagId) {
            for (const auto& suggestion : indexed.suggestions) {
                face.value()["suggestions"].push_back({{"tag_id", suggestion.tagId},
                    {"name", suggestion.name}, {"score", suggestion.score}});
            }
        }
        items.push_back(std::move(face.value()));
    }
    auto after = readReviewStamp(impl_->db.conn_);
    if (!after.isOk()) return after.status();
    if (!(after.value() == snapshot->index->stamp)) {
        impl_->reviewIndex.reset();
        impl_->reviewSnapshots.clear();
        return Status::alreadyExists("Face group snapshot is stale; reload the groups");
    }
    return json{{"snapshot", snapshot->token}, {"key", key}, {"items", std::move(items)},
                {"total", groupIt->facesCount}, {"offset", offset}, {"limit", limit}};
}

Result<std::vector<SuggestionAcceptanceOutcome>> Service::acceptGroup(
    const std::string& snapshotToken, const std::string& key) {
    if (snapshotToken.empty() || key.empty()) {
        return Status::invalidArgument("snapshot and key are required");
    }
    std::lock_guard<std::mutex> reviewLock(impl_->reviewMutex);
    auto snapshotIt = std::find_if(impl_->reviewSnapshots.begin(), impl_->reviewSnapshots.end(),
        [&](const auto& candidate) { return candidate->token == snapshotToken; });
    if (snapshotIt == impl_->reviewSnapshots.end() ||
        std::chrono::steady_clock::now() - (*snapshotIt)->createdAt >= std::chrono::minutes(10)) {
        return Status::alreadyExists("Face group snapshot expired or was evicted; reload the groups");
    }
    auto snapshot = *snapshotIt;
    auto groupIt = std::find_if(snapshot->groups.begin(), snapshot->groups.end(),
        [&](const auto& group) { return group.key == key; });
    if (groupIt == snapshot->groups.end()) return Status::notFound("Face group not found");
    if (groupIt->type != "person" || !groupIt->tagId) {
        return Status::invalidArgument("Only person groups can be accepted");
    }

    std::lock_guard<std::recursive_mutex> dbLock(impl_->db.mutex_);
    auto initialStamp = readReviewStamp(impl_->db.conn_);
    if (!initialStamp.isOk()) return initialStamp.status();
    if (!(initialStamp.value() == snapshot->index->stamp)) {
        impl_->reviewIndex.reset();
        impl_->reviewSnapshots.clear();
        return Status::alreadyExists("Face group snapshot is stale; reload the groups");
    }

    std::vector<SuggestionAcceptanceOutcome> outcomes;
    outcomes.reserve(groupIt->suggestionMembers.size());
    bool wrote = false;
    for (size_t position = 0; position < groupIt->suggestionMembers.size(); ++position) {
        const auto& face = snapshot->index->faces[groupIt->suggestionMembers[position]];
        SuggestionAcceptanceOutcome outcome;
        outcome.id = face.id;
        auto currentVersion = readReviewStamp(impl_->db.conn_);
        if (!currentVersion.isOk()) {
            outcome.status = currentVersion.status();
            outcomes.push_back(std::move(outcome));
            continue;
        }
        if (currentVersion.value().dataVersion != snapshot->index->stamp.dataVersion) {
            outcome.status = Status::alreadyExists("Face group snapshot is stale; reload the groups");
            outcomes.push_back(std::move(outcome));
            for (++position; position < groupIt->suggestionMembers.size(); ++position) {
                SuggestionAcceptanceOutcome remaining;
                remaining.id = snapshot->index->faces[groupIt->suggestionMembers[position]].id;
                remaining.status = Status::alreadyExists("Face group snapshot is stale; reload the groups");
                outcomes.push_back(std::move(remaining));
            }
            break;
        }
        outcome.status = assignExistingPersonTag(impl_->db.conn_, face.id, face.revision,
                                                  *groupIt->tagId, snapshot->index->stamp.dataVersion);
        wrote = wrote || outcome.status.isOk();
        if (!outcome.status.isOk() && outcome.status.message().find("snapshot is stale") != std::string::npos) {
            outcomes.push_back(std::move(outcome));
            for (++position; position < groupIt->suggestionMembers.size(); ++position) {
                SuggestionAcceptanceOutcome remaining;
                remaining.id = snapshot->index->faces[groupIt->suggestionMembers[position]].id;
                remaining.status = Status::alreadyExists("Face group snapshot is stale; reload the groups");
                outcomes.push_back(std::move(remaining));
            }
            break;
        }
        outcomes.push_back(std::move(outcome));
    }
    if (wrote) {
        impl_->reviewIndex.reset();
        impl_->reviewSnapshots.clear();
    }
    return outcomes;
}

Result<json> Service::setIdentity(int64_t faceId, int64_t revision, std::optional<TagId> tagId,
                                  const std::optional<std::string>& name,
                                  bool includeSuggestions) {
    if (faceId <= 0 || revision <= 0 || (tagId.has_value() && name.has_value())) {
        return Status::invalidArgument("Provide a face ID and revision, with at most one of tag_id or name");
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
    Connection& conn = impl_->db.conn_;
    TagId newTag = 0;
    std::string tagName;
    db::Transaction tx(conn);
    if (tagId) {
        if (*tagId <= 0 || !isPeopleTag(conn, *tagId, &tagName)) return Status::invalidArgument("tag_id must identify a people tag");
        newTag = *tagId;
    } else if (name) {
        tagName = trimWhitespace(*name);
        if (tagName.empty() || tagName.size() > 100) return Status::invalidArgument("name must contain between 1 and 100 UTF-8 bytes");
        auto insertRes = conn.prepare("INSERT OR IGNORE INTO tags(name,category) VALUES(?,'people');");
        if (!insertRes.isOk()) return insertRes.status();
        auto insert = std::move(insertRes.value()); insert.bind(1, tagName);
        Status s = execDone(conn, insert, "Failed to create people tag"); if (!s.isOk()) return s;
        auto lookupRes = conn.prepare("SELECT id FROM tags WHERE name=? AND category='people';");
        if (!lookupRes.isOk()) return lookupRes.status();
        auto lookup = std::move(lookupRes.value()); lookup.bind(1, tagName);
        if (lookup.step() != StepResult::Row) return dbError(conn, "Failed to find created people tag");
        newTag = lookup.getInt64(0);
    }

    auto getRes = conn.prepare("SELECT media_id,person_tag_id,revision FROM faces WHERE id=?;");
    if (!getRes.isOk()) return getRes.status();
    auto get = std::move(getRes.value()); get.bind(1, faceId);
    if (get.step() != StepResult::Row) return Status::notFound("Face not found");
    MediaId mediaId = get.getInt64(0);
    std::optional<TagId> oldTag;
    if (!get.isNull(1)) oldTag = get.getInt64(1);
    if (get.getInt64(2) != revision) return Status::alreadyExists("Face review is stale; reload before changing its identity");

    auto updateRes = conn.prepare("UPDATE faces SET person_tag_id=?,revision=revision+1,updated_at=? WHERE id=? AND revision=?;");
    if (!updateRes.isOk()) return updateRes.status();
    auto update = std::move(updateRes.value());
    if (newTag > 0) update.bind(1, newTag); else update.bindNull(1);
    update.bind(2, nowSeconds());
    update.bind(3, faceId); update.bind(4, revision);
    Status s = execDone(conn, update, "Failed to assign face identity"); if (!s.isOk()) return s;
    if (newTag > 0) {
        auto rejectRes = conn.prepare("DELETE FROM face_rejections WHERE face_id=? AND tag_id=?;");
        if (!rejectRes.isOk()) return rejectRes.status();
        auto reject = std::move(rejectRes.value()); reject.bind(1, faceId); reject.bind(2, newTag);
        s = execDone(conn, reject, "Failed to clear identity rejection"); if (!s.isOk()) return s;
    }
    if (oldTag && *oldTag != newTag) { s = syncFaceTagOwnership(conn, mediaId, *oldTag); if (!s.isOk()) return s; }
    if (newTag > 0) { s = syncFaceTagOwnership(conn, mediaId, newTag); if (!s.isOk()) return s; }
    s = tx.commit(); if (!s.isOk()) return s;
    return faceJson(conn, faceId, impl_->config.matchThreshold, nullptr, nullptr,
                    false, nullptr, includeSuggestions);
}

Result<json> Service::acceptSuggestion(int64_t faceId, int64_t revision, TagId tagId) {
    if (faceId <= 0 || revision <= 0 || tagId <= 0) return Status::invalidArgument("face ID, revision, and tag_id must be positive");
    std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
    ExemplarCache exemplarCache;
    RejectionCache rejectionCache;
    auto rejectionStatus = loadRejections(impl_->db.conn_, {faceId}, rejectionCache);
    if (!rejectionStatus.isOk()) return rejectionStatus;
    auto faceRes = faceJson(impl_->db.conn_, faceId, impl_->config.matchThreshold,
                            &exemplarCache, &rejectionCache);
    if (!faceRes.isOk()) return faceRes.status();
    if (faceRes.value()["revision"].get<int64_t>() != revision) return Status::alreadyExists("Face review is stale; reload before accepting a suggestion");
    std::optional<std::string> acceptedName;
    for (const auto& suggestion : faceRes.value()["suggestions"]) {
        if (suggestion["tag_id"].get<int64_t>() == tagId) acceptedName = suggestion["name"].get<std::string>();
    }
    if (!acceptedName) return Status::alreadyExists("That identity is no longer an eligible suggestion");
    auto updated = setIdentity(faceId, revision, tagId, std::nullopt, false);
    return updated;
}

Result<std::vector<SuggestionAcceptanceOutcome>> Service::acceptSuggestionsBatch(
    const std::vector<SuggestionAcceptance>& acceptances) {
    std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
    std::vector<int64_t> faceIds;
    faceIds.reserve(acceptances.size());
    for (const auto& acceptance : acceptances) faceIds.push_back(acceptance.id);
    RejectionCache rejectionCache;
    Status rejectionStatus = loadRejections(impl_->db.conn_, faceIds, rejectionCache);
    if (!rejectionStatus.isOk()) return rejectionStatus;

    ExemplarCache exemplarCache;
    std::vector<SuggestionAcceptanceOutcome> outcomes;
    outcomes.reserve(acceptances.size());
    for (const auto& acceptance : acceptances) {
        SuggestionAcceptanceOutcome outcome;
        outcome.id = acceptance.id;
        if (acceptance.id <= 0 || acceptance.revision <= 0 || acceptance.tagId <= 0) {
            outcome.status = Status::invalidArgument("face ID, revision, and tag_id must be positive");
            outcomes.push_back(std::move(outcome));
            continue;
        }
        FaceMatchInfo matchInfo;
        auto face = faceJson(impl_->db.conn_, acceptance.id, impl_->config.matchThreshold,
                             &exemplarCache, &rejectionCache, false, &matchInfo);
        if (!face.isOk()) {
            outcome.status = face.status();
            outcomes.push_back(std::move(outcome));
            continue;
        }
        if (face.value()["revision"].get<int64_t>() != acceptance.revision) {
            outcome.status = Status::alreadyExists("Face review is stale; reload before accepting a suggestion");
            outcomes.push_back(std::move(outcome));
            continue;
        }
        std::optional<std::string> acceptedName;
        for (const auto& suggestion : face.value()["suggestions"]) {
            if (suggestion["tag_id"].get<int64_t>() == acceptance.tagId) {
                acceptedName = suggestion["name"].get<std::string>();
                break;
            }
        }
        if (!acceptedName) {
            outcome.status = Status::alreadyExists("That identity is no longer an eligible suggestion");
            outcomes.push_back(std::move(outcome));
            continue;
        }

        auto updated = setIdentity(acceptance.id, acceptance.revision, acceptance.tagId,
                                   std::nullopt, false);
        if (!updated.isOk()) {
            outcome.status = updated.status();
            outcomes.push_back(std::move(outcome));
            continue;
        }
        outcome.face = std::move(updated.value());
        updateExemplarCache(exemplarCache, matchInfo, acceptance.id,
                            acceptance.tagId, *acceptedName);
        auto rejected = rejectionCache.find(acceptance.id);
        if (rejected != rejectionCache.end()) rejected->second.erase(acceptance.tagId);
        outcomes.push_back(std::move(outcome));
    }
    return outcomes;
}

Result<std::vector<SuggestionRejectionOutcome>> Service::rejectSuggestionsBatch(
    const std::vector<SuggestionRejection>& rejections) {
    std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
    std::vector<int64_t> faceIds;
    faceIds.reserve(rejections.size());
    for (const auto& r : rejections) faceIds.push_back(r.id);
    RejectionCache rejectionCache;
    Status rejectionStatus = loadRejections(impl_->db.conn_, faceIds, rejectionCache);
    if (!rejectionStatus.isOk()) return rejectionStatus;

    ExemplarCache exemplarCache;
    std::vector<SuggestionRejectionOutcome> outcomes;
    outcomes.reserve(rejections.size());
    Connection& conn = impl_->db.conn_;
    db::Transaction tx(conn);

    auto insRes = conn.prepare("INSERT OR IGNORE INTO face_rejections(face_id,tag_id,rejected_at) VALUES(?,?,?);");
    if (!insRes.isOk()) return insRes.status();
    auto ins = std::move(insRes.value());

    auto updRes = conn.prepare("UPDATE faces SET revision=revision+1,updated_at=? WHERE id=? AND revision=?;");
    if (!updRes.isOk()) return updRes.status();
    auto upd = std::move(updRes.value());

    const int64_t now = nowSeconds();

    for (const auto& r : rejections) {
        SuggestionRejectionOutcome outcome;
        outcome.id = r.id;
        if (r.id <= 0 || r.revision <= 0 || r.tagId <= 0) {
            outcome.status = Status::invalidArgument("face ID, revision, and tag_id must be positive");
            outcomes.push_back(std::move(outcome));
            continue;
        }

        FaceMatchInfo matchInfo;
        auto face = faceJson(conn, r.id, impl_->config.matchThreshold,
                             &exemplarCache, &rejectionCache, false, &matchInfo);
        if (!face.isOk()) {
            outcome.status = face.status();
            outcomes.push_back(std::move(outcome));
            continue;
        }
        if (face.value()["revision"].get<int64_t>() != r.revision) {
            outcome.status = Status::alreadyExists("Face review is stale; reload before rejecting a suggestion");
            outcomes.push_back(std::move(outcome));
            continue;
        }
        bool found = false;
        for (const auto& s : face.value()["suggestions"]) {
            if (s["tag_id"].get<int64_t>() == r.tagId) {
                found = true;
                break;
            }
        }
        if (!found) {
            outcome.status = Status::alreadyExists("That identity is no longer an eligible suggestion");
            outcomes.push_back(std::move(outcome));
            continue;
        }

        conn.execute("SAVEPOINT reject_item;");
        ins.reset();
        ins.bind(1, r.id);
        ins.bind(2, r.tagId);
        ins.bind(3, now);
        Status s = execDone(conn, ins, "Failed to record rejected identity");
        if (!s.isOk()) {
            conn.execute("ROLLBACK TO SAVEPOINT reject_item;");
            conn.execute("RELEASE SAVEPOINT reject_item;");
            outcome.status = s;
            outcomes.push_back(std::move(outcome));
            continue;
        }

        upd.reset();
        upd.bind(1, now);
        upd.bind(2, r.id);
        upd.bind(3, r.revision);
        s = execDone(conn, upd, "Failed to update face revision");
        if (!s.isOk() || conn.changes() == 0) {
            conn.execute("ROLLBACK TO SAVEPOINT reject_item;");
            conn.execute("RELEASE SAVEPOINT reject_item;");
            outcome.status = !s.isOk() ? s : Status::alreadyExists("Face review is stale; reload before rejecting a suggestion");
            outcomes.push_back(std::move(outcome));
            continue;
        }

        conn.execute("RELEASE SAVEPOINT reject_item;");

        rejectionCache[r.id].insert(r.tagId);

        auto updatedFace = faceJson(conn, r.id, impl_->config.matchThreshold,
                                    &exemplarCache, &rejectionCache, false, nullptr, true);
        if (updatedFace.isOk()) {
            outcome.face = std::move(updatedFace.value());
        } else {
            outcome.face = std::move(face.value());
            outcome.face["revision"] = r.revision + 1;
            outcome.face["crop_url"] = "/api/faces/" + std::to_string(r.id) +
                                       "/crop?revision=" + std::to_string(r.revision + 1);
            auto& suggs = outcome.face["suggestions"];
            suggs.erase(std::remove_if(suggs.begin(), suggs.end(), [&](const nlohmann::json& s) {
                return s["tag_id"].get<int64_t>() == r.tagId;
            }), suggs.end());
        }

        outcome.status = Status::ok();
        outcomes.push_back(std::move(outcome));
    }

    Status commitStatus = tx.commit();
    if (!commitStatus.isOk()) return commitStatus;
    return outcomes;
}

Result<json> Service::rejectSuggestion(int64_t faceId, int64_t revision, TagId tagId) {
    if (faceId <= 0 || revision <= 0 || tagId <= 0) return Status::invalidArgument("face ID, revision, and tag_id must be positive");
    auto outcomes = rejectSuggestionsBatch({{faceId, revision, tagId}});
    if (!outcomes.isOk()) return outcomes.status();
    if (outcomes.value().empty()) return Status::internal("Empty outcome for reject suggestion");
    if (!outcomes.value()[0].status.isOk()) return outcomes.value()[0].status;
    return std::move(outcomes.value()[0].face);
}

Result<json> Service::setDismissed(int64_t faceId, int64_t revision, bool dismissed) {
    if (faceId <= 0 || revision <= 0) return Status::invalidArgument("face ID and revision must be positive");
    std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
    Connection& conn = impl_->db.conn_;
    db::Transaction tx(conn);
    auto getRes = conn.prepare("SELECT media_id,person_tag_id,revision FROM faces WHERE id=?;");
    if (!getRes.isOk()) return getRes.status();
    auto get = std::move(getRes.value()); get.bind(1, faceId);
    if (get.step() != StepResult::Row) return Status::notFound("Face not found");
    MediaId mediaId = get.getInt64(0);
    std::optional<TagId> tagId;
    if (!get.isNull(1)) tagId = get.getInt64(1);
    if (get.getInt64(2) != revision) return Status::alreadyExists("Face review is stale; reload before changing dismissal");
    auto updateRes = conn.prepare("UPDATE faces SET dismissed=?,revision=revision+1,updated_at=? WHERE id=? AND revision=?;");
    if (!updateRes.isOk()) return updateRes.status();
    auto update = std::move(updateRes.value()); update.bind(1, dismissed ? 1 : 0);
    update.bind(2, nowSeconds()); update.bind(3, faceId); update.bind(4, revision);
    Status s = execDone(conn, update, "Failed to update face dismissal"); if (!s.isOk()) return s;
    if (tagId) { s = syncFaceTagOwnership(conn, mediaId, *tagId); if (!s.isOk()) return s; }
    s = tx.commit(); if (!s.isOk()) return s;
    return faceJson(conn, faceId, impl_->config.matchThreshold);
}

Result<std::string> Service::cropPath(int64_t faceId, int64_t revision) {
    if (faceId <= 0 || revision <= 0) return Status::invalidArgument("face ID and revision must be positive");
    MediaId mediaId = 0;
    double x=0,y=0,w=0,h=0;
    std::string sourceHash, path;
    int orientation = 1;
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
        auto stmtRes = impl_->db.conn_.prepare(R"SQL(
            SELECT f.media_id,f.x,f.y,f.width,f.height,f.revision,a.source_hash,m.content_hash,m.file_path,m.orientation,a.state
            FROM faces f JOIN face_media_analysis a ON a.media_id=f.media_id
            JOIN media_items m ON m.id=f.media_id WHERE f.id=?;
        )SQL");
        if (!stmtRes.isOk()) return stmtRes.status();
        auto stmt = std::move(stmtRes.value()); stmt.bind(1, faceId);
        if (stmt.step() != StepResult::Row) return Status::notFound("Face not found");
        if (stmt.getInt64(5) != revision) return Status::alreadyExists("Face crop is stale; reload the face first");
        if (stmt.getString(10) != "complete") return Status::notFound("Face geometry is unavailable because the latest scan failed");
        if (stmt.getString(6) != stmt.getString(7)) return Status::notFound("Face geometry is stale because the photo changed");
        mediaId=stmt.getInt64(0); x=stmt.getDouble(1); y=stmt.getDouble(2); w=stmt.getDouble(3); h=stmt.getDouble(4);
        sourceHash=stmt.getString(6); path=stmt.getString(8); orientation=stmt.getInt(9);
    }
    std::string photoPath = impl_->resolver ? impl_->resolver(path) : path;
    auto currentHash = metadata::Hasher::computeFileSha256(photoPath);
    if (!currentHash.isOk()) return currentHash.status();
    if (currentHash.value() != sourceHash) return Status::notFound("Face geometry is stale because the photo file changed");
    std::filesystem::path cropRoot = pathFromUtf8(impl_->cacheDirectory) / "faces";
    std::filesystem::path cropFile = cropRoot / (sourceHash + "-" + std::to_string(mediaId) + "-" + std::to_string(faceId) + "-" + std::to_string(revision) + ".jpg");
    std::error_code ec;
    if (std::filesystem::is_regular_file(cropFile, ec)) return pathToUtf8(cropFile);
    auto loaded = thumbnail::Generator::loadImage(photoPath);
    if (!loaded.isOk()) {
        IMAGINE_LOG_ERROR("Face crop unable to open/load image file: " + photoPath + " (" + loaded.status().message() + ")");
        return loaded.status();
    }
    auto oriented = orientRgb(loaded.value(), orientation);
    const auto& orientedImage = oriented.image();
    int left = std::clamp(static_cast<int>(std::floor(x)), 0, orientedImage.width);
    int top = std::clamp(static_cast<int>(std::floor(y)), 0, orientedImage.height);
    int right = std::clamp(static_cast<int>(std::ceil(x+w)), 0, orientedImage.width);
    int bottom = std::clamp(static_cast<int>(std::ceil(y+h)), 0, orientedImage.height);
    if (right <= left || bottom <= top) return Status::invalidArgument("Face rectangle is outside the source image");
    auto cropped = thumbnail::Generator::crop(orientedImage, left, top, right-left, bottom-top);
    if (!cropped.isOk()) return cropped.status();
    std::string tempPath = pathToUtf8(cropFile) + ".tmp-" +
        std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    Status saved = thumbnail::Generator::saveJpeg(cropped.value(), tempPath, 90);
    if (!saved.isOk()) return saved;
    std::filesystem::rename(tempPath, cropFile, ec);
    if (ec) {
        std::filesystem::remove(tempPath, ec);
        if (std::filesystem::is_regular_file(cropFile, ec)) return pathToUtf8(cropFile);
        return Status::ioError("Failed to publish face crop: " + ec.message());
    }
    return pathToUtf8(cropFile);
}

void Service::runJob(int64_t jobId, std::vector<MediaId> mediaIds, bool force) {
    IMAGINE_LOG_INFO("Face-analysis job " + std::to_string(jobId) + " started processing " + std::to_string(mediaIds.size()) + " items");
    auto updateProgress = [&](const std::string& kind, const std::string& error = "") {
        std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
        std::string sql = "UPDATE face_analysis_jobs SET " + kind + "=" + kind + "+1,remaining=MAX(0,total-processed-skipped-failed-1),updated_at=?,error=CASE WHEN error='' AND ?<>'' THEN ? ELSE error END WHERE id=?;";
        auto updateRes = impl_->db.conn_.prepare(sql);
        if (!updateRes.isOk()) return;
        auto update = std::move(updateRes.value()); update.bind(1, nowSeconds()); update.bind(2, error); update.bind(3, error); update.bind(4, jobId);
        update.step();
    };
    auto finish = [&](const std::string& state, const std::string& error = "") {
        std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
        auto updateRes = impl_->db.conn_.prepare("UPDATE face_analysis_jobs SET state=?,error=CASE WHEN ?<>'' THEN ? ELSE error END,updated_at=? WHERE id=?;");
        if (updateRes.isOk()) {
            auto update = std::move(updateRes.value()); update.bind(1,state); update.bind(2,error); update.bind(3,error); update.bind(4,nowSeconds()); update.bind(5,jobId); update.step();
        }
        if (state == "failed") {
            IMAGINE_LOG_ERROR("Face-analysis job " + std::to_string(jobId) + " failed: " + error);
        } else if (state == "cancelled") {
            IMAGINE_LOG_INFO("Face-analysis job " + std::to_string(jobId) + " was cancelled");
        } else {
            IMAGINE_LOG_INFO("Face-analysis job " + std::to_string(jobId) + " completed successfully");
        }
    };
    auto recordFailure = [&](MediaId mediaId, const MediaItem& resultMedia,
                             const MediaItem& catalogSnapshot, const std::string& error) {
        IMAGINE_LOG_ERROR("Face scan error for media ID " + std::to_string(mediaId) +
                          " (" + resultMedia.file_path + "): " + error);
        Status saved;
        {
            std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
            db::Transaction tx(impl_->db.conn_);
            saved = saveAnalysisFailure(impl_->db.conn_, mediaId, resultMedia, catalogSnapshot,
                                        impl_->runtime, impl_->config, error);
            if (saved.isOk()) saved = tx.commit();
        }
        if (saved.code() == StatusCode::AlreadyExists) updateProgress("skipped", saved.message());
        else updateProgress("failed", saved.isOk() ? error : saved.message());
    };

    try {
        const RuntimeInfo runtime = impl_->runtime;
        SourcePrefetcher sourcePrefetcher;
        std::optional<std::future<Result<EncodedSource>>> prefetchedSource;
        size_t prefetchedIndex = mediaIds.size();
        auto discardPrefetch = [&]() {
            if (!prefetchedSource) return;
            try { (void)prefetchedSource->get(); } catch (...) {}
            prefetchedSource.reset();
            prefetchedIndex = mediaIds.size();
        };

        for (size_t index = 0; index < mediaIds.size(); ++index) {
            if (impl_->cancelRequested.load()) {
                discardPrefetch();
                finish("cancelled");
                return;
            }
            const MediaId mediaId = mediaIds[index];
            auto mediaRes = impl_->db.getMediaById(mediaId);
            if (!mediaRes.isOk()) {
                IMAGINE_LOG_ERROR("Face scan unable to find media ID " + std::to_string(mediaId) + ": " + mediaRes.status().message());
                updateProgress("failed", mediaRes.status().message()); continue;
            }
            MediaItem media = mediaRes.value();
            if (media.media_type != "photo") { updateProgress("skipped"); continue; }

            bool reusable = false;
            bool preserveIdentities = false;
            bool requiresForce = false;
            {
                std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
                reusable = !force && analysisReusable(impl_->db.conn_, media, runtime, impl_->config);
                preserveIdentities = !force && recognizerRefreshNeeded(impl_->db.conn_, media, runtime, impl_->config);
                requiresForce = !force && detectorRefreshRequiresForce(impl_->db.conn_, media, runtime, impl_->config);
            }
            if (reusable || requiresForce) {
                if (reusable) {
                    updateProgress("skipped");
                } else {
                    updateProgress("skipped", "Detector or analysis settings changed; use force to replace reviewed face results");
                }
                if (prefetchedSource && prefetchedIndex == index) discardPrefetch();
                continue;
            }

            std::string photoPath = impl_->resolver ? impl_->resolver(media.file_path) : media.file_path;
            Result<EncodedSource> source = [&]() -> Result<EncodedSource> {
                if (prefetchedSource && prefetchedIndex == index) {
                    auto ready = std::move(*prefetchedSource);
                    prefetchedSource.reset();
                    prefetchedIndex = mediaIds.size();
                    return ready.get();
                }
                return readEncodedSource(photoPath);
            }();
            if (!source.isOk() || source.value().hash != media.content_hash) {
                std::string message = source.isOk() ? "Photo file changed since it was cataloged; reimport it before face analysis" : source.status().message();
                IMAGINE_LOG_ERROR("Face scan unable to verify/open photo file for media ID " + std::to_string(mediaId) + " (" + photoPath + "): " + message);
                recordFailure(mediaId, media, media, message);
                continue;
            }

            auto loaded = [&]() {
                auto encodedBytes = std::move(source.value().bytes);
                return thumbnail::Generator::loadImageFromMemory(encodedBytes.data(), encodedBytes.size());
            }();
            if (!loaded.isOk()) {
                IMAGINE_LOG_ERROR("Face scan unable to open/load image file for media ID " + std::to_string(mediaId) + " (" + photoPath + "): " + loaded.status().message());
                recordFailure(mediaId, media, media, loaded.status().message());
                continue;
            }
            auto oriented = orientRgb(loaded.value(), media.exif.orientation);

            if (!impl_->cancelRequested.load()) {
                for (size_t nextIndex = index + 1; nextIndex < mediaIds.size(); ++nextIndex) {
                    if (impl_->cancelRequested.load()) break;
                    auto nextMedia = impl_->db.getMediaById(mediaIds[nextIndex]);
                    if (!nextMedia.isOk() || nextMedia.value().media_type != "photo") continue;
                    bool nextReusable = false;
                    bool nextRequiresForce = false;
                    {
                        std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
                        nextReusable = !force && analysisReusable(impl_->db.conn_, nextMedia.value(), runtime, impl_->config);
                        nextRequiresForce = !force && detectorRefreshRequiresForce(impl_->db.conn_, nextMedia.value(), runtime, impl_->config);
                    }
                    if (nextReusable || nextRequiresForce) break;
                    std::string nextPath = impl_->resolver ? impl_->resolver(nextMedia.value().file_path)
                                                           : nextMedia.value().file_path;
                    if (impl_->cancelRequested.load()) break;
                    if (auto scheduled = sourcePrefetcher.schedule(std::move(nextPath))) {
                        prefetchedSource.emplace(std::move(*scheduled));
                        prefetchedIndex = nextIndex;
                    } else {
                        prefetchedSource.reset();
                        prefetchedIndex = mediaIds.size();
                    }
                    break;
                }
            }

            const auto& orientedImage = oriented.image();
            MediaItem analysisMedia = media;
            analysisMedia.width = orientedImage.width;
            analysisMedia.height = orientedImage.height;
            auto detected = impl_->analyzer ? impl_->analyzer(orientedImage) : impl_->engine.analyze(orientedImage);
            if (!detected.isOk()) {
                IMAGINE_LOG_ERROR("Face scan inference failed for media ID " + std::to_string(mediaId) + " (" + photoPath + "): " + detected.status().message());
                recordFailure(mediaId, analysisMedia, media, detected.status().message());
                continue;
            }
            Status geometryStatus = normalizeCandidateGeometry(detected.value(), orientedImage.width, orientedImage.height);
            if (!geometryStatus.isOk()) {
                IMAGINE_LOG_ERROR("Face scan candidate geometry invalid for media ID " + std::to_string(mediaId) + " (" + photoPath + "): " + geometryStatus.message());
                recordFailure(mediaId, analysisMedia, media, geometryStatus.message());
                continue;
            }
            auto hashAfter = metadata::Hasher::computeFileSha256(photoPath);
            if (!hashAfter.isOk() || hashAfter.value() != media.content_hash) {
                const std::string message = "Photo file changed during face analysis; reimport it before face analysis";
                IMAGINE_LOG_ERROR("Face scan file changed during analysis for media ID " + std::to_string(mediaId) + " (" + photoPath + "): " + message);
                recordFailure(mediaId, analysisMedia, media, message);
                continue;
            }

            Status persisted;
            {
                std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
                persisted = persistAnalysis(impl_->db.conn_, mediaId, analysisMedia, media,
                                           runtime, impl_->config, detected.value(), preserveIdentities);
            }
            if (!persisted.isOk()) {
                if (persisted.code() == StatusCode::AlreadyExists) { updateProgress("skipped", persisted.message()); }
                else {
                    IMAGINE_LOG_ERROR("Face scan failed to persist results for media ID " + std::to_string(mediaId) + ": " + persisted.message());
                    updateProgress("failed", persisted.message());
                }
                continue;
            }
            updateProgress("processed");
        }
        finish(impl_->cancelRequested.load() ? "cancelled" : "completed");
    } catch (const std::exception& ex) {
        IMAGINE_LOG_ERROR("Face-analysis job " + std::to_string(jobId) + " exception: " + std::string(ex.what()));
        finish("failed", ex.what());
    } catch (...) {
        IMAGINE_LOG_ERROR("Face-analysis job " + std::to_string(jobId) + " unknown failure");
        finish("failed", "Unknown face-analysis failure");
    }
}

} // namespace imagine::faces
