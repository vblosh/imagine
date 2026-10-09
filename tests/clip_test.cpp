#include <gtest/gtest.h>
#include "imagine/clip/engine.hpp"
#include "imagine/clip/service.hpp"
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


