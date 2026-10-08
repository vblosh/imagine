#include "imagine/clip/engine.hpp"
#include "tokenizer.hpp"
#include "imagine/common/logger.hpp"
#include "stb_image_resize2.h"
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <cmath>
#include <algorithm>
#include <fstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef IMAGINE_FACE_ANALYSIS_BUILT
#define IMAGINE_FACE_ANALYSIS_BUILT 0
#endif

#if IMAGINE_FACE_ANALYSIS_BUILT
#include <onnxruntime_cxx_api.h>
#include <nlohmann/json.hpp>
#endif

namespace imagine::clip {

#if IMAGINE_FACE_ANALYSIS_BUILT
static void l2Normalize(std::vector<float>& v) {
    double sum = 0.0;
    for (float x : v) sum += static_cast<double>(x) * x;
    float inv = 1.0f / std::sqrt(static_cast<float>(std::max(sum, 1e-24)));
    for (float& x : v) x *= inv;
}

static Ort::Env& ortEnvironment() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "clip-semantic");
    return env;
}
#endif

struct Engine::Impl {
    ClipConfig config;
    ClipRuntimeInfo info;
    mutable std::shared_mutex sessionMutex;
    mutable std::mutex timingsMutex;
    ClipTimings lastTimings;
    Tokenizer tokenizer;

#if IMAGINE_FACE_ANALYSIS_BUILT
    std::unique_ptr<Ort::Session> imageSession;
    std::unique_ptr<Ort::Session> textSession;
    std::string imageInputName;
    std::string imageOutputName;
    std::string textInputIdsName;
    std::string textAttentionMaskName;
    std::string textOutputName;

    Status loadLocked();
#endif
};

#if IMAGINE_FACE_ANALYSIS_BUILT
Status Engine::Impl::loadLocked() {
    if (info.loaded && imageSession && textSession) {
        return Status::ok();
    }
    try {
        std::filesystem::path dir(config.modelDirectory);
        auto imageModelPath = dir / "image_encoder.onnx";
        auto textModelPath = dir / "text_encoder.onnx";

        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(config.cpuThreads);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        options.SetExecutionMode(ORT_SEQUENTIAL);

        if (config.device == "cuda") {
            OrtCUDAProviderOptions cudaOptions{};
            cudaOptions.device_id = 0;
            options.AppendExecutionProvider_CUDA(cudaOptions);
            info.executionProvider = "cuda";
        } else {
            info.executionProvider = "cpu";
        }

        imageSession = std::make_unique<Ort::Session>(ortEnvironment(), imageModelPath.native().c_str(), options);
        textSession = std::make_unique<Ort::Session>(ortEnvironment(), textModelPath.native().c_str(), options);

        Ort::AllocatorWithDefaultOptions allocator;
        imageInputName = imageSession->GetInputNameAllocated(0, allocator).get();
        imageOutputName = imageSession->GetOutputNameAllocated(0, allocator).get();

        size_t textInputs = textSession->GetInputCount();
        for (size_t i = 0; i < textInputs; ++i) {
            std::string name = textSession->GetInputNameAllocated(i, allocator).get();
            if (name.find("input_ids") != std::string::npos) textInputIdsName = name;
            else if (name.find("attention_mask") != std::string::npos) textAttentionMaskName = name;
        }
        textOutputName = textSession->GetOutputNameAllocated(0, allocator).get();

        auto typeInfo = textSession->GetOutputTypeInfo(0);
        auto shape = typeInfo.GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() > 1) {
            info.vectorDim = static_cast<int>(shape.back());
        } else if (info.vectorDim == 0) {
            info.vectorDim = 512;
        }

        info.runtimeVersion = OrtGetApiBase()->GetVersionString();

        // Warmup text
        {
            auto tok = tokenizer.encode("warmup");
            std::vector<int64_t> inputShape = {1, static_cast<int64_t>(tok.input_ids.size())};
            auto memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            auto inputTensor = Ort::Value::CreateTensor<int64_t>(memoryInfo, tok.input_ids.data(), tok.input_ids.size(), inputShape.data(), inputShape.size());
            auto maskTensor = Ort::Value::CreateTensor<int64_t>(memoryInfo, tok.attention_mask.data(), tok.attention_mask.size(), inputShape.data(), inputShape.size());
            const char* inputNames[] = {textInputIdsName.c_str(), textAttentionMaskName.c_str()};
            Ort::Value inputValues[] = {std::move(inputTensor), std::move(maskTensor)};
            const char* outputNames[] = {textOutputName.c_str()};
            textSession->Run(Ort::RunOptions{nullptr}, inputNames, inputValues, 2, outputNames, 1);
        }

        // Warmup image
        {
            std::vector<float> dummy(3 * 224 * 224, 0.0f);
            std::vector<int64_t> inputShape = {1, 3, 224, 224};
            auto memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            auto inputTensor = Ort::Value::CreateTensor<float>(memoryInfo, dummy.data(), dummy.size(), inputShape.data(), inputShape.size());
            const char* inputNames[] = {imageInputName.c_str()};
            const char* outputNames[] = {imageOutputName.c_str()};
            imageSession->Run(Ort::RunOptions{nullptr}, inputNames, &inputTensor, 1, outputNames, 1);
        }

        info.loaded = true;
        info.error.clear();
        IMAGINE_LOG_INFO("CLIP engine models loaded into memory (provider: " + info.executionProvider + ")");
        return Status::ok();
    } catch (const std::exception& e) {
        info.error = e.what();
        info.loaded = false;
        imageSession.reset();
        textSession.reset();
        return Status::internal(e.what());
    }
}
#endif

Engine::Engine(ClipConfig config) : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(config);
    impl_->info.built = IMAGINE_FACE_ANALYSIS_BUILT != 0;
}

Engine::~Engine() = default;

Status Engine::initialize(bool eager) {
#if !IMAGINE_FACE_ANALYSIS_BUILT
    (void)eager;
    return Status::internal("Semantic search requires ONNX Runtime");
#else
    std::unique_lock lock(impl_->sessionMutex);
    try {
        std::filesystem::path dir(impl_->config.modelDirectory);
        if (impl_->config.modelDirectory.empty() || !std::filesystem::is_directory(dir)) {
            impl_->info.ready = false;
            impl_->info.loaded = false;
            impl_->info.error = "Model directory does not exist: " + (impl_->config.modelDirectory.empty() ? "(empty)" : impl_->config.modelDirectory);
            return Status::internal(impl_->info.error);
        }

        auto imageModelPath = dir / "image_encoder.onnx";
        auto textModelPath = dir / "text_encoder.onnx";
        
        if (!std::filesystem::is_regular_file(imageModelPath) || !std::filesystem::is_regular_file(textModelPath)) {
            impl_->info.ready = false;
            impl_->info.loaded = false;
            impl_->info.error = "Missing image_encoder.onnx or text_encoder.onnx in " + dir.string();
            return Status::internal(impl_->info.error);
        }

        auto manifestPath = dir / "model_manifest.json";
        if (std::filesystem::is_regular_file(manifestPath)) {
            std::ifstream mf(manifestPath);
            if (mf.is_open()) {
                nlohmann::json j;
                mf >> j;
                impl_->info.modelId = j.value("model_id", "");
                impl_->info.modelRevision = j.value("revision", "");
                impl_->info.vectorDim = j.value("vector_dim", 512);
                if (j.contains("preprocess")) {
                    impl_->info.preprocessId = j["preprocess"].dump();
                }
            }
        }
        if (impl_->info.modelId.empty()) {
            impl_->info.modelId = "clip-onnx";
        }
        if (impl_->info.vectorDim == 0) {
            impl_->info.vectorDim = 512;
        }

        auto vocabPath = dir / "vocab.json";
        auto mergesPath = dir / "merges.txt";
        Status tokStatus = impl_->tokenizer.load(vocabPath.string(), mergesPath.string());
        if (!tokStatus.isOk()) {
            impl_->info.ready = false;
            impl_->info.loaded = false;
            impl_->info.error = tokStatus.message();
            return tokStatus;
        }

        impl_->info.executionProvider = (impl_->config.device == "cuda") ? "cuda" : "cpu";
        impl_->info.runtimeVersion = OrtGetApiBase()->GetVersionString();
        impl_->info.ready = true;
        impl_->info.error.clear();

        if (eager) {
            return impl_->loadLocked();
        }

        return Status::ok();
    } catch (const std::exception& e) {
        impl_->info.ready = false;
        impl_->info.loaded = false;
        impl_->info.error = e.what();
        return Status::internal(e.what());
    }
#endif
}

Status Engine::load() {
#if !IMAGINE_FACE_ANALYSIS_BUILT
    return Status::internal("Semantic search requires ONNX Runtime");
#else
    std::unique_lock lock(impl_->sessionMutex);
    if (!impl_->info.ready) {
        return Status::internal(impl_->info.error.empty() ? "CLIP engine is not ready (missing models)" : impl_->info.error);
    }
    return impl_->loadLocked();
#endif
}

void Engine::unload() {
#if IMAGINE_FACE_ANALYSIS_BUILT
    std::unique_lock lock(impl_->sessionMutex);
    if (!impl_->info.loaded && !impl_->imageSession && !impl_->textSession) {
        return;
    }
    impl_->imageSession.reset();
    impl_->textSession.reset();
    impl_->imageInputName.clear();
    impl_->imageOutputName.clear();
    impl_->textInputIdsName.clear();
    impl_->textAttentionMaskName.clear();
    impl_->textOutputName.clear();
    impl_->info.loaded = false;
#if defined(_WIN32)
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
#endif
    IMAGINE_LOG_INFO("CLIP engine models unloaded; working set memory reclaimed");
#endif
}

bool Engine::isLoaded() const {
#if !IMAGINE_FACE_ANALYSIS_BUILT
    return false;
#else
    std::shared_lock lock(impl_->sessionMutex);
    return impl_->info.loaded && impl_->imageSession != nullptr && impl_->textSession != nullptr;
#endif
}

ClipRuntimeInfo Engine::info() const {
    std::shared_lock lock(impl_->sessionMutex);
    return impl_->info;
}

ClipTimings Engine::lastTimings() const {
    std::lock_guard lock(impl_->timingsMutex);
    return impl_->lastTimings;
}

Result<std::vector<float>> Engine::encodeImage(const thumbnail::ImageBuffer& image) {
#if !IMAGINE_FACE_ANALYSIS_BUILT
    (void)image;
    return Status::internal("Semantic search requires ONNX Runtime");
#else
    {
        std::shared_lock readLock(impl_->sessionMutex);
        if (!impl_->info.ready) {
            return Status::internal("Engine not ready");
        }
        if (!impl_->info.loaded || !impl_->imageSession) {
            readLock.unlock();
            std::unique_lock writeLock(impl_->sessionMutex);
            if (!impl_->info.ready) {
                return Status::internal("Engine not ready");
            }
            if (!impl_->info.loaded || !impl_->imageSession) {
                Status loadStatus = impl_->loadLocked();
                if (!loadStatus.isOk()) {
                    return loadStatus;
                }
            }
        }
    }

    std::shared_lock lock(impl_->sessionMutex);
    if (!impl_->info.ready || !impl_->info.loaded || !impl_->imageSession) {
        return Status::internal(impl_->info.error.empty() ? "Engine not ready" : impl_->info.error);
    }
    auto startPre = std::chrono::steady_clock::now();

    int shortEdge = std::min(image.width, image.height);
    float scale = 224.0f / shortEdge;
    int rw = std::max(224, static_cast<int>(image.width * scale));
    int rh = std::max(224, static_cast<int>(image.height * scale));

    std::vector<uint8_t> resized(rw * rh * 3);
    stbir_resize_uint8_linear(image.data.data(), image.width, image.height, 0,
                              resized.data(), rw, rh, 0, STBIR_RGB);

    int cropX = (rw - 224) / 2;
    int cropY = (rh - 224) / 2;

    std::vector<float> chw(3 * 224 * 224);
    const float means[3] = {0.48145466f, 0.4578275f, 0.40821073f};
    const float stds[3] = {0.26862954f, 0.26130258f, 0.27577711f};

    for (int y = 0; y < 224; ++y) {
        for (int x = 0; x < 224; ++x) {
            int srcIdx = ((cropY + y) * rw + (cropX + x)) * 3;
            for (int c = 0; c < 3; ++c) {
                float val = resized[srcIdx + c] / 255.0f;
                val = (val - means[c]) / stds[c];
                chw[c * 224 * 224 + y * 224 + x] = val;
            }
        }
    }

    auto startInf = std::chrono::steady_clock::now();
    double preMs = std::chrono::duration<double, std::milli>(startInf - startPre).count();

    std::vector<int64_t> inputShape = {1, 3, 224, 224};
    auto memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto inputTensor = Ort::Value::CreateTensor<float>(memoryInfo, chw.data(), chw.size(), inputShape.data(), inputShape.size());

    const char* inputNames[] = {impl_->imageInputName.c_str()};
    const char* outputNames[] = {impl_->imageOutputName.c_str()};
    
    auto outputTensors = impl_->imageSession->Run(Ort::RunOptions{nullptr}, inputNames, &inputTensor, 1, outputNames, 1);
    
    auto endInf = std::chrono::steady_clock::now();
    double infMs = std::chrono::duration<double, std::milli>(endInf - startInf).count();

    const float* outData = outputTensors[0].GetTensorData<float>();
    size_t count = outputTensors[0].GetTensorTypeAndShapeInfo().GetElementCount();
    
    std::vector<float> result(outData, outData + count);
    l2Normalize(result);

    {
        std::lock_guard timingsLock(impl_->timingsMutex);
        impl_->lastTimings.preprocessMs = preMs;
        impl_->lastTimings.inferenceMs = infMs;
        impl_->lastTimings.totalMs = preMs + infMs;
    }

    return result;
#endif
}

Result<std::vector<float>> Engine::encodeText(std::string_view query) {
#if !IMAGINE_FACE_ANALYSIS_BUILT
    (void)query;
    return Status::internal("Semantic search requires ONNX Runtime");
#else
    {
        std::shared_lock readLock(impl_->sessionMutex);
        if (!impl_->info.ready) {
            return Status::internal("Engine not ready");
        }
        if (!impl_->info.loaded || !impl_->textSession) {
            readLock.unlock();
            std::unique_lock writeLock(impl_->sessionMutex);
            if (!impl_->info.ready) {
                return Status::internal("Engine not ready");
            }
            if (!impl_->info.loaded || !impl_->textSession) {
                Status loadStatus = impl_->loadLocked();
                if (!loadStatus.isOk()) {
                    return loadStatus;
                }
            }
        }
    }

    std::shared_lock lock(impl_->sessionMutex);
    if (!impl_->info.ready || !impl_->info.loaded || !impl_->textSession) {
        return Status::internal(impl_->info.error.empty() ? "Engine not ready" : impl_->info.error);
    }
    auto startTok = std::chrono::steady_clock::now();

    auto tokOut = impl_->tokenizer.encode(query);
    
    auto startInf = std::chrono::steady_clock::now();
    double tokMs = std::chrono::duration<double, std::milli>(startInf - startTok).count();

    std::vector<int64_t> inputShape = {1, static_cast<int64_t>(tokOut.input_ids.size())};
    auto memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto inputIdsTensor = Ort::Value::CreateTensor<int64_t>(memoryInfo, tokOut.input_ids.data(), tokOut.input_ids.size(), inputShape.data(), inputShape.size());
    auto maskTensor = Ort::Value::CreateTensor<int64_t>(memoryInfo, tokOut.attention_mask.data(), tokOut.attention_mask.size(), inputShape.data(), inputShape.size());

    const char* inputNames[] = {impl_->textInputIdsName.c_str(), impl_->textAttentionMaskName.c_str()};
    Ort::Value inputValues[] = {std::move(inputIdsTensor), std::move(maskTensor)};
    const char* outputNames[] = {impl_->textOutputName.c_str()};
    
    auto outputTensors = impl_->textSession->Run(Ort::RunOptions{nullptr}, inputNames, inputValues, 2, outputNames, 1);

    auto endInf = std::chrono::steady_clock::now();
    double infMs = std::chrono::duration<double, std::milli>(endInf - startInf).count();

    const float* outData = outputTensors[0].GetTensorData<float>();
    size_t count = outputTensors[0].GetTensorTypeAndShapeInfo().GetElementCount();
    
    std::vector<float> result(outData, outData + count);
    l2Normalize(result);

    {
        std::lock_guard timingsLock(impl_->timingsMutex);
        impl_->lastTimings.tokenizeMs = tokMs;
        impl_->lastTimings.inferenceMs = infMs;
        impl_->lastTimings.totalMs = tokMs + infMs;
    }

    return result;
#endif
}

} // namespace imagine::clip
