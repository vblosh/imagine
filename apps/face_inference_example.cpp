#include "imagine/faces/engine.hpp"
#include "imagine/metadata/exif_reader.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace {

using imagine::thumbnail::ImageBuffer;

void setPixel(ImageBuffer& image, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || y < 0 || x >= image.width || y >= image.height || image.channels != 3) return;
    const size_t offset = (static_cast<size_t>(y) * image.width + x) * 3;
    image.data[offset] = r;
    image.data[offset + 1] = g;
    image.data[offset + 2] = b;
}

void drawLine(ImageBuffer& image, int x0, int y0, int x1, int y1) {
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        setPixel(image, x0, y0, 0, 255, 0);
        if (x0 == x1 && y0 == y1) break;
        const int twice = error * 2;
        if (twice >= dy) { error += dy; x0 += sx; }
        if (twice <= dx) { error += dx; y0 += sy; }
    }
}

void drawOverlay(ImageBuffer& image, const std::vector<imagine::faces::Detection>& detections) {
    for (const auto& face : detections) {
        const int left = static_cast<int>(std::floor(face.x));
        const int top = static_cast<int>(std::floor(face.y));
        const int right = static_cast<int>(std::ceil(face.x + face.width)) - 1;
        const int bottom = static_cast<int>(std::ceil(face.y + face.height)) - 1;
        if (right < left || bottom < top) continue;
        drawLine(image, left, top, right, top);
        drawLine(image, right, top, right, bottom);
        drawLine(image, right, bottom, left, bottom);
        drawLine(image, left, bottom, left, top);
        for (const auto& point : face.landmarks) {
            const int x = static_cast<int>(std::lround(point.x));
            const int y = static_cast<int>(std::lround(point.y));
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    if (dx * dx + dy * dy <= 4) setPixel(image, x + dx, y + dy, 255, 32, 32);
                }
            }
        }
    }
}

void printUsage() {
    std::cerr << "Usage: face_inference_example --models <buffalo_l-directory> --image <photo> "
                 "[--device cpu|auto|cuda] [--warmups N] [--repeats N] "
                 "[--overlay <output.jpg|output.png>]\n";
}

bool parseCount(const std::string& value, size_t& count) {
    try {
        size_t consumed = 0;
        const auto parsed = std::stoull(value, &consumed);
        if (consumed != value.size() || parsed > 10000) return false;
        count = static_cast<size_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const size_t rank = static_cast<size_t>(std::ceil(p * values.size()));
    return values[std::clamp(rank, size_t{1}, values.size()) - 1];
}

double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const size_t middle = values.size() / 2;
    if (values.size() % 2 != 0) return values[middle];
    return (values[middle - 1] + values[middle]) / 2.0;
}

nlohmann::json summarize(const std::vector<double>& values) {
    if (values.empty()) return {{"mean", 0.0}, {"median", 0.0}, {"p95", 0.0}};
    const double mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    const double middle = median(values);
    const double p95 = percentile(values, 0.95);
    return {{"mean", mean}, {"median", middle}, {"p95", p95}};
}

nlohmann::json peakWorkingSet() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = static_cast<DWORD>(sizeof(counters));
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&counters),
                             static_cast<DWORD>(sizeof(counters)))) {
        const auto bytes = static_cast<uint64_t>(counters.PeakWorkingSetSize);
        return {{"supported", true}, {"source", "Windows PeakWorkingSetSize"},
                {"bytes", bytes}, {"mib", static_cast<double>(bytes) / (1024.0 * 1024.0)}};
    }
#endif
    return {{"supported", false}, {"source", "unavailable on this platform"}, {"bytes", nullptr}, {"mib", nullptr}};
}

} // namespace

int main(int argc, char** argv) {
    imagine::faces::Config config;
    std::string imagePath;
    std::string overlayPath;
    size_t warmups = 0;
    size_t repeats = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--models" || arg == "--model-dir") && i + 1 < argc) {
            config.modelDirectory = argv[++i];
        } else if (arg == "--image" && i + 1 < argc) {
            imagePath = argv[++i];
        } else if (arg == "--device" && i + 1 < argc) {
            config.device = argv[++i];
        } else if (arg == "--overlay" && i + 1 < argc) {
            overlayPath = argv[++i];
        } else if (arg == "--warmups" && i + 1 < argc) {
            if (!parseCount(argv[++i], warmups)) {
                std::cerr << "--warmups must be an integer between 0 and 10000\n";
                return 2;
            }
        } else if (arg == "--repeats" && i + 1 < argc) {
            if (!parseCount(argv[++i], repeats) || repeats == 0) {
                std::cerr << "--repeats must be an integer between 1 and 10000\n";
                return 2;
            }
        } else {
            printUsage();
            return 2;
        }
    }
    if (config.modelDirectory.empty() || imagePath.empty()) {
        printUsage();
        return 2;
    }

    imagine::faces::Engine engine(config);
    const auto initializationStart = std::chrono::steady_clock::now();
    const auto init = engine.initialize(true);
    const double initializationMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - initializationStart).count();
    if (!init.isOk()) {
        nlohmann::json result{
            {"status", "error"}, {"error", init.message()}, {"built", engine.info().built},
            {"runtime_version", engine.info().runtimeVersion},
        };
        std::cout << result.dump(2) << '\n';
        return 1;
    }

    const auto imageDecodeStart = std::chrono::steady_clock::now();
    auto image = imagine::thumbnail::Generator::loadImage(imagePath);
    const double imageDecodeMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - imageDecodeStart).count();
    if (!image.isOk()) {
        std::cerr << "Image decode failed: " << image.status().message() << '\n';
        return 1;
    }
    auto exif = imagine::metadata::ExifReader::readFromFile(imagePath);
    const int orientation = exif.isOk() ? exif.value().orientation : 1;
    ImageBuffer oriented = imagine::thumbnail::Generator::rotate(image.value(), orientation);
    for (size_t i = 0; i < warmups; ++i) {
        const auto warmup = engine.analyze(oriented);
        if (!warmup.isOk()) {
            std::cerr << "Face inference warmup failed: " << warmup.status().message() << '\n';
            return 1;
        }
    }
    std::vector<imagine::faces::AnalysisTimings> timings;
    timings.reserve(repeats);
    std::vector<double> preprocessSamples, detectorSamples, decodeSamples, nmsSamples;
    std::vector<double> detectionSamples, recognitionSamples, totalSamples;
    preprocessSamples.reserve(repeats);
    detectorSamples.reserve(repeats);
    decodeSamples.reserve(repeats);
    nmsSamples.reserve(repeats);
    detectionSamples.reserve(repeats);
    recognitionSamples.reserve(repeats);
    totalSamples.reserve(repeats);
    std::vector<imagine::faces::Detection> finalDetections;
    for (size_t i = 0; i < repeats; ++i) {
        const auto analyzed = engine.analyze(oriented);
        if (!analyzed.isOk()) {
            std::cerr << "Face inference failed: " << analyzed.status().message() << '\n';
            return 1;
        }
        finalDetections = analyzed.value();
        timings.push_back(engine.lastTimings());
        const auto& timing = timings.back();
        preprocessSamples.push_back(timing.detectorPreprocessMs);
        detectorSamples.push_back(timing.detectorInferenceMs);
        decodeSamples.push_back(timing.detectorDecodeMs);
        nmsSamples.push_back(timing.detectorNmsMs);
        detectionSamples.push_back(timing.detectionMs);
        recognitionSamples.push_back(timing.recognitionMs);
        totalSamples.push_back(timing.totalMs);
    }

    const auto runtime = engine.info();
    nlohmann::json result{
        {"status", "ok"},
        {"image", imagePath},
        {"width", oriented.width},
        {"height", oriented.height},
        {"orientation", orientation},
        {"pipeline_version", runtime.pipelineVersion},
        {"runtime_version", runtime.runtimeVersion},
        {"detector_checksum", runtime.detectorChecksum},
        {"recognizer_checksum", runtime.recognizerChecksum},
        {"detector_provider", runtime.detectorProvider},
        {"recognizer_provider", runtime.recognizerProvider},
        {"fallback_reason", runtime.fallbackReason},
        {"performance", {
            {"initialization_ms", initializationMs},
            {"image_decode_ms", imageDecodeMs},
            {"warmups", warmups},
            {"repeats", repeats},
            {"last", {
                {"detector_preprocess_ms", timings.back().detectorPreprocessMs},
                {"detector_inference_ms", timings.back().detectorInferenceMs},
                {"decode_ms", timings.back().detectorDecodeMs},
                {"nms_ms", timings.back().detectorNmsMs},
                {"detection_ms", timings.back().detectionMs},
                {"recognition_ms", timings.back().recognitionMs},
                {"total_ms", timings.back().totalMs},
                {"detections", timings.back().detections},
                {"embeddings", timings.back().embeddings},
            }},
            {"summary_ms", {
                {"detector_preprocess", summarize(preprocessSamples)},
                {"detector_inference", summarize(detectorSamples)},
                {"decode", summarize(decodeSamples)},
                {"nms", summarize(nmsSamples)},
                {"detection", summarize(detectionSamples)},
                {"recognition", summarize(recognitionSamples)},
                {"total", summarize(totalSamples)},
            }},
            {"peak_working_set", nullptr},
        }},
        {"detections", nlohmann::json::array()},
    };
    for (const auto& face : finalDetections) {
        nlohmann::json landmarks = nlohmann::json::array();
        for (const auto& point : face.landmarks) landmarks.push_back({{"x", point.x}, {"y", point.y}});
        result["detections"].push_back({
            {"x", face.x}, {"y", face.y}, {"width", face.width}, {"height", face.height},
            {"score", face.score}, {"landmarks", std::move(landmarks)},
            {"embedding_dimensions", face.embedding.size()}, {"embedding_error", face.embeddingError},
            {"embedding", face.embedding},
        });
    }
    result["performance"]["peak_working_set"] = peakWorkingSet();
    std::cout << result.dump(2) << '\n';

    if (!overlayPath.empty()) {
        drawOverlay(oriented, finalDetections);
        imagine::Status saved;
        if (std::filesystem::path(overlayPath).extension() == ".png") {
            saved = imagine::thumbnail::Generator::savePng(oriented, overlayPath);
        } else {
            saved = imagine::thumbnail::Generator::saveJpeg(oriented, overlayPath, 92);
        }
        if (!saved.isOk()) {
            std::cerr << "Overlay write failed: " << saved.message() << '\n';
            return 1;
        }
    }
    return 0;
}
