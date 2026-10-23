#include "imagine/clip/service.hpp"
#include "imagine/clip/smart_tags.hpp"

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
            std::sort(pairs.begin(), pairs.end(), [](const VisualDuplicatePair& lhs, const VisualDuplicatePair& rhs) {
                if (lhs.a != rhs.a) return lhs.a < rhs.a;
                if (lhs.b != rhs.b) return lhs.b < rhs.b;
                return lhs.similarity > rhs.similarity;
            });
            return pairs;
        }
    } index;
    
    std::atomic<bool> cancelRequested{false};
    std::atomic<bool> stopping{false};
    std::mutex jobStartMutex;

    TagTaxonomy taxonomy;
    bool taxonomyReady{false};
    std::string taxonomyError;

    struct LabelCache {
        std::string key;
        std::vector<std::vector<float>> embeddings;
    };
    mutable std::mutex labelCacheMutex;
    mutable LabelCache labelCache;
    
    Impl(db::CatalogDb& db, std::string cacheDir, PathResolver res)
        : db(db), cacheDirectory(std::move(cacheDir)), resolver(std::move(res)),
          config{
              envString("IMAGINE_CLIP_MODEL_DIR", cacheDirectory + "/clip_models"),
              envString("IMAGINE_CLIP_DEVICE", "cpu"),
              4
          },
          engine(config) {
        auto taxRes = loadTaxonomy();
        if (taxRes.isOk()) {
            taxonomy = std::move(taxRes.value());
            taxonomyReady = true;
            taxonomyError = "";
        } else {
            taxonomyReady = false;
            taxonomyError = taxRes.status().message();
        }
    }

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
        std::stop_callback stopWake(stopToken, [this]() {
            std::lock_guard<std::mutex> wakeLock(idleMutex);
            idleCv.notify_all();
        });
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

void Service::stop() {
    if (!impl_) return;
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
    std::lock_guard<std::mutex> lock(impl_->jobStartMutex);
    impl_->stopping.store(true);
    if (impl_->worker.joinable()) {
        impl_->worker.request_stop();
        impl_->worker.join();
    }
}

Service::~Service() {
    stop();
    impl_->engine.unload();
}

void Service::loadIndex() {
    if (!impl_->engine.info().ready) return;
    auto t0 = std::chrono::steady_clock::now();
    std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
    
    auto stmtRes = impl_->db.conn_.prepare(
        "SELECT s.media_id, s.embedding, s.vector_dim FROM semantic_index_state s "
        "JOIN media_items m ON s.media_id = m.id AND m.media_type = 'photo' "
        "WHERE s.embedding IS NOT NULL AND (s.error_text IS NULL OR s.error_text = '') AND s.model_id = ? AND s.model_revision = ? "
        "ORDER BY s.media_id;"
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

    if (request.filterMediaIds && request.allowedMediaIds.empty()) {
        auto t1 = std::chrono::steady_clock::now();
        return SearchResult{
            impl_->engine.info().modelId,
            std::chrono::duration<double, std::milli>(t1 - t0).count(),
            {}
        };
    }
    
    auto encodeRes = impl_->engine.encodeText(request.query);
    if (!encodeRes.isOk()) return encodeRes.status();
    const auto& queryVec = encodeRes.value();
    
    const bool hasMediaTypeFilter = request.mediaType.has_value() && *request.mediaType != "photo";
    const bool hasFilters = request.filterMediaIds ||
                            request.ratingMin.has_value() || request.dateFrom != 0 ||
                            request.dateTo != 0 || hasMediaTypeFilter;

    std::unordered_set<MediaId> matchingFilterIds;
    if (request.filterMediaIds) {
        matchingFilterIds.insert(request.allowedMediaIds.begin(), request.allowedMediaIds.end());
    } else if (hasFilters) {
        std::string sql = "SELECT id FROM media_items WHERE media_type = 'photo'";
        if (request.ratingMin.has_value()) {
            sql += " AND rating >= " + std::to_string(*request.ratingMin);
        }
        if (request.dateFrom != 0) {
            sql += " AND date_taken != 0 AND date_taken >= " + std::to_string(request.dateFrom);
        }
        if (request.dateTo != 0) {
            sql += " AND date_taken != 0 AND date_taken <= " + std::to_string(request.dateTo);
        }
        if (hasMediaTypeFilter) {
            sql += " AND media_type = ?";
        }
        std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
        auto stmtRes = impl_->db.conn_.prepare(sql);
        if (!stmtRes.isOk()) return stmtRes.status();
        auto stmt = std::move(stmtRes.value());
        if (hasMediaTypeFilter) stmt.bind(1, *request.mediaType);
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
        
        std::string sql = "SELECT id FROM media_items WHERE media_type = 'photo' AND id IN " + inClause;
        
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
    if (impl_->stopping.load()) {
        return Status::internal("CLIP service is stopping");
    }
    
    {
        std::lock_guard<std::mutex> idleLock(impl_->idleMutex);
        if (impl_->jobRunning) {
            return Status::alreadyExists("A semantic index job is already running");
        }
    }

    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    impl_->cancelRequested.store(false);

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
            "INSERT INTO semantic_jobs (scope, state, model_id, kind, total, processed, skipped, failed, remaining, created_at, updated_at) "
            "VALUES (?, 'running', ?, 'index', ?, 0, 0, 0, ?, ?, ?);"
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
        if (ins.step() != StepResult::Done) {
            return Status::databaseError("Failed to insert semantic index job row");
        }
        jobId = conn.lastInsertRowId();
        if (jobId <= 0) {
            return Status::databaseError("Failed to obtain semantic index job ID");
        }
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

Result<nlohmann::json> Service::getJob(int64_t jobId, const std::string& expectedKind) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
    auto stmtRes = impl_->db.conn_.prepare(
        "SELECT id, state, scope, total, processed, skipped, failed, remaining, error, model_id, kind FROM semantic_jobs WHERE id=?;"
    );
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, jobId);
    if (stmt.step() != StepResult::Row) return Status::notFound("Job not found");

    std::string kind = stmt.getString(10);
    if (!expectedKind.empty() && kind != expectedKind) {
        return Status::notFound("Job not found");
    }
    
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
        {"model_id", stmt.getString(9)},
        {"kind", kind}
    };
}

Status Service::cancelJob(int64_t jobId, const std::string& expectedKind) {
    std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
    auto checkRes = impl_->db.conn_.prepare("SELECT kind FROM semantic_jobs WHERE id=?;");
    if (!checkRes.isOk()) return checkRes.status();
    auto checkStmt = std::move(checkRes.value());
    checkStmt.bind(1, jobId);
    if (checkStmt.step() != StepResult::Row) {
        return Status::notFound("Job not found");
    }
    std::string kind = checkStmt.getString(0);
    if (!expectedKind.empty() && kind != expectedKind) {
        return Status::notFound("Job not found");
    }

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


nlohmann::json Service::smartTagStatus() const {
    const auto info = impl_->engine.info();
    int64_t indexedPhotos = 0;
    int64_t labeledPhotos = 0;
    int64_t tagAssignments = 0;

    {
        std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
        Connection& conn = impl_->db.conn_;

        auto idxStmtRes = conn.prepare(
            "SELECT COUNT(*) FROM media_items m "
            "JOIN semantic_index_state s ON m.id = s.media_id AND m.content_hash = s.content_hash "
            "WHERE m.media_type = 'photo' AND s.model_id = ? AND s.model_revision = ? AND s.vector_dim = ? "
            "AND s.embedding IS NOT NULL AND (s.error_text IS NULL OR s.error_text = '');"
        );
        if (idxStmtRes.isOk()) {
            auto& stmt = idxStmtRes.value();
            stmt.bind(1, info.modelId);
            stmt.bind(2, info.modelRevision);
            stmt.bind(3, info.vectorDim);
            if (stmt.step() == StepResult::Row) {
                indexedPhotos = stmt.getInt64(0);
            }
        }

        auto labStmtRes = conn.prepare(
            "SELECT COUNT(*) FROM smart_tag_state st "
            "JOIN media_items m ON st.media_id = m.id AND st.content_hash = m.content_hash "
            "WHERE st.model_id = ? AND st.model_revision = ? AND st.taxonomy_version = ?;"
        );
        if (labStmtRes.isOk()) {
            auto& stmt = labStmtRes.value();
            stmt.bind(1, info.modelId);
            stmt.bind(2, info.modelRevision);
            stmt.bind(3, impl_->taxonomy.version);
            if (stmt.step() == StepResult::Row) {
                labeledPhotos = stmt.getInt64(0);
            }
        }

        auto assStmtRes = conn.prepare("SELECT COUNT(*) FROM media_tag_provenance WHERE source = 'clip';");
        if (assStmtRes.isOk()) {
            auto& stmt = assStmtRes.value();
            if (stmt.step() == StepResult::Row) {
                tagAssignments = stmt.getInt64(0);
            }
        }
    }

    return nlohmann::json{
        {"built", info.built},
        {"ready", info.ready},
        {"taxonomyReady", impl_->taxonomyReady},
        {"taxonomyError", impl_->taxonomyError},
        {"taxonomyVersion", impl_->taxonomy.version},
        {"labelCount", static_cast<int64_t>(impl_->taxonomy.labels.size())},
        {"indexedPhotos", indexedPhotos},
        {"labeledPhotos", labeledPhotos},
        {"tagAssignments", tagAssignments},
        {"modelId", info.modelId}
    };
}

Status Service::startSmartTagJob(const std::string& scope, const std::vector<MediaId>& mediaIds, bool force, int64_t& jobId) {
    std::lock_guard<std::mutex> startLock(impl_->jobStartMutex);
    if (impl_->stopping.load()) {
        return Status::internal("CLIP service is stopping");
    }

    {
        std::lock_guard<std::mutex> idleLock(impl_->idleMutex);
        if (impl_->jobRunning) {
            return Status::alreadyExists("A job is already running");
        }
    }

    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    impl_->cancelRequested.store(false);

    const auto info = impl_->engine.info();
    if (!info.ready) return Status::internal("CLIP model unavailable");
    if (!impl_->taxonomyReady) return Status::internal("Taxonomy not ready: " + impl_->taxonomyError);

    std::vector<MediaId> candidates;
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
        Connection& conn = impl_->db.conn_;

        auto countRes = conn.prepare("SELECT COUNT(*) FROM semantic_jobs WHERE state IN ('running','cancelling');");
        if (!countRes.isOk()) return countRes.status();
        auto countStmt = std::move(countRes.value());
        if (countStmt.step() == StepResult::Row && countStmt.getInt64(0) > 0) {
            return Status::alreadyExists("A job is already running");
        }

        auto idxStmtRes = conn.prepare(
            "SELECT COUNT(*) FROM media_items m "
            "JOIN semantic_index_state s ON m.id = s.media_id AND m.content_hash = s.content_hash "
            "WHERE m.media_type = 'photo' AND s.model_id = ? AND s.model_revision = ? "
            "AND s.embedding IS NOT NULL AND (s.error_text IS NULL OR s.error_text = '');"
        );
        if (!idxStmtRes.isOk()) return idxStmtRes.status();
        auto idxStmt = std::move(idxStmtRes.value());
        idxStmt.bind(1, info.modelId);
        idxStmt.bind(2, info.modelRevision);
        int64_t indexedPhotos = 0;
        if (idxStmt.step() == StepResult::Row) {
            indexedPhotos = idxStmt.getInt64(0);
        }
        if (indexedPhotos <= 0) {
            return Status::invalidArgument("Build the search index first (0 photos indexed)");
        }

        if (scope == "selected") {
            candidates = mediaIds;
        } else {
            auto stmtRes = conn.prepare(R"SQL(
                SELECT m.id FROM media_items m
                JOIN semantic_index_state s
                  ON m.id = s.media_id
                  AND m.content_hash = s.content_hash
                  AND s.model_id = ?
                  AND s.model_revision = ?
                  AND s.vector_dim = ?
                  AND s.embedding IS NOT NULL
                  AND (s.error_text IS NULL OR s.error_text = '')
                WHERE m.media_type = 'photo'
                ORDER BY m.id;
            )SQL");
            if (!stmtRes.isOk()) return stmtRes.status();
            auto stmt = std::move(stmtRes.value());
            stmt.bind(1, info.modelId);
            stmt.bind(2, info.modelRevision);
            stmt.bind(3, info.vectorDim);
            while (stmt.step() == StepResult::Row) {
                candidates.push_back(stmt.getInt64(0));
            }
        }

        auto insRes = conn.prepare(
            "INSERT INTO semantic_jobs (scope, state, model_id, kind, total, processed, skipped, failed, remaining, created_at, updated_at) "
            "VALUES (?, 'running', ?, 'tags', ?, 0, 0, 0, ?, ?, ?);"
        );
        if (!insRes.isOk()) return insRes.status();
        auto ins = std::move(insRes.value());
        ins.bind(1, scope);
        ins.bind(2, info.modelId);
        ins.bind(3, static_cast<int64_t>(candidates.size()));
        ins.bind(4, static_cast<int64_t>(candidates.size()));
        int64_t now = nowSeconds();
        ins.bind(5, now);
        ins.bind(6, now);
        if (ins.step() != StepResult::Done) {
            return Status::databaseError("Failed to insert smart tag job row");
        }
        jobId = conn.lastInsertRowId();
        if (jobId <= 0) {
            return Status::databaseError("Failed to obtain smart tag job ID");
        }
    }

    {
        std::lock_guard<std::mutex> idleLock(impl_->idleMutex);
        impl_->jobRunning = true;
    }
    impl_->idleCv.notify_all();

    impl_->worker = std::jthread([this, jobId, scope, candidates = std::move(candidates), force] {
        runTagJob(jobId, scope, std::move(candidates), force);
    });

    return Status::ok();
}

Status Service::clearSmartTags() {
    std::lock_guard<std::mutex> startLock(impl_->jobStartMutex);
    {
        std::lock_guard<std::mutex> idleLock(impl_->idleMutex);
        if (impl_->jobRunning) {
            return Status::alreadyExists("A job is currently running");
        }
    }
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
        auto countRes = impl_->db.conn_.prepare("SELECT COUNT(*) FROM semantic_jobs WHERE state IN ('running','cancelling');");
        if (countRes.isOk()) {
            auto& stmt = countRes.value();
            if (stmt.step() == StepResult::Row && stmt.getInt64(0) > 0) {
                return Status::alreadyExists("A job is currently running");
            }
        }
    }
    return impl_->db.clearSmartTags();
}

void Service::runTagJobForTest(int64_t jobId, std::string scope, std::vector<MediaId> mediaIds, bool force) {
    impl_->cancelRequested.store(false);
    runTagJob(jobId, std::move(scope), std::move(mediaIds), force);
}

void Service::runTagJob(int64_t jobId, std::string scope, std::vector<MediaId> candidateIds, bool force) {
    IMAGINE_LOG_INFO("Smart tag job " + std::to_string(jobId) + " started processing " + std::to_string(candidateIds.size()) + " items");
    {
        std::lock_guard<std::mutex> lock(impl_->idleMutex);
        impl_->jobRunning = true;
    }
    impl_->idleCv.notify_all();

    struct TagJobGuard {
        Service::Impl& impl;
        int64_t jobId;
        ~TagJobGuard() {
            {
                std::lock_guard<std::mutex> lock(impl.idleMutex);
                impl.jobRunning = false;
                impl.lastActivityTime = std::chrono::steady_clock::now();
            }
            impl.cancelRequested.store(false);
            impl.idleCv.notify_all();
        }
    } guard{*impl_, jobId};

    try {
        const auto info = impl_->engine.info();
        if (!info.ready || !impl_->taxonomyReady) {
            std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
            auto stmtRes = impl_->db.conn_.prepare("UPDATE semantic_jobs SET state='failed', error=?, remaining=0, updated_at=? WHERE id=?;");
            if (stmtRes.isOk()) {
                auto& stmt = stmtRes.value();
                stmt.bind(1, !info.ready ? "CLIP engine is not ready" : impl_->taxonomyError);
                stmt.bind(2, nowSeconds());
                stmt.bind(3, jobId);
                stmt.step();
            }
            return;
        }

        // Lazy tag mapping: only create/fetch tag row when a label is assigned
        std::unordered_map<std::string, TagId> labelIdToTagId;

        // Compute or load label embeddings from cache
        std::vector<std::vector<float>> labelEmbeddings;
        {
            std::lock_guard<std::mutex> cacheLock(impl_->labelCacheMutex);
            std::string cacheKey = info.modelId + ":" + info.modelRevision + ":" + impl_->taxonomy.version;
            if (impl_->labelCache.key == cacheKey && impl_->labelCache.embeddings.size() == impl_->taxonomy.labels.size()) {
                labelEmbeddings = impl_->labelCache.embeddings;
            } else {
                IMAGINE_LOG_INFO("Encoding label prompts for Smart Tags taxonomy (version " + impl_->taxonomy.version + ")...");
                labelEmbeddings.reserve(impl_->taxonomy.labels.size());
                for (const auto& label : impl_->taxonomy.labels) {
                    std::vector<std::vector<float>> promptEmbeddings;
                    promptEmbeddings.reserve(label.prompts.size());
                    for (const auto& prompt : label.prompts) {
                        auto encRes = impl_->engine.encodeText(prompt);
                        if (!encRes.isOk()) {
                            IMAGINE_LOG_ERROR("Failed to encode text prompt '" + prompt + "': " + encRes.status().message());
                            std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
                            auto stmtRes = impl_->db.conn_.prepare("UPDATE semantic_jobs SET state='failed', error=?, remaining=0, updated_at=? WHERE id=?;");
                            if (stmtRes.isOk()) {
                                auto& stmt = stmtRes.value();
                                stmt.bind(1, "Failed to encode text: " + encRes.status().message());
                                stmt.bind(2, nowSeconds());
                                stmt.bind(3, jobId);
                                stmt.step();
                            }
                            return;
                        }
                        promptEmbeddings.push_back(std::move(encRes.value()));
                    }
                    labelEmbeddings.push_back(meanPoolAndNormalize(promptEmbeddings));
                }
                impl_->labelCache.key = std::move(cacheKey);
                impl_->labelCache.embeddings = labelEmbeddings;
            }
        }

        std::unordered_map<MediaId, std::string> candidateHashes;
        std::unordered_set<MediaId> upToDateMediaIds;
        std::unordered_set<uint64_t> rejections;

        {
            std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
            Connection& conn = impl_->db.conn_;

            auto rejRes = conn.prepare("SELECT media_id, tag_id FROM smart_tag_rejections;");
            if (rejRes.isOk()) {
                auto& rejStmt = rejRes.value();
                while (rejStmt.step() == StepResult::Row) {
                    MediaId mid = rejStmt.getInt64(0);
                    TagId tid = rejStmt.getInt64(1);
                    uint64_t key = (static_cast<uint64_t>(mid) << 32) | static_cast<uint32_t>(tid);
                    rejections.insert(key);
                }
            }

            for (size_t offset = 0; offset < candidateIds.size(); offset += 500) {
                size_t chunkEnd = std::min(offset + 500, candidateIds.size());
                std::string inClause = "(";
                for (size_t i = offset; i < chunkEnd; ++i) {
                    inClause += std::to_string(candidateIds[i]);
                    if (i < chunkEnd - 1) inClause += ",";
                }
                inClause += ")";
                auto hashRes = conn.prepare("SELECT id, content_hash FROM media_items WHERE id IN " + inClause + ";");
                if (hashRes.isOk()) {
                    auto& stmt = hashRes.value();
                    while (stmt.step() == StepResult::Row) {
                        candidateHashes[stmt.getInt64(0)] = stmt.getString(1);
                    }
                }
            }

            if (!force) {
                auto stRes = conn.prepare(
                    "SELECT media_id, content_hash FROM smart_tag_state "
                    "WHERE model_id = ? AND model_revision = ? AND taxonomy_version = ?;"
                );
                if (stRes.isOk()) {
                    auto& stmt = stRes.value();
                    stmt.bind(1, info.modelId);
                    stmt.bind(2, info.modelRevision);
                    stmt.bind(3, impl_->taxonomy.version);
                    while (stmt.step() == StepResult::Row) {
                        MediaId mid = stmt.getInt64(0);
                        std::string ch = stmt.getString(1);
                        auto it = candidateHashes.find(mid);
                        if (it != candidateHashes.end() && it->second == ch) {
                            upToDateMediaIds.insert(mid);
                        }
                    }
                }
            }
        }

        SmartTagOptions options = loadSmartTagOptionsFromEnv();

        int64_t processed = 0;
        int64_t skipped = 0;
        int64_t failed = 0;
        int64_t total = static_cast<int64_t>(candidateIds.size());

        std::vector<db::SmartTagRecord> batchRecords;
        batchRecords.reserve(200);

        auto flushBatch = [&]() {
            if (batchRecords.empty()) return;
            Status st = impl_->db.replaceSmartTags(batchRecords, info.modelId, info.modelRevision, impl_->taxonomy.version);
            if (st.isOk()) {
                processed += static_cast<int64_t>(batchRecords.size());
            } else {
                IMAGINE_LOG_WARN("Batch replaceSmartTags failed (" + st.message() + "); retrying individually");
                for (const auto& rec : batchRecords) {
                    Status singleSt = impl_->db.replaceSmartTags({rec}, info.modelId, info.modelRevision, impl_->taxonomy.version);
                    if (singleSt.isOk()) {
                        processed++;
                    } else {
                        failed++;
                    }
                }
            }
            batchRecords.clear();

            std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
            int64_t remaining = std::max<int64_t>(0, total - (processed + skipped + failed));
            auto upRes = impl_->db.conn_.prepare(
                "UPDATE semantic_jobs SET processed=?, skipped=?, failed=?, remaining=?, updated_at=? WHERE id=?;"
            );
            if (upRes.isOk()) {
                auto& stmt = upRes.value();
                stmt.bind(1, processed);
                stmt.bind(2, skipped);
                stmt.bind(3, failed);
                stmt.bind(4, remaining);
                stmt.bind(5, nowSeconds());
                stmt.bind(6, jobId);
                stmt.step();
            }
        };

        bool cancelled = false;

        for (size_t i = 0; i < candidateIds.size(); ++i) {
            if (impl_->cancelRequested.load()) {
                cancelled = true;
                if (!batchRecords.empty()) {
                    flushBatch();
                }
                break;
            }

            MediaId mid = candidateIds[i];

            auto hashIt = candidateHashes.find(mid);
            if (hashIt == candidateHashes.end()) {
                failed++;
                continue;
            }
            const std::string& contentHash = hashIt->second;

            if (!force && upToDateMediaIds.find(mid) != upToDateMediaIds.end()) {
                skipped++;
                continue;
            }

            // Always read DB embedding matching current content_hash to prevent stale in-memory vectors
            int vecDim = 0;
            std::vector<float> imageVec;
            {
                std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
                auto stmtRes = impl_->db.conn_.prepare(
                    "SELECT embedding, vector_dim FROM semantic_index_state "
                    "WHERE media_id = ? AND content_hash = ? AND model_id = ? AND model_revision = ? "
                    "AND vector_dim = ? AND embedding IS NOT NULL AND (error_text IS NULL OR error_text = '');"
                );
                if (stmtRes.isOk()) {
                    auto& stmt = stmtRes.value();
                    stmt.bind(1, mid);
                    stmt.bind(2, contentHash);
                    stmt.bind(3, info.modelId);
                    stmt.bind(4, info.modelRevision);
                    stmt.bind(5, info.vectorDim);
                    if (stmt.step() == StepResult::Row) {
                        vecDim = stmt.getInt(1);
                        imageVec = readVector(stmt, 0, vecDim);
                    }
                }
            }

            if (imageVec.empty() || vecDim != info.vectorDim) {
                skipped++;
                continue;
            }

            auto selected = selectSmartLabels(imageVec, labelEmbeddings, impl_->taxonomy.labels, options);

            std::vector<TagId> tagIds;
            for (const auto& s : selected) {
                TagId tid = 0;
                auto tagIdIt = labelIdToTagId.find(s.id);
                if (tagIdIt != labelIdToTagId.end()) {
                    tid = tagIdIt->second;
                } else {
                    std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
                    auto tagRes = impl_->db.createOrGetTag(s.name, "keyword");
                    if (!tagRes.isOk()) continue;
                    tid = tagRes.value();
                    labelIdToTagId[s.id] = tid;
                }
                uint64_t key = (static_cast<uint64_t>(mid) << 32) | static_cast<uint32_t>(tid);
                if (rejections.find(key) != rejections.end()) {
                    continue;
                }
                tagIds.push_back(tid);
            }

            batchRecords.push_back(db::SmartTagRecord{mid, std::move(tagIds), contentHash});

            if (batchRecords.size() >= 200) {
                flushBatch();
            }
        }

        if (!batchRecords.empty()) {
            flushBatch();
        }

        std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
        int64_t remaining = std::max<int64_t>(0, total - (processed + skipped + failed));
        std::string finalState = cancelled ? "cancelled" : (processed == 0 && failed > 0 ? "failed" : "completed");
        std::string finalError = (!cancelled && processed == 0 && failed > 0) ? "All candidate media failed to be labeled" : "";
        auto finalRes = impl_->db.conn_.prepare(
            "UPDATE semantic_jobs SET state=?, error=?, processed=?, skipped=?, failed=?, remaining=?, updated_at=? WHERE id=?;"
        );
        if (finalRes.isOk()) {
            auto& stmt = finalRes.value();
            stmt.bind(1, finalState);
            stmt.bind(2, finalError);
            stmt.bind(3, processed);
            stmt.bind(4, skipped);
            stmt.bind(5, failed);
            stmt.bind(6, remaining);
            stmt.bind(7, nowSeconds());
            stmt.bind(8, jobId);
            stmt.step();
        }
        IMAGINE_LOG_INFO("Smart tag job " + std::to_string(jobId) + " " + finalState + " (processed=" + std::to_string(processed) +
                         ", skipped=" + std::to_string(skipped) + ", failed=" + std::to_string(failed) + ")");
    } catch (const std::exception& ex) {
        IMAGINE_LOG_ERROR("Smart tag job " + std::to_string(jobId) + " failed with exception: " + ex.what());
        std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
        auto finalRes = impl_->db.conn_.prepare(
            "UPDATE semantic_jobs SET state='failed', error=?, remaining=0, updated_at=? WHERE id=?;"
        );
        if (finalRes.isOk()) {
            auto& stmt = finalRes.value();
            stmt.bind(1, std::string("Internal error: ") + ex.what());
            stmt.bind(2, nowSeconds());
            stmt.bind(3, jobId);
            stmt.step();
        }
    } catch (...) {
        IMAGINE_LOG_ERROR("Smart tag job " + std::to_string(jobId) + " failed with unknown exception");
        std::lock_guard<std::recursive_mutex> lock(impl_->db.mutex_);
        auto finalRes = impl_->db.conn_.prepare(
            "UPDATE semantic_jobs SET state='failed', error='Internal error: unknown exception', remaining=0, updated_at=? WHERE id=?;"
        );
        if (finalRes.isOk()) {
            auto& stmt = finalRes.value();
            stmt.bind(1, nowSeconds());
            stmt.bind(2, jobId);
            stmt.step();
        }
    }
}

} // namespace imagine::clip
