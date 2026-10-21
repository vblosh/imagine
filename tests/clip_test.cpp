#include <gtest/gtest.h>
#include "imagine/clip/engine.hpp"
#include "imagine/clip/service.hpp"
#include "imagine/clip/smart_tags.hpp"
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include "imagine/db/catalog_db.hpp"
#include "imagine/db/schema.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>
#include "imagine/thumbnail/generator.hpp"
#include "imagine/metadata/hasher.hpp"
#include "../src/clip/tokenizer.hpp"

namespace fs = std::filesystem;

namespace {

void setTestEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) {
        unsetenv(name);
    } else {
        setenv(name, value.c_str(), 1);
    }
#endif
}

void unsetTestEnv(const char* name) {
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

} // namespace

TEST(ClipEngineTest, InfoReportsBuiltStatus) {
    imagine::clip::ClipConfig config;
    config.modelDirectory = "nonexistent_dir";
    imagine::clip::Engine engine(config);
    const auto info = engine.info();
#if IMAGINE_FACE_ANALYSIS_BUILT
    EXPECT_TRUE(info.built);
    EXPECT_FALSE(info.ready);
    EXPECT_FALSE(info.loaded);
#else
    EXPECT_FALSE(info.built);
    EXPECT_FALSE(info.ready);
    EXPECT_FALSE(info.loaded);
#endif
}

TEST(ClipEngineTest, InitializeFailsWithoutModels) {
    imagine::clip::ClipConfig config;
    config.modelDirectory = "nonexistent_dir";
    imagine::clip::Engine engine(config);
    auto status = engine.initialize();
    EXPECT_FALSE(status.isOk());
}

TEST(ClipEngineTest, EncodeTextFailsWhenNotReady) {
    imagine::clip::ClipConfig config;
    config.modelDirectory = "nonexistent_dir";
    imagine::clip::Engine engine(config);
    auto result = engine.encodeText("test query");
    EXPECT_FALSE(result.isOk());
}

TEST(ClipEngineTest, EncodeImageFailsWhenNotReady) {
    imagine::clip::ClipConfig config;
    config.modelDirectory = "nonexistent_dir";
    imagine::clip::Engine engine(config);
    imagine::thumbnail::ImageBuffer buf;
    buf.width = 10; buf.height = 10; buf.channels = 3;
    buf.data.resize(10 * 10 * 3, 128);
    auto result = engine.encodeImage(buf);
    EXPECT_FALSE(result.isOk());
}

TEST(SemanticSchemaTest, MigrationV7CreatesSemanticTables) {
    std::error_code ec;
    auto testDb = fs::temp_directory_path() / ("test_semantic_schema_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    fs::remove(testDb, ec);
    fs::remove(testDb.string() + "-wal", ec);
    fs::remove(testDb.string() + "-shm", ec);
    
    {
        imagine::db::CatalogDb db;
        auto s = db.open(testDb.string());
        ASSERT_TRUE(s.isOk()) << s.message();
        
        // Verify semantic tables exist by running a simple query
        auto& conn = db.connection();
        {
            auto stmtRes = conn.prepare("SELECT COUNT(*) FROM semantic_index_state;");
            EXPECT_TRUE(stmtRes.isOk()) << "semantic_index_state table should exist";
        }
        {
            auto stmtRes = conn.prepare("SELECT COUNT(*) FROM semantic_models;");
            EXPECT_TRUE(stmtRes.isOk()) << "semantic_models table should exist";
        }
        {
            auto stmtRes = conn.prepare("SELECT COUNT(*) FROM semantic_jobs;");
            EXPECT_TRUE(stmtRes.isOk()) << "semantic_jobs table should exist";
        }
        db.close();
    }
    fs::remove(testDb, ec);
    fs::remove(testDb.string() + "-wal", ec);
    fs::remove(testDb.string() + "-shm", ec);
}

TEST(ClipServiceTest, StatusAndSearchWhenNotReady) {
    std::error_code ec;
    auto testDb = fs::temp_directory_path() / ("test_clip_svc_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    auto tempCache = fs::temp_directory_path() / ("test_clip_cache_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tempCache, ec);
    
    const char* prevEnv = std::getenv("IMAGINE_CLIP_MODEL_DIR");
    std::string prevModelDir = prevEnv ? prevEnv : "";
    setTestEnv("IMAGINE_CLIP_MODEL_DIR", (tempCache / "nonexistent_models").string());
    struct EnvCleanup {
        std::string prev;
        ~EnvCleanup() {
            setTestEnv("IMAGINE_CLIP_MODEL_DIR", prev);
        }
    } envCleanup{prevModelDir};

    {
        imagine::db::CatalogDb db;
        ASSERT_TRUE(db.open(testDb.string()).isOk());
        
        imagine::clip::Service svc(db, tempCache.string(), [](const std::string& p) { return p; });
        auto status = svc.status();
        EXPECT_TRUE(status.contains("built"));
        EXPECT_TRUE(status.contains("ready"));
        EXPECT_TRUE(status.contains("loaded"));
        EXPECT_FALSE(status["ready"].get<bool>());
        EXPECT_FALSE(status["loaded"].get<bool>());
        EXPECT_EQ(status["indexSize"].get<int>(), 0);
        
        imagine::clip::SearchRequest sreq;
        sreq.query = "red car";
        auto searchRes = svc.search(sreq);
        EXPECT_FALSE(searchRes.isOk());
        
        auto simRes = svc.findSimilar(1, 10);
        EXPECT_FALSE(simRes.isOk());
        
        int64_t jobId = 0;
        auto jobStatus = svc.startJob("catalog", {}, false, jobId);
        EXPECT_FALSE(jobStatus.isOk());
        
        db.close();
    }
    
    fs::remove(testDb, ec);
    fs::remove(testDb.string() + "-wal", ec);
    fs::remove(testDb.string() + "-shm", ec);
    fs::remove_all(tempCache, ec);
}

TEST(ClipTokenizerTest, LoadAndEncode) {
    std::error_code ec;
    auto tempDir = fs::temp_directory_path() / ("test_clip_tok_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tempDir, ec);
    
    auto vocabPath = tempDir / "vocab.json";
    auto mergesPath = tempDir / "merges.txt";
    
    // Write mock vocab
    nlohmann::json mockVocab = {
        {"<|startoftext|>", 49406},
        {"<|endoftext|>", 49407},
        {"test</w>", 100},
        {"photo</w>", 101}
    };
    std::ofstream vf(vocabPath);
    vf << mockVocab.dump();
    vf.close();
    
    // Write mock merges
    std::ofstream mf(mergesPath);
    mf << "#version: 0.2\n";
    mf << "t e\n";
    mf << "te s\n";
    mf << "tes t</w>\n";
    mf << "p h\n";
    mf << "ph o\n";
    mf << "pho t\n";
    mf << "phot o</w>\n";
    mf.close();
    
    imagine::clip::Tokenizer tok;
    EXPECT_FALSE(tok.isLoaded());
    
    auto loadStatus = tok.load(vocabPath.string(), mergesPath.string());
    ASSERT_TRUE(loadStatus.isOk()) << loadStatus.message();
    EXPECT_TRUE(tok.isLoaded());
    
    auto encoded = tok.encode("test photo", 77);
    EXPECT_EQ(encoded.input_ids.size(), 77u);
    EXPECT_EQ(encoded.attention_mask.size(), 77u);
    
    // First token is SOT
    EXPECT_EQ(encoded.input_ids[0], 49406);
    // Followed by test and photo tokens
    EXPECT_EQ(encoded.input_ids[1], 100);
    EXPECT_EQ(encoded.input_ids[2], 101);
    // Followed by EOT
    EXPECT_EQ(encoded.input_ids[3], 49407);
    // Followed by padding
    EXPECT_EQ(encoded.input_ids[4], 0);
    
    // Attention mask: 1 for active tokens, 0 for padding
    EXPECT_EQ(encoded.attention_mask[0], 1);
    EXPECT_EQ(encoded.attention_mask[1], 1);
    EXPECT_EQ(encoded.attention_mask[2], 1);
    EXPECT_EQ(encoded.attention_mask[3], 1);
    EXPECT_EQ(encoded.attention_mask[4], 0);
    EXPECT_EQ(encoded.attention_mask[76], 0);
    
    fs::remove_all(tempDir, ec);
}

TEST(ClipEngineTest, LazyLoadingProbesMetadataWithoutLoading) {
#if !IMAGINE_FACE_ANALYSIS_BUILT
    GTEST_SKIP() << "ONNX Runtime not enabled in this build";
#endif
    std::error_code ec;
    auto tempDir = fs::temp_directory_path() / ("test_clip_lazy_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tempDir, ec);

    // Write mock vocab & merges
    nlohmann::json mockVocab = {
        {"<|startoftext|>", 49406},
        {"<|endoftext|>", 49407},
        {"photo</w>", 100}
    };
    std::ofstream vf(tempDir / "vocab.json");
    vf << mockVocab.dump();
    vf.close();

    std::ofstream mf(tempDir / "merges.txt");
    mf << "#version: 0.2\n";
    mf.close();

    nlohmann::json manifest = {
        {"model_id", "clip-vit-lazy-test"},
        {"revision", "r1"},
        {"vector_dim", 512}
    };
    std::ofstream mnf(tempDir / "model_manifest.json");
    mnf << manifest.dump();
    mnf.close();

    // Create dummy model files
    std::ofstream imf(tempDir / "image_encoder.onnx");
    imf << "dummy";
    imf.close();

    std::ofstream tmf(tempDir / "text_encoder.onnx");
    tmf << "dummy";
    tmf.close();

    imagine::clip::ClipConfig config;
    config.modelDirectory = tempDir.string();
    imagine::clip::Engine engine(config);

    // 1. By default, initialize() is lazy (eager = false)
    auto initStatus = engine.initialize(false);
    ASSERT_TRUE(initStatus.isOk()) << initStatus.message();
    auto info = engine.info();
    EXPECT_TRUE(info.ready);
    EXPECT_FALSE(info.loaded);
    EXPECT_FALSE(engine.isLoaded());
    EXPECT_EQ(info.modelId, "clip-vit-lazy-test");
    EXPECT_EQ(info.vectorDim, 512);

    // 2. Unload while unloaded is a safe no-op
    engine.unload();
    EXPECT_FALSE(engine.isLoaded());
    EXPECT_FALSE(engine.info().loaded);
    EXPECT_TRUE(engine.info().ready);

    fs::remove_all(tempDir, ec);
}

TEST(ClipServiceTest, StatusReflectsLazyLoadingAndUnload) {
#if !IMAGINE_FACE_ANALYSIS_BUILT
    GTEST_SKIP() << "ONNX Runtime not enabled in this build";
#endif
    std::error_code ec;
    auto testDb = fs::temp_directory_path() / ("test_clip_svc_lazy_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    auto tempCache = fs::temp_directory_path() / ("test_clip_cache_lazy_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto tempModels = fs::temp_directory_path() / ("test_clip_models_lazy_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tempCache, ec);
    fs::create_directories(tempModels, ec);

    // Write mock vocab, merges, manifest, and dummy models
    nlohmann::json mockVocab = {
        {"<|startoftext|>", 49406},
        {"<|endoftext|>", 49407}
    };
    std::ofstream vf(tempModels / "vocab.json");
    vf << mockVocab.dump();
    vf.close();

    std::ofstream mf(tempModels / "merges.txt");
    mf << "#version: 0.2\n";
    mf.close();

    nlohmann::json manifest = {
        {"model_id", "clip-service-test"},
        {"revision", "v2"},
        {"vector_dim", 512}
    };
    std::ofstream mnf(tempModels / "model_manifest.json");
    mnf << manifest.dump();
    mnf.close();

    std::ofstream imf(tempModels / "image_encoder.onnx");
    imf << "dummy";
    imf.close();

    std::ofstream tmf(tempModels / "text_encoder.onnx");
    tmf << "dummy";
    tmf.close();

    const char* prevEnv = std::getenv("IMAGINE_CLIP_MODEL_DIR");
    std::string prevModelDir = prevEnv ? prevEnv : "";
    setTestEnv("IMAGINE_CLIP_MODEL_DIR", tempModels.string());
    setTestEnv("IMAGINE_CLIP_IDLE_UNLOAD_SEC", "0");

    struct EnvGuard {
        std::string prevModel;
        ~EnvGuard() {
            setTestEnv("IMAGINE_CLIP_MODEL_DIR", prevModel);
            unsetTestEnv("IMAGINE_CLIP_IDLE_UNLOAD_SEC");
        }
    } guard{prevModelDir};

    {
        imagine::db::CatalogDb db;
        ASSERT_TRUE(db.open(testDb.string()).isOk());

        imagine::clip::Service svc(db, tempCache.string(), [](const std::string& p) { return p; });
        auto status = svc.status();
        EXPECT_TRUE(status["ready"].get<bool>());
        EXPECT_FALSE(status["loaded"].get<bool>());
        EXPECT_EQ(status["modelId"].get<std::string>(), "clip-service-test");
        EXPECT_EQ(status["modelRevision"].get<std::string>(), "v2");
        EXPECT_EQ(status["vectorDim"].get<int>(), 512);

        db.close();
    }

    fs::remove(testDb, ec);
    fs::remove(testDb.string() + "-wal", ec);
    fs::remove(testDb.string() + "-shm", ec);
    fs::remove_all(tempCache, ec);
    fs::remove_all(tempModels, ec);
}

TEST(ClipEngineTest, RealModelsSupportLazyLoadingAndExplicitUnloadCycle) {
    const char* modelDirectory = std::getenv("IMAGINE_CLIP_MODEL_DIR");
    if (!modelDirectory || !*modelDirectory) {
        GTEST_SKIP() << "Set IMAGINE_CLIP_MODEL_DIR to run the real CLIP lazy loading cycle";
    }
    if (IMAGINE_FACE_ANALYSIS_BUILT == 0) {
        GTEST_SKIP() << "This build does not include ONNX Runtime";
    }

    imagine::clip::ClipConfig config;
    config.modelDirectory = modelDirectory;
    config.device = "cpu";
    config.cpuThreads = 1;
    imagine::clip::Engine engine(config);

    // 1. Lazy initialize
    auto initStatus = engine.initialize(false);
    ASSERT_TRUE(initStatus.isOk()) << initStatus.message();
    EXPECT_TRUE(engine.info().ready);
    EXPECT_FALSE(engine.info().loaded);
    EXPECT_FALSE(engine.isLoaded());

    // 2. Explicit load
    auto loadStatus = engine.load();
    ASSERT_TRUE(loadStatus.isOk()) << loadStatus.message();
    EXPECT_TRUE(engine.isLoaded());
    EXPECT_TRUE(engine.info().loaded);

    // 3. Explicit unload
    engine.unload();
    EXPECT_FALSE(engine.isLoaded());
    EXPECT_FALSE(engine.info().loaded);
    EXPECT_TRUE(engine.info().ready);

    // 4. On-demand reload via encodeText
    auto textRes = engine.encodeText("test query");
    ASSERT_TRUE(textRes.isOk()) << textRes.status().message();
    EXPECT_EQ(textRes.value().size(), static_cast<size_t>(engine.info().vectorDim));
    EXPECT_TRUE(engine.isLoaded());
    EXPECT_TRUE(engine.info().loaded);

    // 5. Unload again
    engine.unload();
    EXPECT_FALSE(engine.isLoaded());
    EXPECT_FALSE(engine.info().loaded);
}

TEST(ClipServiceTest, RealModelsSearchPerformsOnDemandLoadAndUnload) {
    const char* modelDirectory = std::getenv("IMAGINE_CLIP_MODEL_DIR");
    if (!modelDirectory || !*modelDirectory) {
        GTEST_SKIP() << "Set IMAGINE_CLIP_MODEL_DIR to run the real CLIP search cycle";
    }
    if (IMAGINE_FACE_ANALYSIS_BUILT == 0) {
        GTEST_SKIP() << "This build does not include ONNX Runtime";
    }

    setTestEnv("IMAGINE_CLIP_IDLE_UNLOAD_SEC", "0");
    struct EnvGuard {
        ~EnvGuard() {
            unsetTestEnv("IMAGINE_CLIP_IDLE_UNLOAD_SEC");
        }
    } guard;

    std::error_code ec;
    auto testDb = fs::temp_directory_path() / ("test_clip_real_svc_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    auto tempCache = fs::temp_directory_path() / ("test_clip_real_cache_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tempCache, ec);

    {
        imagine::db::CatalogDb db;
        ASSERT_TRUE(db.open(testDb.string()).isOk());

        imagine::clip::Service svc(db, tempCache.string(), [](const std::string& p) { return p; });

        // 1. Not loaded initially
        auto statusBefore = svc.status();
        EXPECT_TRUE(statusBefore["ready"].get<bool>());
        EXPECT_FALSE(statusBefore["loaded"].get<bool>());

        // 2. Search query loads on demand
        imagine::clip::SearchRequest sreq;
        sreq.query = "a photo of a cat";
        auto searchRes = svc.search(sreq);
        ASSERT_TRUE(searchRes.isOk()) << searchRes.status().message();

        // 3. Since IMAGINE_CLIP_IDLE_UNLOAD_SEC=0, unloaded immediately after search
        auto statusAfter = svc.status();
        EXPECT_FALSE(statusAfter["loaded"].get<bool>());

        db.close();
    }

    fs::remove(testDb, ec);
    fs::remove(testDb.string() + "-wal", ec);
    fs::remove(testDb.string() + "-shm", ec);
    fs::remove_all(tempCache, ec);
}

TEST(ClipServiceTest, IndexingSkipsAlreadyIndexedUnlessForce) {
    const char* modelDirectory = std::getenv("IMAGINE_CLIP_MODEL_DIR");
    if (!modelDirectory || !*modelDirectory) {
        GTEST_SKIP() << "Set IMAGINE_CLIP_MODEL_DIR to run the real CLIP indexing cycle";
    }
    if (IMAGINE_FACE_ANALYSIS_BUILT == 0) {
        GTEST_SKIP() << "This build does not include ONNX Runtime";
    }

    std::error_code ec;
    auto testDb = fs::temp_directory_path() / ("test_clip_idx_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    auto tempCache = fs::temp_directory_path() / ("test_clip_idx_cache_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto tempMedia = fs::temp_directory_path() / ("test_clip_idx_media_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tempCache, ec);
    fs::create_directories(tempMedia, ec);

    {
        imagine::db::CatalogDb db;
        ASSERT_TRUE(db.open(testDb.string()).isOk());

        // Create 2 simple test images on disk
        imagine::thumbnail::ImageBuffer img1;
        img1.width = 64; img1.height = 64; img1.channels = 3;
        img1.data.assign(static_cast<size_t>(img1.width * img1.height * 3), 100);
        std::string p1 = (tempMedia / "photo1.jpg").string();
        ASSERT_TRUE(imagine::thumbnail::Generator::saveJpeg(img1, p1).isOk());

        imagine::thumbnail::ImageBuffer img2;
        img2.width = 64; img2.height = 64; img2.channels = 3;
        img2.data.assign(static_cast<size_t>(img2.width * img2.height * 3), 200);
        std::string p2 = (tempMedia / "photo2.jpg").string();
        ASSERT_TRUE(imagine::thumbnail::Generator::saveJpeg(img2, p2).isOk());

        auto addMedia = [&](const std::string& path) -> imagine::MediaId {
            imagine::MediaItem item;
            item.file_path = path;
            item.file_name = fs::path(path).filename().string();
            item.content_hash = imagine::metadata::Hasher::computeFileSha256(path).value();
            item.width = 64; item.height = 64;
            item.media_type = "photo";
            return db.insertMedia(item).value();
        };

        imagine::MediaId id1 = addMedia(p1);
        imagine::MediaId id2 = addMedia(p2);

        imagine::clip::Service svc(db, tempCache.string(), [](const std::string& p) { return p; });

        auto waitForJob = [&](int64_t jobId) -> imagine::Result<nlohmann::json> {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (std::chrono::steady_clock::now() < deadline) {
                auto res = svc.getJob(jobId);
                if (res.isOk() && res.value()["state"] != "running" && res.value()["state"] != "cancelling") {
                    return res;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            return svc.getJob(jobId);
        };

        // 1. Initial indexing of photo 1 (force = false)
        int64_t job1 = 0;
        ASSERT_TRUE(svc.startJob("selected", {id1}, false, job1).isOk());
        auto r1 = waitForJob(job1);
        ASSERT_TRUE(r1.isOk());
        EXPECT_EQ(r1.value()["state"], "completed");
        EXPECT_EQ(r1.value()["total"], 1);
        EXPECT_EQ(r1.value()["processed"], 1);
        EXPECT_EQ(r1.value()["skipped"], 0);
        EXPECT_EQ(r1.value()["failed"], 0);

        int64_t t1 = 0;
        std::vector<uint8_t> blob1;
        {
            auto stmtRes = db.connection().prepare("SELECT indexed_at, embedding FROM semantic_index_state WHERE media_id = ?;");
            ASSERT_TRUE(stmtRes.isOk());
            auto stmt = std::move(stmtRes.value());
            stmt.bind(1, id1);
            ASSERT_EQ(stmt.step(), imagine::db::StepResult::Row);
            t1 = stmt.getInt64(0);
            int bytes = sqlite3_column_bytes(stmt.raw(), 1);
            const uint8_t* ptr = static_cast<const uint8_t*>(sqlite3_column_blob(stmt.raw(), 1));
            blob1.assign(ptr, ptr + bytes);
            EXPECT_GT(bytes, 0);
        }

        // Wait at least 1s so timestamp changes if rewritten
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));

        // 2. Index both photo 1 and photo 2 with force = false (Reindex all photos NOT checked)
        // Photo 1 should be skipped, photo 2 processed.
        int64_t job2 = 0;
        ASSERT_TRUE(svc.startJob("selected", {id1, id2}, false, job2).isOk());
        auto r2 = waitForJob(job2);
        ASSERT_TRUE(r2.isOk());
        EXPECT_EQ(r2.value()["state"], "completed");
        EXPECT_EQ(r2.value()["total"], 2);
        EXPECT_EQ(r2.value()["processed"], 1);
        EXPECT_EQ(r2.value()["skipped"], 1);
        EXPECT_EQ(r2.value()["failed"], 0);

        // Verify photo 1's embedding and timestamp were completely untouched (not overwritten)
        {
            auto stmtRes = db.connection().prepare("SELECT indexed_at, embedding FROM semantic_index_state WHERE media_id = ?;");
            ASSERT_TRUE(stmtRes.isOk());
            auto stmt = std::move(stmtRes.value());
            stmt.bind(1, id1);
            ASSERT_EQ(stmt.step(), imagine::db::StepResult::Row);
            EXPECT_EQ(stmt.getInt64(0), t1);
            int bytes = sqlite3_column_bytes(stmt.raw(), 1);
            const uint8_t* ptr = static_cast<const uint8_t*>(sqlite3_column_blob(stmt.raw(), 1));
            std::vector<uint8_t> currentBlob(ptr, ptr + bytes);
            EXPECT_EQ(currentBlob, blob1);
        }

        // 3. Catalog scan with force = false: everything is already indexed, so total == 0
        int64_t job3 = 0;
        ASSERT_TRUE(svc.startJob("catalog", {}, false, job3).isOk());
        auto r3 = waitForJob(job3);
        ASSERT_TRUE(r3.isOk());
        EXPECT_EQ(r3.value()["state"], "completed");
        EXPECT_EQ(r3.value()["total"], 0);
        EXPECT_EQ(r3.value()["processed"], 0);
        EXPECT_EQ(r3.value()["skipped"], 0);

        // 4. Force reindex photo 1 with force = true ('Reindex all photos' checked)
        int64_t job4 = 0;
        ASSERT_TRUE(svc.startJob("selected", {id1}, true, job4).isOk());
        auto r4 = waitForJob(job4);
        ASSERT_TRUE(r4.isOk());
        EXPECT_EQ(r4.value()["state"], "completed");
        EXPECT_EQ(r4.value()["total"], 1);
        EXPECT_EQ(r4.value()["processed"], 1);
        EXPECT_EQ(r4.value()["skipped"], 0);

        // Verify photo 1's embedding timestamp was updated (overwritten)
        {
            auto stmtRes = db.connection().prepare("SELECT indexed_at FROM semantic_index_state WHERE media_id = ?;");
            ASSERT_TRUE(stmtRes.isOk());
            auto stmt = std::move(stmtRes.value());
            stmt.bind(1, id1);
            ASSERT_EQ(stmt.step(), imagine::db::StepResult::Row);
            EXPECT_GT(stmt.getInt64(0), t1);
        }

        // 5. Force catalog scan with force = true: all photos reindexed
        int64_t job5 = 0;
        ASSERT_TRUE(svc.startJob("catalog", {}, true, job5).isOk());
        auto r5 = waitForJob(job5);
        ASSERT_TRUE(r5.isOk());
        EXPECT_EQ(r5.value()["state"], "completed");
        EXPECT_EQ(r5.value()["total"], 2);
        EXPECT_EQ(r5.value()["processed"], 2);
        EXPECT_EQ(r5.value()["skipped"], 0);

        db.close();
    }

    fs::remove(testDb, ec);
    fs::remove(testDb.string() + "-wal", ec);
    fs::remove(testDb.string() + "-shm", ec);
    fs::remove_all(tempCache, ec);
    fs::remove_all(tempMedia, ec);
}

TEST(ClipTokenizerTest, MultiByteUtf8AndCrlf) {
    std::error_code ec;
    auto tempDir = fs::temp_directory_path() / ("test_clip_tok_utf8_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tempDir, ec);

    auto vocabPath = tempDir / "vocab.json";
    auto mergesPath = tempDir / "merges.txt";

    nlohmann::json mockVocab = {
        {"<|startoftext|>", 49406},
        {"<|endoftext|>", 49407},
        {"cafÃ©</w>", 200},
        {"photo</w>", 201},
        {"!</w>", 202}
    };
    std::ofstream vf(vocabPath);
    vf << mockVocab.dump();
    vf.close();

    std::ofstream mf(mergesPath, std::ios::binary);
    mf << "#version: 0.2\r\n";
    mf << "c a\r\n";
    mf << "ca f\r\n";
    mf << "Ã ©</w>\r\n";
    mf << "caf Ã©</w>\r\n";
    mf << "p h\r\n";
    mf << "ph o\r\n";
    mf << "pho t\r\n";
    mf << "phot o</w>\r\n";
    mf.close();

    imagine::clip::Tokenizer tok;
    auto loadStatus = tok.load(vocabPath.string(), mergesPath.string());
    ASSERT_TRUE(loadStatus.isOk()) << loadStatus.message();
    EXPECT_TRUE(tok.isLoaded());

    auto encoded = tok.encode("Café photo!", 77);
    EXPECT_EQ(encoded.input_ids.size(), 77u);
    EXPECT_EQ(encoded.input_ids[0], 49406); // SOT
    EXPECT_EQ(encoded.input_ids[1], 200);   // café</w>
    EXPECT_EQ(encoded.input_ids[2], 201);   // photo</w>
    EXPECT_EQ(encoded.input_ids[3], 202);   // !</w>
    EXPECT_EQ(encoded.input_ids[4], 49407); // EOT

    fs::remove_all(tempDir, ec);
}

TEST(ClipTokenizerTest, ReloadClearsState) {
    std::error_code ec;
    auto tempDir = fs::temp_directory_path() / ("test_clip_tok_reload_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tempDir, ec);

    auto vocabPath = tempDir / "vocab.json";
    auto mergesPath = tempDir / "merges.txt";

    nlohmann::json mockVocab1 = {{"<|startoftext|>", 1}, {"<|endoftext|>", 2}, {"a</w>", 10}};
    std::ofstream vf(vocabPath); vf << mockVocab1.dump(); vf.close();
    std::ofstream mf(mergesPath); mf << "#version: 0.2\n"; mf.close();

    imagine::clip::Tokenizer tok;
    ASSERT_TRUE(tok.load(vocabPath.string(), mergesPath.string()).isOk());
    EXPECT_EQ(tok.encode("a", 10).input_ids[1], 10);

    nlohmann::json mockVocab2 = {{"<|startoftext|>", 1}, {"<|endoftext|>", 2}, {"b</w>", 20}};
    std::ofstream vf2(vocabPath); vf2 << mockVocab2.dump(); vf2.close();
    ASSERT_TRUE(tok.load(vocabPath.string(), mergesPath.string()).isOk());
    auto enc = tok.encode("a", 10);
    EXPECT_NE(enc.input_ids[1], 10);
    EXPECT_EQ(tok.encode("b", 10).input_ids[1], 20);

    fs::remove_all(tempDir, ec);
}

TEST(ClipEngineTest, EncodeImageValidatesBuffer) {
    imagine::clip::ClipConfig config;
    config.modelDirectory = "nonexistent";
    imagine::clip::Engine engine(config);

    imagine::thumbnail::ImageBuffer emptyBuf;
    EXPECT_FALSE(engine.encodeImage(emptyBuf).isOk());

    imagine::thumbnail::ImageBuffer zeroW;
    zeroW.width = 0; zeroW.height = 10; zeroW.channels = 3;
    EXPECT_FALSE(engine.encodeImage(zeroW).isOk());

    imagine::thumbnail::ImageBuffer negH;
    negH.width = 10; negH.height = -5; negH.channels = 3;
    EXPECT_FALSE(engine.encodeImage(negH).isOk());

    imagine::thumbnail::ImageBuffer trunc;
    trunc.width = 10; trunc.height = 10; trunc.channels = 3;
    trunc.data.resize(5);
    EXPECT_FALSE(engine.encodeImage(trunc).isOk());
}

TEST(ClipServiceTest, RemoveMediaAndFindSimilarExcludesDeleted) {
#if !IMAGINE_FACE_ANALYSIS_BUILT
    GTEST_SKIP() << "Semantic search requires ONNX Runtime (IMAGINE_FACE_ANALYSIS_BUILT)";
#endif
    std::error_code ec;
    auto testDb = fs::temp_directory_path() / ("test_clip_del_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    auto tempCache = fs::temp_directory_path() / ("test_clip_del_cache_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto tempModels = fs::temp_directory_path() / ("test_clip_del_models_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tempCache, ec);
    fs::create_directories(tempModels, ec);

    nlohmann::json mockVocab = {{"<|startoftext|>", 49406}, {"<|endoftext|>", 49407}};
    std::ofstream vf(tempModels / "vocab.json"); vf << mockVocab.dump(); vf.close();
    std::ofstream mf(tempModels / "merges.txt"); mf << "#version: 0.2\n"; mf.close();
    nlohmann::json manifest = {{"model_id", "clip-del-test"}, {"revision", "v1"}, {"vector_dim", 4}};
    std::ofstream mnf(tempModels / "model_manifest.json"); mnf << manifest.dump(); mnf.close();
    std::ofstream(tempModels / "image_encoder.onnx") << "dummy";
    std::ofstream(tempModels / "text_encoder.onnx") << "dummy";

    const char* prevEnv = std::getenv("IMAGINE_CLIP_MODEL_DIR");
    std::string prevModelDir = prevEnv ? prevEnv : "";
    setTestEnv("IMAGINE_CLIP_MODEL_DIR", tempModels.string());
    struct EnvGuard {
        std::string prev;
        ~EnvGuard() { setTestEnv("IMAGINE_CLIP_MODEL_DIR", prev); }
    } guard{prevModelDir};

    {
        imagine::db::CatalogDb db;
        ASSERT_TRUE(db.open(testDb.string()).isOk());

        imagine::MediaItem m1; m1.file_path = "p1.jpg"; m1.file_name = "p1.jpg"; m1.content_hash = "h1"; m1.media_type = "photo";
        imagine::MediaItem m2; m2.file_path = "p2.jpg"; m2.file_name = "p2.jpg"; m2.content_hash = "h2"; m2.media_type = "photo";
        imagine::MediaId id1 = db.insertMedia(m1).value();
        imagine::MediaId id2 = db.insertMedia(m2).value();

        std::vector<float> v1 = {1.0f, 0.0f, 0.0f, 0.0f};
        std::vector<float> v2 = {0.9f, 0.1f, 0.0f, 0.0f};
        auto insertState = [&](imagine::MediaId mid, const std::string& h, const std::vector<float>& vec) {
            auto stmtRes = db.connection().prepare(
                "INSERT INTO semantic_index_state (media_id, model_id, model_revision, preprocess_id, vector_dim, embedding, content_hash, indexed_at) "
                "VALUES (?, 'clip-del-test', 'v1', '', 4, ?, ?, 100);"
            );
            ASSERT_TRUE(stmtRes.isOk());
            auto stmt = std::move(stmtRes.value());
            stmt.bind(1, mid);
            sqlite3_bind_blob(stmt.raw(), 2, vec.data(), static_cast<int>(vec.size() * sizeof(float)), SQLITE_TRANSIENT);
            stmt.bind(3, h);
            stmt.step();
        };
        insertState(id1, "h1", v1);
        insertState(id2, "h2", v2);

        imagine::clip::Service svc(db, tempCache.string(), [](const std::string& p) { return p; });
        EXPECT_EQ(svc.status()["indexSize"].get<int>(), 2);

        auto sim1 = svc.findSimilar(id1, 10);
        ASSERT_TRUE(sim1.isOk());
        EXPECT_EQ(sim1.value().items.size(), 1u);
        EXPECT_EQ(sim1.value().items[0].mediaId, id2);

        ASSERT_TRUE(db.deleteMedia(id2).isOk());
        svc.removeMedia(id2);
        EXPECT_EQ(svc.status()["indexSize"].get<int>(), 1);

        auto sim2 = svc.findSimilar(id1, 10);
        ASSERT_TRUE(sim2.isOk());
        EXPECT_EQ(sim2.value().items.size(), 0u);

        db.close();
    }

    fs::remove(testDb, ec);
    fs::remove(testDb.string() + "-wal", ec);
    fs::remove(testDb.string() + "-shm", ec);
    fs::remove_all(tempCache, ec);
    fs::remove_all(tempModels, ec);
}

TEST(ClipTokenizerTest, EncodeClampMinLength) {
    std::error_code ec;
    auto tempDir = fs::temp_directory_path() / ("test_clip_tok_clamp_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tempDir, ec);

    auto vocabPath = tempDir / "vocab.json";
    auto mergesPath = tempDir / "merges.txt";

    nlohmann::json mockVocab = {
        {"<|startoftext|>", 49406},
        {"<|endoftext|>", 49407},
        {"test</w>", 100}
    };
    std::ofstream vf(vocabPath);
    vf << mockVocab.dump();
    vf.close();

    std::ofstream mf(mergesPath);
    mf << "#version: 0.2\n";
    mf.close();

    imagine::clip::Tokenizer tok;
    ASSERT_TRUE(tok.load(vocabPath.string(), mergesPath.string()).isOk());

    for (int len : {0, 1, -5}) {
        auto out = tok.encode("test", len);
        EXPECT_EQ(out.input_ids.size(), 2u);
        EXPECT_EQ(out.attention_mask.size(), 2u);
        EXPECT_EQ(out.input_ids[0], 49406); // SOT
        EXPECT_EQ(out.input_ids[1], 49407); // EOT
        EXPECT_EQ(out.attention_mask[0], 1);
        EXPECT_EQ(out.attention_mask[1], 1);
    }

    fs::remove_all(tempDir, ec);
}

TEST(ClipEngineTest, EncodeImageValidatesChannelsAndOverflow) {
    imagine::clip::ClipConfig config;
    config.modelDirectory = "nonexistent";
    imagine::clip::Engine engine(config);

    // Channels 5 is invalid
    imagine::thumbnail::ImageBuffer chan5;
    chan5.width = 10; chan5.height = 10; chan5.channels = 5;
    chan5.data.resize(10 * 10 * 5, 128);
    auto res5 = engine.encodeImage(chan5);
    EXPECT_FALSE(res5.isOk());
    EXPECT_EQ(res5.status().code(), imagine::StatusCode::InvalidArgument);

    // Channels 0 is invalid
    imagine::thumbnail::ImageBuffer chan0;
    chan0.width = 10; chan0.height = 10; chan0.channels = 0;
    auto res0 = engine.encodeImage(chan0);
    EXPECT_FALSE(res0.isOk());
    EXPECT_EQ(res0.status().code(), imagine::StatusCode::InvalidArgument);

    // Overflow dimensions
    imagine::thumbnail::ImageBuffer overflowBuf;
    overflowBuf.width = 2000000;
    overflowBuf.height = 2000000;
    overflowBuf.channels = 3;
    overflowBuf.data.resize(10);
    auto resOvf = engine.encodeImage(overflowBuf);
    EXPECT_FALSE(resOvf.isOk());
    EXPECT_EQ(resOvf.status().code(), imagine::StatusCode::InvalidArgument);

    // Channels 2 (grayscale + alpha) passes validation (fails with engine not ready, not invalidArgument)
    imagine::thumbnail::ImageBuffer chan2;
    chan2.width = 10; chan2.height = 10; chan2.channels = 2;
    chan2.data.resize(10 * 10 * 2, 128);
    auto res2 = engine.encodeImage(chan2);
    EXPECT_FALSE(res2.isOk());
    EXPECT_NE(res2.status().code(), imagine::StatusCode::InvalidArgument);
}

TEST(ClipServiceTest, StartJobWhenAlreadyRunningReturnsAlreadyExists) {
    std::error_code ec;
    auto testDb = fs::temp_directory_path() / ("test_clip_running_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    auto tempCache = fs::temp_directory_path() / ("test_clip_running_cache_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto tempModels = fs::temp_directory_path() / ("test_clip_running_models_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tempCache, ec);
    fs::create_directories(tempModels, ec);

    nlohmann::json mockVocab = {{"<|startoftext|>", 49406}, {"<|endoftext|>", 49407}};
    std::ofstream(tempModels / "vocab.json") << mockVocab.dump();
    std::ofstream(tempModels / "merges.txt") << "#version: 0.2\n";
    nlohmann::json manifest = {{"model_id", "clip-running-test"}, {"revision", "v1"}, {"vector_dim", 4}};
    std::ofstream(tempModels / "model_manifest.json") << manifest.dump();
    std::ofstream(tempModels / "image_encoder.onnx") << "dummy";
    std::ofstream(tempModels / "text_encoder.onnx") << "dummy";

    const char* prevEnv = std::getenv("IMAGINE_CLIP_MODEL_DIR");
    std::string prevModelDir = prevEnv ? prevEnv : "";
    setTestEnv("IMAGINE_CLIP_MODEL_DIR", tempModels.string());
    struct EnvGuard {
        std::string prev;
        ~EnvGuard() { setTestEnv("IMAGINE_CLIP_MODEL_DIR", prev); }
    } guard{prevModelDir};

    {
        imagine::db::CatalogDb db;
        ASSERT_TRUE(db.open(testDb.string()).isOk());

        imagine::clip::Service svc(db, tempCache.string(), [](const std::string& p) { return p; });

        // Insert a simulated active job after service startup
        {
            auto insRes = db.connection().prepare(
                "INSERT INTO semantic_jobs (scope, state, model_id, total, processed, skipped, failed, remaining, created_at, updated_at) "
                "VALUES ('catalog', 'running', 'clip-running-test', 10, 0, 0, 0, 10, 100, 100);"
            );
            ASSERT_TRUE(insRes.isOk());
            auto ins = std::move(insRes.value());
            ins.step();
        }

        int64_t newJobId = 0;
        auto st = svc.startJob("catalog", {}, false, newJobId);
        EXPECT_FALSE(st.isOk());
        EXPECT_EQ(st.code(), imagine::StatusCode::AlreadyExists);

        // Verify the existing job in the database was NOT cancelled or altered
        {
            auto selRes = db.connection().prepare("SELECT state FROM semantic_jobs WHERE id=1;");
            ASSERT_TRUE(selRes.isOk());
            auto sel = std::move(selRes.value());
            ASSERT_EQ(sel.step(), imagine::db::StepResult::Row);
            EXPECT_EQ(sel.getString(0), "running");
        }

        db.close();
    }

    fs::remove(testDb, ec);
    fs::remove(testDb.string() + "-wal", ec);
    fs::remove(testDb.string() + "-shm", ec);
    fs::remove_all(tempCache, ec);
    fs::remove_all(tempModels, ec);
}

TEST(ClipServiceTest, SearchAndFindSimilarClampingAndDateFilters) {
#if !IMAGINE_FACE_ANALYSIS_BUILT
    GTEST_SKIP() << "Semantic search requires ONNX Runtime (IMAGINE_FACE_ANALYSIS_BUILT)";
#endif
    std::error_code ec;
    auto testDb = fs::temp_directory_path() / ("test_clip_clamp_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    auto tempCache = fs::temp_directory_path() / ("test_clip_clamp_cache_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto tempModels = fs::temp_directory_path() / ("test_clip_clamp_models_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tempCache, ec);
    fs::create_directories(tempModels, ec);

    nlohmann::json mockVocab = {{"<|startoftext|>", 49406}, {"<|endoftext|>", 49407}};
    std::ofstream(tempModels / "vocab.json") << mockVocab.dump();
    std::ofstream(tempModels / "merges.txt") << "#version: 0.2\n";
    nlohmann::json manifest = {{"model_id", "clip-clamp-test"}, {"revision", "v1"}, {"vector_dim", 4}};
    std::ofstream(tempModels / "model_manifest.json") << manifest.dump();
    std::ofstream(tempModels / "image_encoder.onnx") << "dummy";
    std::ofstream(tempModels / "text_encoder.onnx") << "dummy";

    const char* prevEnv = std::getenv("IMAGINE_CLIP_MODEL_DIR");
    std::string prevModelDir = prevEnv ? prevEnv : "";
    setTestEnv("IMAGINE_CLIP_MODEL_DIR", tempModels.string());
    struct EnvGuard {
        std::string prev;
        ~EnvGuard() { setTestEnv("IMAGINE_CLIP_MODEL_DIR", prev); }
    } guard{prevModelDir};

    {
        imagine::db::CatalogDb db;
        ASSERT_TRUE(db.open(testDb.string()).isOk());

        imagine::MediaItem m1;
        m1.file_path = "vintage.jpg"; m1.file_name = "vintage.jpg"; m1.content_hash = "hv"; m1.media_type = "photo";
        m1.date_taken = -100000; // Pre-1970
        imagine::MediaItem m2;
        m2.file_path = "modern.jpg"; m2.file_name = "modern.jpg"; m2.content_hash = "hm"; m2.media_type = "photo";
        m2.date_taken = 100000; // Post-1970
        imagine::MediaItem m3;
        m3.file_path = "nodate.jpg"; m3.file_name = "nodate.jpg"; m3.content_hash = "hn"; m3.media_type = "photo";
        m3.date_taken = 0; // Unset date

        imagine::MediaId id1 = db.insertMedia(m1).value();
        imagine::MediaId id2 = db.insertMedia(m2).value();
        imagine::MediaId id3 = db.insertMedia(m3).value();

        std::vector<float> v1 = {1.0f, 0.0f, 0.0f, 0.0f};
        std::vector<float> v2 = {0.8f, 0.2f, 0.0f, 0.0f};
        std::vector<float> v3 = {0.5f, 0.5f, 0.0f, 0.0f};

        auto insertState = [&](imagine::MediaId mid, const std::string& h, const std::vector<float>& vec) {
            auto stmtRes = db.connection().prepare(
                "INSERT INTO semantic_index_state (media_id, model_id, model_revision, preprocess_id, vector_dim, embedding, content_hash, indexed_at) "
                "VALUES (?, 'clip-clamp-test', 'v1', '', 4, ?, ?, 100);"
            );
            ASSERT_TRUE(stmtRes.isOk());
            auto stmt = std::move(stmtRes.value());
            stmt.bind(1, mid);
            sqlite3_bind_blob(stmt.raw(), 2, vec.data(), static_cast<int>(vec.size() * sizeof(float)), SQLITE_TRANSIENT);
            stmt.bind(3, h);
            stmt.step();
        };
        insertState(id1, "hv", v1);
        insertState(id2, "hm", v2);
        insertState(id3, "hn", v3);

        imagine::clip::Service svc(db, tempCache.string(), [](const std::string& p) { return p; });
        EXPECT_EQ(svc.status()["indexSize"].get<int>(), 3);

        // Negative limit on findSimilar must not throw length_error
        auto simNeg = svc.findSimilar(id1, -5);
        ASSERT_TRUE(simNeg.isOk());
        EXPECT_LE(simNeg.value().items.size(), 1u);

        // findSimilar with limit = 0 clamped to 1
        auto simZero = svc.findSimilar(id1, 0);
        ASSERT_TRUE(simZero.isOk());
        EXPECT_LE(simZero.value().items.size(), 1u);

        db.close();
    }

    fs::remove(testDb, ec);
    fs::remove(testDb.string() + "-wal", ec);
    fs::remove(testDb.string() + "-shm", ec);
    fs::remove_all(tempCache, ec);
    fs::remove_all(tempModels, ec);
}

TEST(ClipServiceTest, VisualDuplicateScanHelperAndUnionFindGrouping) {
    std::error_code ec;
    auto testDb = fs::temp_directory_path() / ("test_clip_dedup_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    auto tempCache = fs::temp_directory_path() / ("test_clip_dedup_cache_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tempCache, ec);

    {
        imagine::db::CatalogDb db;
        ASSERT_TRUE(db.open(testDb.string()).isOk());

        imagine::MediaItem m1;
        m1.file_path = "photo1.jpg"; m1.file_name = "photo1.jpg"; m1.content_hash = "h1";
        m1.width = 1000; m1.height = 1000; m1.file_size = 500000; m1.date_taken = 1000;
        m1.media_type = "photo";

        imagine::MediaItem m2;
        m2.file_path = "photo2_copy.jpg"; m2.file_name = "photo2_copy.jpg"; m2.content_hash = "h2";
        m2.width = 800; m2.height = 800; m2.file_size = 400000; m2.date_taken = 2000;
        m2.media_type = "photo";

        imagine::MediaItem m3;
        m3.file_path = "photo3.jpg"; m3.file_name = "photo3.jpg"; m3.content_hash = "h3";
        m3.width = 1200; m3.height = 1200; m3.file_size = 600000; m3.date_taken = 5000;
        m3.media_type = "photo";

        imagine::MediaItem m4;
        m4.file_path = "photo4.jpg"; m4.file_name = "photo4.jpg"; m4.content_hash = "h4";
        m4.width = 1000; m4.height = 1000; m4.file_size = 500000; m4.date_taken = 6000;
        m4.media_type = "photo";

        imagine::MediaId id1 = db.insertMedia(m1).value();
        imagine::MediaId id2 = db.insertMedia(m2).value();
        imagine::MediaId id3 = db.insertMedia(m3).value();
        imagine::MediaId id4 = db.insertMedia(m4).value();

        imagine::clip::Service svc(db, tempCache.string(), [](const std::string& p) { return p; });

        std::vector<float> v1 = {1.0f, 0.0f, 0.0f, 0.0f};
        std::vector<float> v2 = {0.98f, 0.198997f, 0.0f, 0.0f};
        std::vector<float> v3 = {0.0f, 1.0f, 0.0f, 0.0f};
        std::vector<float> v4 = {0.0f, 0.99f, 0.141067f, 0.0f};

        svc.addMediaVector(id1, v1);
        svc.addMediaVector(id2, v2);
        svc.addMediaVector(id3, v3);
        svc.addMediaVector(id4, v4);

        // 1. findVisualDuplicatePairs helper
        auto pairs = svc.findVisualDuplicatePairs(0.97f);
        ASSERT_EQ(pairs.size(), 2u);
        EXPECT_EQ(pairs[0].a, id1);
        EXPECT_EQ(pairs[0].b, id2);
        EXPECT_NEAR(pairs[0].similarity, 0.98f, 0.01f);
        EXPECT_EQ(pairs[1].a, id3);
        EXPECT_EQ(pairs[1].b, id4);
        EXPECT_NEAR(pairs[1].similarity, 0.99f, 0.01f);

        // Filter with allowedIds
        auto filteredPairs = svc.findVisualDuplicatePairs(0.97f, {id1, id3});
        EXPECT_TRUE(filteredPairs.empty());

        // 2. scanVisualDuplicates grouping with keeper ranking
        auto groups = svc.scanVisualDuplicates(0.97f, 86400);
        ASSERT_EQ(groups.size(), 2u);
        bool foundGroup1 = false;
        bool foundGroup2 = false;
        for (const auto& g : groups) {
            ASSERT_EQ(g.memberIds.size(), 2u);
            if (g.memberIds[0] == id1 || g.memberIds[1] == id1) {
                foundGroup1 = true;
                EXPECT_EQ(g.memberIds[0], id1);
                EXPECT_EQ(g.memberIds[1], id2);
                EXPECT_NEAR(g.minSimilarity, 0.98f, 0.01f);
            } else if (g.memberIds[0] == id3 || g.memberIds[1] == id3) {
                foundGroup2 = true;
                EXPECT_EQ(g.memberIds[0], id3);
                EXPECT_EQ(g.memberIds[1], id4);
                EXPECT_NEAR(g.minSimilarity, 0.99f, 0.01f);
            }
        }
        EXPECT_TRUE(foundGroup1);
        EXPECT_TRUE(foundGroup2);

        // 3. Dismissal exclusion: dismiss (id1, id2)
        {
            auto stmtRes = db.connection().prepare(
                "INSERT INTO duplicate_dismissals (media_id_a, media_id_b, dismissed_at) VALUES (?, ?, ?);"
            );
            ASSERT_TRUE(stmtRes.isOk());
            auto stmt = std::move(stmtRes.value());
            stmt.bind(1, std::min(id1, id2));
            stmt.bind(2, std::max(id1, id2));
            stmt.bind(3, 1000);
            stmt.step();
        }

        auto groupsAfterDismissal = svc.scanVisualDuplicates(0.97f, 86400);
        ASSERT_EQ(groupsAfterDismissal.size(), 1u);
        EXPECT_EQ(groupsAfterDismissal[0].memberIds[0], id3);

        // 4. Exact hash member exclusion
        auto groupsWithExactExclude = svc.scanVisualDuplicates(0.97f, 86400, {}, {id3});
        EXPECT_TRUE(groupsWithExactExclude.empty());

        // 5. Date window check: update id4 date_taken to far future
        ASSERT_TRUE(db.updateDateTaken(id4, 200000, "").isOk());
        auto groupsDateWindow = svc.scanVisualDuplicates(0.97f, 86400);
        EXPECT_TRUE(groupsDateWindow.empty());

        // Zero date window disables date window check
        auto groupsNoWindow = svc.scanVisualDuplicates(0.97f, 0);
        ASSERT_EQ(groupsNoWindow.size(), 1u);
        EXPECT_EQ(groupsNoWindow[0].memberIds[0], id3);

        db.close();
    }

    fs::remove(testDb, ec);
    fs::remove(testDb.string() + "-wal", ec);
    fs::remove(testDb.string() + "-shm", ec);
    fs::remove_all(tempCache, ec);
}






TEST(SmartTagsTest, DefaultTaxonomyIsValidAndHasExpectedLabels) {
    const char* jsonStr = imagine::clip::getDefaultTaxonomyJson();
    ASSERT_NE(jsonStr, nullptr);
    EXPECT_GT(std::strlen(jsonStr), 0u);

    auto taxRes = imagine::clip::parseTaxonomyJson(jsonStr);
    ASSERT_TRUE(taxRes.isOk()) << taxRes.status().message();
    const auto& tax = taxRes.value();

    // Verify label count (75 labels)
    EXPECT_GE(tax.labels.size(), 70u);
    EXPECT_LE(tax.labels.size(), 85u);
    EXPECT_EQ(tax.labels.size(), 75u);

    // Verify version hash is 16 hex chars
    EXPECT_EQ(tax.version.size(), 16u);
    for (char c : tax.version) {
        EXPECT_TRUE(std::isxdigit(static_cast<unsigned char>(c)));
    }

    // Verify determinism of version hash
    auto taxRes2 = imagine::clip::parseTaxonomyJson(jsonStr);
    ASSERT_TRUE(taxRes2.isOk());
    EXPECT_EQ(tax.version, taxRes2.value().version);

    // Verify loadTaxonomy defaults match
    auto taxLoaded = imagine::clip::loadTaxonomy();
    ASSERT_TRUE(taxLoaded.isOk());
    EXPECT_EQ(tax.version, taxLoaded.value().version);
    EXPECT_EQ(tax.labels.size(), taxLoaded.value().labels.size());

    // Verify unique IDs and names
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> names;
    std::unordered_set<std::string> groups;

    for (const auto& label : tax.labels) {
        EXPECT_FALSE(label.id.empty());
        EXPECT_FALSE(label.name.empty());
        EXPECT_FALSE(label.group.empty());
        EXPECT_GT(label.threshold, 0.0f);
        EXPECT_LE(label.threshold, 1.0f);
        EXPECT_GE(label.prompts.size(), 2u);
        for (const auto& p : label.prompts) {
            EXPECT_FALSE(p.empty());
        }

        EXPECT_TRUE(ids.insert(label.id).second) << "Duplicate id: " << label.id;
        EXPECT_TRUE(names.insert(label.name).second) << "Duplicate name: " << label.name;
        groups.insert(label.group);
    }

    // Verify 9 groups
    EXPECT_EQ(groups.size(), 9u);
    EXPECT_TRUE(groups.count("Nature"));
    EXPECT_TRUE(groups.count("Animals"));
    EXPECT_TRUE(groups.count("Food & Drink"));
    EXPECT_TRUE(groups.count("People & Events"));
    EXPECT_TRUE(groups.count("Places & Travel"));
    EXPECT_TRUE(groups.count("Architecture & Landmarks"));
    EXPECT_TRUE(groups.count("Activities"));
    EXPECT_TRUE(groups.count("Objects & Home"));
    EXPECT_TRUE(groups.count("Mood & Weather"));

    // Verify presence of key labels
    const std::vector<std::string> expectedSlugs = {
        "beach", "mountains", "forest", "lake", "river", "ocean", "sunset", "sunrise", "snow", "flowers", "sky", "clouds",
        "dog", "cat", "bird", "horse", "wildlife",
        "food", "coffee", "wine", "cake",
        "wedding", "birthday", "concert", "portrait", "party",
        "city", "street", "airplane", "car", "bicycle", "boat", "train",
        "architecture", "bridge", "church", "cathedral", "castle", "fortress", "museum",
        "palace", "ruins", "temple", "mosque", "monument", "skyscraper", "tower", "lighthouse", "plaza", "library",
        "stained_glass", "colonnade",
        "camping", "hiking", "swimming", "skiing", "running",
        "book", "instrument", "art", "garden", "interior",
        "night", "autumn", "spring", "summer", "winter", "rain"
    };
    for (const auto& slug : expectedSlugs) {
        EXPECT_TRUE(ids.count(slug)) << "Missing expected label slug: " << slug;
    }
}

TEST(SmartTagsTest, InvalidTaxonomyJsonErrors) {
    // Malformed JSON syntax
    auto res1 = imagine::clip::parseTaxonomyJson("{ not valid json ");
    EXPECT_FALSE(res1.isOk());

    // Root not an object
    auto res2 = imagine::clip::parseTaxonomyJson("[1, 2, 3]");
    EXPECT_FALSE(res2.isOk());

    // Missing labels array
    auto res3 = imagine::clip::parseTaxonomyJson(R"({"version": 1})");
    EXPECT_FALSE(res3.isOk());

    // Empty labels array
    auto res4 = imagine::clip::parseTaxonomyJson(R"({"labels": []})");
    EXPECT_FALSE(res4.isOk());

    // Duplicate label ID
    auto res5 = imagine::clip::parseTaxonomyJson(R"({
        "labels": [
            {"id": "dog", "name": "Dog", "group": "Animals", "prompts": ["a dog"]},
            {"id": "dog", "name": "Puppy", "group": "Animals", "prompts": ["a puppy"]}
        ]
    })");
    EXPECT_FALSE(res5.isOk());

    // Duplicate label ID (case-insensitive)
    auto res6 = imagine::clip::parseTaxonomyJson(R"({
        "labels": [
            {"id": "dog", "name": "Dog", "group": "Animals", "prompts": ["a dog"]},
            {"id": "DOG", "name": "Puppy", "group": "Animals", "prompts": ["a puppy"]}
        ]
    })");
    EXPECT_FALSE(res6.isOk());

    // Duplicate label name
    auto res7 = imagine::clip::parseTaxonomyJson(R"({
        "labels": [
            {"id": "dog1", "name": "Dog", "group": "Animals", "prompts": ["a dog"]},
            {"id": "dog2", "name": "Dog", "group": "Animals", "prompts": ["a puppy"]}
        ]
    })");
    EXPECT_FALSE(res7.isOk());

    // Empty ID
    auto res8 = imagine::clip::parseTaxonomyJson(R"({
        "labels": [
            {"id": "", "name": "Dog", "group": "Animals", "prompts": ["a dog"]}
        ]
    })");
    EXPECT_FALSE(res8.isOk());

    // Empty Name
    auto res9 = imagine::clip::parseTaxonomyJson(R"({
        "labels": [
            {"id": "dog", "name": "", "group": "Animals", "prompts": ["a dog"]}
        ]
    })");
    EXPECT_FALSE(res9.isOk());

    // Empty Group
    auto res10 = imagine::clip::parseTaxonomyJson(R"({
        "labels": [
            {"id": "dog", "name": "Dog", "group": "  ", "prompts": ["a dog"]}
        ]
    })");
    EXPECT_FALSE(res10.isOk());

    // Threshold <= 0
    auto res11 = imagine::clip::parseTaxonomyJson(R"({
        "labels": [
            {"id": "dog", "name": "Dog", "group": "Animals", "threshold": 0.0, "prompts": ["a dog"]}
        ]
    })");
    EXPECT_FALSE(res11.isOk());

    // Threshold > 1
    auto res12 = imagine::clip::parseTaxonomyJson(R"({
        "labels": [
            {"id": "dog", "name": "Dog", "group": "Animals", "threshold": 1.05, "prompts": ["a dog"]}
        ]
    })");
    EXPECT_FALSE(res12.isOk());

    // Threshold not a number
    auto res13 = imagine::clip::parseTaxonomyJson(R"({
        "labels": [
            {"id": "dog", "name": "Dog", "group": "Animals", "threshold": "high", "prompts": ["a dog"]}
        ]
    })");
    EXPECT_FALSE(res13.isOk());

    // Missing prompts array
    auto res14 = imagine::clip::parseTaxonomyJson(R"({
        "labels": [
            {"id": "dog", "name": "Dog", "group": "Animals"}
        ]
    })");
    EXPECT_FALSE(res14.isOk());

    // Empty prompts array
    auto res15 = imagine::clip::parseTaxonomyJson(R"({
        "labels": [
            {"id": "dog", "name": "Dog", "group": "Animals", "prompts": []}
        ]
    })");
    EXPECT_FALSE(res15.isOk());

    // Empty prompt string
    auto res16 = imagine::clip::parseTaxonomyJson(R"({
        "labels": [
            {"id": "dog", "name": "Dog", "group": "Animals", "prompts": [""]}
        ]
    })");
    EXPECT_FALSE(res16.isOk());

    // Non-string prompt
    auto res17 = imagine::clip::parseTaxonomyJson(R"({
        "labels": [
            {"id": "dog", "name": "Dog", "group": "Animals", "prompts": [42]}
        ]
    })");
    EXPECT_FALSE(res17.isOk());
}

TEST(SmartTagsTest, LoadTaxonomyCustomPathAndEnv) {
    std::error_code ec;
    auto tempDir = fs::temp_directory_path() / ("test_taxonomy_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tempDir, ec);
    auto tempFile = tempDir / "custom_taxonomy.json";

    const char* customJson = R"({
        "labels": [
            {
                "id": "sunset_custom",
                "name": "Custom Sunset",
                "group": "Nature",
                "threshold": 0.28,
                "prompts": ["a stunning golden sunset", "evening red sky"]
            }
        ]
    })";
    std::ofstream(tempFile) << customJson;

    // Load from explicit custom path
    auto customRes = imagine::clip::loadTaxonomy(tempFile.string());
    ASSERT_TRUE(customRes.isOk());
    EXPECT_EQ(customRes.value().labels.size(), 1u);
    EXPECT_EQ(customRes.value().labels[0].id, "sunset_custom");
    EXPECT_FLOAT_EQ(customRes.value().labels[0].threshold, 0.28f);

    // Non-existent file fails
    auto failRes = imagine::clip::loadTaxonomy((tempDir / "does_not_exist.json").string());
    EXPECT_FALSE(failRes.isOk());

    // Load via IMAGINE_SMART_TAGS_TAXONOMY environment variable
    setTestEnv("IMAGINE_SMART_TAGS_TAXONOMY", tempFile.string());
    auto envRes = imagine::clip::loadTaxonomy();
    unsetTestEnv("IMAGINE_SMART_TAGS_TAXONOMY");
    ASSERT_TRUE(envRes.isOk());
    EXPECT_EQ(envRes.value().labels.size(), 1u);
    EXPECT_EQ(envRes.value().labels[0].id, "sunset_custom");

    // Clean up
    fs::remove_all(tempDir, ec);
}

TEST(SmartTagsTest, MeanPoolAndNormalize) {
    // Empty input returns empty
    EXPECT_TRUE(imagine::clip::meanPoolAndNormalize({}).empty());

    // Zero-length embeddings returns empty
    EXPECT_TRUE(imagine::clip::meanPoolAndNormalize({{}}).empty());

    // Single vector [3.0f, 4.0f] normalized to [0.6f, 0.8f]
    std::vector<std::vector<float>> v1 = {{3.0f, 4.0f}};
    auto r1 = imagine::clip::meanPoolAndNormalize(v1);
    ASSERT_EQ(r1.size(), 2u);
    EXPECT_NEAR(r1[0], 0.6f, 1e-6f);
    EXPECT_NEAR(r1[1], 0.8f, 1e-6f);
    EXPECT_NEAR(r1[0] * r1[0] + r1[1] * r1[1], 1.0f, 1e-6f);

    // Two orthogonal unit vectors [1, 0] and [0, 1]
    // Mean is [0.5, 0.5], normalized is [1/sqrt(2), 1/sqrt(2)] ~ 0.70710678
    std::vector<std::vector<float>> v2 = {{1.0f, 0.0f}, {0.0f, 1.0f}};
    auto r2 = imagine::clip::meanPoolAndNormalize(v2);
    ASSERT_EQ(r2.size(), 2u);
    EXPECT_NEAR(r2[0], 0.70710678f, 1e-5f);
    EXPECT_NEAR(r2[1], 0.70710678f, 1e-5f);
    EXPECT_NEAR(r2[0] * r2[0] + r2[1] * r2[1], 1.0f, 1e-6f);

    // Three vectors in 3D: [1, 2, 3], [4, 5, 6], [7, 8, 9] -> Mean = [4, 5, 6]
    // Norm = sqrt(16 + 25 + 36) = sqrt(77) ≈ 8.774964
    std::vector<std::vector<float>> v3 = {
        {1.0f, 2.0f, 3.0f},
        {4.0f, 5.0f, 6.0f},
        {7.0f, 8.0f, 9.0f}
    };
    auto r3 = imagine::clip::meanPoolAndNormalize(v3);
    ASSERT_EQ(r3.size(), 3u);
    float norm77 = std::sqrt(77.0f);
    EXPECT_NEAR(r3[0], 4.0f / norm77, 1e-5f);
    EXPECT_NEAR(r3[1], 5.0f / norm77, 1e-5f);
    EXPECT_NEAR(r3[2], 6.0f / norm77, 1e-5f);
    EXPECT_NEAR(r3[0] * r3[0] + r3[1] * r3[1] + r3[2] * r3[2], 1.0f, 1e-5f);

    // All zero vectors: norm <= 1e-12, returns zeros
    std::vector<std::vector<float>> vZero = {{0.0f, 0.0f}, {0.0f, 0.0f}};
    auto rZero = imagine::clip::meanPoolAndNormalize(vZero);
    ASSERT_EQ(rZero.size(), 2u);
    EXPECT_FLOAT_EQ(rZero[0], 0.0f);
    EXPECT_FLOAT_EQ(rZero[1], 0.0f);

    // Inconsistent dimensions returns empty
    std::vector<std::vector<float>> vMismatch = {{1.0f, 2.0f}, {1.0f}};
    EXPECT_TRUE(imagine::clip::meanPoolAndNormalize(vMismatch).empty());
}

TEST(SmartTagsTest, SelectSmartLabelsThresholdFiltering) {
    std::vector<imagine::clip::TagLabel> labels = {
        {"beach", "Beach", "Nature", 0.25f, {"beach"}},
        {"forest", "Forest", "Nature", 0.30f, {"forest"}},
        {"dog", "Dog", "Animals", 0.20f, {"dog"}}
    };

    // Label embeddings corresponding to dot products with {0.26f, 0.0f}:
    // beach: {1.0f, 0.0f} -> dot = 0.26. required = max(0.24, 0.25) = 0.25 -> 0.26 >= 0.25 PASS
    // forest: {1.0f, 0.0f} -> dot = 0.26. required = max(0.24, 0.30) = 0.30 -> 0.26 < 0.30 FAIL (label threshold)
    // dog: {0.8f, 0.0f} -> dot = 0.208. required = max(0.24, 0.20) = 0.24 -> 0.208 < 0.24 FAIL (minScore)
    std::vector<std::vector<float>> labelEmbeddings = {
        {1.0f, 0.0f},
        {1.0f, 0.0f},
        {0.8f, 0.0f}
    };

    std::vector<float> imageEmbedding = {0.26f, 0.0f};

    imagine::clip::SmartTagOptions options;
    options.minScore = 0.24f;
    options.maxLabels = 5;
    options.maxPerGroup = 5;

    auto selected = imagine::clip::selectSmartLabels(imageEmbedding, labelEmbeddings, labels, options);
    ASSERT_EQ(selected.size(), 1u);
    EXPECT_EQ(selected[0].id, "beach");
    EXPECT_NEAR(selected[0].score, 0.26f, 1e-5f);
}

TEST(SmartTagsTest, SelectSmartLabelsTopKLimiting) {
    std::vector<imagine::clip::TagLabel> labels = {
        {"l1", "Label 1", "Nature", 0.20f, {"p"}},
        {"l2", "Label 2", "Nature", 0.20f, {"p"}},
        {"l3", "Label 3", "Animals", 0.20f, {"p"}},
        {"l4", "Label 4", "Animals", 0.20f, {"p"}},
        {"l5", "Label 5", "Food", 0.20f, {"p"}}
    };

    std::vector<std::vector<float>> labelEmbeddings = {
        {0.9f}, {0.8f}, {0.7f}, {0.6f}, {0.5f}
    };
    std::vector<float> imageEmbedding = {1.0f};

    imagine::clip::SmartTagOptions options;
    options.minScore = 0.20f;
    options.maxLabels = 2; // Top-2
    options.maxPerGroup = 5;

    auto selected = imagine::clip::selectSmartLabels(imageEmbedding, labelEmbeddings, labels, options);
    ASSERT_EQ(selected.size(), 2u);
    EXPECT_EQ(selected[0].id, "l1");
    EXPECT_NEAR(selected[0].score, 0.9f, 1e-5f);
    EXPECT_EQ(selected[1].id, "l2");
    EXPECT_NEAR(selected[1].score, 0.8f, 1e-5f);
}

TEST(SmartTagsTest, SelectSmartLabelsPerGroupCap) {
    std::vector<imagine::clip::TagLabel> labels = {
        {"n1", "Nature 1", "Nature", 0.20f, {"p"}},
        {"n2", "Nature 2", "Nature", 0.20f, {"p"}},
        {"n3", "Nature 3", "Nature", 0.20f, {"p"}},
        {"n4", "Nature 4", "Nature", 0.20f, {"p"}},
        {"a1", "Animal 1", "Animals", 0.20f, {"p"}}
    };

    std::vector<std::vector<float>> labelEmbeddings = {
        {0.9f}, {0.8f}, {0.7f}, {0.6f}, {0.5f}
    };
    std::vector<float> imageEmbedding = {1.0f};

    imagine::clip::SmartTagOptions options;
    options.minScore = 0.20f;
    options.maxLabels = 4;
    options.maxPerGroup = 2; // At most 2 from Nature

    auto selected = imagine::clip::selectSmartLabels(imageEmbedding, labelEmbeddings, labels, options);
    ASSERT_EQ(selected.size(), 3u);
    EXPECT_EQ(selected[0].id, "n1");
    EXPECT_EQ(selected[1].id, "n2");
    EXPECT_EQ(selected[2].id, "a1");
}

TEST(SmartTagsTest, SelectSmartLabelsDeterministicTieBreaking) {
    std::vector<imagine::clip::TagLabel> labels = {
        {"zebra", "Zebra", "Animals", 0.20f, {"p"}},
        {"apple", "Apple", "Food", 0.20f, {"p"}},
        {"mango", "Mango", "Food", 0.20f, {"p"}}
    };

    std::vector<std::vector<float>> labelEmbeddings = {
        {0.5f}, {0.5f}, {0.5f}
    };
    std::vector<float> imageEmbedding = {1.0f};

    imagine::clip::SmartTagOptions options;
    options.minScore = 0.20f;
    options.maxLabels = 2;
    options.maxPerGroup = 2;

    auto selected = imagine::clip::selectSmartLabels(imageEmbedding, labelEmbeddings, labels, options);
    ASSERT_EQ(selected.size(), 2u);
    EXPECT_EQ(selected[0].id, "apple");
    EXPECT_EQ(selected[1].id, "mango");
}

TEST(SmartTagsTest, SelectSmartLabelsEmptyAndMismatchedInputs) {
    std::vector<imagine::clip::TagLabel> labels = {
        {"beach", "Beach", "Nature", 0.20f, {"p"}}
    };
    std::vector<std::vector<float>> labelEmbeddings = {{1.0f, 0.0f}};
    std::vector<float> imageEmbedding = {1.0f, 0.0f};
    imagine::clip::SmartTagOptions options;

    // Empty image embedding
    EXPECT_TRUE(imagine::clip::selectSmartLabels({}, labelEmbeddings, labels, options).empty());

    // Empty label embeddings
    EXPECT_TRUE(imagine::clip::selectSmartLabels(imageEmbedding, {}, labels, options).empty());

    // Empty labels
    EXPECT_TRUE(imagine::clip::selectSmartLabels(imageEmbedding, labelEmbeddings, {}, options).empty());

    // maxLabels <= 0
    options.maxLabels = 0;
    EXPECT_TRUE(imagine::clip::selectSmartLabels(imageEmbedding, labelEmbeddings, labels, options).empty());
    options.maxLabels = 3;

    // maxPerGroup <= 0
    options.maxPerGroup = 0;
    EXPECT_TRUE(imagine::clip::selectSmartLabels(imageEmbedding, labelEmbeddings, labels, options).empty());
    options.maxPerGroup = 2;

    // Dimension mismatch between image (2D) and label (3D)
    std::vector<std::vector<float>> mismatchedEmbeddings = {{1.0f, 0.0f, 0.0f}};
    EXPECT_TRUE(imagine::clip::selectSmartLabels(imageEmbedding, mismatchedEmbeddings, labels, options).empty());
}

TEST(SmartTagsTest, LoadSmartTagOptionsFromEnv) {
    // Clean environment initially
    unsetTestEnv("IMAGINE_SMART_TAGS_MIN_SCORE");
    unsetTestEnv("IMAGINE_SMART_TAGS_MAX_LABELS");
    unsetTestEnv("IMAGINE_SMART_TAGS_MAX_PER_GROUP");

    // Defaults
    auto opts = imagine::clip::loadSmartTagOptionsFromEnv();
    EXPECT_FLOAT_EQ(opts.minScore, 0.23f);
    EXPECT_EQ(opts.maxLabels, 3);
    EXPECT_EQ(opts.maxPerGroup, 2);

    // Custom valid values
    setTestEnv("IMAGINE_SMART_TAGS_MIN_SCORE", "0.45");
    setTestEnv("IMAGINE_SMART_TAGS_MAX_LABELS", "5");
    setTestEnv("IMAGINE_SMART_TAGS_MAX_PER_GROUP", "4");

    opts = imagine::clip::loadSmartTagOptionsFromEnv();
    EXPECT_FLOAT_EQ(opts.minScore, 0.45f);
    EXPECT_EQ(opts.maxLabels, 5);
    EXPECT_EQ(opts.maxPerGroup, 4);

    // Clamping to upper bounds
    setTestEnv("IMAGINE_SMART_TAGS_MIN_SCORE", "1.5");
    setTestEnv("IMAGINE_SMART_TAGS_MAX_LABELS", "99");
    setTestEnv("IMAGINE_SMART_TAGS_MAX_PER_GROUP", "50");

    opts = imagine::clip::loadSmartTagOptionsFromEnv();
    EXPECT_FLOAT_EQ(opts.minScore, 1.0f);
    EXPECT_EQ(opts.maxLabels, 20);
    EXPECT_EQ(opts.maxPerGroup, 10);

    // Clamping to lower bounds
    setTestEnv("IMAGINE_SMART_TAGS_MIN_SCORE", "-0.2");
    setTestEnv("IMAGINE_SMART_TAGS_MAX_LABELS", "0");
    setTestEnv("IMAGINE_SMART_TAGS_MAX_PER_GROUP", "-3");

    opts = imagine::clip::loadSmartTagOptionsFromEnv();
    EXPECT_FLOAT_EQ(opts.minScore, 0.0f);
    EXPECT_EQ(opts.maxLabels, 1);
    EXPECT_EQ(opts.maxPerGroup, 1);

    // Invalid non-numeric strings fallback to defaults
    setTestEnv("IMAGINE_SMART_TAGS_MIN_SCORE", "invalid_float");
    setTestEnv("IMAGINE_SMART_TAGS_MAX_LABELS", "not_an_int");
    setTestEnv("IMAGINE_SMART_TAGS_MAX_PER_GROUP", "xyz");

    opts = imagine::clip::loadSmartTagOptionsFromEnv();
    EXPECT_FLOAT_EQ(opts.minScore, 0.23f);
    EXPECT_EQ(opts.maxLabels, 3);
    EXPECT_EQ(opts.maxPerGroup, 2);

    // Clean up
    unsetTestEnv("IMAGINE_SMART_TAGS_MIN_SCORE");
    unsetTestEnv("IMAGINE_SMART_TAGS_MAX_LABELS");
    unsetTestEnv("IMAGINE_SMART_TAGS_MAX_PER_GROUP");
}


TEST(ClipServiceSmartTagsTest, StatusAndStartJobWhenNotReady) {
    std::error_code ec;
    auto testDb = fs::temp_directory_path() / ("test_clip_st_svc_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    auto tempCache = fs::temp_directory_path() / ("test_clip_st_cache_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tempCache, ec);

    const char* prevEnv = std::getenv("IMAGINE_CLIP_MODEL_DIR");
    std::string prevModelDir = prevEnv ? prevEnv : "";
    setTestEnv("IMAGINE_CLIP_MODEL_DIR", (tempCache / "nonexistent_models").string());
    struct EnvCleanup {
        std::string prev;
        ~EnvCleanup() {
            setTestEnv("IMAGINE_CLIP_MODEL_DIR", prev);
        }
    } envCleanup{prevModelDir};

    {
        imagine::db::CatalogDb db;
        ASSERT_TRUE(db.open(testDb.string()).isOk());

        imagine::clip::Service svc(db, tempCache.string(), [](const std::string& p) { return p; });
        auto status = svc.smartTagStatus();
        EXPECT_TRUE(status.contains("built"));
        EXPECT_TRUE(status.contains("ready"));
        EXPECT_FALSE(status["ready"].get<bool>());
        EXPECT_TRUE(status.contains("taxonomyReady"));
        EXPECT_TRUE(status["taxonomyReady"].get<bool>());
        EXPECT_GT(status["labelCount"].get<int64_t>(), 50);
        EXPECT_EQ(status["indexedPhotos"].get<int64_t>(), 0);
        EXPECT_EQ(status["labeledPhotos"].get<int64_t>(), 0);
        EXPECT_EQ(status["tagAssignments"].get<int64_t>(), 0);

        int64_t jobId = 0;
        auto jobStatus = svc.startSmartTagJob("catalog", {}, false, jobId);
        EXPECT_FALSE(jobStatus.isOk());

        auto clearStatus = svc.clearSmartTags();
        EXPECT_TRUE(clearStatus.isOk());

        db.close();
    }

    fs::remove(testDb, ec);
    fs::remove(testDb.string() + "-wal", ec);
    fs::remove(testDb.string() + "-shm", ec);
    fs::remove_all(tempCache, ec);
}

TEST(ClipServiceSmartTagsTest, JobOrchestrationAndFailureHandling) {
    std::error_code ec;
    auto testDb = fs::temp_directory_path() / ("test_clip_st_orch_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    auto tempCache = fs::temp_directory_path() / ("test_clip_st_orch_cache_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tempCache, ec);

    {
        imagine::db::CatalogDb db;
        ASSERT_TRUE(db.open(testDb.string()).isOk());

        // 1. Insert a media item
        imagine::MediaItem item;
        item.file_path = (tempCache / "img.jpg").string();
        item.file_name = "img.jpg";
        item.file_size = 1000;
        item.content_hash = "h1";
        auto addRes = db.insertMedia(item);
        ASSERT_TRUE(addRes.isOk());
        imagine::MediaId mid = addRes.value();

        imagine::clip::Service svc(db, tempCache.string(), [](const std::string& p) { return p; });
        auto statusInfo = svc.status();
        std::string mId = statusInfo.value("modelId", "clip-vit-base-patch32");
        std::string mRev = statusInfo.value("modelRevision", "v1");

        // 2. Insert vector in semantic_index_state matching active model
        std::vector<float> vec(512, 0.05f);
        {
            auto stmtRes = db.connection().prepare(
                "INSERT INTO semantic_index_state (media_id, model_id, model_revision, preprocess_id, vector_dim, embedding, content_hash, indexed_at) "
                "VALUES (?, ?, ?, 'p1', 512, ?, 'h1', 1000);"
            );
            ASSERT_TRUE(stmtRes.isOk());
            auto stmt = std::move(stmtRes.value());
            stmt.bind(1, mid);
            stmt.bind(2, mId);
            stmt.bind(3, mRev);
            stmt.bindBlob(4, vec.data(), vec.size() * sizeof(float));
            ASSERT_EQ(stmt.step(), imagine::db::StepResult::Done);
        }

        // 3. Test getJob on nonexistent job
        auto noJob = svc.getJob(999, "tags");
        EXPECT_FALSE(noJob.isOk());
        EXPECT_EQ(noJob.status().code(), imagine::StatusCode::NotFound);

        // 4. Insert dummy completed semantic_jobs (1 tags, 1 index)
        int64_t tagJobId = 0;
        int64_t indexJobId = 0;
        {
            auto ins1 = db.connection().prepare("INSERT INTO semantic_jobs (scope, state, model_id, kind, total, created_at, updated_at) VALUES ('catalog', 'completed', 'clip', 'tags', 1, 100, 100);");
            ASSERT_TRUE(ins1.isOk());
            ASSERT_EQ(ins1.value().step(), imagine::db::StepResult::Done);
            tagJobId = db.connection().lastInsertRowId();

            auto ins2 = db.connection().prepare("INSERT INTO semantic_jobs (scope, state, model_id, kind, total, created_at, updated_at) VALUES ('catalog', 'completed', 'clip', 'index', 1, 100, 100);");
            ASSERT_TRUE(ins2.isOk());
            ASSERT_EQ(ins2.value().step(), imagine::db::StepResult::Done);
            indexJobId = db.connection().lastInsertRowId();
        }

        // 5. Test kind isolation in getJob
        EXPECT_FALSE(svc.getJob(indexJobId, "tags").isOk());
        EXPECT_TRUE(svc.getJob(tagJobId, "tags").isOk());

        // 6. Test kind isolation in cancelJob
        int64_t activeTagJobId = 0;
        {
            auto insActive = db.connection().prepare("INSERT INTO semantic_jobs (scope, state, model_id, kind, total, created_at, updated_at) VALUES ('catalog', 'running', 'clip', 'tags', 1, 100, 100);");
            ASSERT_TRUE(insActive.isOk());
            ASSERT_EQ(insActive.value().step(), imagine::db::StepResult::Done);
            activeTagJobId = db.connection().lastInsertRowId();
        }
        EXPECT_FALSE(svc.cancelJob(indexJobId, "tags").isOk());
        EXPECT_TRUE(svc.cancelJob(activeTagJobId, "tags").isOk());

        // Mark cancelled so single active job slot is free
        ASSERT_TRUE(db.connection().execute("UPDATE semantic_jobs SET state = 'cancelled' WHERE id = " + std::to_string(activeTagJobId) + ";").isOk());

        // 7. Test runTagJob happy path, incremental skip, and failure handling
        int64_t testJobId = 0;
        {
            auto ins3 = db.connection().prepare("INSERT INTO semantic_jobs (scope, state, model_id, kind, total, created_at, updated_at) VALUES ('catalog', 'running', 'clip', 'tags', 1, 100, 100);");
            ASSERT_TRUE(ins3.isOk());
            ASSERT_EQ(ins3.value().step(), imagine::db::StepResult::Done);
            testJobId = db.connection().lastInsertRowId();
        }

        bool engineReady = svc.smartTagStatus()["ready"].get<bool>();
        if (engineReady) {
            // Happy path: models are available, job processes photo and commits smart_tag_state
            svc.runTagJobForTest(testJobId, "catalog", {mid}, false);
            auto completedJob = svc.getJob(testJobId, "tags");
            ASSERT_TRUE(completedJob.isOk());
            EXPECT_EQ(completedJob.value()["state"].get<std::string>(), "completed");
            EXPECT_EQ(completedJob.value()["processed"].get<int64_t>(), 1);

            // Verify smart_tag_state was recorded
            auto stateCount = db.connection().prepare("SELECT COUNT(*) FROM smart_tag_state WHERE media_id = " + std::to_string(mid) + ";");
            ASSERT_TRUE(stateCount.isOk());
            auto& scStmt = stateCount.value();
            EXPECT_EQ(scStmt.step(), imagine::db::StepResult::Row);
            EXPECT_EQ(scStmt.getInt64(0), 1);

            // Incremental skip: repeat run skips already labeled photo
            int64_t skipJobId = 0;
            {
                auto insSkip = db.connection().prepare("INSERT INTO semantic_jobs (scope, state, model_id, kind, total, created_at, updated_at) VALUES ('catalog', 'running', 'clip', 'tags', 1, 100, 100);");
                ASSERT_TRUE(insSkip.isOk());
                ASSERT_EQ(insSkip.value().step(), imagine::db::StepResult::Done);
                skipJobId = db.connection().lastInsertRowId();
            }
            svc.runTagJobForTest(skipJobId, "catalog", {mid}, false);
            auto skippedJob = svc.getJob(skipJobId, "tags");
            ASSERT_TRUE(skippedJob.isOk());
            EXPECT_EQ(skippedJob.value()["state"].get<std::string>(), "completed");
            EXPECT_EQ(skippedJob.value()["skipped"].get<int64_t>(), 1);
            EXPECT_EQ(skippedJob.value()["processed"].get<int64_t>(), 0);
        } else {
            // Engine not ready: marks failed without crashing
            svc.runTagJobForTest(testJobId, "catalog", {mid}, false);
            auto failedJob = svc.getJob(testJobId, "tags");
            ASSERT_TRUE(failedJob.isOk());
            EXPECT_EQ(failedJob.value()["state"].get<std::string>(), "failed");
            EXPECT_FALSE(failedJob.value()["error"].get<std::string>().empty());
        }

        db.close();
    }

    fs::remove(testDb, ec);
    fs::remove(testDb.string() + "-wal", ec);
    fs::remove(testDb.string() + "-shm", ec);
    fs::remove_all(tempCache, ec);
}
