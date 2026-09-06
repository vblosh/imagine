#include "imagine/core/importer.hpp"
#include "imagine/metadata/hasher.hpp"
#include "imagine/metadata/exif_reader.hpp"
#include "imagine/thumbnail/generator.hpp"
#include "imagine/common/logger.hpp"
#include <fstream>
#include <filesystem>
#include <chrono>
#include <cctype>

namespace imagine::core {

void to_json(nlohmann::json& j, const ImportProgress& p) {
    j = nlohmann::json{
        {"total_files", p.total_files.load()},
        {"processed_files", p.processed_files.load()},
        {"imported_files", p.imported_files.load()},
        {"skipped_files", p.skipped_files.load()},
        {"failed_files", p.failed_files.load()},
        {"cancelled_files", p.cancelled_files.load()},
        {"current_file", p.current_file},
        {"is_running", p.is_running}
    };
}

void from_json(const nlohmann::json& j, ImportProgress& p) {
    if (j.contains("total_files") && j["total_files"].is_number()) {
        p.total_files.store(j["total_files"].get<int64_t>());
    }
    if (j.contains("processed_files") && j["processed_files"].is_number()) {
        p.processed_files.store(j["processed_files"].get<int64_t>());
    }
    if (j.contains("imported_files") && j["imported_files"].is_number()) {
        p.imported_files.store(j["imported_files"].get<int64_t>());
    }
    if (j.contains("skipped_files") && j["skipped_files"].is_number()) {
        p.skipped_files.store(j["skipped_files"].get<int64_t>());
    }
    if (j.contains("failed_files") && j["failed_files"].is_number()) {
        p.failed_files.store(j["failed_files"].get<int64_t>());
    }
    if (j.contains("cancelled_files") && j["cancelled_files"].is_number()) {
        p.cancelled_files.store(j["cancelled_files"].get<int64_t>());
    }
    if (j.contains("current_file") && j["current_file"].is_string()) {
        p.current_file = j["current_file"].get<std::string>();
    }
    if (j.contains("is_running") && j["is_running"].is_boolean()) {
        p.is_running = j["is_running"].get<bool>();
    }
}

Importer::Importer(db::CatalogDb& db, thumbnail::Cache& cache, concurrency::ThreadPool* pool, std::string photosDir)
    : db_(db), cache_(cache), pool_(pool), photosDir_(std::move(photosDir)) {}

bool Importer::isInsideRootDir(const std::filesystem::path& target, const std::filesystem::path& rootDir) {
    if (rootDir.empty() || target.empty()) {
        return false;
    }
    std::error_code ec;
    auto canTarget = std::filesystem::weakly_canonical(target, ec);
    if (ec) canTarget = target.lexically_normal();
    auto canRoot = std::filesystem::weakly_canonical(rootDir, ec);
    if (ec) canRoot = rootDir.lexically_normal();

    auto rel = std::filesystem::relative(canTarget, canRoot, ec);
    if (ec || rel.empty()) {
        return false;
    }
    std::string relStr = rel.generic_string();
    if (relStr.empty() || relStr == ".") {
        return false;
    }
    if (relStr == ".." || relStr.rfind("../", 0) == 0) {
        return false;
    }
    return true;
}

std::string Importer::toRelativePath(const std::filesystem::path& fullPath, const std::filesystem::path& baseDir) {
    if (baseDir.empty()) {
        return fullPath.lexically_normal().generic_string();
    }
    std::error_code ec;
    auto canTarget = std::filesystem::weakly_canonical(fullPath, ec);
    if (ec) canTarget = fullPath.lexically_normal();
    auto canRoot = std::filesystem::weakly_canonical(baseDir, ec);
    if (ec) canRoot = baseDir.lexically_normal();

    auto rel = std::filesystem::relative(canTarget, canRoot, ec);
    if (ec || rel.empty()) {
        return fullPath.lexically_normal().generic_string();
    }
    std::string relStr = rel.generic_string();
    if (relStr.rfind("../", 0) == 0 || relStr == "..") {
        return fullPath.lexically_normal().generic_string();
    }
    return relStr;
}

bool Importer::isSupportedExtension(const std::string& path) {
    std::filesystem::path p(path);
    std::string ext = p.extension().string();
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" ||
           ext == ".bmp" || ext == ".webp" || ext == ".tiff" || ext == ".tif";
}

std::string Importer::normalizePath(const std::string& path) {
    std::error_code ec;
    // Preserve symlink rejection: do not follow or normalize symlink paths
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec))) {
        return path;
    }
    auto weakly = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return weakly.string();
    }
    return std::filesystem::path(path).lexically_normal().string();
}

void Importer::cancel() noexcept {
    cancelled_.store(true);
    memory_cv_.notify_all();
}

bool Importer::isCancelled() const noexcept {
    return cancelled_.load();
}

bool Importer::isRunning() const noexcept {
    return running_.load();
}

ImportProgress Importer::currentProgress() const {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    return current_progress_;
}

Importer::ProcessStatus Importer::processFileInternal(const std::string& filePath, MediaItem* outItem, bool commitToDb) {
    std::error_code ec;
    // Symlink restriction: disallow importing symlink files
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(filePath, ec))) {
        IMAGINE_LOG_ERROR("Symlink file rejected for security: " + filePath);
        return ProcessStatus::Failed;
    }

    if (!std::filesystem::exists(filePath, ec) || !std::filesystem::is_regular_file(filePath, ec)) {
        IMAGINE_LOG_ERROR("File does not exist or is not a regular file: " + filePath);
        return ProcessStatus::Failed;
    }

    // Supported file extension check
    if (!isSupportedExtension(filePath)) {
        IMAGINE_LOG_ERROR("Unsupported file type: " + filePath);
        return ProcessStatus::Failed;
    }

    // Path normalization for deduplication and consistent DB querying
    std::string normalizedPath = normalizePath(filePath);

    // Root containment check: if photosDir_ is set, file must be inside photosDir_
    if (!photosDir_.empty()) {
        if (!isInsideRootDir(normalizedPath, photosDir_)) {
            IMAGINE_LOG_ERROR("File rejected: must be located inside photos root directory (" + photosDir_ + "): " + filePath);
            return ProcessStatus::Failed;
        }
    }

    std::string storedPath = !photosDir_.empty() ? toRelativePath(normalizedPath, photosDir_) : normalizedPath;

    auto fsize = std::filesystem::file_size(filePath, ec);
    if (ec) {
        IMAGINE_LOG_ERROR("Failed to get file size for " + filePath + ": " + ec.message());
        return ProcessStatus::Failed;
    }

    // File size limit: reject 0-byte files or files exceeding 500 MB limit
    constexpr uintmax_t kMaxFileSize = 500ULL * 1024ULL * 1024ULL; // 500 MB
    if (fsize == 0 || fsize > kMaxFileSize) {
        IMAGINE_LOG_ERROR("File size invalid or exceeds 500MB limit: " + filePath);
        return ProcessStatus::Failed;
    }

    auto lwt = std::filesystem::last_write_time(filePath, ec);
    if (ec) {
        IMAGINE_LOG_ERROR("Failed to get modified time for " + filePath + ": " + ec.message());
        return ProcessStatus::Failed;
    }

    auto sctp = std::chrono::clock_cast<std::chrono::system_clock>(lwt);
    int64_t modifiedTime = std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count();

    // Check existing file by relative stored path (or normalized absolute path fallback for legacy DBs)
    auto existingRes = db_.getMediaByPath(storedPath);
    if (!existingRes.isOk() && storedPath != normalizedPath) {
        existingRes = db_.getMediaByPath(normalizedPath);
    }
    bool isUpdate = existingRes.isOk();
    if (isUpdate) {
        const auto& existing = existingRes.value();
        if (existing.file_modified_time == modifiedTime && existing.file_size == static_cast<int64_t>(fsize)) {
            if (outItem) {
                *outItem = existing;
            }
            return ProcessStatus::Skipped;
        }
    }

    // Memory throttling: limit aggregate in-flight memory allocation across workers.
    // Files larger than kMaxInFlightBytes are allowed to proceed as a single exclusive
    // allocation when in_flight_bytes_ == 0.
    const size_t requiredBudget = std::min<size_t>(static_cast<size_t>(fsize), kMaxInFlightBytes);
    {
        std::unique_lock<std::mutex> memLock(memory_mutex_);
        memory_cv_.wait(memLock, [&]() {
            return cancelled_.load() || in_flight_bytes_ == 0 || (in_flight_bytes_ + requiredBudget <= kMaxInFlightBytes);
        });
        if (cancelled_.load()) {
            return ProcessStatus::Failed;
        }
        in_flight_bytes_ += requiredBudget;
    }

    // RAII guard to guarantee memory permits are released
    struct MemoryGuard {
        std::mutex& mtx;
        std::condition_variable& cv;
        size_t& tracker;
        size_t bytes;
        bool released{false};

        void release() {
            if (!released && bytes > 0) {
                std::lock_guard<std::mutex> lock(mtx);
                tracker -= bytes;
                released = true;
                cv.notify_all();
            }
        }

        ~MemoryGuard() {
            release();
        }
    } memGuard{memory_mutex_, memory_cv_, in_flight_bytes_, requiredBudget};

    // Read file once into memory buffer
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        IMAGINE_LOG_ERROR("Unable to open file for import: " + filePath);
        return ProcessStatus::Failed;
    }

    std::vector<uint8_t> fileBytes(static_cast<size_t>(fsize));
    file.read(reinterpret_cast<char*>(fileBytes.data()), static_cast<std::streamsize>(fsize));
    size_t bytesRead = static_cast<size_t>(file.gcount());
    if (bytesRead == 0) {
        IMAGINE_LOG_ERROR("Failed to read file content (0 bytes read): " + filePath);
        return ProcessStatus::Failed;
    }
    if (bytesRead != static_cast<size_t>(fsize)) {
        IMAGINE_LOG_WARN("File size changed during read for " + filePath + ": expected " +
                         std::to_string(fsize) + ", read " + std::to_string(bytesRead));
        fileBytes.resize(bytesRead);
        fsize = bytesRead;
    }

    // TOCTOU verification: re-fetch modification time after reading
    auto lwtPost = std::filesystem::last_write_time(filePath, ec);
    if (!ec) {
        auto sctpPost = std::chrono::clock_cast<std::chrono::system_clock>(lwtPost);
        modifiedTime = std::chrono::duration_cast<std::chrono::seconds>(sctpPost.time_since_epoch()).count();
    }

    // Extract SHA-256 hash using in-memory bytes
    std::string hash = metadata::Hasher::computeBytesSha256(fileBytes.data(), fileBytes.size());
    if (hash.empty()) {
        IMAGINE_LOG_ERROR("Failed to compute SHA-256 for " + filePath);
        return ProcessStatus::Failed;
    }

    // Extract EXIF data using in-memory buffer
    ExifData exif;
    auto exifRes = metadata::ExifReader::readFromBuffer(fileBytes.data(), fileBytes.size());
    if (exifRes.isOk()) {
        exif = exifRes.value();
    }

    // Date taken fallback
    int64_t dateTaken = exif.date_taken;
    if (dateTaken <= 0) {
        dateTaken = modifiedTime;
    }

    // Thumbnails & dimensions via cache
    int width = 0;
    int height = 0;
    std::string thumbSmall;
    std::string thumbLarge;
    auto thumbRes = cache_.ensureDualThumbnailsFromMemory(
        fileBytes.data(), fileBytes.size(), hash, exif.orientation, &width, &height
    );
    if (thumbRes.isOk()) {
        thumbSmall = thumbRes.value().first;
        thumbLarge = thumbRes.value().second;
    } else {
        IMAGINE_LOG_WARN("Failed to generate thumbnails for " + filePath + ": " + thumbRes.status().message());
    }

    // Deallocate in-memory image buffer immediately and release memory permit
    std::vector<uint8_t>().swap(fileBytes);
    memGuard.release();

    // Populate media item
    MediaItem item;
    if (isUpdate) {
        item = existingRes.value();
    }
    item.file_path = storedPath;
    item.file_name = std::filesystem::path(normalizedPath).filename().string();
    item.file_size = static_cast<int64_t>(fsize);
    item.file_modified_time = modifiedTime;
    item.content_hash = hash;
    item.width = width;
    item.height = height;
    item.date_taken = dateTaken;
    item.exif = exif;
    item.thumb_small = thumbnail::Cache::getRelativeThumbnailPath(hash, thumbnail::Cache::SmallSize);
    item.thumb_large = thumbnail::Cache::getRelativeThumbnailPath(hash, thumbnail::Cache::LargeSize);

    // Database persistence: if commitToDb is true, persist immediately.
    // Otherwise, persistence is deferred to batch persistence in importDirectory.
    if (commitToDb) {
        if (isUpdate) {
            Status s = db_.updateMedia(item);
            if (!s.isOk()) {
                IMAGINE_LOG_ERROR("Failed to update media in DB for " + storedPath + ": " + s.message());
                return ProcessStatus::Failed;
            }
        } else {
            auto insRes = db_.insertMedia(item);
            if (!insRes.isOk()) {
                IMAGINE_LOG_ERROR("Failed to insert media into DB for " + storedPath + ": " + insRes.status().message());
                return ProcessStatus::Failed;
            }
            item.id = insRes.value();
        }
    } else {
        item.id = isUpdate ? existingRes.value().id : 0;
    }

    if (outItem) {
        *outItem = std::move(item);
    }
    return ProcessStatus::Imported;
}

Result<MediaItem> Importer::importFile(const std::string& filePath) {
    if (!isSupportedExtension(filePath)) {
        return Status::invalidArgument("Unsupported image extension: " + filePath);
    }
    std::string normPath = normalizePath(filePath);
    if (photosDir_.empty()) {
        photosDir_ = std::filesystem::path(normPath).parent_path().string();
    } else if (!isInsideRootDir(normPath, photosDir_)) {
        return Status::invalidArgument("File is outside photos root directory (" + photosDir_ + "): " + filePath);
    }
    MediaItem item;
    ProcessStatus st = processFileInternal(filePath, &item, true);
    if (st == ProcessStatus::Failed) {
        return Status::internal("Failed to import image: " + filePath);
    }
    return item;
}

Result<ImportProgress> Importer::importDirectory(
    const std::string& directoryPath,
    bool recursive,
    ProgressCallback progressCb
) {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return Status::alreadyExists("Importer is already running");
    }
    cancelled_.store(false);

    // RAII guard ensuring running_ is always reset on exit or exceptions
    struct RunningGuard {
        std::atomic<bool>& flag;
        ~RunningGuard() { flag.store(false); }
    } runningGuard{running_};

    std::error_code ec;
    if (!std::filesystem::exists(directoryPath, ec) || !std::filesystem::is_directory(directoryPath, ec)) {
        return Status::notFound("Directory does not exist: " + directoryPath);
    }

    std::string normDir = normalizePath(directoryPath);
    if (photosDir_.empty()) {
        photosDir_ = normDir;
    } else {
        std::string normRoot = normalizePath(photosDir_);
        if (normDir != normRoot && !isInsideRootDir(normDir, normRoot)) {
            return Status::invalidArgument("Directory is outside photos root directory (" + photosDir_ + "): " + directoryPath);
        }
    }

    std::vector<std::string> files;
    if (recursive) {
        std::filesystem::recursive_directory_iterator it(
            directoryPath,
            std::filesystem::directory_options::skip_permission_denied,
            ec
        );
        if (ec) {
            IMAGINE_LOG_ERROR("Failed to open directory for recursive scan: " + directoryPath + " (" + ec.message() + ")");
            return Status::ioError("Directory iteration failed: " + ec.message());
        }
        std::filesystem::recursive_directory_iterator end;
        while (it != end && !ec) {
            const auto& entry = *it;
            std::error_code entryEc;
            if (entry.is_regular_file(entryEc) && !entry.is_symlink(entryEc) && isSupportedExtension(entry.path().string())) {
                files.push_back(entry.path().string());
            }
            it.increment(ec);
        }
    } else {
        std::filesystem::directory_iterator it(
            directoryPath,
            std::filesystem::directory_options::skip_permission_denied,
            ec
        );
        if (ec) {
            IMAGINE_LOG_ERROR("Failed to open directory for scan: " + directoryPath + " (" + ec.message() + ")");
            return Status::ioError("Directory iteration failed: " + ec.message());
        }
        std::filesystem::directory_iterator end;
        while (it != end && !ec) {
            const auto& entry = *it;
            std::error_code entryEc;
            if (entry.is_regular_file(entryEc) && !entry.is_symlink(entryEc) && isSupportedExtension(entry.path().string())) {
                files.push_back(entry.path().string());
            }
            it.increment(ec);
        }
    }

    // Check directory iteration errors
    if (ec) {
        IMAGINE_LOG_ERROR("Directory iteration error in " + directoryPath + ": " + ec.message());
        return Status::ioError("Directory iteration failed: " + ec.message());
    }

    files.shrink_to_fit();

    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        current_progress_.total_files = static_cast<int64_t>(files.size());
        current_progress_.processed_files = 0;
        current_progress_.imported_files = 0;
        current_progress_.skipped_files = 0;
        current_progress_.failed_files = 0;
        current_progress_.cancelled_files = 0;
        current_progress_.current_file.clear();
        current_progress_.is_running = true;
    }

    // Progress callback serialization and throttling
    std::mutex callbackMutex;
    auto lastCallbackTime = std::chrono::steady_clock::time_point::min();
    constexpr auto kProgressThrottleInterval = std::chrono::milliseconds(100);

    auto emitProgress = [&](bool force = false) {
        if (!progressCb) return;

        auto now = std::chrono::steady_clock::now();
        if (!force && (now - lastCallbackTime < kProgressThrottleInterval)) {
            return;
        }

        std::unique_lock<std::mutex> cbLock(callbackMutex, std::defer_lock);
        if (force) {
            cbLock.lock();
        } else {
            if (!cbLock.try_lock()) {
                return;
            }
            if (now - lastCallbackTime < kProgressThrottleInterval) {
                return;
            }
        }

        ImportProgress snapshot;
        {
            std::lock_guard<std::mutex> pLock(progress_mutex_);
            snapshot = current_progress_;
        }

        lastCallbackTime = std::chrono::steady_clock::now();
        progressCb(snapshot);
    };

    emitProgress(true); // Emit initial state

    // Batch persistence structures
    std::mutex batchMutex;
    std::vector<MediaItem> pendingInserts;
    std::vector<MediaItem> pendingUpdates;
    constexpr size_t kBatchSize = 64;
    pendingInserts.reserve(kBatchSize);
    pendingUpdates.reserve(kBatchSize);

    auto flushBatch = [&]() -> BatchFlushResult {
        std::lock_guard<std::mutex> bLock(batchMutex);
        BatchFlushResult result;

        // 1. Flush pending updates transactionally
        if (!pendingUpdates.empty()) {
            auto uRes = db_.updateMediaBatch(pendingUpdates);
            if (uRes.isOk()) {
                size_t count = uRes.value();
                result.persisted_count += count;
                std::lock_guard<std::mutex> pLock(progress_mutex_);
                current_progress_.imported_files += static_cast<int64_t>(count);
                current_progress_.processed_files += static_cast<int64_t>(count);
            } else {
                result.batch_transaction_failed = true;
                IMAGINE_LOG_WARN("Batch update failed (" + uRes.status().message() + "), falling back to individual updates");
                for (const auto& item : pendingUpdates) {
                    Status s = db_.updateMedia(item);
                    std::lock_guard<std::mutex> pLock(progress_mutex_);
                    if (s.isOk()) {
                        ++result.persisted_count;
                        ++current_progress_.imported_files;
                        ++current_progress_.processed_files;
                    } else {
                        ++result.failed_count;
                        IMAGINE_LOG_ERROR("Failed to update media in DB for " + item.file_path + ": " + s.message());
                        ++current_progress_.failed_files;
                        ++current_progress_.processed_files;
                    }
                }
            }
            pendingUpdates.clear();
            pendingUpdates.reserve(kBatchSize);
        }

        // 2. Flush pending inserts transactionally
        if (!pendingInserts.empty()) {
            auto bRes = db_.insertMediaBatch(pendingInserts);
            if (bRes.isOk()) {
                size_t count = bRes.value();
                result.persisted_count += count;
                std::lock_guard<std::mutex> pLock(progress_mutex_);
                current_progress_.imported_files += static_cast<int64_t>(count);
                current_progress_.processed_files += static_cast<int64_t>(count);
            } else {
                result.batch_transaction_failed = true;
                IMAGINE_LOG_WARN("Batch insert failed (" + bRes.status().message() + "), falling back to individual inserts");
                for (auto& item : pendingInserts) {
                    auto insRes = db_.insertMedia(item);
                    if (insRes.isOk()) {
                        ++result.persisted_count;
                        std::lock_guard<std::mutex> pLock(progress_mutex_);
                        ++current_progress_.imported_files;
                        ++current_progress_.processed_files;
                    } else {
                        // Atomic upsert fallback for path uniqueness conflict races
                        auto existing = db_.getMediaByPath(item.file_path);
                        if (existing.isOk()) {
                            item.id = existing.value().id;
                            auto updRes = db_.updateMedia(item);
                            if (updRes.isOk()) {
                                ++result.persisted_count;
                                std::lock_guard<std::mutex> pLock(progress_mutex_);
                                ++current_progress_.imported_files;
                                ++current_progress_.processed_files;
                                continue;
                            }
                        }
                        ++result.failed_count;
                        IMAGINE_LOG_ERROR("Failed to insert media into DB for " + item.file_path + ": " + insRes.status().message());
                        std::lock_guard<std::mutex> pLock(progress_mutex_);
                        ++current_progress_.failed_files;
                        ++current_progress_.processed_files;
                    }
                }
            }
            pendingInserts.clear();
            pendingInserts.reserve(kBatchSize);
        }

        return result;
    };

    auto processOne = [&](const std::string& file) {
        if (cancelled_.load()) {
            {
                std::lock_guard<std::mutex> lock(progress_mutex_);
                ++current_progress_.cancelled_files;
                ++current_progress_.processed_files;
            }
            emitProgress();
            return;
        }

        {
            std::lock_guard<std::mutex> lock(progress_mutex_);
            current_progress_.current_file = file;
        }

        MediaItem item;
        ProcessStatus st = processFileInternal(file, &item, false);
        switch (st) {
            case ProcessStatus::Imported: {
                bool needFlush = false;
                {
                    std::lock_guard<std::mutex> bLock(batchMutex);
                    if (item.id > 0) {
                        pendingUpdates.push_back(std::move(item));
                        if (pendingUpdates.size() >= kBatchSize) {
                            needFlush = true;
                        }
                    } else {
                        pendingInserts.push_back(std::move(item));
                        if (pendingInserts.size() >= kBatchSize) {
                            needFlush = true;
                        }
                    }
                }
                if (needFlush) {
                    auto fRes = flushBatch();
                    if (fRes.failed_count > 0 || fRes.batch_transaction_failed) {
                        IMAGINE_LOG_WARN("Batch flush completed with " + std::to_string(fRes.failed_count) + " failures");
                    }
                    emitProgress();
                }
                break;
            }
            case ProcessStatus::Skipped: {
                std::lock_guard<std::mutex> lock(progress_mutex_);
                ++current_progress_.skipped_files;
                ++current_progress_.processed_files;
                break;
            }
            case ProcessStatus::Failed: {
                std::lock_guard<std::mutex> lock(progress_mutex_);
                ++current_progress_.failed_files;
                ++current_progress_.processed_files;
                break;
            }
        }

        if (file_hook_) {
            file_hook_(file);
        }

        emitProgress();
    };

    if (pool_ != nullptr) {
        std::atomic<size_t> remainingTasks{0};
        std::mutex taskMutex;
        std::condition_variable taskCv;

        const size_t maxInFlight = std::max<size_t>(16, pool_->size() * 4);

        for (size_t i = 0; i < files.size(); ++i) {
            if (cancelled_.load()) {
                size_t remaining = files.size() - i;
                std::lock_guard<std::mutex> lock(progress_mutex_);
                current_progress_.cancelled_files += static_cast<int64_t>(remaining);
                current_progress_.processed_files += static_cast<int64_t>(remaining);
                break;
            }

            // Task queue backpressure
            {
                std::unique_lock<std::mutex> lock(taskMutex);
                taskCv.wait(lock, [&]() {
                    return cancelled_.load() || remainingTasks.load() < maxInFlight;
                });
            }

            if (cancelled_.load()) {
                size_t remaining = files.size() - i;
                std::lock_guard<std::mutex> lock(progress_mutex_);
                current_progress_.cancelled_files += static_cast<int64_t>(remaining);
                current_progress_.processed_files += static_cast<int64_t>(remaining);
                break;
            }

            ++remainingTasks;
            try {
                pool_->enqueue([&, file = files[i]]() {
                    struct TaskCompletionGuard {
                        std::atomic<size_t>& rem;
                        std::mutex& mtx;
                        std::condition_variable& cv;
                        ~TaskCompletionGuard() {
                            --rem;
                            std::lock_guard<std::mutex> lock(mtx);
                            cv.notify_all();
                        }
                    } tGuard{remainingTasks, taskMutex, taskCv};

                    processOne(file);
                });
            } catch (const std::exception& ex) {
                --remainingTasks;
                IMAGINE_LOG_ERROR("Failed to enqueue import task: " + std::string(ex.what()));
                std::lock_guard<std::mutex> lock(progress_mutex_);
                ++current_progress_.failed_files;
                ++current_progress_.processed_files;
            }
        }

        // Wait for all submitted tasks to complete
        {
            std::unique_lock<std::mutex> lock(taskMutex);
            taskCv.wait(lock, [&]() {
                return remainingTasks.load() == 0;
            });
        }
    } else {
        for (size_t i = 0; i < files.size(); ++i) {
            if (cancelled_.load()) {
                size_t remaining = files.size() - i;
                std::lock_guard<std::mutex> lock(progress_mutex_);
                current_progress_.cancelled_files += static_cast<int64_t>(remaining);
                current_progress_.processed_files += static_cast<int64_t>(remaining);
                break;
            }
            processOne(files[i]);
        }
    }

    // Flush any remaining items in the batch
    auto finalFlushRes = flushBatch();
    if (finalFlushRes.failed_count > 0 || finalFlushRes.batch_transaction_failed) {
        IMAGINE_LOG_WARN("Final batch flush completed with " + std::to_string(finalFlushRes.failed_count) + " failures");
    }

    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        current_progress_.is_running = false;
        current_progress_.current_file.clear();
    }

    emitProgress(true); // Final progress callback

    ImportProgress finalProgress;
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        finalProgress = current_progress_;
    }
    return finalProgress;
}

} // namespace imagine::core
