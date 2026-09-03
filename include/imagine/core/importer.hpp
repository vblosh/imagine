#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include "imagine/common/types.hpp"
#include "imagine/common/error.hpp"
#include "imagine/db/catalog_db.hpp"
#include "imagine/thumbnail/cache.hpp"
#include "imagine/concurrency/thread_pool.hpp"

namespace imagine::core {

struct ImportProgress {
    std::atomic<int64_t> total_files{0};
    std::atomic<int64_t> processed_files{0};
    std::atomic<int64_t> imported_files{0};
    std::atomic<int64_t> skipped_files{0};
    std::atomic<int64_t> failed_files{0};
    std::string current_file;
    bool is_running{false};

    ImportProgress() = default;

    ImportProgress(const ImportProgress& other) {
        total_files.store(other.total_files.load());
        processed_files.store(other.processed_files.load());
        imported_files.store(other.imported_files.load());
        skipped_files.store(other.skipped_files.load());
        failed_files.store(other.failed_files.load());
        current_file = other.current_file;
        is_running = other.is_running;
    }

    ImportProgress& operator=(const ImportProgress& other) {
        if (this != &other) {
            total_files.store(other.total_files.load());
            processed_files.store(other.processed_files.load());
            imported_files.store(other.imported_files.load());
            skipped_files.store(other.skipped_files.load());
            failed_files.store(other.failed_files.load());
            current_file = other.current_file;
            is_running = other.is_running;
        }
        return *this;
    }
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

private:
    ProcessStatus processFileInternal(const std::string& filePath, MediaItem* outItem = nullptr);

    db::CatalogDb& db_;
    thumbnail::Cache& cache_;
    concurrency::ThreadPool* pool_{nullptr};

    std::atomic<bool> cancelled_{false};
    std::atomic<bool> running_{false};
    mutable std::mutex progress_mutex_;
    ImportProgress current_progress_;
};

} // namespace imagine::core

namespace imagine {
using core::ImportProgress;
using core::Importer;
}
