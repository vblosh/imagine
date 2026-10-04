#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include "imagine/common/error.hpp"
#include "imagine/thumbnail/generator.hpp"

namespace imagine::faces {

struct Point { float x{0}; float y{0}; };
struct Detection {
    float x{0}, y{0}, width{0}, height{0}, score{0};
    std::array<Point, 5> landmarks{};
    std::vector<float> embedding;
    std::string embeddingError;
};
struct Config {
    std::string modelDirectory;
    std::string device{"cpu"};
    float confidence{0.5f};
    float nmsThreshold{0.4f};
    float matchThreshold{0.5f};
    int cpuThreads{4};
};
struct RuntimeInfo {
    bool built{false};
    bool ready{false};
    std::string error;
    std::string detectorChecksum, recognizerChecksum, runtimeVersion;
    std::string detectorProvider, recognizerProvider, fallbackReason;
    std::string pipelineVersion{"insightface-0.7.3-v1"};
};
struct AnalysisTimings {
    double detectorPreprocessMs{0.0};
    double detectorInferenceMs{0.0};
    double detectorDecodeMs{0.0};
    double detectorNmsMs{0.0};
    double detectionMs{0.0};
    double recognitionMs{0.0};
    double totalMs{0.0};
    size_t detections{0};
    size_t embeddings{0};
};
class Engine {
public:
    explicit Engine(Config config);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Status initialize();
    const RuntimeInfo& info() const;
    // Timings for the most recent analyze() call that passed input validation.
    // Stages exclude initialization so callers can report cold startup separately.
    AnalysisTimings lastTimings() const;
    const Config& config() const;
    // Input: already EXIF-oriented, interleaved RGB8, original resolution.
    // Output: oriented source pixels, exclusive right/bottom; landmarks in
    // image-left eye, image-right eye, nose, image-left mouth, image-right mouth order.
    Result<std::vector<Detection>> analyze(const thumbnail::ImageBuffer& image);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace imagine::faces
