#include "imagine/faces/service.hpp"

#include "imagine/db/catalog_db.hpp"
#include "imagine/faces/engine.hpp"
#include "imagine/thumbnail/generator.hpp"
#include "imagine/metadata/hasher.hpp"
#include "imagine/common/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <numeric>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace imagine::faces {
namespace {

using nlohmann::json;
using db::Connection;
using db::Statement;
using db::StepResult;

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

std::vector<Suggestion> suggestionsFor(Connection& conn, int64_t faceId,
                                       const std::vector<float>& query,
                                       const std::string& recognizerChecksum,
                                       const std::string& pipelineVersion,
                                       float threshold, ExemplarCache* cache = nullptr) {
    std::vector<Suggestion> result;
    if (query.size() != 512 || recognizerChecksum.empty() || pipelineVersion.empty()) return result;

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

    auto rejectedRes = conn.prepare("SELECT tag_id FROM face_rejections WHERE face_id=?;");
    if (!rejectedRes.isOk()) return result;
    auto rejectedStmt = std::move(rejectedRes.value()); rejectedStmt.bind(1, faceId);
    std::unordered_set<TagId> rejected;
    while (rejectedStmt.step() == StepResult::Row) rejected.insert(rejectedStmt.getInt64(0));

    std::unordered_map<TagId, Suggestion> best;
    for (const auto& exemplar : *exemplars) {
        if (exemplar.faceId == faceId || rejected.contains(exemplar.tagId)) continue;
        double dot = 0;
        for (size_t i = 0; i < query.size(); ++i) dot += static_cast<double>(query[i]) * exemplar.embedding[i];
        float score = static_cast<float>(dot);
        if (!std::isfinite(score) || score < threshold) continue;
        score = std::clamp(score, -1.0f, 1.0f);
        auto it = best.find(exemplar.tagId);
        if (it == best.end() || score > it->second.score) {
            best[exemplar.tagId] = Suggestion{exemplar.tagId, exemplar.name, score};
        }
    }
    for (auto& [id, s] : best) result.push_back(std::move(s));
    std::sort(result.begin(), result.end(), [](const Suggestion& a, const Suggestion& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.name < b.name;
    });
    if (result.size() > 3) result.resize(3);
    return result;
}

Result<json> faceJson(Connection& conn, int64_t faceId, float matchThreshold,
                      ExemplarCache* exemplarCache = nullptr) {
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
    if (stmt.getString(17) == "complete" && stmt.getString(12).empty() &&
        !stmt.isNull(13) && stmt.getInt(14) == 512) {
        auto embedding = readVector(stmt, 13, 512);
        auto suggestions = suggestionsFor(conn, faceId, embedding, stmt.getString(15),
                                          stmt.getString(16), matchThreshold, exemplarCache);
        for (const auto& s : suggestions) {
            out["suggestions"].push_back({{"tag_id", s.tagId}, {"name", s.name}, {"score", s.score}});
        }
    }
    return out;
}

Status execDone(Connection& conn, Statement& stmt, const std::string& context) {
    if (stmt.step() != StepResult::Done) return dbError(conn, context);
    return Status::ok();
}

Status syncFaceTagOwnership(Connection& conn, MediaId mediaId, TagId tagId) {
    auto countRes = conn.prepare(
        "SELECT COUNT(*) FROM faces WHERE media_id=? AND person_tag_id=? AND dismissed=0;");
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

thumbnail::ImageBuffer orientRgb(const thumbnail::ImageBuffer& src, int orientation) {
    return thumbnail::Generator::rotate(src, orientation);
}

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
            auto mediaRes = impl_->db.conn_.prepare("SELECT id FROM media_items WHERE media_type='photo' ORDER BY id;");
            if (!mediaRes.isOk()) return mediaRes.status();
            auto media = std::move(mediaRes.value());
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
    while (faces.step() == StepResult::Row) {
        auto face = faceJson(impl_->db.conn_, faces.getInt64(0), impl_->config.matchThreshold);
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
    json items = json::array();
    while (stmt.step() == StepResult::Row) {
        auto face = faceJson(impl_->db.conn_, stmt.getInt64(0), impl_->config.matchThreshold);
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
    json items = json::array();
    ExemplarCache exemplarCache;
    while (itemStmt.step() == StepResult::Row) {
        auto face = faceJson(conn, itemStmt.getInt64(0), impl_->config.matchThreshold, &exemplarCache);
        if (face.isOk()) items.push_back(std::move(face.value()));
    }
    return json{{"items", std::move(items)}, {"total", total}};
}

Result<json> Service::setIdentity(int64_t faceId, int64_t revision, std::optional<TagId> tagId,
                                  const std::optional<std::string>& name) {
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
    return faceJson(conn, faceId, impl_->config.matchThreshold);
}

Result<json> Service::acceptSuggestion(int64_t faceId, int64_t revision, TagId tagId) {
    if (faceId <= 0 || revision <= 0 || tagId <= 0) return Status::invalidArgument("face ID, revision, and tag_id must be positive");
    std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
    auto faceRes = faceJson(impl_->db.conn_, faceId, impl_->config.matchThreshold);
    if (!faceRes.isOk()) return faceRes.status();
    if (faceRes.value()["revision"].get<int64_t>() != revision) return Status::alreadyExists("Face review is stale; reload before accepting a suggestion");
    bool found = false;
    for (const auto& suggestion : faceRes.value()["suggestions"]) if (suggestion["tag_id"].get<int64_t>() == tagId) found = true;
    if (!found) return Status::alreadyExists("That identity is no longer an eligible suggestion");
    return setIdentity(faceId, revision, tagId, std::nullopt);
}

Result<json> Service::rejectSuggestion(int64_t faceId, int64_t revision, TagId tagId) {
    if (faceId <= 0 || revision <= 0 || tagId <= 0) return Status::invalidArgument("face ID, revision, and tag_id must be positive");
    std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
    Connection& conn = impl_->db.conn_;
    db::Transaction tx(conn);
    auto faceRes = faceJson(conn, faceId, impl_->config.matchThreshold);
    if (!faceRes.isOk()) return faceRes.status();
    if (faceRes.value()["revision"].get<int64_t>() != revision) return Status::alreadyExists("Face review is stale; reload before rejecting a suggestion");
    bool found = false;
    for (const auto& suggestion : faceRes.value()["suggestions"]) if (suggestion["tag_id"].get<int64_t>() == tagId) found = true;
    if (!found) return Status::alreadyExists("That identity is no longer an eligible suggestion");
    auto insRes = conn.prepare("INSERT OR IGNORE INTO face_rejections(face_id,tag_id,rejected_at) VALUES(?,?,?);");
    if (!insRes.isOk()) return insRes.status();
    auto ins = std::move(insRes.value()); ins.bind(1, faceId); ins.bind(2, tagId); ins.bind(3, nowSeconds());
    Status s = execDone(conn, ins, "Failed to record rejected identity"); if (!s.isOk()) return s;
    auto updRes = conn.prepare("UPDATE faces SET revision=revision+1,updated_at=? WHERE id=? AND revision=?;");
    if (!updRes.isOk()) return updRes.status();
    auto upd = std::move(updRes.value()); upd.bind(1, nowSeconds()); upd.bind(2, faceId); upd.bind(3, revision);
    s = execDone(conn, upd, "Failed to update face revision"); if (!s.isOk()) return s;
    s = tx.commit(); if (!s.isOk()) return s;
    return faceJson(conn, faceId, impl_->config.matchThreshold);
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
    if (!loaded.isOk()) return loaded.status();
    auto oriented = orientRgb(loaded.value(), orientation);
    int left = std::clamp(static_cast<int>(std::floor(x)), 0, oriented.width);
    int top = std::clamp(static_cast<int>(std::floor(y)), 0, oriented.height);
    int right = std::clamp(static_cast<int>(std::ceil(x+w)), 0, oriented.width);
    int bottom = std::clamp(static_cast<int>(std::ceil(y+h)), 0, oriented.height);
    if (right <= left || bottom <= top) return Status::invalidArgument("Face rectangle is outside the source image");
    auto cropped = thumbnail::Generator::crop(oriented, left, top, right-left, bottom-top);
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
    };
    auto recordFailure = [&](MediaId mediaId, const MediaItem& resultMedia,
                             const MediaItem& catalogSnapshot, const std::string& error) {
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
        for (MediaId mediaId : mediaIds) {
            if (impl_->cancelRequested.load()) { finish("cancelled"); return; }
            auto mediaRes = impl_->db.getMediaById(mediaId);
            if (!mediaRes.isOk()) {
                updateProgress("failed", mediaRes.status().message()); continue;
            }
            MediaItem media = mediaRes.value();
            if (media.media_type != "photo") { updateProgress("skipped"); continue; }

            std::string photoPath = impl_->resolver ? impl_->resolver(media.file_path) : media.file_path;
            auto hashBefore = metadata::Hasher::computeFileSha256(photoPath);
            if (!hashBefore.isOk() || hashBefore.value() != media.content_hash) {
                std::string message = hashBefore.isOk() ? "Photo file changed since it was cataloged; reimport it before face analysis" : hashBefore.status().message();
                recordFailure(mediaId, media, media, message);
                continue;
            }

            bool reusable = false;
            bool preserveIdentities = false;
            bool requiresForce = false;
            {
                std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
                reusable = !force && analysisReusable(impl_->db.conn_, media, runtime, impl_->config);
                preserveIdentities = !force && recognizerRefreshNeeded(impl_->db.conn_, media, runtime, impl_->config);
                requiresForce = !force && detectorRefreshRequiresForce(impl_->db.conn_, media, runtime, impl_->config);
            }
            if (reusable) { updateProgress("skipped"); continue; }
            if (requiresForce) {
                updateProgress("skipped", "Detector or analysis settings changed; use force to replace reviewed face results");
                continue;
            }
            auto loaded = thumbnail::Generator::loadImage(photoPath);
            if (!loaded.isOk()) {
                recordFailure(mediaId, media, media, loaded.status().message());
                continue;
            }
            auto oriented = orientRgb(loaded.value(), media.exif.orientation);
            MediaItem analysisMedia = media;
            analysisMedia.width = oriented.width;
            analysisMedia.height = oriented.height;
            auto detected = impl_->analyzer ? impl_->analyzer(oriented) : impl_->engine.analyze(oriented);
            if (!detected.isOk()) {
                recordFailure(mediaId, analysisMedia, media, detected.status().message());
                continue;
            }
            Status geometryStatus = normalizeCandidateGeometry(detected.value(), oriented.width, oriented.height);
            if (!geometryStatus.isOk()) {
                recordFailure(mediaId, analysisMedia, media, geometryStatus.message());
                continue;
            }
            auto hashAfter = metadata::Hasher::computeFileSha256(photoPath);
            if (!hashAfter.isOk() || hashAfter.value() != media.content_hash) {
                const std::string message = "Photo file changed during face analysis; reimport it before face analysis";
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
                else { updateProgress("failed", persisted.message()); }
                continue;
            }
            updateProgress("processed");
        }
        finish(impl_->cancelRequested.load() ? "cancelled" : "completed");
    } catch (const std::exception& ex) {
        finish("failed", ex.what());
    } catch (...) {
        finish("failed", "Unknown face-analysis failure");
    }
}

} // namespace imagine::faces
