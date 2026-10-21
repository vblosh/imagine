#include "imagine/clip/service.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <shared_mutex>
#include <thread>
#include <unordered_set>

#include "imagine/db/catalog_db.hpp"
#include "imagine/core/deduplicator.hpp"
#include "imagine/common/logger.hpp"
#include "imagine/thumbnail/generator.hpp"
#include "imagine/thumbnail/cache.hpp"
#include "imagine/metadata/hasher.hpp"

#if defined(_MSC_VER)
#include <intrin.h>
#include <immintrin.h>
#elif defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#include <immintrin.h>
#endif

namespace imagine::clip {

namespace {

using db::Connection;
using db::Statement;
using db::StepResult;
using nlohmann::json;

int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string envString(const char* key, const std::string& fallback) {
    const char* value = std::getenv(key);
    return value ? std::string(value) : fallback;
}

int envInt(const char* key, int fallback) {
    const char* value = std::getenv(key);
    if (!value || !*value) return fallback;
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

inline float dotProductScalar(const float* a, const float* b, int dim) {
    double dot = 0;
    for (int i = 0; i < dim; ++i) {
        dot += static_cast<double>(a[i]) * b[i];
    }
    return static_cast<float>(dot);
}

#if (defined(_M_X64) || defined(__x86_64__))
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2,fma")))
#endif
inline float dotProductAvx2(const float* a, const float* b, int dim) {
    __m256d sum0 = _mm256_setzero_pd();
    __m256d sum1 = _mm256_setzero_pd();
    __m256d sum2 = _mm256_setzero_pd();
    __m256d sum3 = _mm256_setzero_pd();
    int i = 0;
    for (; i <= dim - 16; i += 16) {
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
    _mm256_storeu_pd(vals, sum);
    double dot = vals[0] + vals[1] + vals[2] + vals[3];
    for (; i < dim; ++i) {
        dot += static_cast<double>(a[i]) * b[i];
    }
    return static_cast<float>(dot);
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

inline float dotProduct(const float* a, const float* b, int dim) {
#if (defined(_M_X64) || defined(__x86_64__))
    if (hasAvx2Fma()) {
        return dotProductAvx2(a, b, dim);
    }
#endif
    return dotProductScalar(a, b, dim);
}

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

void bindEmbedding(Statement& stmt, int index, const float* data, size_t size) {
    sqlite3_stmt* raw = stmt.raw();
    if (!data || size == 0) {
        sqlite3_bind_null(raw, index);
    } else {
        sqlite3_bind_blob(raw, index, data,
                          static_cast<int>(size * sizeof(float)), SQLITE_TRANSIENT);
    }
}

void bindEmbedding(Statement& stmt, int index, const std::vector<float>& embedding) {
    bindEmbedding(stmt, index, embedding.data(), embedding.size());
}

} // namespace

struct Service::Impl {
    db::CatalogDb& db;
    std::string cacheDirectory;
    PathResolver resolver;
    ClipConfig config;
    Engine engine;
    
    struct VectorIndex {
        int dim{0};
        std::vector<MediaId> ids;
        std::vector<float> vectors;
        mutable std::shared_mutex mutex;
        
        void add(MediaId id, const float* embedding, int d) {
            std::unique_lock lock(mutex);
            if (dim == 0 || ids.empty()) dim = d;
            if (d != dim) return;
            auto it = std::lower_bound(ids.begin(), ids.end(), id);
            if (it != ids.end() && *it == id) {
                size_t idx = std::distance(ids.begin(), it);
                std::memcpy(&vectors[idx * dim], embedding, dim * sizeof(float));
            } else {
                size_t idx = std::distance(ids.begin(), it);
                ids.insert(it, id);
                vectors.insert(vectors.begin() + idx * dim, embedding, embedding + dim);
            }
        }
        
        void remove(MediaId id) {
            std::unique_lock lock(mutex);
            auto it = std::lower_bound(ids.begin(), ids.end(), id);
            if (it != ids.end() && *it == id) {
                size_t idx = std::distance(ids.begin(), it);
                ids.erase(it);
                vectors.erase(vectors.begin() + idx * dim, vectors.begin() + (idx + 1) * dim);
            }
        }
        
        void clear() {
            std::unique_lock lock(mutex);
            dim = 0;
            ids.clear();
            vectors.clear();
        }

        void loadBulk(std::vector<MediaId> newIds, std::vector<float> newVectors, int d) {
            std::unique_lock lock(mutex);
            dim = d;
            ids = std::move(newIds);
            vectors = std::move(newVectors);
        }

        std::vector<float> get(MediaId id, int& outDim) const {
            std::shared_lock lock(mutex);
            auto it = std::lower_bound(ids.begin(), ids.end(), id);
            if (it == ids.end() || *it != id || dim <= 0) return {};
            size_t idx = static_cast<size_t>(std::distance(ids.begin(), it));
            outDim = dim;
            const float* start = &vectors[idx * static_cast<size_t>(dim)];
            return std::vector<float>(start, start + dim);
        }
        
        bool has(MediaId id) const {
            std::shared_lock lock(mutex);
            auto it = std::lower_bound(ids.begin(), ids.end(), id);
            return (it != ids.end() && *it == id);
        }

        std::vector<SearchHit> searchUnlocked(const float* query, int k, int d,
                                              const std::unordered_set<MediaId>* allowedIds = nullptr) const {
            if (ids.empty() || dim != d) return {};
            std::vector<SearchHit> hits;
            hits.reserve(allowedIds ? std::min(ids.size(), allowedIds->size()) : ids.size());
            for (size_t i = 0; i < ids.size(); ++i) {
                MediaId id = ids[i];
                if (allowedIds && allowedIds->find(id) == allowedIds->end()) {
                    continue;
                }
                hits.push_back(SearchHit{id, dotProduct(query, &vectors[i * dim], dim)});
            }
            if (k > 0 && k < static_cast<int>(hits.size())) {
                std::partial_sort(hits.begin(), hits.begin() + k, hits.end(),
                                  [](const SearchHit& a, const SearchHit& b) { return a.score > b.score; });
                hits.resize(k);
            } else {
                std::sort(hits.begin(), hits.end(),
                          [](const SearchHit& a, const SearchHit& b) { return a.score > b.score; });
            }
            return hits;
        }

        std::vector<SearchHit> search(const float* query, int k, int d,
                                      const std::unordered_set<MediaId>* allowedIds = nullptr) const {
            std::shared_lock lock(mutex);
            return searchUnlocked(query, k, d, allowedIds);
        }

        std::vector<VisualDuplicatePair> findDuplicatePairs(
            float threshold,
            const std::unordered_set<MediaId>& allowedIds
        ) const {
            std::shared_lock lock(mutex);
            if (ids.size() < 2 || dim <= 0) return {};

            std::vector<size_t> indices;
            indices.reserve(ids.size());
            for (size_t i = 0; i < ids.size(); ++i) {
                if (allowedIds.empty() || allowedIds.count(ids[i])) {
                    indices.push_back(i);
                }
            }
            if (indices.size() < 2) return {};

            std::unordered_map<uint64_t, float> pairMap;
            constexpr int kCandidatesPerSeed = 50;

            for (size_t idxA : indices) {
                MediaId idA = ids[idxA];
                const float* vecA = &vectors[idxA * dim];
                auto hits = searchUnlocked(vecA, kCandidatesPerSeed + 1, dim, allowedIds.empty() ? nullptr : &allowedIds);
                for (const auto& hit : hits) {
                    if (hit.mediaId == idA) continue;
                    if (hit.score < threshold) continue;
                    MediaId low = std::min(idA, hit.mediaId);
                    MediaId high = std::max(idA, hit.mediaId);
                    uint64_t key = (static_cast<uint64_t>(low) << 32) | (static_cast<uint64_t>(high) & 0xFFFFFFFFULL);
                    auto it = pairMap.find(key);
                    if (it == pairMap.end() || hit.score > it->second) {
                        pairMap[key] = hit.score;
                    }
                }
            }

            std::vector<VisualDuplicatePair> pairs;
            pairs.reserve(pairMap.size());
            for (const auto& [key, sim] : pairMap) {
                pairs.push_back(VisualDuplicatePair{
                    static_cast<MediaId>(key >> 32),
                    static_cast<MediaId>(key & 0xFFFFFFFFULL),
                    sim
                });
            }
            return pairs;
        }
    } index;
    
    std::atomic<bool> cancelRequested{false};
    std::mutex jobStartMutex;
    
    Impl(db::CatalogDb& db, std::string cacheDir, PathResolver res)
        : db(db), cacheDirectory(std::move(cacheDir)), resolver(std::move(res)),
          config{
              envString("IMAGINE_CLIP_MODEL_DIR", cacheDirectory + "/clip_models"),
              envString("IMAGINE_CLIP_DEVICE", "cpu"),
              4
          },
          engine(config) {}

    int idleUnloadSec{30};
    std::mutex idleMutex;
    std::condition_variable idleCv;
    bool idleWorkerStop{false};
    std::chrono::steady_clock::time_point lastActivityTime{};
    bool jobRunning{false};
    int activeSearchCount{0};
    std::jthread idleWorker;
    std::jthread worker;

    void idleLoop(std::stop_token stopToken) {
        std::unique_lock lock(idleMutex);
        while (!stopToken.stop_requested() && !idleWorkerStop) {
            const bool busy = jobRunning || activeSearchCount > 0;
            if (!busy && engine.isLoaded()) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastActivityTime).count();
                if (elapsed >= idleUnloadSec) {
                    bool shouldUnload = false;
                    if (!jobRunning && activeSearchCount == 0) {
                        shouldUnload = true;
                    }
                    if (shouldUnload) {
                        lock.unlock();
                        engine.unload();
                        lock.lock();
                    }
                } else {
                    auto waitTime = std::chrono::seconds(idleUnloadSec - elapsed);
                    idleCv.wait_for(lock, waitTime, [&] {
                        return stopToken.stop_requested() || idleWorkerStop || jobRunning || activeSearchCount > 0;
                    });
                }
            } else {
                idleCv.wait(lock, [&] {
                    return stopToken.stop_requested() || idleWorkerStop || (!jobRunning && activeSearchCount == 0 && engine.isLoaded());
                });
            }
        }
    }
};

Service::Service(db::CatalogDb& db, std::string cacheDirectory, PathResolver resolver)
    : impl_(std::make_unique<Impl>(db, std::move(cacheDirectory), std::move(resolver))) {
    
    Status s = impl_->engine.initialize(false);
    if (!s.isOk()) {
        IMAGINE_LOG_WARN("CLIP semantic search is inactive (" + s.message() + "). Models can be placed in " + impl_->config.modelDirectory + " or configured via IMAGINE_CLIP_MODEL_DIR.");
    } else {
        IMAGINE_LOG_INFO("CLIP engine probed successfully from " + impl_->config.modelDirectory + " (models will load on demand)");
    }
    
    loadIndex();
    
    std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
    auto updateRes = impl_->db.conn_.prepare("UPDATE semantic_jobs SET state='interrupted' WHERE state IN ('running','cancelling');");
    if (updateRes.isOk()) {
        auto update = std::move(updateRes.value());
        update.step();
    }

    impl_->idleUnloadSec = envInt("IMAGINE_CLIP_IDLE_UNLOAD_SEC", 30);
    if (impl_->idleUnloadSec > 0 && impl_->engine.info().ready) {
        impl_->idleWorker = std::jthread([this](std::stop_token stopToken) { impl_->idleLoop(stopToken); });
    }
}

Service::~Service() {
    impl_->cancelRequested.store(true);
    {
        std::lock_guard<std::mutex> lock(impl_->idleMutex);
        impl_->idleWorkerStop = true;
    }
    impl_->idleCv.notify_all();
    if (impl_->idleWorker.joinable()) {
        impl_->idleWorker.request_stop();
        impl_->idleWorker.join();
    }
    if (impl_->worker.joinable()) {
        impl_->worker.request_stop();
        impl_->worker.join();
    }
    impl_->engine.unload();
}

void Service::loadIndex() {
    if (!impl_->engine.info().ready) return;
    auto t0 = std::chrono::steady_clock::now();
    std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
    
    auto stmtRes = impl_->db.conn_.prepare(
        "SELECT media_id, embedding, vector_dim FROM semantic_index_state "
        "WHERE embedding IS NOT NULL AND (error_text IS NULL OR error_text = '') AND model_id = ? AND model_revision = ? "
        "ORDER BY media_id;"
    );
    if (!stmtRes.isOk()) return;
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, impl_->engine.info().modelId);
    stmt.bind(2, impl_->engine.info().modelRevision);
    
    int targetDim = impl_->engine.info().vectorDim;
    std::vector<MediaId> loadedIds;
    std::vector<float> loadedVectors;

    while (stmt.step() == StepResult::Row) {
        MediaId id = stmt.getInt64(0);
        int dim = stmt.getInt(2);
        if (targetDim > 0 && dim != targetDim) continue;
        std::vector<float> vec = readVector(stmt, 1, dim);
        if (vec.size() == static_cast<size_t>(dim)) {
            loadedIds.push_back(id);
            loadedVectors.insert(loadedVectors.end(), vec.begin(), vec.end());
        }
    }
    int count = static_cast<int>(loadedIds.size());
    impl_->index.loadBulk(std::move(loadedIds), std::move(loadedVectors), targetDim);

    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    IMAGINE_LOG_INFO("Loaded " + std::to_string(count) + " semantic vectors in " + std::to_string(ms) + " ms");
}

void Service::removeMedia(MediaId mediaId) {
    impl_->index.remove(mediaId);
}

bool Service::hasMedia(MediaId mediaId) const {
    return impl_->index.has(mediaId);
}

nlohmann::json Service::status() const {
    const auto info = impl_->engine.info();
    const bool isLoaded = impl_->engine.isLoaded();
    size_t size = 0;
    {
        std::shared_lock lock(impl_->index.mutex);
        size = impl_->index.ids.size();
    }
    return json{
        {"built", info.built},
        {"ready", info.ready},
        {"loaded", isLoaded},
        {"modelId", info.modelId},
        {"modelRevision", info.modelRevision},
        {"vectorDim", info.vectorDim},
        {"indexSize", size},
        {"executionProvider", info.executionProvider}
    };
}

Result<SearchResult> Service::search(const SearchRequest& request) {
    if (!impl_->engine.info().ready) return Status::internal("CLIP engine is not ready");

    {
        std::lock_guard<std::mutex> lock(impl_->idleMutex);
        impl_->activeSearchCount++;
    }
    impl_->idleCv.notify_all();

    struct ActivityGuard {
        Service::Impl& impl;
        ~ActivityGuard() {
            bool shouldUnload = false;
            {
                std::lock_guard<std::mutex> lock(impl.idleMutex);
                impl.activeSearchCount--;
                impl.lastActivityTime = std::chrono::steady_clock::now();
                if (impl.idleUnloadSec <= 0 && !impl.jobRunning && impl.activeSearchCount == 0) {
                    shouldUnload = true;
                }
            }
            if (shouldUnload) {
                impl.engine.unload();
            } else {
                impl.idleCv.notify_all();
            }
        }
    };
    ActivityGuard activityGuard{*impl_};

    auto t0 = std::chrono::steady_clock::now();
    const int limit = std::clamp(request.limit, 1, 1000);
    
    auto encodeRes = impl_->engine.encodeText(request.query);
    if (!encodeRes.isOk()) return encodeRes.status();
    const auto& queryVec = encodeRes.value();
    
    const bool hasFilters = request.ratingMin.has_value() || request.dateFrom != 0 ||
                            request.dateTo != 0 || request.mediaType.has_value();

    std::unordered_set<MediaId> matchingFilterIds;
    if (hasFilters) {
        std::string sql = "SELECT id FROM media_items WHERE 1=1";
        if (request.ratingMin.has_value()) {
            sql += " AND rating >= " + std::to_string(*request.ratingMin);
        }
        if (request.dateFrom != 0) {
            sql += " AND date_taken != 0 AND date_taken >= " + std::to_string(request.dateFrom);
        }
        if (request.dateTo != 0) {
            sql += " AND date_taken != 0 AND date_taken <= " + std::to_string(request.dateTo);
        }
        if (request.mediaType.has_value()) {
            sql += " AND media_type = ?";
        }
        std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
        auto stmtRes = impl_->db.conn_.prepare(sql);
        if (!stmtRes.isOk()) return stmtRes.status();
        auto stmt = std::move(stmtRes.value());
        if (request.mediaType.has_value()) stmt.bind(1, *request.mediaType);
        while (stmt.step() == StepResult::Row) {
            matchingFilterIds.insert(stmt.getInt64(0));
        }
        if (matchingFilterIds.empty()) {
            auto t1 = std::chrono::steady_clock::now();
            return SearchResult{
                impl_->engine.info().modelId,
                std::chrono::duration<double, std::milli>(t1 - t0).count(),
                {}
            };
        }
    }

    auto hits = impl_->index.search(
        queryVec.data(),
        limit,
        impl_->engine.info().vectorDim,
        hasFilters ? &matchingFilterIds : nullptr
    );
    
    if (hits.empty()) {
        auto t1 = std::chrono::steady_clock::now();
        return SearchResult{
            impl_->engine.info().modelId,
            std::chrono::duration<double, std::milli>(t1 - t0).count(),
            {}
        };
    }

    if (hasFilters) {
        auto t1 = std::chrono::steady_clock::now();
        return SearchResult{
            impl_->engine.info().modelId,
            std::chrono::duration<double, std::milli>(t1 - t0).count(),
            std::move(hits)
        };
    }
    
    std::unordered_set<MediaId> filteredIds;
    for (size_t offset = 0; offset < hits.size(); offset += 500) {
        size_t chunkEnd = std::min(offset + 500, hits.size());
        std::string inClause = "(";
        for (size_t i = offset; i < chunkEnd; ++i) {
            inClause += std::to_string(hits[i].mediaId);
            if (i < chunkEnd - 1) inClause += ",";
        }
        inClause += ")";
        
        std::string sql = "SELECT id FROM media_items WHERE id IN " + inClause;
        
        std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
        auto stmtRes = impl_->db.conn_.prepare(sql);
        if (stmtRes.isOk()) {
            auto stmt = std::move(stmtRes.value());
            while (stmt.step() == StepResult::Row) {
                filteredIds.insert(stmt.getInt64(0));
            }
        }
    }
    
    std::vector<SearchHit> finalHits;
    finalHits.reserve(limit);
    for (const auto& hit : hits) {
        if (filteredIds.find(hit.mediaId) != filteredIds.end()) {
            finalHits.push_back(hit);
            if (finalHits.size() >= static_cast<size_t>(limit)) break;
        }
    }
    
    auto t1 = std::chrono::steady_clock::now();
    return SearchResult{
        impl_->engine.info().modelId,
        std::chrono::duration<double, std::milli>(t1 - t0).count(),
        finalHits
    };
}

Result<SearchResult> Service::findSimilar(MediaId mediaId, int limit) {
    auto t0 = std::chrono::steady_clock::now();
    const int clampedLimit = std::clamp(limit, 1, 1000);
    
    int dim = 0;
    std::vector<float> queryVec = impl_->index.get(mediaId, dim);
    if (queryVec.empty()) {
        if (!impl_->engine.info().ready) return Status::internal("CLIP engine is not ready");
        std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
        auto stmtRes = impl_->db.conn_.prepare("SELECT embedding, vector_dim FROM semantic_index_state WHERE media_id=? AND model_id=? AND model_revision=?;");
        if (stmtRes.isOk()) {
            auto stmt = std::move(stmtRes.value());
            stmt.bind(1, mediaId);
            stmt.bind(2, impl_->engine.info().modelId);
            stmt.bind(3, impl_->engine.info().modelRevision);
            if (stmt.step() == StepResult::Row) {
                dim = stmt.getInt(1);
                queryVec = readVector(stmt, 0, dim);
            }
        }
    }
    
    if (queryVec.empty()) return Status::notFound("Media embedding not found");
    
    auto hits = impl_->index.search(queryVec.data(), clampedLimit + 1, dim);
    if (hits.empty()) {
        auto t1 = std::chrono::steady_clock::now();
        return SearchResult{
            impl_->engine.info().modelId,
            std::chrono::duration<double, std::milli>(t1 - t0).count(),
            {}
        };
    }

    std::unordered_set<MediaId> existingIds;
    for (size_t offset = 0; offset < hits.size(); offset += 500) {
        size_t chunkEnd = std::min(offset + 500, hits.size());
        std::string inClause = "(";
        for (size_t i = offset; i < chunkEnd; ++i) {
            inClause += std::to_string(hits[i].mediaId);
            if (i < chunkEnd - 1) inClause += ",";
        }
        inClause += ")";

        std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
        auto stmtRes = impl_->db.conn_.prepare("SELECT id FROM media_items WHERE id IN " + inClause + ";");
        if (stmtRes.isOk()) {
            auto stmt = std::move(stmtRes.value());
            while (stmt.step() == StepResult::Row) {
                existingIds.insert(stmt.getInt64(0));
            }
        }
    }
    
    std::vector<SearchHit> finalHits;
    finalHits.reserve(clampedLimit);
    for (const auto& hit : hits) {
        if (hit.mediaId != mediaId && existingIds.find(hit.mediaId) != existingIds.end()) {
            finalHits.push_back(hit);
            if (finalHits.size() >= static_cast<size_t>(clampedLimit)) break;
        }
    }
    
    auto t1 = std::chrono::steady_clock::now();
    return SearchResult{
        impl_->engine.info().modelId,
        std::chrono::duration<double, std::milli>(t1 - t0).count(),
        finalHits
    };
}

Status Service::startJob(const std::string& scope, const std::vector<MediaId>& mediaIds, bool force, int64_t& jobId) {
    std::lock_guard<std::mutex> startLock(impl_->jobStartMutex);
    
    {
        std::lock_guard<std::mutex> idleLock(impl_->idleMutex);
        if (impl_->jobRunning) {
            return Status::alreadyExists("A semantic index job is already running");
        }
    }

    std::vector<MediaId> toIndex;
    const auto info = impl_->engine.info();
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
        Connection& conn = impl_->db.conn_;
        
        auto countRes = conn.prepare("SELECT COUNT(*) FROM semantic_jobs WHERE state IN ('running','cancelling');");
        if (!countRes.isOk()) return countRes.status();
        auto countStmt = std::move(countRes.value());
        if (countStmt.step() == StepResult::Row && countStmt.getInt64(0) > 0) {
            return Status::alreadyExists("A semantic index job is already running");
        }

        if (!info.ready) return Status::internal("CLIP engine is not ready");

        if (impl_->worker.joinable()) {
            impl_->worker.join();
        }
        impl_->cancelRequested.store(false);
        
        if (scope == "selected") {
            toIndex = mediaIds;
        } else {
            auto stmtRes = conn.prepare(R"SQL(
                SELECT m.id FROM media_items m
                LEFT JOIN semantic_index_state s
                  ON m.id = s.media_id
                  AND s.model_id = ?
                  AND s.model_revision = ?
                  AND s.vector_dim = ?
                  AND s.content_hash = m.content_hash
                  AND s.embedding IS NOT NULL
                  AND (s.error_text IS NULL OR s.error_text = '')
                WHERE m.media_type = 'photo'
                  AND (? = 1 OR s.media_id IS NULL)
                ORDER BY m.id;
            )SQL");
            if (!stmtRes.isOk()) return stmtRes.status();
            auto stmt = std::move(stmtRes.value());
            stmt.bind(1, info.modelId);
            stmt.bind(2, info.modelRevision);
            stmt.bind(3, info.vectorDim);
            stmt.bind(4, force ? 1 : 0);
            while (stmt.step() == StepResult::Row) {
                toIndex.push_back(stmt.getInt64(0));
            }
        }
        
        auto insRes = conn.prepare(
            "INSERT INTO semantic_jobs (scope, state, model_id, total, processed, skipped, failed, remaining, created_at, updated_at) "
            "VALUES (?, 'running', ?, ?, 0, 0, 0, ?, ?, ?);"
        );
        if (!insRes.isOk()) return insRes.status();
        auto ins = std::move(insRes.value());
        ins.bind(1, scope);
        ins.bind(2, info.modelId);
        ins.bind(3, static_cast<int64_t>(toIndex.size()));
        ins.bind(4, static_cast<int64_t>(toIndex.size()));
        int64_t now = nowSeconds();
        ins.bind(5, now);
        ins.bind(6, now);
        ins.step();
        jobId = conn.lastInsertRowId();
    }
    
    {
        std::lock_guard<std::mutex> idleLock(impl_->idleMutex);
        impl_->jobRunning = true;
    }
    impl_->idleCv.notify_all();

    impl_->worker = std::jthread([this, jobId, scope, toIndex = std::move(toIndex), force] {
        runJob(jobId, scope, std::move(toIndex), force);
    });
    
    return Status::ok();
}

Result<nlohmann::json> Service::getJob(int64_t jobId) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
    auto stmtRes = impl_->db.conn_.prepare(
        "SELECT id, state, scope, total, processed, skipped, failed, remaining, error, model_id FROM semantic_jobs WHERE id=?;"
    );
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, jobId);
    if (stmt.step() != StepResult::Row) return Status::notFound("Job not found");
    
    return json{
        {"id", stmt.getInt64(0)},
        {"state", stmt.getString(1)},
        {"scope", stmt.getString(2)},
        {"total", stmt.getInt64(3)},
        {"processed", stmt.getInt64(4)},
        {"skipped", stmt.getInt64(5)},
        {"failed", stmt.getInt64(6)},
        {"remaining", stmt.getInt64(7)},
        {"error", stmt.getString(8)},
        {"model_id", stmt.getString(9)}
    };
}

Status Service::cancelJob(int64_t jobId) {
    std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
    auto stmtRes = impl_->db.conn_.prepare("UPDATE semantic_jobs SET state='cancelling' WHERE id=? AND state='running';");
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, jobId);
    stmt.step();
    if (impl_->db.conn_.changes() > 0) {
        impl_->cancelRequested.store(true);
    }
    return Status::ok();
}

void Service::runJob(int64_t jobId, std::string scope, std::vector<MediaId> mediaIds, bool force) {
    IMAGINE_LOG_INFO("Semantic index job " + std::to_string(jobId) + " started processing " + std::to_string(mediaIds.size()) + " items");
    {
        std::lock_guard<std::mutex> lock(impl_->idleMutex);
        impl_->jobRunning = true;
    }
    impl_->idleCv.notify_all();

    struct JobGuard {
        Service::Impl& impl;
        ~JobGuard() {
            bool shouldUnload = false;
            {
                std::lock_guard<std::mutex> lock(impl.idleMutex);
                impl.jobRunning = false;
                impl.lastActivityTime = std::chrono::steady_clock::now();
                if (impl.idleUnloadSec <= 0 && impl.activeSearchCount == 0) {
                    shouldUnload = true;
                }
            }
            if (shouldUnload) {
                impl.engine.unload();
            } else {
                impl.idleCv.notify_all();
            }
        }
    };
    JobGuard jobGuard{*impl_};

    if (mediaIds.empty()) {
        std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
        auto updateRes = impl_->db.conn_.prepare("UPDATE semantic_jobs SET state='completed', updated_at=? WHERE id=?;");
        if (updateRes.isOk()) {
            auto update = std::move(updateRes.value());
            update.bind(1, nowSeconds());
            update.bind(2, jobId);
            update.step();
        }
        IMAGINE_LOG_INFO("Semantic index job " + std::to_string(jobId) + " completed (0 items to index)");
        return;
    }

    ClipRuntimeInfo runtimeInfo = impl_->engine.info();

    auto updateProgress = [&](const std::string& kind, const std::string& error = "") {
        std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
        std::string sql = "UPDATE semantic_jobs SET " + kind + "=" + kind +
                          "+1, remaining=MAX(0, total - processed - skipped - failed - 1), updated_at=?, "
                          "error=CASE WHEN error='' AND ?<>'' THEN ? ELSE error END WHERE id=?;";
        auto updateRes = impl_->db.conn_.prepare(sql);
        if (updateRes.isOk()) {
            auto update = std::move(updateRes.value());
            update.bind(1, nowSeconds());
            update.bind(2, error);
            update.bind(3, error);
            update.bind(4, jobId);
            update.step();
        }
    };

    auto recordFailure = [&](MediaId mediaId, const std::string& hash, const std::string& reason) {
        impl_->index.remove(mediaId);
        std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
        db::Transaction tx(impl_->db.conn_);
        auto insRes = impl_->db.conn_.prepare(
            "INSERT OR REPLACE INTO semantic_index_state "
            "(media_id, model_id, model_revision, preprocess_id, vector_dim, embedding, content_hash, error_text, indexed_at) "
            "VALUES (?, ?, ?, ?, ?, NULL, ?, ?, ?);"
        );
        if (insRes.isOk()) {
            auto ins = std::move(insRes.value());
            ins.bind(1, mediaId);
            ins.bind(2, runtimeInfo.modelId);
            ins.bind(3, runtimeInfo.modelRevision);
            ins.bind(4, runtimeInfo.preprocessId);
            ins.bind(5, runtimeInfo.vectorDim);
            ins.bind(6, hash);
            ins.bind(7, reason);
            ins.bind(8, nowSeconds());
            ins.step();
        }
        std::string sql = "UPDATE semantic_jobs SET failed=failed+1, "
                          "remaining=MAX(0, total - processed - skipped - failed - 1), updated_at=?, "
                          "error=CASE WHEN error='' AND ?<>'' THEN ? ELSE error END WHERE id=?;";
        auto updateRes = impl_->db.conn_.prepare(sql);
        if (updateRes.isOk()) {
            auto update = std::move(updateRes.value());
            update.bind(1, nowSeconds());
            update.bind(2, reason);
            update.bind(3, reason);
            update.bind(4, jobId);
            update.step();
        }
        tx.commit();
    };

    auto isAlreadyIndexed = [&](MediaId id, const std::string& hash) -> bool {
        std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
        auto stmtRes = impl_->db.conn_.prepare(
            "SELECT 1 FROM semantic_index_state "
            "WHERE media_id = ? "
            "  AND model_id = ? "
            "  AND model_revision = ? "
            "  AND vector_dim = ? "
            "  AND content_hash = ? "
            "  AND embedding IS NOT NULL "
            "  AND (error_text IS NULL OR error_text = '') "
            "LIMIT 1;"
        );
        if (!stmtRes.isOk()) return false;
        auto stmt = std::move(stmtRes.value());
        stmt.bind(1, id);
        stmt.bind(2, runtimeInfo.modelId);
        stmt.bind(3, runtimeInfo.modelRevision);
        stmt.bind(4, runtimeInfo.vectorDim);
        stmt.bind(5, hash);
        return stmt.step() == StepResult::Row;
    };

    try {
        thumbnail::Cache thumbCache(impl_->cacheDirectory);
        for (MediaId mediaId : mediaIds) {
            if (impl_->cancelRequested.load()) break;

            MediaItem media;
            {
                auto mediaRes = impl_->db.getMediaById(mediaId);
                if (!mediaRes.isOk()) {
                    updateProgress("failed", "Media not found");
                    continue;
                }
                media = mediaRes.value();
            }

            if (media.media_type != "photo") {
                updateProgress("skipped");
                continue;
            }

            if (scope == "selected" && !force && isAlreadyIndexed(mediaId, media.content_hash)) {
                updateProgress("skipped");
                continue;
            }

            if (!impl_->engine.isLoaded()) {
                Status loadStatus = impl_->engine.load();
                if (!loadStatus.isOk()) {
                    IMAGINE_LOG_ERROR("Semantic index unable to load CLIP engine for job " +
                                      std::to_string(jobId) + ": " + loadStatus.message());
                    std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
                    auto updateRes = impl_->db.conn_.prepare("UPDATE semantic_jobs SET state='failed', error=?, updated_at=? WHERE id=?;");
                    if (updateRes.isOk()) {
                        auto update = std::move(updateRes.value());
                        update.bind(1, loadStatus.message());
                        update.bind(2, nowSeconds());
                        update.bind(3, jobId);
                        update.step();
                    }
                    return;
                }
                runtimeInfo = impl_->engine.info();
            }

            std::string smallThumb = thumbCache.getThumbnailPath(media.content_hash, thumbnail::Cache::SmallSize);
            std::string largeThumb = thumbCache.getThumbnailPath(media.content_hash, thumbnail::Cache::LargeSize);

            thumbnail::ImageBuffer orientedImage;
            bool loadedImage = false;
            std::error_code fsEc;

            if (std::filesystem::exists(pathFromUtf8(smallThumb), fsEc) && !fsEc) {
                auto b = thumbnail::Generator::loadImageBytes(smallThumb);
                if (b.isOk()) {
                    auto dec = thumbnail::Generator::loadImageFromMemory(b.value().data(), b.value().size());
                    if (dec.isOk()) {
                        orientedImage = std::move(dec.value());
                        loadedImage = true;
                    }
                }
            }
            if (!loadedImage && std::filesystem::exists(pathFromUtf8(largeThumb), fsEc) && !fsEc) {
                auto b = thumbnail::Generator::loadImageBytes(largeThumb);
                if (b.isOk()) {
                    auto dec = thumbnail::Generator::loadImageFromMemory(b.value().data(), b.value().size());
                    if (dec.isOk()) {
                        orientedImage = std::move(dec.value());
                        loadedImage = true;
                    }
                }
            }
            if (!loadedImage) {
                std::string photoPath = impl_->resolver ? impl_->resolver(media.file_path) : media.file_path;
                auto bytes = thumbnail::Generator::loadImageBytes(photoPath);
                if (!bytes.isOk()) {
                    recordFailure(mediaId, media.content_hash, "Failed to load image");
                    continue;
                }

                auto loaded = thumbnail::Generator::loadImageFromMemory(bytes.value().data(), bytes.value().size());
                if (!loaded.isOk()) {
                    recordFailure(mediaId, media.content_hash, "Failed to decode image");
                    continue;
                }

                if (media.exif.orientation > 1) {
                    orientedImage = thumbnail::Generator::rotate(loaded.value(), media.exif.orientation);
                } else {
                    orientedImage = std::move(loaded.value());
                }
            }

            auto encodeRes = impl_->engine.encodeImage(orientedImage);
            if (!encodeRes.isOk()) {
                recordFailure(mediaId, media.content_hash, encodeRes.status().message());
                continue;
            }

            impl_->index.add(mediaId, encodeRes.value().data(), static_cast<int>(encodeRes.value().size()));
            {
                std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
                db::Transaction tx(impl_->db.conn_);
                auto insRes = impl_->db.conn_.prepare(
                    "INSERT OR REPLACE INTO semantic_index_state "
                    "(media_id, model_id, model_revision, preprocess_id, vector_dim, embedding, content_hash, error_text, indexed_at) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, '', ?);"
                );
                if (insRes.isOk()) {
                    auto ins = std::move(insRes.value());
                    ins.bind(1, mediaId);
                    ins.bind(2, runtimeInfo.modelId);
                    ins.bind(3, runtimeInfo.modelRevision);
                    ins.bind(4, runtimeInfo.preprocessId);
                    ins.bind(5, static_cast<int>(encodeRes.value().size()));
                    bindEmbedding(ins, 6, encodeRes.value());
                    ins.bind(7, media.content_hash);
                    ins.bind(8, nowSeconds());
                    ins.step();
                }
                std::string sql = "UPDATE semantic_jobs SET processed=processed+1, "
                                  "remaining=MAX(0, total - processed - skipped - failed - 1), updated_at=? WHERE id=?;";
                auto updateRes = impl_->db.conn_.prepare(sql);
                if (updateRes.isOk()) {
                    auto update = std::move(updateRes.value());
                    update.bind(1, nowSeconds());
                    update.bind(2, jobId);
                    update.step();
                }
                tx.commit();
            }
        }
    } catch (const std::exception& e) {
        IMAGINE_LOG_ERROR("Semantic index job " + std::to_string(jobId) + " failed with exception: " + e.what());
        std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
        auto updateRes = impl_->db.conn_.prepare("UPDATE semantic_jobs SET state='failed', error=?, updated_at=? WHERE id=?;");
        if (updateRes.isOk()) {
            auto update = std::move(updateRes.value());
            update.bind(1, std::string(e.what()));
            update.bind(2, nowSeconds());
            update.bind(3, jobId);
            update.step();
        }
        return;
    }

    std::string finalState = impl_->cancelRequested.load() ? "cancelled" : "completed";
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
        auto updateRes = impl_->db.conn_.prepare("UPDATE semantic_jobs SET state=?, updated_at=? WHERE id=?;");
        if (updateRes.isOk()) {
            auto update = std::move(updateRes.value());
            update.bind(1, finalState);
            update.bind(2, nowSeconds());
            update.bind(3, jobId);
            update.step();
        }
    }

    if (finalState == "cancelled") {
        IMAGINE_LOG_INFO("Semantic index job " + std::to_string(jobId) + " was cancelled");
    } else {
        IMAGINE_LOG_INFO("Semantic index job " + std::to_string(jobId) + " completed successfully");
    }
}

namespace {

inline std::unordered_set<uint64_t> loadDismissedPairs(db::CatalogDb& db) {
    std::unordered_set<uint64_t> dismissed;
    auto res = db.getDismissedDuplicatePairs();
    if (res.isOk()) {
        for (const auto& [a, b] : res.value()) {
            MediaId low = std::min(a, b);
            MediaId high = std::max(a, b);
            dismissed.insert((static_cast<uint64_t>(low) << 32) | (static_cast<uint64_t>(high) & 0xFFFFFFFFULL));
        }
    }
    return dismissed;
}

inline std::unordered_set<MediaId> loadExactDuplicateMembers(db::CatalogDb& db) {
    std::unordered_set<MediaId> exactIds;
    auto res = db.findExactDuplicateGroups();
    if (res.isOk()) {
        for (const auto& group : res.value()) {
            exactIds.insert(group.begin(), group.end());
        }
    }
    return exactIds;
}

struct DisjointSet {
    std::unordered_map<MediaId, MediaId> parent;
    MediaId find(MediaId x) {
        if (!parent.count(x)) parent[x] = x;
        if (parent[x] == x) return x;
        return parent[x] = find(parent[x]);
    }
    void unite(MediaId x, MediaId y) {
        MediaId rx = find(x);
        MediaId ry = find(y);
        if (rx != ry) {
            parent[rx] = ry;
        }
    }
};

inline void rankKeeperMembers(std::vector<MediaId>& members, db::CatalogDb& db) {
    if (members.size() <= 1) return;
    std::vector<MediaItem> items;
    items.reserve(members.size());
    for (MediaId id : members) {
        auto r = db.getMediaById(id);
        if (r.isOk()) {
            items.push_back(r.value());
        } else {
            MediaItem dummy;
            dummy.id = id;
            items.push_back(dummy);
        }
    }

    std::stable_sort(items.begin(), items.end(), core::isBetterKeeper);

    for (size_t i = 0; i < items.size(); ++i) {
        members[i] = items[i].id;
    }
}

} // namespace

std::vector<VisualDuplicatePair> Service::findVisualDuplicatePairs(
    float threshold,
    const std::unordered_set<MediaId>& allowedIds
) const {
    return impl_->index.findDuplicatePairs(threshold, allowedIds);
}

std::vector<VisualDuplicateGroup> Service::scanVisualDuplicates(
    float threshold,
    int64_t dateWindowSec,
    const std::unordered_set<MediaId>& allowedIds,
    const std::unordered_set<MediaId>& exactHashMemberIds
) const {
    auto dismissed = loadDismissedPairs(impl_->db);
    std::unordered_set<MediaId> exactMembers = exactHashMemberIds.empty()
        ? loadExactDuplicateMembers(impl_->db)
        : exactHashMemberIds;

    std::vector<MediaId> seedIds;
    if (!allowedIds.empty()) {
        seedIds.assign(allowedIds.begin(), allowedIds.end());
    } else {
        std::shared_lock lock(impl_->index.mutex);
        seedIds = impl_->index.ids;
    }
    if (seedIds.size() < 2) return {};

    // Candidate generation seeded by nearest-neighbor index search
    std::unordered_map<uint64_t, float> candidatePairs;
    constexpr int kCandidatesPerSeed = 50;

    for (MediaId seedId : seedIds) {
        if (exactMembers.count(seedId)) continue; // exact-group members excluded before visual pass

        int dim = 0;
        std::vector<float> queryVec = impl_->index.get(seedId, dim);
        if (queryVec.empty()) continue;

        auto hits = impl_->index.search(
            queryVec.data(),
            kCandidatesPerSeed + 1,
            dim,
            allowedIds.empty() ? nullptr : &allowedIds
        );

        for (const auto& hit : hits) {
            if (hit.mediaId == seedId) continue;
            if (hit.score < threshold) continue;
            if (exactMembers.count(hit.mediaId)) continue; // exclude exact-group members

            MediaId low = std::min(seedId, hit.mediaId);
            MediaId high = std::max(seedId, hit.mediaId);
            uint64_t key = (static_cast<uint64_t>(low) << 32) | (static_cast<uint64_t>(high) & 0xFFFFFFFFULL);

            if (dismissed.count(key)) continue; // exclude dismissed pairs

            auto it = candidatePairs.find(key);
            if (it == candidatePairs.end() || hit.score > it->second) {
                candidatePairs[key] = hit.score;
            }
        }
    }

    if (candidatePairs.empty()) return {};

    std::vector<VisualDuplicatePair> surviving;
    surviving.reserve(candidatePairs.size());

    std::unordered_map<MediaId, int64_t> dateTakenCache;
    if (dateWindowSec > 0) {
        std::unordered_set<MediaId> needed;
        for (const auto& [key, _] : candidatePairs) {
            needed.insert(static_cast<MediaId>(key >> 32));
            needed.insert(static_cast<MediaId>(key & 0xFFFFFFFFULL));
        }
        for (MediaId id : needed) {
            auto m = impl_->db.getMediaById(id);
            if (m.isOk()) {
                dateTakenCache[id] = m.value().date_taken;
            }
        }
    }

    for (const auto& [key, sim] : candidatePairs) {
        MediaId low = static_cast<MediaId>(key >> 32);
        MediaId high = static_cast<MediaId>(key & 0xFFFFFFFFULL);

        if (dateWindowSec > 0) {
            auto itA = dateTakenCache.find(low);
            auto itB = dateTakenCache.find(high);
            int64_t dtA = (itA != dateTakenCache.end()) ? itA->second : 0;
            int64_t dtB = (itB != dateTakenCache.end()) ? itB->second : 0;
            if (dtA != 0 && dtB != 0 && std::abs(dtA - dtB) > dateWindowSec) {
                continue;
            }
        }

        surviving.push_back(VisualDuplicatePair{low, high, sim});
    }

    if (surviving.empty()) return {};

    DisjointSet ds;
    for (const auto& p : surviving) {
        ds.unite(p.a, p.b);
    }

    std::unordered_map<MediaId, std::vector<MediaId>> clusters;
    std::unordered_set<MediaId> allParticipants;
    for (const auto& p : surviving) {
        allParticipants.insert(p.a);
        allParticipants.insert(p.b);
    }
    for (MediaId id : allParticipants) {
        clusters[ds.find(id)].push_back(id);
    }

    std::vector<VisualDuplicateGroup> groups;
    for (auto& [root, members] : clusters) {
        if (members.size() < 2) continue;

        float minSim = 1.0f;
        for (const auto& p : surviving) {
            if (ds.find(p.a) == root) {
                minSim = std::min(minSim, p.similarity);
            }
        }

        rankKeeperMembers(members, impl_->db);
        groups.push_back(VisualDuplicateGroup{std::move(members), minSim});
    }

    std::sort(groups.begin(), groups.end(), [](const VisualDuplicateGroup& a, const VisualDuplicateGroup& b) {
        if (a.memberIds.size() != b.memberIds.size()) {
            return a.memberIds.size() > b.memberIds.size();
        }
        MediaId kA = a.memberIds.empty() ? 0 : a.memberIds[0];
        MediaId kB = b.memberIds.empty() ? 0 : b.memberIds[0];
        return kA < kB;
    });

    return groups;
}

void Service::addMediaVector(MediaId id, const float* embedding, int dim) {
    impl_->index.add(id, embedding, dim);
}

void Service::addMediaVector(MediaId id, const std::vector<float>& embedding) {
    impl_->index.add(id, embedding.data(), static_cast<int>(embedding.size()));
}

void Service::reloadIndex() {
    loadIndex();
}

} // namespace imagine::clip
