#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <ostream>
#include <compare>
#include <nlohmann/json.hpp>
#include "imagine/common/types.hpp"
#include "imagine/common/error.hpp"
#include "imagine/db/catalog_db.hpp"
#include "imagine/thumbnail/cache.hpp"
#include "imagine/concurrency/thread_pool.hpp"

namespace imagine::core {

/**
 * @brief Result structure returned by internal batch persistence operations.
 */
struct BatchFlushResult {
    size_t persisted_count{0};
    size_t failed_count{0};
    bool batch_transaction_failed{false};
};

/**
 * @brief Lightweight snapshot integer wrapper that supports load()/store()
 * and transparent integer conversions, allowing ImportProgress to be fully
 * movable and copyable without embedding atomics in the snapshot type.
 *
 * @note IMPORTANT: ProgressValue is a non-atomic value type. It provides load(),
 * store(), and integer conversions solely for API backward-compatibility with code
 * that previously accessed atomic progress fields. It is NOT thread-safe for concurrent
 * unsynchronized mutations. Thread-safe progress observation should be performed by
 * obtaining a snapshot via Importer::currentProgress() or through the ProgressCallback.
 */
struct ProgressValue {
    int64_t val{0};

    constexpr ProgressValue() noexcept = default;
    constexpr ProgressValue(int64_t v) noexcept : val(v) {}
    constexpr ProgressValue(const ProgressValue&) noexcept = default;
    constexpr ProgressValue(ProgressValue&&) noexcept = default;
    constexpr ProgressValue& operator=(const ProgressValue&) noexcept = default;
    constexpr ProgressValue& operator=(ProgressValue&&) noexcept = default;

    constexpr ProgressValue& operator=(int64_t v) noexcept {
        val = v;
        return *this;
    }

    [[nodiscard]] constexpr int64_t load() const noexcept { return val; }
    void store(int64_t v) noexcept { val = v; }

    constexpr operator int64_t() const noexcept { return val; }

    ProgressValue& operator++() noexcept { ++val; return *this; }
    ProgressValue operator++(int) noexcept { ProgressValue tmp = *this; ++val; return tmp; }
    ProgressValue& operator+=(int64_t delta) noexcept { val += delta; return *this; }

    auto operator<=>(const ProgressValue&) const = default;
    auto operator<=>(int64_t other) const noexcept { return val <=> other; }
    bool operator==(int64_t other) const noexcept { return val == other; }

    friend std::ostream& operator<<(std::ostream& os, const ProgressValue& pv) {
        return os << pv.val;
    }
};

inline void to_json(nlohmann::json& j, const ProgressValue& v) { j = v.val; }
inline void from_json(const nlohmann::json& j, ProgressValue& v) { v.val = j.get<int64_t>(); }

struct ImportProgress {
    ProgressValue total_files{0};
    ProgressValue processed_files{0};
    ProgressValue imported_files{0};
    ProgressValue skipped_files{0};
    ProgressValue failed_files{0};
    ProgressValue cancelled_files{0};
    std::string current_file;
    bool is_running{false};

    ImportProgress() = default;
    ImportProgress(const ImportProgress&) = default;
    ImportProgress(ImportProgress&&) noexcept = default;
    ImportProgress& operator=(const ImportProgress&) = default;
    ImportProgress& operator=(ImportProgress&&) noexcept = default;
    ~ImportProgress() = default;
};

void to_json(nlohmann::json& j, const ImportProgress& p);
void from_json(const nlohmann::json& j, ImportProgress& p);

class Importer {
public:
    using ProgressCallback = std::function<void(const ImportProgress&)>;

    enum class ProcessStatus {
        Imported,
        Skipped,
        Failed
    };

    Importer(db::CatalogDb& db, thumbnail::Cache& cache, concurrency::ThreadPool* pool = nullptr);
    ~Importer() = default;

    Importer(const Importer&) = delete;
    Importer& operator=(const Importer&) = delete;

    Result<ImportProgress> importDirectory(
        const std::string& directoryPath,
        bool recursive = true,
        ProgressCallback progressCb = nullptr
    );

    Result<MediaItem> importFile(const std::string& filePath);

    void setThreadPool(concurrency::ThreadPool* pool) noexcept { pool_ = pool; }
    concurrency::ThreadPool* threadPool() const noexcept { return pool_; }

    void cancel() noexcept;
    bool isCancelled() const noexcept;
    bool isRunning() const noexcept;
    ImportProgress currentProgress() const;

    static bool isSupportedExtension(const std::string& path);
    static std::string normalizePath(const std::string& path);

    using FileHook = std::function<void(const std::string&)>;
    void setFileHook(FileHook hook) { file_hook_ = std::move(hook); }

private:
    ProcessStatus processFileInternal(const std::string& filePath, MediaItem* outItem = nullptr, bool commitToDb = true);

    db::CatalogDb& db_;
    thumbnail::Cache& cache_;
    concurrency::ThreadPool* pool_{nullptr};

    std::atomic<bool> cancelled_{false};
    std::atomic<bool> running_{false};
    mutable std::mutex progress_mutex_;
    ImportProgress current_progress_;

    // Aggregate memory throttling for concurrent file processing
    mutable std::mutex memory_mutex_;
    mutable std::condition_variable memory_cv_;
    size_t in_flight_bytes_{0};
    static constexpr size_t kMaxInFlightBytes = 256ULL * 1024ULL * 1024ULL; // 256 MB aggregate budget
    FileHook file_hook_{nullptr};
};

} // namespace imagine::core

namespace imagine {
using core::ImportProgress;
using core::Importer;
}
