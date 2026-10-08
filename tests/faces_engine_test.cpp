#include "imagine/faces/engine.hpp"
#include "imagine/metadata/exif_reader.hpp"
#include "imagine/thumbnail/generator.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <utility>

#ifndef IMAGINE_FACE_ANALYSIS_BUILT
#define IMAGINE_FACE_ANALYSIS_BUILT 0
#endif

using imagine::faces::Config;
using imagine::faces::Engine;
using imagine::thumbnail::ImageBuffer;

TEST(FaceEngineTest, ReportsBuildAvailabilityAndRejectsInvalidConfiguration) {
    Engine defaultEngine(Config{});
    EXPECT_EQ(defaultEngine.info().built, IMAGINE_FACE_ANALYSIS_BUILT != 0);

    Config invalid;
    invalid.device = "gpu-ish";
    Engine invalidEngine(invalid);
    const auto status = invalidEngine.initialize();
    EXPECT_FALSE(status.isOk());
    EXPECT_EQ(status.code(), imagine::StatusCode::InvalidArgument);
    EXPECT_NE(status.message().find("cpu, auto, or cuda"), std::string::npos);
}

#if !IMAGINE_FACE_ANALYSIS_BUILT
TEST(FaceEngineTest, FeatureOffProvidesActionableUnavailableStatus) {
    Engine engine(Config{});
    const auto status = engine.initialize();
    EXPECT_FALSE(status.isOk());
    EXPECT_NE(status.message().find("without ONNX Runtime 1.30.0"), std::string::npos);
    EXPECT_FALSE(engine.info().ready);

    const auto result = engine.analyze(ImageBuffer{});
    EXPECT_FALSE(result.isOk());
    EXPECT_NE(result.status().message().find("without ONNX Runtime 1.30.0"), std::string::npos);
    EXPECT_DOUBLE_EQ(engine.lastTimings().totalMs, 0.0);
}
#endif

TEST(FaceEngineTest, BuffaloLModelsProduceFiniteGeometryAndNormalizedEmbeddingsWhenConfigured) {
    const char* modelDirectory = std::getenv("IMAGINE_FACE_TEST_MODELS");
    if (!modelDirectory || !*modelDirectory) {
        GTEST_SKIP() << "Set IMAGINE_FACE_TEST_MODELS to a buffalo_l model directory for the real-model check";
    }
    if (IMAGINE_FACE_ANALYSIS_BUILT == 0) {
        GTEST_SKIP() << "This build does not include the optional ONNX Runtime feature";
    }

    Config config;
    config.modelDirectory = modelDirectory;
    config.device = "cpu";
    config.cpuThreads = 1;
    Engine engine(config);
    const auto initialized = engine.initialize();
    ASSERT_TRUE(initialized.isOk()) << initialized.message();
    EXPECT_EQ(engine.info().runtimeVersion, "1.30.0");
    EXPECT_EQ(engine.info().detectorChecksum.size(), 64u);
    EXPECT_EQ(engine.info().recognizerChecksum.size(), 64u);
    EXPECT_EQ(engine.info().detectorProvider, "CPUExecutionProvider");
    EXPECT_EQ(engine.info().recognizerProvider, "CPUExecutionProvider");

    const auto expectMalformed = [&engine](ImageBuffer image) {
        const auto result = engine.analyze(image);
        if (result.isOk()) {
            ADD_FAILURE() << "Malformed RGB buffer was accepted";
            return;
        }
        EXPECT_EQ(result.status().code(), imagine::StatusCode::InvalidArgument);
    };
    ImageBuffer negativeDimensions;
    negativeDimensions.width = -1;
    negativeDimensions.height = 1;
    negativeDimensions.channels = 3;
    negativeDimensions.data = {0, 0, 0};
    expectMalformed(std::move(negativeDimensions));

    ImageBuffer wrongChannels;
    wrongChannels.width = 1;
    wrongChannels.height = 1;
    wrongChannels.channels = 4;
    wrongChannels.data = {0, 0, 0, 255};
    expectMalformed(std::move(wrongChannels));

    ImageBuffer shortBuffer;
    shortBuffer.width = 2;
    shortBuffer.height = 2;
    shortBuffer.channels = 3;
    shortBuffer.data.assign(11, 0);
    expectMalformed(std::move(shortBuffer));

    ImageBuffer blank;
    blank.width = 64;
    blank.height = 48;
    blank.channels = 3;
    blank.data.assign(static_cast<size_t>(blank.width) * blank.height * 3, 128);
    const auto detections = engine.analyze(blank);
    ASSERT_TRUE(detections.isOk()) << detections.status().message();
    const auto& timing = engine.lastTimings();
    EXPECT_TRUE(std::isfinite(timing.detectorPreprocessMs));
    EXPECT_TRUE(std::isfinite(timing.detectorInferenceMs));
    EXPECT_TRUE(std::isfinite(timing.detectorDecodeMs));
    EXPECT_TRUE(std::isfinite(timing.detectorNmsMs));
    EXPECT_TRUE(std::isfinite(timing.detectionMs));
    EXPECT_TRUE(std::isfinite(timing.recognitionMs));
    EXPECT_TRUE(std::isfinite(timing.totalMs));
    EXPECT_GE(timing.totalMs, timing.detectorPreprocessMs);
    EXPECT_GE(timing.detectionMs, timing.detectorPreprocessMs);
    EXPECT_EQ(timing.detections, detections.value().size());
    size_t embeddedFaces = 0;
    for (const auto& face : detections.value()) {
        EXPECT_TRUE(std::isfinite(face.x));
        EXPECT_TRUE(std::isfinite(face.y));
        EXPECT_TRUE(std::isfinite(face.width));
        EXPECT_TRUE(std::isfinite(face.height));
        EXPECT_GT(face.width, 0.0f);
        EXPECT_GT(face.height, 0.0f);
        EXPECT_TRUE(std::isfinite(face.score));
        for (const auto& point : face.landmarks) {
            EXPECT_TRUE(std::isfinite(point.x));
            EXPECT_TRUE(std::isfinite(point.y));
        }
        if (!face.embedding.empty()) {
            ASSERT_EQ(face.embedding.size(), 512u);
            ++embeddedFaces;
            double norm = 0.0;
            for (float value : face.embedding) norm += static_cast<double>(value) * value;
            EXPECT_NEAR(std::sqrt(norm), 1.0, 1e-4);
        } else {
            EXPECT_FALSE(face.embeddingError.empty());
        }
    }
    EXPECT_EQ(timing.embeddings, embeddedFaces);
}

TEST(FaceEngineTest, RealFaceFixtureProducesNormalizedEmbeddingWhenConfigured) {
    if (IMAGINE_FACE_ANALYSIS_BUILT == 0) {
        GTEST_SKIP() << "This build does not include the optional ONNX Runtime feature";
    }
    const char* modelDirectory = std::getenv("IMAGINE_FACE_TEST_MODELS");
    const char* imagePath = std::getenv("IMAGINE_FACE_TEST_IMAGE");
    if (!modelDirectory || !*modelDirectory || !imagePath || !*imagePath) {
        GTEST_SKIP() << "Set IMAGINE_FACE_TEST_MODELS and IMAGINE_FACE_TEST_IMAGE for the real-face check";
    }

    Config config;
    config.modelDirectory = modelDirectory;
    config.device = "cpu";
    config.cpuThreads = 1;
    Engine engine(config);
    ASSERT_TRUE(engine.initialize().isOk()) << engine.info().error;
    auto decoded = imagine::thumbnail::Generator::loadImage(imagePath);
    ASSERT_TRUE(decoded.isOk()) << decoded.status().message();
    const auto exif = imagine::metadata::ExifReader::readFromFile(imagePath);
    const int orientation = exif.isOk() ? exif.value().orientation : 1;
    const auto oriented = imagine::thumbnail::Generator::rotate(decoded.value(), orientation);

    const auto detections = engine.analyze(oriented);
    ASSERT_TRUE(detections.isOk()) << detections.status().message();
    ASSERT_FALSE(detections.value().empty()) << "The configured fixture must contain a visible face";
    EXPECT_EQ(engine.lastTimings().detections, detections.value().size());
    EXPECT_EQ(engine.lastTimings().embeddings, detections.value().size());
    const auto& face = detections.value().front();
    ASSERT_EQ(face.embedding.size(), 512u) << face.embeddingError;
    double squaredNorm = 0.0;
    for (const float value : face.embedding) {
        ASSERT_TRUE(std::isfinite(value));
        squaredNorm += static_cast<double>(value) * value;
    }
    EXPECT_NEAR(std::sqrt(squaredNorm), 1.0, 1e-4);
}

TEST(FaceEngineTest, EngineSupportsLazyLoadingAndExplicitUnloadCycle) {
    const char* modelDirectory = std::getenv("IMAGINE_FACE_TEST_MODELS");
    if (!modelDirectory || !*modelDirectory) {
        GTEST_SKIP() << "Set IMAGINE_FACE_TEST_MODELS to a buffalo_l model directory for the lazy loading check";
    }
    if (IMAGINE_FACE_ANALYSIS_BUILT == 0) {
        GTEST_SKIP() << "This build does not include the optional ONNX Runtime feature";
    }

    Config config;
    config.modelDirectory = modelDirectory;
    config.device = "cpu";
    config.cpuThreads = 1;
    Engine engine(config);

    // 1. By default, initialize() is lazy (eager = false)
    const auto initialized = engine.initialize(false);
    ASSERT_TRUE(initialized.isOk()) << initialized.message();
    EXPECT_TRUE(engine.info().ready);
    EXPECT_FALSE(engine.info().loaded);
    EXPECT_FALSE(engine.isLoaded());

    // 2. Explicit load() loads model sessions
    const auto loaded = engine.load();
    ASSERT_TRUE(loaded.isOk()) << loaded.message();
    EXPECT_TRUE(engine.isLoaded());
    EXPECT_TRUE(engine.info().loaded);

    // 3. Explicit unload() drops sessions and marks unloaded
    engine.unload();
    EXPECT_FALSE(engine.isLoaded());
    EXPECT_FALSE(engine.info().loaded);
    EXPECT_TRUE(engine.info().ready);

    // 4. Calling analyze() while unloaded automatically reloads on demand
    ImageBuffer blank;
    blank.width = 64;
    blank.height = 48;
    blank.channels = 3;
    blank.data.assign(static_cast<size_t>(blank.width) * blank.height * 3, 128);
    const auto detections = engine.analyze(blank);
    ASSERT_TRUE(detections.isOk()) << detections.status().message();
    EXPECT_TRUE(engine.isLoaded());
    EXPECT_TRUE(engine.info().loaded);

    // 5. Unload again cleans up
    engine.unload();
    EXPECT_FALSE(engine.isLoaded());
    EXPECT_FALSE(engine.info().loaded);
}
