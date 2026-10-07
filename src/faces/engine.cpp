#include "imagine/faces/engine.hpp"

#include "imagine/metadata/hasher.hpp"
#include "imagine/common/logger.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

#ifndef IMAGINE_FACE_ANALYSIS_BUILT
#define IMAGINE_FACE_ANALYSIS_BUILT 0
#endif

#if IMAGINE_FACE_ANALYSIS_BUILT
#include <onnxruntime_cxx_api.h>
#include <nlohmann/json.hpp>
#endif

namespace imagine::faces {
namespace {

constexpr const char* kPipelineVersion = "insightface-0.7.3-v1";

Status validateConfig(const Config& config) {
    if (config.device != "cpu" && config.device != "auto" && config.device != "cuda") {
        return Status::invalidArgument("Face device must be cpu, auto, or cuda");
    }
    if (!std::isfinite(config.confidence) || config.confidence < 0.0f || config.confidence > 1.0f) {
        return Status::invalidArgument("Face confidence threshold must be finite and between 0 and 1");
    }
    if (!std::isfinite(config.nmsThreshold) || config.nmsThreshold < 0.0f || config.nmsThreshold > 1.0f) {
        return Status::invalidArgument("Face NMS threshold must be finite and between 0 and 1");
    }
    if (!std::isfinite(config.matchThreshold) || config.matchThreshold < -1.0f || config.matchThreshold > 1.0f) {
        return Status::invalidArgument("Face match threshold must be finite and between -1 and 1");
    }
    if (config.cpuThreads <= 0 || config.cpuThreads > 256) {
        return Status::invalidArgument("Face CPU thread count must be between 1 and 256");
    }
    return Status::ok();
}

#if IMAGINE_FACE_ANALYSIS_BUILT

constexpr int kDetectorSide = 640;
constexpr int kRecognizerSide = 112;
constexpr int kEmbeddingDimensions = 512;
constexpr std::array<int, 3> kFeatureStrides{8, 16, 32};
constexpr size_t kAnchorsPerLocation = 2;

std::string exceptionMessage(const std::exception& ex) {
    return ex.what() ? ex.what() : "Unknown inference error";
}

struct Candidate {
    float x1{0.0f};
    float y1{0.0f};
    float x2{0.0f};
    float y2{0.0f};
    float score{0.0f};
    std::array<Point, 5> landmarks{};
    size_t order{0};
};

struct LoadedModel {
    std::unique_ptr<Ort::Session> session;
    std::string inputName;
    std::vector<std::string> outputNames;
    std::vector<std::vector<int64_t>> outputShapes;
    std::vector<size_t> outputElementCounts;
    std::string provider;
    bool dynamicBatch{false};
};

Ort::Env& ortEnvironment() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "ImagineFaceAnalysis");
    return env;
}

std::vector<const char*> cNames(const std::vector<std::string>& names) {
    std::vector<const char*> result;
    result.reserve(names.size());
    for (const auto& name : names) result.push_back(name.c_str());
    return result;
}

bool isExpectedOrDynamic(int64_t actual, int64_t expected) {
    return actual == expected || actual == -1;
}

std::vector<int64_t> inputShape(Ort::Session& session, int64_t expectedSide, const char* label, bool* dynamicBatchOut = nullptr) {
    auto info = session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo();
    if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        throw std::runtime_error(std::string(label) + " input must be float32");
    }
    auto shape = info.GetShape();
    if (shape.size() != 4 || shape[1] != 3 ||
        !isExpectedOrDynamic(shape[2], expectedSide) || !isExpectedOrDynamic(shape[3], expectedSide)) {
        throw std::runtime_error(std::string(label) + " input must be NCHW RGB with a " +
                                 std::to_string(expectedSide) + "x" + std::to_string(expectedSide) + " canvas");
    }
    if (!isExpectedOrDynamic(shape[0], 1)) {
        throw std::runtime_error(std::string(label) + " model must accept a single image per call");
    }
    if (dynamicBatchOut) {
        *dynamicBatchOut = (shape[0] == -1);
    }
    shape[0] = 1;
    shape[1] = 3;
    shape[2] = expectedSide;
    shape[3] = expectedSide;
    return shape;
}

LoadedModel inspectSession(std::unique_ptr<Ort::Session> session, const char* label) {
    Ort::AllocatorWithDefaultOptions allocator;
    LoadedModel model;
    model.session = std::move(session);
    if (model.session->GetInputCount() != 1) {
        throw std::runtime_error(std::string(label) + " model must have exactly one input");
    }
    auto inName = model.session->GetInputNameAllocated(0, allocator);
    model.inputName = inName.get();
    const size_t outputs = model.session->GetOutputCount();
    model.outputNames.reserve(outputs);
    model.outputShapes.reserve(outputs);
    for (size_t i = 0; i < outputs; ++i) {
        auto outName = model.session->GetOutputNameAllocated(i, allocator);
        model.outputNames.emplace_back(outName.get());
        auto type = model.session->GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo();
        if (type.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            throw std::runtime_error(std::string(label) + " outputs must be float32");
        }
        model.outputShapes.push_back(type.GetShape());
    }
    return model;
}

bool hasExpectedShape(const std::vector<int64_t>& actual,
                      const std::vector<int64_t>& expected) {
    if (actual.size() == expected.size()) {
        auto expectedDimension = expected.begin();
        for (size_t i = 0; i < actual.size(); ++i, ++expectedDimension) {
            if (i == 0 && actual[i] == -1 && *expectedDimension == 1) continue;
            if (actual[i] != *expectedDimension) return false;
        }
        return true;
    }
    // Some compatible exporters omit the singleton batch dimension on heads.
    if (actual.size() + 1 == expected.size()) {
        return std::equal(actual.begin(), actual.end(), std::next(expected.begin()));
    }
    return false;
}

void validateOutputContract(LoadedModel& model, const char* label) {
    std::vector<std::vector<int64_t>> expectedShapes;
    if (std::string(label) == "SCRFD detector") {
        constexpr std::array<int64_t, 3> counts{12800, 3200, 800};
        expectedShapes.reserve(9);
        for (const auto count : counts) expectedShapes.push_back({1, count, 1});
        for (const auto count : counts) expectedShapes.push_back({1, count, 4});
        for (const auto count : counts) expectedShapes.push_back({1, count, 10});
    } else {
        expectedShapes.push_back({1, kEmbeddingDimensions});
    }
    if (model.outputShapes.size() != expectedShapes.size()) {
        throw std::runtime_error(std::string(label) + " output count does not match its pinned model contract");
    }
    model.outputElementCounts.clear();
    model.outputElementCounts.reserve(expectedShapes.size());
    for (size_t i = 0; i < expectedShapes.size(); ++i) {
        if (!hasExpectedShape(model.outputShapes[i], expectedShapes[i])) {
            throw std::runtime_error(std::string(label) + " output " + std::to_string(i) +
                                     " has an incompatible tensor shape");
        }
        size_t count = 1;
        for (auto dimension = expectedShapes[i].begin(); dimension != expectedShapes[i].end(); ++dimension) {
            if (dimension == expectedShapes[i].begin() && *dimension == 1) continue;
            count *= static_cast<size_t>(*dimension);
        }
        model.outputElementCounts.push_back(count);
    }
}

std::vector<std::vector<float>> runModel(LoadedModel& model,
                                         const float* input,
                                         const std::vector<int64_t>& shape) {
    if (shape.size() != 4 || model.outputElementCounts.size() != model.outputNames.size()) {
        throw std::runtime_error("ONNX input or output metadata is invalid");
    }
    size_t inputElementCount = 1;
    for (const auto dimension : shape) {
        if (dimension <= 0 || inputElementCount > std::numeric_limits<size_t>::max() / static_cast<size_t>(dimension)) {
            throw std::runtime_error("ONNX input tensor dimensions are invalid");
        }
        inputElementCount *= static_cast<size_t>(dimension);
    }
    const std::array<int64_t, 4> fixedShape{shape[0], shape[1], shape[2], shape[3]};
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto tensor = Ort::Value::CreateTensor<float>(memory, const_cast<float*>(input),
        inputElementCount, fixedShape.data(), fixedShape.size());
    auto inputName = model.inputName.c_str();
    auto outputNames = cNames(model.outputNames);
    auto values = model.session->Run(Ort::RunOptions{nullptr}, &inputName, &tensor, 1,
                                     outputNames.data(), outputNames.size());
    if (values.size() != model.outputElementCounts.size()) {
        throw std::runtime_error("ONNX model returned an unexpected output count");
    }
    std::vector<std::vector<float>> result;
    result.reserve(values.size());
    const size_t batchSize = static_cast<size_t>(shape[0]);
    for (size_t i = 0; i < values.size(); ++i) {
        const auto& value = values[i];
        if (!value.IsTensor()) throw std::runtime_error("ONNX model produced a non-tensor output");
        auto tensorInfo = value.GetTensorTypeAndShapeInfo();
        const size_t expectedOutputCount = model.outputElementCounts[i] * batchSize;
        if (tensorInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            tensorInfo.GetElementCount() != expectedOutputCount) {
            throw std::runtime_error("ONNX model returned an output incompatible with its declared shape");
        }
        const float* data = value.GetTensorData<float>();
        if (!data && expectedOutputCount != 0) throw std::runtime_error("ONNX model returned a null tensor buffer");
        result.emplace_back(data, data + expectedOutputCount);
    }
    return result;
}

std::vector<float> detectorDummyInput() {
    return std::vector<float>(3 * kDetectorSide * kDetectorSide, 0.0f);
}

std::vector<float> recognizerDummyInput() {
    return std::vector<float>(3 * kRecognizerSide * kRecognizerSide, 0.0f);
}

bool profileHasCudaKernel(const std::filesystem::path& profilePath) {
    std::ifstream file(profilePath, std::ios::binary);
    if (!file) return false;
    try {
        nlohmann::json events;
        file >> events;
        if (!events.is_array()) return false;
        for (const auto& event : events) {
            if (!event.is_object() || event.value("cat", std::string{}) != "Node") continue;
            const auto args = event.value("args", nlohmann::json::object());
            if (args.is_object() && args.value("provider", std::string{}) == "CUDAExecutionProvider") {
                const auto name = event.value("name", std::string{});
                if (name.find("_kernel_time") != std::string::npos) return true;
            }
        }
    } catch (...) {
        return false;
    }
    return false;
}

std::filesystem::path endProfile(Ort::Session& session) {
    Ort::AllocatorWithDefaultOptions allocator;
    auto allocated = session.EndProfilingAllocated(allocator);
    if (!allocated || !allocated.get()) return {};
    return std::filesystem::path(allocated.get());
}

std::atomic<uint64_t> gProfileCounter{0};

LoadedModel openModel(const std::filesystem::path& path,
                      const char* label,
                      int cpuThreads,
                      bool requestCuda) {
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(cpuThreads);
    options.SetInterOpNumThreads(1);
    options.SetExecutionMode(ORT_SEQUENTIAL);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    options.SetLogSeverityLevel(3);

    std::filesystem::path profilePrefix;
    if (requestCuda) {
        profilePrefix = std::filesystem::temp_directory_path() /
            ("imagine-face-ort-" + std::to_string(gProfileCounter.fetch_add(1)));
        options.EnableProfiling(profilePrefix.native().c_str());
        OrtCUDAProviderOptions cudaOptions{};
        cudaOptions.device_id = 0;
        options.AppendExecutionProvider_CUDA(cudaOptions);
    }

    const auto nativePath = path.native();
    auto session = std::make_unique<Ort::Session>(ortEnvironment(), nativePath.c_str(), options);
    auto model = inspectSession(std::move(session), label);
    validateOutputContract(model, label);
    if (std::string(label) == "SCRFD detector") {
        inputShape(*model.session, kDetectorSide, label, &model.dynamicBatch);
        if (model.outputNames.size() != 9) {
            throw std::runtime_error("SCRFD buffalo_l detector must expose nine outputs (three score, box, and landmark heads)");
        }
    } else {
        inputShape(*model.session, kRecognizerSide, label, &model.dynamicBatch);
        if (model.outputNames.size() != 1) {
            throw std::runtime_error("ArcFace buffalo_l recognizer must expose exactly one embedding output");
        }
    }

    const auto dummy = std::string(label) == "SCRFD detector" ? detectorDummyInput() : recognizerDummyInput();
    const std::vector<int64_t> shape = std::string(label) == "SCRFD detector"
        ? std::vector<int64_t>{1, 3, kDetectorSide, kDetectorSide}
        : std::vector<int64_t>{1, 3, kRecognizerSide, kRecognizerSide};
    const auto outputs = runModel(model, dummy.data(), shape);
    if (std::string(label) == "SCRFD detector") {
        constexpr std::array<size_t, 3> headSizes{12800, 3200, 800};
        if (outputs.size() != 9) throw std::runtime_error("SCRFD detector output count changed after initialization");
        for (size_t level = 0; level < headSizes.size(); ++level) {
            const size_t boxes = headSizes[level] * 4;
            const size_t keypoints = headSizes[level] * 10;
            if (outputs[level].size() != headSizes[level] || outputs[level + 3].size() != boxes ||
                outputs[level + 6].size() != keypoints) {
                throw std::runtime_error("SCRFD output shapes do not match the 640x640 three-level, two-anchor buffalo_l detector");
            }
        }
    } else if (outputs.size() != 1 || outputs.front().size() != kEmbeddingDimensions) {
        throw std::runtime_error("ArcFace buffalo_l recognizer must produce 512 float values");
    }

    if (requestCuda) {
        const auto profile = endProfile(*model.session);
        const bool didCudaWork = profileHasCudaKernel(profile);
        std::error_code removeError;
        if (!profile.empty()) std::filesystem::remove(profile, removeError);
        if (!didCudaWork) {
            throw std::runtime_error(std::string(label) + " initialized but profiling found no CUDA model kernels");
        }
        model.provider = "CUDAExecutionProvider+CPUExecutionProvider";
    } else {
        model.provider = "CPUExecutionProvider";
    }
    return model;
}

std::vector<Candidate> decodeDetections(const std::vector<std::vector<float>>& outputs,
                                        float scale,
                                        float confidence) {
    if (outputs.size() != 9 || !std::isfinite(scale) || scale <= 0.0f) {
        throw std::runtime_error("SCRFD output layout or resize scale is invalid");
    }
    std::vector<Candidate> candidates;
    candidates.reserve(64);
    size_t globalIndex = 0;
    for (size_t level = 0; level < kFeatureStrides.size(); ++level) {
        const int stride = kFeatureStrides[level];
        const int featureHeight = kDetectorSide / stride;
        const int featureWidth = kDetectorSide / stride;
        const size_t cells = static_cast<size_t>(featureHeight) * featureWidth;
        const size_t count = cells * kAnchorsPerLocation;
        const auto& scores = outputs[level];
        const auto& boxes = outputs[level + 3];
        const auto& landmarks = outputs[level + 6];
        if (scores.size() != count || boxes.size() != count * 4 || landmarks.size() != count * 10) {
            throw std::runtime_error("SCRFD output tensor shape does not match the 640x640 buffalo_l heads");
        }
        for (int row = 0; row < featureHeight; ++row) {
            for (int col = 0; col < featureWidth; ++col) {
                for (size_t anchor = 0; anchor < kAnchorsPerLocation; ++anchor, ++globalIndex) {
                    const size_t i = (static_cast<size_t>(row) * featureWidth + col) * kAnchorsPerLocation + anchor;
                    const float score = scores[i];
                    if (!std::isfinite(score) || score < confidence || score > 1.0f) continue;
                    const float cx = static_cast<float>(col * stride);
                    const float cy = static_cast<float>(row * stride);
                    const size_t boxOffset = i * 4;
                    Candidate candidate;
                    candidate.x1 = (cx - boxes[boxOffset] * stride) / scale;
                    candidate.y1 = (cy - boxes[boxOffset + 1] * stride) / scale;
                    candidate.x2 = (cx + boxes[boxOffset + 2] * stride) / scale;
                    candidate.y2 = (cy + boxes[boxOffset + 3] * stride) / scale;
                    candidate.score = score;
                    candidate.order = globalIndex;
                    for (size_t point = 0; point < candidate.landmarks.size(); ++point) {
                        const size_t offset = i * 10 + point * 2;
                        candidate.landmarks[point] = {
                            (cx + landmarks[offset] * stride) / scale,
                            (cy + landmarks[offset + 1] * stride) / scale,
                        };
                    }
                    if (!std::isfinite(candidate.x1) || !std::isfinite(candidate.y1) ||
                        !std::isfinite(candidate.x2) || !std::isfinite(candidate.y2) ||
                        candidate.x2 <= candidate.x1 || candidate.y2 <= candidate.y1 ||
                        !std::isfinite(candidate.x2 - candidate.x1) ||
                        !std::isfinite(candidate.y2 - candidate.y1)) continue;
                    bool finiteLandmarks = true;
                    for (const auto& p : candidate.landmarks) {
                        finiteLandmarks = finiteLandmarks && std::isfinite(p.x) && std::isfinite(p.y);
                    }
                    if (finiteLandmarks) candidates.push_back(candidate);
                }
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.order > b.order;
    });
    return candidates;
}

float intersectionOverUnionInclusive(const Candidate& a, const Candidate& b) {
    const float left = std::max(a.x1, b.x1);
    const float top = std::max(a.y1, b.y1);
    const float right = std::min(a.x2, b.x2);
    const float bottom = std::min(a.y2, b.y2);
    const float width = std::max(0.0f, right - left + 1.0f);
    const float height = std::max(0.0f, bottom - top + 1.0f);
    const float intersection = width * height;
    const float areaA = std::max(0.0f, a.x2 - a.x1 + 1.0f) * std::max(0.0f, a.y2 - a.y1 + 1.0f);
    const float areaB = std::max(0.0f, b.x2 - b.x1 + 1.0f) * std::max(0.0f, b.y2 - b.y1 + 1.0f);
    const float denominator = areaA + areaB - intersection;
    return denominator > 0.0f ? intersection / denominator : 0.0f;
}

std::vector<Candidate> suppress(std::vector<Candidate> candidates, float threshold) {
    std::vector<Candidate> kept;
    kept.reserve(candidates.size());
    std::vector<bool> removed(candidates.size(), false);
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (removed[i]) continue;
        kept.push_back(candidates[i]);
        for (size_t j = i + 1; j < candidates.size(); ++j) {
            if (!removed[j] && intersectionOverUnionInclusive(candidates[i], candidates[j]) > threshold) {
                removed[j] = true;
            }
        }
    }
    return kept;
}

struct Affine {
    double a{1.0}, b{0.0}, tx{0.0};
    double c{0.0}, d{1.0}, ty{0.0};
};

Affine estimateArcFaceTransform(const std::array<Point, 5>& source) {
    constexpr std::array<std::array<float, 2>, 5> target{{
        {{38.2946, 51.6963}}, {{73.5318, 51.5014}}, {{56.0252, 71.7366}},
        {{41.5493, 92.3655}}, {{70.7299, 92.2041}},
    }};
    double sourceX = 0.0, sourceY = 0.0, targetX = 0.0, targetY = 0.0;
    for (size_t i = 0; i < source.size(); ++i) {
        if (!std::isfinite(source[i].x) || !std::isfinite(source[i].y)) {
            throw std::runtime_error("Cannot align a face with non-finite landmarks");
        }
        sourceX += source[i].x;
        sourceY += source[i].y;
        targetX += target[i][0];
        targetY += target[i][1];
    }
    sourceX /= source.size(); sourceY /= source.size();
    targetX /= target.size(); targetY /= target.size();

    double denominator = 0.0;
    double dot = 0.0;
    double cross = 0.0;
    for (size_t i = 0; i < source.size(); ++i) {
        const double sx = source[i].x - sourceX;
        const double sy = source[i].y - sourceY;
        const double tx = target[i][0] - targetX;
        const double ty = target[i][1] - targetY;
        denominator += sx * sx + sy * sy;
        dot += sx * tx + sy * ty;
        cross += sx * ty - sy * tx;
    }
    if (!std::isfinite(denominator) || denominator <= std::numeric_limits<double>::epsilon()) {
        throw std::runtime_error("Face landmarks have no usable spatial extent");
    }
    const double scaleCos = dot / denominator;
    const double scaleSin = cross / denominator;
    Affine transform;
    transform.a = scaleCos;
    transform.b = -scaleSin;
    transform.c = scaleSin;
    transform.d = scaleCos;
    transform.tx = targetX - transform.a * sourceX - transform.b * sourceY;
    transform.ty = targetY - transform.c * sourceX - transform.d * sourceY;
    return transform;
}

void alignAndNormalizeTo(const thumbnail::ImageBuffer& image,
                         const std::array<Point, 5>& landmarks,
                         float* outR, float* outG, float* outB) {
    const Affine transform = estimateArcFaceTransform(landmarks);
    const double determinant = transform.a * transform.d - transform.b * transform.c;
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-15) {
        throw std::runtime_error("Face alignment transform is singular");
    }
    const double ia = transform.d / determinant;
    const double ib = -transform.b / determinant;
    const double ic = -transform.c / determinant;
    const double id = transform.a / determinant;

    static const auto kAlignLut = [] {
        std::array<float, 256> table{};
        for (int i = 0; i < 256; ++i) {
            table[i] = static_cast<float>(static_cast<double>(i) / 127.5 - 1.0);
        }
        return table;
    }();

    const uint8_t* imgBytes = image.data.data();
    const size_t imgStride = static_cast<size_t>(image.width) * 3;
    const int imgW = image.width;
    const int imgH = image.height;

    const double stepX_x = ia * 32.0;
    const double stepX_y = ic * 32.0;

    for (int y = 0; y < kRecognizerSide; ++y) {
        const double dy = static_cast<double>(y) - transform.ty;
        const double rowSx = (-ia * transform.tx + ib * dy) * 32.0 + 0.5;
        const double rowSy = (-ic * transform.tx + id * dy) * 32.0 + 0.5;
        const size_t destRow = static_cast<size_t>(y) * kRecognizerSide;

        for (int x = 0; x < kRecognizerSide; ++x) {
            const double sx32 = rowSx + stepX_x * static_cast<double>(x);
            const double sy32 = rowSy + stepX_y * static_cast<double>(x);

            const double maxCoordX = 32.0 * imgW;
            const double maxCoordY = 32.0 * imgH;
            if (!std::isfinite(sx32) || !std::isfinite(sy32) ||
                sx32 <= -31.5 || sy32 <= -31.5 || sx32 >= maxCoordX || sy32 >= maxCoordY) {
                constexpr float black = -1.0f;
                outR[destRow + x] = black;
                outG[destRow + x] = black;
                outB[destRow + x] = black;
                continue;
            }

            const int coordX = static_cast<int>(std::floor(sx32));
            const int coordY = static_cast<int>(std::floor(sy32));

            const int ix = coordX >> 5;
            const int iy = coordY >> 5;
            const int ax = coordX & 31;
            const int ay = coordY & 31;

            const int w00 = (32 - ax) * (32 - ay);
            const int w10 = ax * (32 - ay);
            const int w01 = (32 - ax) * ay;
            const int w11 = ax * ay;

            if (ix >= 0 && iy >= 0 && ix + 1 < imgW && iy + 1 < imgH) {
                const uint8_t* p0 = imgBytes + static_cast<size_t>(iy) * imgStride + static_cast<size_t>(ix) * 3;
                const uint8_t* p1 = p0 + imgStride;

                const int r = (p0[0] * w00 + p0[3] * w10 + p1[0] * w01 + p1[3] * w11 + 512) >> 10;
                const int g = (p0[1] * w00 + p0[4] * w10 + p1[1] * w01 + p1[4] * w11 + 512) >> 10;
                const int b = (p0[2] * w00 + p0[5] * w10 + p1[2] * w01 + p1[5] * w11 + 512) >> 10;

                outR[destRow + x] = kAlignLut[static_cast<size_t>(std::clamp(r, 0, 255))];
                outG[destRow + x] = kAlignLut[static_cast<size_t>(std::clamp(g, 0, 255))];
                outB[destRow + x] = kAlignLut[static_cast<size_t>(std::clamp(b, 0, 255))];
            } else {
                auto sample = [&](int px, int py, int channel) -> int {
                    if (px < 0 || py < 0 || px >= imgW || py >= imgH) return 0;
                    return imgBytes[(static_cast<size_t>(py) * imgW + px) * 3 + channel];
                };
                for (int c = 0; c < 3; ++c) {
                    const int pixel = sample(ix, iy, c) * w00 + sample(ix + 1, iy, c) * w10 +
                                      sample(ix, iy + 1, c) * w01 + sample(ix + 1, iy + 1, c) * w11;
                    const int val = std::clamp((pixel + 512) >> 10, 0, 255);
                    const float norm = kAlignLut[static_cast<size_t>(val)];
                    if (c == 0) outR[destRow + x] = norm;
                    else if (c == 1) outG[destRow + x] = norm;
                    else outB[destRow + x] = norm;
                }
            }
        }
    }
}

std::vector<float> alignAndNormalize(const thumbnail::ImageBuffer& image,
                                     const std::array<Point, 5>& landmarks) {
    constexpr size_t kPlaneSize = static_cast<size_t>(kRecognizerSide) * kRecognizerSide;
    std::vector<float> chw(static_cast<size_t>(3) * kPlaneSize, 0.0f);
    alignAndNormalizeTo(image, landmarks, chw.data(), chw.data() + kPlaneSize, chw.data() + 2 * kPlaneSize);
    return chw;
}

std::string appendReason(const std::string& prior, const std::string& next) {
    if (prior.empty()) return next;
    return prior + "; " + next;
}

#endif // IMAGINE_FACE_ANALYSIS_BUILT

} // namespace

struct Engine::Impl {
    explicit Impl(Config value) : config(std::move(value)) {
        info.built = IMAGINE_FACE_ANALYSIS_BUILT != 0;
        info.pipelineVersion = kPipelineVersion;
    }

    Config config;
    RuntimeInfo info;
    mutable std::mutex timingsMutex;
    AnalysisTimings timings;
    bool initialized{false};
#if IMAGINE_FACE_ANALYSIS_BUILT
    std::unique_ptr<LoadedModel> detector;
    std::unique_ptr<LoadedModel> recognizer;
    std::vector<int64_t> detectorInputShape;
    std::vector<int64_t> recognizerInputShape;
#endif
};

Engine::Engine(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}
Engine::~Engine() = default;

Status Engine::initialize() {
    if (const auto configStatus = validateConfig(impl_->config); !configStatus.isOk()) {
        impl_->info.error = configStatus.message();
        return configStatus;
    }
#if !IMAGINE_FACE_ANALYSIS_BUILT
    impl_->info.ready = false;
    impl_->info.error = "Face analysis is unavailable: this build was configured without ONNX Runtime 1.30.0";
    return Status::internal(impl_->info.error);
#else
    impl_->info.ready = false;
    impl_->info.error.clear();
    impl_->info.fallbackReason.clear();
    try {
        const std::filesystem::path directory(impl_->config.modelDirectory);
        if (impl_->config.modelDirectory.empty() || !std::filesystem::is_directory(directory)) {
            throw std::runtime_error("Face model directory does not exist: " + impl_->config.modelDirectory);
        }
        const auto detectorPath = directory / "det_10g.onnx";
        const auto recognizerPath = directory / "w600k_r50.onnx";
        if (!std::filesystem::is_regular_file(detectorPath) || !std::filesystem::is_regular_file(recognizerPath)) {
            throw std::runtime_error("Model directory must contain det_10g.onnx and w600k_r50.onnx from buffalo_l");
        }

        auto detectorHash = metadata::Hasher::computeFileSha256(detectorPath.string());
        auto recognizerHash = metadata::Hasher::computeFileSha256(recognizerPath.string());
        if (!detectorHash.isOk() || !recognizerHash.isOk()) {
            throw std::runtime_error(!detectorHash.isOk() ? detectorHash.status().message() : recognizerHash.status().message());
        }
        impl_->info.detectorChecksum = detectorHash.value();
        impl_->info.recognizerChecksum = recognizerHash.value();

        const char* runtimeVersion = OrtGetApiBase()->GetVersionString();
        impl_->info.runtimeVersion = runtimeVersion ? runtimeVersion : "unknown";
        if (impl_->info.runtimeVersion != "1.30.0") {
            throw std::runtime_error("ONNX Runtime 1.30.0 is required; loaded " + impl_->info.runtimeVersion);
        }

        impl_->detector = std::make_unique<LoadedModel>();
        impl_->recognizer = std::make_unique<LoadedModel>();
        const bool tryCuda = impl_->config.device != "cpu";
        auto loadOne = [&](LoadedModel& target, const std::filesystem::path& path, const char* label) {
            if (!tryCuda) {
                target = openModel(path, label, impl_->config.cpuThreads, false);
                return;
            }
            try {
                target = openModel(path, label, impl_->config.cpuThreads, true);
            } catch (const std::exception& ex) {
                if (impl_->config.device == "cuda") throw;
                const std::string fallback = std::string(label) + ": " + exceptionMessage(ex);
                impl_->info.fallbackReason = appendReason(impl_->info.fallbackReason, fallback);
                target = openModel(path, label, impl_->config.cpuThreads, false);
            }
        };
        loadOne(*impl_->detector, detectorPath, "SCRFD detector");
        loadOne(*impl_->recognizer, recognizerPath, "ArcFace recognizer");

        impl_->detectorInputShape = inputShape(*impl_->detector->session, kDetectorSide, "SCRFD detector");
        impl_->recognizerInputShape = inputShape(*impl_->recognizer->session, kRecognizerSide, "ArcFace recognizer");
        if (impl_->recognizer->outputShapes.empty()) {
            throw std::runtime_error("ArcFace recognizer output metadata is missing");
        }
        const auto& embeddingShape = impl_->recognizer->outputShapes.front();
        size_t embeddingSize = 1;
        for (size_t i = 0; i < embeddingShape.size(); ++i) {
            const int64_t dimension = embeddingShape[i];
            if (i == 0 && (dimension == 1 || dimension < 0)) continue;
            if (dimension <= 0 || embeddingSize > static_cast<size_t>(kEmbeddingDimensions) / static_cast<size_t>(dimension)) {
                throw std::runtime_error("ArcFace recognizer output must contain 512 values");
            }
            embeddingSize *= static_cast<size_t>(dimension);
        }
        if (embeddingSize != kEmbeddingDimensions) {
            throw std::runtime_error("ArcFace recognizer output must contain 512 values");
        }
        impl_->info.detectorProvider = impl_->detector->provider;
        impl_->info.recognizerProvider = impl_->recognizer->provider;
        impl_->info.ready = true;
        impl_->info.error.clear();
        impl_->initialized = true;
        return Status::ok();
    } catch (const std::exception& ex) {
        impl_->info.ready = false;
        impl_->info.error = exceptionMessage(ex);
        impl_->initialized = false;
        impl_->detector.reset();
        impl_->recognizer.reset();
        IMAGINE_LOG_ERROR("Face analysis engine initialization failed: " + impl_->info.error);
        return Status::internal(impl_->info.error);
    }
#endif
}

const RuntimeInfo& Engine::info() const { return impl_->info; }
AnalysisTimings Engine::lastTimings() const {
    std::lock_guard lock(impl_->timingsMutex);
    return impl_->timings;
}
const Config& Engine::config() const { return impl_->config; }

Result<std::vector<Detection>> Engine::analyze(const thumbnail::ImageBuffer& image) {
#if !IMAGINE_FACE_ANALYSIS_BUILT
    (void)image;
    return Status::internal("Face analysis is unavailable: this build was configured without ONNX Runtime 1.30.0");
#else
    if (!impl_->initialized || !impl_->info.ready || !impl_->detector || !impl_->recognizer) {
        return Status::internal(impl_->info.error.empty() ? "Face analysis engine is not initialized" : impl_->info.error);
    }
    AnalysisTimings timings{};
    const auto totalStart = std::chrono::steady_clock::now();
    if (image.width <= 0 || image.height <= 0 || image.channels != 3 || image.data.empty()) {
        return Status::invalidArgument("Face analysis requires a complete, interleaved RGB8 image");
    }
    const size_t imageWidth = static_cast<size_t>(image.width);
    const size_t imageHeight = static_cast<size_t>(image.height);
    if (imageWidth > std::numeric_limits<size_t>::max() / imageHeight ||
        imageWidth * imageHeight > std::numeric_limits<size_t>::max() / 3 ||
        image.data.size() != imageWidth * imageHeight * 3) {
        return Status::invalidArgument("Face analysis image dimensions overflow or do not match its RGB8 buffer");
    }
    try {
        const auto preprocessStart = std::chrono::steady_clock::now();
        const double imageRatio = static_cast<double>(image.height) / image.width;
        constexpr double modelRatio = 1.0;
        int resizedHeight;
        int resizedWidth;
        if (imageRatio > modelRatio) {
            resizedHeight = kDetectorSide;
            resizedWidth = static_cast<int>(resizedHeight / imageRatio);
        } else {
            resizedWidth = kDetectorSide;
            resizedHeight = static_cast<int>(resizedWidth * imageRatio);
        }
        resizedWidth = std::clamp(resizedWidth, 1, kDetectorSide);
        resizedHeight = std::clamp(resizedHeight, 1, kDetectorSide);
        const float scale = static_cast<float>(resizedHeight) / image.height;

        std::vector<float> detectorInput(static_cast<size_t>(3) * kDetectorSide * kDetectorSide,
                                         -127.5f / 128.0f);
        constexpr int kResizeCoefficientBits = 11;
        constexpr int kResizeCoefficientScale = 1 << kResizeCoefficientBits;

        static const auto kNormLut = [] {
            std::array<float, 256> table{};
            for (int i = 0; i < 256; ++i) {
                table[i] = (static_cast<float>(i) - 127.5f) / 128.0f;
            }
            return table;
        }();

        struct ResampleX {
            size_t off0;
            size_t off1;
            int alpha0;
            int alpha1;
        };
        std::vector<ResampleX> xWeights(resizedWidth);
        for (int x = 0; x < resizedWidth; ++x) {
            float sourceX = static_cast<float>((static_cast<double>(x) + 0.5) * image.width / resizedWidth - 0.5);
            int x0 = static_cast<int>(std::floor(sourceX));
            float fx = sourceX - x0;
            if (x0 < 0) { x0 = 0; fx = 0.0f; }
            if (x0 >= image.width - 1) { x0 = image.width - 1; fx = 0.0f; }
            const int x1 = std::min(x0 + 1, image.width - 1);
            xWeights[x] = {
                static_cast<size_t>(x0) * 3,
                static_cast<size_t>(x1) * 3,
                static_cast<int>(std::lrint((1.0f - fx) * kResizeCoefficientScale)),
                static_cast<int>(std::lrint(fx * kResizeCoefficientScale))
            };
        }

        constexpr size_t kPlaneSize = static_cast<size_t>(kDetectorSide) * kDetectorSide;
        float* planeR = detectorInput.data();
        float* planeG = planeR + kPlaneSize;
        float* planeB = planeG + kPlaneSize;
        const uint8_t* imgBytes = image.data.data();
        const size_t imgStride = static_cast<size_t>(image.width) * 3;

        const int hardwareThreads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
        const int targetThreads = impl_->config.cpuThreads > 0 ? impl_->config.cpuThreads : hardwareThreads;
        const int numThreads = std::clamp(targetThreads, 1, std::min(4, hardwareThreads));
        auto processRows = [&](int startY, int endY) {
            for (int y = startY; y < endY; ++y) {
                float sourceY = static_cast<float>((static_cast<double>(y) + 0.5) * image.height / resizedHeight - 0.5);
                int y0 = static_cast<int>(std::floor(sourceY));
                float fy = sourceY - y0;
                if (y0 < 0) { y0 = 0; fy = 0.0f; }
                if (y0 >= image.height - 1) { y0 = image.height - 1; fy = 0.0f; }
                const int y1 = std::min(y0 + 1, image.height - 1);
                const int beta0 = static_cast<int>(std::lrint((1.0f - fy) * kResizeCoefficientScale));
                const int beta1 = static_cast<int>(std::lrint(fy * kResizeCoefficientScale));

                const uint8_t* row0 = imgBytes + static_cast<size_t>(y0) * imgStride;
                const uint8_t* row1 = imgBytes + static_cast<size_t>(y1) * imgStride;
                const size_t destRow = static_cast<size_t>(y) * kDetectorSide;
                float* outR = planeR + destRow;
                float* outG = planeG + destRow;
                float* outB = planeB + destRow;

                for (int x = 0; x < resizedWidth; ++x) {
                    const auto& xw = xWeights[x];
                    const size_t off0 = xw.off0;
                    const size_t off1 = xw.off1;

                    const int topR = row0[off0] * xw.alpha0 + row0[off1] * xw.alpha1;
                    const int botR = row1[off0] * xw.alpha0 + row1[off1] * xw.alpha1;
                    const int valR = (topR * beta0 + botR * beta1 + (1 << (2 * kResizeCoefficientBits - 1))) >>
                                     (2 * kResizeCoefficientBits);
                    outR[x] = kNormLut[static_cast<size_t>(std::clamp(valR, 0, 255))];

                    const int topG = row0[off0 + 1] * xw.alpha0 + row0[off1 + 1] * xw.alpha1;
                    const int botG = row1[off0 + 1] * xw.alpha0 + row1[off1 + 1] * xw.alpha1;
                    const int valG = (topG * beta0 + botG * beta1 + (1 << (2 * kResizeCoefficientBits - 1))) >>
                                     (2 * kResizeCoefficientBits);
                    outG[x] = kNormLut[static_cast<size_t>(std::clamp(valG, 0, 255))];

                    const int topB = row0[off0 + 2] * xw.alpha0 + row0[off1 + 2] * xw.alpha1;
                    const int botB = row1[off0 + 2] * xw.alpha0 + row1[off1 + 2] * xw.alpha1;
                    const int valB = (topB * beta0 + botB * beta1 + (1 << (2 * kResizeCoefficientBits - 1))) >>
                                     (2 * kResizeCoefficientBits);
                    outB[x] = kNormLut[static_cast<size_t>(std::clamp(valB, 0, 255))];
                }
            }
        };

        if (numThreads <= 1 || resizedHeight <= 16) {
            processRows(0, resizedHeight);
        } else {
            std::vector<std::jthread> threads;
            threads.reserve(numThreads - 1);
            const int chunkSize = (resizedHeight + numThreads - 1) / numThreads;
            for (int t = 1; t < numThreads; ++t) {
                const int start = t * chunkSize;
                const int end = std::min(start + chunkSize, resizedHeight);
                if (start < end) {
                    threads.emplace_back(processRows, start, end);
                }
            }
            processRows(0, std::min(chunkSize, resizedHeight));
        }
        const auto detectorStart = std::chrono::steady_clock::now();
        timings.detectorPreprocessMs = std::chrono::duration<double, std::milli>(detectorStart - preprocessStart).count();

        const auto detectorOutputs = runModel(*impl_->detector, detectorInput.data(), impl_->detectorInputShape);
        const auto postprocessStart = std::chrono::steady_clock::now();
        timings.detectorInferenceMs = std::chrono::duration<double, std::milli>(postprocessStart - detectorStart).count();
        auto candidates = decodeDetections(detectorOutputs, scale, impl_->config.confidence);
        const auto nmsStart = std::chrono::steady_clock::now();
        timings.detectorDecodeMs = std::chrono::duration<double, std::milli>(nmsStart - postprocessStart).count();
        candidates = suppress(std::move(candidates), impl_->config.nmsThreshold);
        const auto recognitionStart = std::chrono::steady_clock::now();
        timings.detectorNmsMs = std::chrono::duration<double, std::milli>(recognitionStart - nmsStart).count();

        std::vector<Detection> detections;
        detections.resize(candidates.size());

        for (size_t i = 0; i < candidates.size(); ++i) {
            const auto& candidate = candidates[i];
            detections[i].x = candidate.x1;
            detections[i].y = candidate.y1;
            detections[i].width = candidate.x2 - candidate.x1;
            detections[i].height = candidate.y2 - candidate.y1;
            detections[i].score = candidate.score;
            detections[i].landmarks = candidate.landmarks;
        }

        const size_t maxBatch = impl_->recognizer->dynamicBatch ? 16 : 1;
        constexpr size_t kCropElements = static_cast<size_t>(3) * kRecognizerSide * kRecognizerSide;
        constexpr size_t kPlaneElements = static_cast<size_t>(kRecognizerSide) * kRecognizerSide;

        for (size_t chunkStart = 0; chunkStart < candidates.size(); chunkStart += maxBatch) {
            const size_t chunkSize = std::min(maxBatch, candidates.size() - chunkStart);
            std::vector<float> batchInput(chunkSize * kCropElements);
            std::vector<size_t> validChunkIndices;
            validChunkIndices.reserve(chunkSize);

            for (size_t b = 0; b < chunkSize; ++b) {
                const size_t candIdx = chunkStart + b;
                try {
                    float* crop = batchInput.data() + validChunkIndices.size() * kCropElements;
                    alignAndNormalizeTo(image, candidates[candIdx].landmarks,
                                        crop, crop + kPlaneElements, crop + 2 * kPlaneElements);
                    validChunkIndices.push_back(candIdx);
                } catch (const std::exception& ex) {
                    detections[candIdx].embeddingError = exceptionMessage(ex);
                }
            }

            if (validChunkIndices.empty()) continue;

            const size_t validCount = validChunkIndices.size();
            const std::vector<int64_t> batchShape{static_cast<int64_t>(validCount), 3, kRecognizerSide, kRecognizerSide};
            try {
                auto embeddingOutput = runModel(*impl_->recognizer, batchInput.data(), batchShape);
                if (embeddingOutput.size() != 1 || embeddingOutput.front().size() != validCount * kEmbeddingDimensions) {
                    throw std::runtime_error("ArcFace inference returned an invalid embedding shape");
                }
                const auto& rawAll = embeddingOutput.front();
                for (size_t b = 0; b < validCount; ++b) {
                    const size_t candIdx = validChunkIndices[b];
                    const float* raw = rawAll.data() + b * kEmbeddingDimensions;
                    double normSquared = 0.0;
                    for (size_t d = 0; d < kEmbeddingDimensions; ++d) {
                        const float value = raw[d];
                        if (!std::isfinite(value)) {
                            detections[candIdx].embeddingError = "ArcFace inference returned a non-finite embedding";
                            break;
                        }
                        normSquared += static_cast<double>(value) * value;
                    }
                    if (!detections[candIdx].embeddingError.empty()) continue;
                    const double norm = std::sqrt(normSquared);
                    if (!std::isfinite(norm) || norm <= 1e-12) {
                        detections[candIdx].embeddingError = "ArcFace inference returned a zero-norm embedding";
                        continue;
                    }
                    detections[candIdx].embedding.reserve(kEmbeddingDimensions);
                    for (size_t d = 0; d < kEmbeddingDimensions; ++d) {
                        detections[candIdx].embedding.push_back(static_cast<float>(raw[d] / norm));
                    }
                }
            } catch (const std::exception& ex) {
                const std::string err = exceptionMessage(ex);
                for (size_t b = 0; b < validCount; ++b) {
                    const size_t candIdx = validChunkIndices[b];
                    detections[candIdx].embeddingError = err;
                    detections[candIdx].embedding.clear();
                }
            }
        }
        const auto finish = std::chrono::steady_clock::now();
        timings.recognitionMs = std::chrono::duration<double, std::milli>(finish - recognitionStart).count();
        timings.detectionMs = timings.detectorPreprocessMs + timings.detectorInferenceMs +
                              timings.detectorDecodeMs + timings.detectorNmsMs;
        timings.totalMs = std::chrono::duration<double, std::milli>(finish - totalStart).count();
        timings.detections = detections.size();
        timings.embeddings = static_cast<size_t>(std::count_if(detections.begin(), detections.end(), [](const Detection& face) {
            return face.embedding.size() == kEmbeddingDimensions;
        }));
        {
            std::lock_guard lock(impl_->timingsMutex);
            impl_->timings = timings;
        }
        return detections;
    } catch (const Ort::Exception& ex) {
        timings.totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - totalStart).count();
        std::lock_guard lock(impl_->timingsMutex);
        impl_->timings = timings;
        IMAGINE_LOG_ERROR("ONNX face inference failed: " + exceptionMessage(ex));
        return Status::internal("ONNX inference failed: " + exceptionMessage(ex));
    } catch (const std::exception& ex) {
        timings.totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - totalStart).count();
        std::lock_guard lock(impl_->timingsMutex);
        impl_->timings = timings;
        IMAGINE_LOG_ERROR("Face analysis failed: " + exceptionMessage(ex));
        return Status::internal("Face analysis failed: " + exceptionMessage(ex));
    }
#endif
}

} // namespace imagine::faces
