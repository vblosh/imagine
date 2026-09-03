#include "imagine/core/importer.hpp"
#include "imagine/metadata/hasher.hpp"
#include "imagine/metadata/exif_reader.hpp"
#include "imagine/thumbnail/generator.hpp"
#include "imagine/common/logger.hpp"
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
    if (j.contains("current_file") && j["current_file"].is_string()) {
        p.current_file = j["current_file"].get<std::string>();
    }
    if (j.contains("is_running") && j["is_running"].is_boolean()) {
        p.is_running = j["is_running"].get<bool>();
    }
}

Importer::Importer(db::CatalogDb& db, thumbnail::Cache& cache, concurrency::ThreadPool* pool)
    : db_(db), cache_(cache), pool_(pool) {}

bool Importer::isSupportedExtension(const std::string& path) {
    std::filesystem::path p(path);
    std::string ext = p.extension().string();
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" ||
           ext == ".bmp" || ext == ".webp" || ext == ".tiff" || ext == ".tif";
}

void Importer::cancel() noexcept {
    cancelled_.store(true);
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

Importer::ProcessStatus Importer::processFileInternal(const std::string& filePath, MediaItem* outItem) {
    std::error_code ec;
    if (!std::filesystem::exists(filePath, ec) || !std::filesystem::is_regular_file(filePath, ec)) {
        IMAGINE_LOG_ERROR("File does not exist or is not a regular file: " + filePath);
        return ProcessStatus::Failed;
    }

    auto fsize = std::filesystem::file_size(filePath, ec);
    if (ec) {
        IMAGINE_LOG_ERROR("Failed to get file size for " + filePath + ": " + ec.message());
        return ProcessStatus::Failed;
    }

    auto lwt = std::filesystem::last_write_time(filePath, ec);
    if (ec) {
        IMAGINE_LOG_ERROR("Failed to get modified time for " + filePath + ": " + ec.message());
        return ProcessStatus::Failed;
    }

    auto sctp = std::chrono::file_clock::to_sys(lwt);
    int64_t modifiedTime = std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count();

    // Stage 2: Check existing file by path and modified time in DB. If unchanged, mark skipped.
    auto existingRes = db_.getMediaByPath(filePath);
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

    // Stage 3: Extract SHA-256 hash using Hasher
    auto hashRes = metadata::Hasher::computeFileSha256(filePath);
    if (!hashRes.isOk()) {
        IMAGINE_LOG_ERROR("Failed to compute SHA-256 for " + filePath + ": " + hashRes.status().message());
        return ProcessStatus::Failed;
    }
    std::string hash = hashRes.value();

    // Extract EXIF data using ExifReader
    ExifData exif;
    auto exifRes = metadata::ExifReader::readFromFile(filePath);
    if (exifRes.isOk()) {
        exif = exifRes.value();
    }

    // Dimensions
    int32_t width = 0;
    int32_t height = 0;
    auto dimRes = thumbnail::Generator::getImageDimensions(filePath);
    if (dimRes.isOk()) {
        width = dimRes.value().first;
        height = dimRes.value().second;
    }

    // Date taken
    int64_t dateTaken = exif.date_taken;
    if (dateTaken <= 0) {
        dateTaken = modifiedTime;
    }

    // Thumbnails small (256) and large (1024) via ThumbnailCache
    std::string thumbSmall;
    std::string thumbLarge;
    auto thumbRes = cache_.ensureDualThumbnails(filePath, hash, exif.orientation);
    if (thumbRes.isOk()) {
        thumbSmall = thumbRes.value().first;
        thumbLarge = thumbRes.value().second;
    } else {
        IMAGINE_LOG_WARN("Failed to generate thumbnails for " + filePath + ": " + thumbRes.status().message());
    }

    // Stage 4: Insert/Update media item in CatalogDb
    MediaItem item;
    if (isUpdate) {
        item = existingRes.value();
    }
    item.file_path = filePath;
    item.file_name = std::filesystem::path(filePath).filename().string();
    item.file_size = static_cast<int64_t>(fsize);
    item.file_modified_time = modifiedTime;
    item.content_hash = hash;
    item.width = width;
    item.height = height;
    item.date_taken = dateTaken;
    item.exif = exif;
    item.thumb_small = thumbSmall;
    item.thumb_large = thumbLarge;

    if (isUpdate) {
        Status s = db_.updateMedia(item);
        if (!s.isOk()) {
            IMAGINE_LOG_ERROR("Failed to update media in DB for " + filePath + ": " + s.message());
            return ProcessStatus::Failed;
        }
    } else {
        auto insRes = db_.insertMedia(item);
        if (!insRes.isOk()) {
            IMAGINE_LOG_ERROR("Failed to insert media into DB for " + filePath + ": " + insRes.status().message());
            return ProcessStatus::Failed;
        }
        item.id = insRes.value();
    }

    if (outItem) {
        *outItem = item;
    }
    return ProcessStatus::Imported;
}

Result<MediaItem> Importer::importFile(const std::string& filePath) {
    if (!isSupportedExtension(filePath)) {
        return Status::invalidArgument("Unsupported image extension: " + filePath);
    }
    MediaItem item;
    ProcessStatus st = processFileInternal(filePath, &item);
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

    std::error_code ec;
    if (!std::filesystem::exists(directoryPath, ec) || !std::filesystem::is_directory(directoryPath, ec)) {
        running_.store(false);
        return Status::notFound("Directory does not exist: " + directoryPath);
    }

    std::vector<std::string> files;
    if (recursive) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 directoryPath, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (entry.is_regular_file(ec) && isSupportedExtension(entry.path().string())) {
                files.push_back(entry.path().string());
            }
        }
    } else {
        for (const auto& entry : std::filesystem::directory_iterator(
                 directoryPath, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (entry.is_regular_file(ec) && isSupportedExtension(entry.path().string())) {
                files.push_back(entry.path().string());
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        current_progress_.total_files.store(static_cast<int64_t>(files.size()));
        current_progress_.processed_files.store(0);
        current_progress_.imported_files.store(0);
        current_progress_.skipped_files.store(0);
        current_progress_.failed_files.store(0);
        current_progress_.current_file.clear();
        current_progress_.is_running = true;
    }

    if (progressCb) {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progressCb(current_progress_);
    }

    if (pool_ != nullptr) {
        for (const auto& file : files) {
            if (cancelled_.load()) {
                break;
            }

            pool_->enqueue([this, file, progressCb]() {
                if (cancelled_.load()) {
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(progress_mutex_);
                    current_progress_.current_file = file;
                }

                ProcessStatus st = processFileInternal(file);
                switch (st) {
                    case ProcessStatus::Imported:
                        ++current_progress_.imported_files;
                        break;
                    case ProcessStatus::Skipped:
                        ++current_progress_.skipped_files;
                        break;
                    case ProcessStatus::Failed:
                        ++current_progress_.failed_files;
                        break;
                }
                ++current_progress_.processed_files;

                if (progressCb) {
                    std::lock_guard<std::mutex> lock(progress_mutex_);
                    progressCb(current_progress_);
                }
            });
        }
        pool_->waitAll();
    } else {
        for (const auto& file : files) {
            if (cancelled_.load()) {
                break;
            }

            {
                std::lock_guard<std::mutex> lock(progress_mutex_);
                current_progress_.current_file = file;
            }

            ProcessStatus st = processFileInternal(file);
            switch (st) {
                case ProcessStatus::Imported:
                    ++current_progress_.imported_files;
                    break;
                case ProcessStatus::Skipped:
                    ++current_progress_.skipped_files;
                    break;
                case ProcessStatus::Failed:
                    ++current_progress_.failed_files;
                    break;
            }
            ++current_progress_.processed_files;

            if (progressCb) {
                std::lock_guard<std::mutex> lock(progress_mutex_);
                progressCb(current_progress_);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        current_progress_.is_running = false;
        current_progress_.current_file.clear();
    }
    running_.store(false);

    if (progressCb) {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progressCb(current_progress_);
    }

    ImportProgress finalProgress;
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        finalProgress = current_progress_;
    }
    return finalProgress;
}

} // namespace imagine::core
