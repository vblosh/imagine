#pragma once

#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>
#include "imagine/common/error.hpp"
#include "imagine/thumbnail/generator.hpp"

namespace imagine::clip {

struct ClipConfig {
    std::string modelDirectory;     // Path containing ONNX models + tokenizer files
    std::string device{"cpu"};      // "cpu" or "cuda"
    int cpuThreads{4};
};

struct ClipRuntimeInfo {
    bool built{false};              // Compiled with ONNX Runtime support
    bool ready{false};              // Metadata probed, configuration valid, ready to load
    bool loaded{false};             // Models loaded into memory and sessions active
    std::string error;
    std::string modelId;            // e.g. "clip-vit-b32"
    std::string modelRevision;
    int vectorDim{0};               // e.g. 512
    std::string preprocessId;       // Identifies preprocessing config
    std::string runtimeVersion;
    std::string executionProvider;  // "cpu" or "cuda"
};

struct ClipTimings {
    double preprocessMs{0.0};
    double inferenceMs{0.0};
    double tokenizeMs{0.0};
    double totalMs{0.0};
};

class Engine {
public:
    explicit Engine(ClipConfig config);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    Status initialize(bool eager = false);
    Status load();
    void unload();
    bool isLoaded() const;
    ClipRuntimeInfo info() const;
    ClipTimings lastTimings() const;

    // Encode a decoded, EXIF-oriented RGB image -> L2-normalized embedding
    Result<std::vector<float>> encodeImage(const thumbnail::ImageBuffer& image);

    // Encode a text query -> L2-normalized embedding
    Result<std::vector<float>> encodeText(std::string_view query);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace imagine::clip
