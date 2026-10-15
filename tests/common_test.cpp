#include <gtest/gtest.h>
#include "imagine/common/error.hpp"
#include "imagine/common/logger.hpp"
#include "imagine/common/types.hpp"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <nlohmann/json.hpp>

using namespace imagine;

TEST(ErrorTest, StatusConstructorsAndFactories) {
    Status sDefault;
    EXPECT_TRUE(sDefault.isOk());
    EXPECT_TRUE(static_cast<bool>(sDefault));
    EXPECT_EQ(sDefault.code(), StatusCode::Ok);
    EXPECT_TRUE(sDefault.message().empty());

    Status sOk = Status::ok();
    EXPECT_TRUE(sOk.isOk());
    EXPECT_EQ(sOk.code(), StatusCode::Ok);

    Status sNotFound = Status::notFound("media 123");
    EXPECT_FALSE(sNotFound.isOk());
    EXPECT_EQ(sNotFound.code(), StatusCode::NotFound);
    EXPECT_EQ(sNotFound.message(), "media 123");

    Status sAlready = Status::alreadyExists("tag exists");
    EXPECT_EQ(sAlready.code(), StatusCode::AlreadyExists);
    EXPECT_EQ(sAlready.message(), "tag exists");

    Status sInvalid = Status::invalidArgument("bad rating");
    EXPECT_EQ(sInvalid.code(), StatusCode::InvalidArgument);
    EXPECT_EQ(sInvalid.message(), "bad rating");

    Status sIo = Status::ioError("file error");
    EXPECT_EQ(sIo.code(), StatusCode::IoError);
    EXPECT_EQ(sIo.message(), "file error");

    Status sDb = Status::databaseError("db locked");
    EXPECT_EQ(sDb.code(), StatusCode::DatabaseError);
    EXPECT_EQ(sDb.message(), "db locked");

    Status sParse = Status::parseError("bad json");
    EXPECT_EQ(sParse.code(), StatusCode::ParseError);
    EXPECT_EQ(sParse.message(), "bad json");

    Status sInternal = Status::internal("internal crash");
    EXPECT_EQ(sInternal.code(), StatusCode::InternalError);
    EXPECT_EQ(sInternal.message(), "internal crash");
}

TEST(ErrorTest, ResultValueAndErrorHandling) {
    Result<std::string> successRes("hello world");
    EXPECT_TRUE(successRes.isOk());
    EXPECT_TRUE(static_cast<bool>(successRes));
    EXPECT_EQ(successRes.status().code(), StatusCode::Ok);
    EXPECT_EQ(successRes.value(), "hello world");
    EXPECT_EQ(successRes.valueOr("default"), "hello world");

    // Non-const value reference
    successRes.value() = "modified";
    EXPECT_EQ(successRes.value(), "modified");

    // Rvalue value
    std::string moved = std::move(successRes).value();
    EXPECT_EQ(moved, "modified");

    // Failure Result
    Result<std::string> failRes = Result<std::string>::failure(Status::notFound("not found item"));
    EXPECT_FALSE(failRes.isOk());
    EXPECT_FALSE(static_cast<bool>(failRes));
    EXPECT_EQ(failRes.status().code(), StatusCode::NotFound);
    EXPECT_EQ(failRes.status().message(), "not found item");
    EXPECT_EQ(failRes.valueOr("fallback"), "fallback");

    // Accessing value() on error must throw runtime_error
    EXPECT_THROW((void)failRes.value(), std::runtime_error);

    // Non-const and rvalue value on error must also throw
    EXPECT_THROW({
        auto& nonConst = failRes.value();
        (void)nonConst;
    }, std::runtime_error);

    EXPECT_THROW({
        auto movedVal = std::move(failRes).value();
        (void)movedVal;
    }, std::runtime_error);
}

TEST(LoggerTest, LevelsAndLogging) {
    auto& logger = Logger::instance();
    auto origLevel = logger.level();

    logger.setLevel(LogLevel::Debug);
    EXPECT_EQ(logger.level(), LogLevel::Debug);

    // Log messages at each level
    IMAGINE_LOG_DEBUG("Test debug message");
    IMAGINE_LOG_INFO("Test info message");
    IMAGINE_LOG_WARN("Test warn message");
    IMAGINE_LOG_ERROR("Test error message");

    // Filtering when level is higher
    logger.setLevel(LogLevel::Error);
    EXPECT_EQ(logger.level(), LogLevel::Error);
    IMAGINE_LOG_DEBUG("Should be skipped");
    IMAGINE_LOG_INFO("Should be skipped");
    IMAGINE_LOG_WARN("Should be skipped");
    IMAGINE_LOG_ERROR("Should be logged");

    // LogLevel None disables everything
    logger.setLevel(LogLevel::None);
    EXPECT_EQ(logger.level(), LogLevel::None);
    IMAGINE_LOG_ERROR("Should not be logged when None");

    // Restore
    logger.setLevel(origLevel);
    EXPECT_EQ(logger.level(), origLevel);
}

TEST(LoggerTest, ParseLevel) {
    EXPECT_EQ(Logger::parseLevel("debug"), LogLevel::Debug);
    EXPECT_EQ(Logger::parseLevel("DEBUG"), LogLevel::Debug);
    EXPECT_EQ(Logger::parseLevel("info"), LogLevel::Info);
    EXPECT_EQ(Logger::parseLevel("INFO"), LogLevel::Info);
    EXPECT_EQ(Logger::parseLevel("warn"), LogLevel::Warn);
    EXPECT_EQ(Logger::parseLevel("warning"), LogLevel::Warn);
    EXPECT_EQ(Logger::parseLevel("error"), LogLevel::Error);
    EXPECT_EQ(Logger::parseLevel("none"), LogLevel::None);
    EXPECT_EQ(Logger::parseLevel("off"), LogLevel::None);
    EXPECT_EQ(Logger::parseLevel("invalid_fallback"), LogLevel::Info);
}

TEST(LoggerTest, FileOutputJsonAndText) {
    auto& logger = Logger::instance();
    auto origLevel = logger.level();
    auto origFormat = logger.fileFormat();
    auto origConsole = logger.isConsoleEnabled();

    auto tempDir = std::filesystem::temp_directory_path() / ("imagine_logger_test_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(tempDir);
    auto logFilePath = tempDir / "test.log";

    // Clean up any existing file
    std::error_code ec;
    std::filesystem::remove(logFilePath, ec);

    // Test JSON logging
    logger.setLevel(LogLevel::Debug);
    logger.setConsoleEnabled(false);
    logger.setFileFormat(LogFormat::Json);
    logger.setLogFile(logFilePath.string());

    IMAGINE_LOG_INFO("Hello \"JSON\" world\nline2");
    IMAGINE_LOG_WARN("Warning test");
    IMAGINE_LOG_ERROR("Error test");

    const std::vector<std::string> malformed = {
        std::string("bad\xff", 4), std::string("\xc0\xaf", 2),
        std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4),
        std::string("\xe2\x82", 2), std::string("\xe2(\xa1", 3)
    };
    const std::string replacement = "\xef\xbf\xbd";
    const std::vector<std::string> sanitized = {
        "bad" + replacement, replacement + replacement,
        replacement + replacement + replacement,
        replacement + replacement + replacement + replacement,
        replacement + replacement, replacement + "(" + replacement
    };
    for (const auto& message : malformed) {
        logger.log(LogLevel::Info, "test.cpp", 1, message);
    }
    const std::string validUtf8 = "\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80";
    logger.log(LogLevel::Info, "test.cpp", 1, validUtf8);

    logger.closeLogFile();

    // Verify file content
    {
        std::ifstream ifs(logFilePath);
        ASSERT_TRUE(ifs.is_open());
        std::string line;
        int count = 0;
        while (std::getline(ifs, line)) {
            if (line.empty()) continue;
            count++;
            auto j = nlohmann::json::parse(line);
            EXPECT_TRUE(j.contains("ts"));
            EXPECT_TRUE(j.contains("level"));
            EXPECT_TRUE(j.contains("file"));
            EXPECT_TRUE(j.contains("line"));
            EXPECT_TRUE(j.contains("msg"));
            if (count == 1) {
                EXPECT_EQ(j["level"], "INFO");
                EXPECT_EQ(j["msg"], "Hello \"JSON\" world\nline2");
            } else if (count == 2) {
                EXPECT_EQ(j["level"], "WARN");
                EXPECT_EQ(j["msg"], "Warning test");
            } else if (count == 3) {
                EXPECT_EQ(j["level"], "ERROR");
                EXPECT_EQ(j["msg"], "Error test");
            } else if (count <= 3 + static_cast<int>(sanitized.size())) {
                EXPECT_EQ(j["msg"], sanitized[count - 4]);
            } else {
                EXPECT_EQ(j["msg"], validUtf8);
            }
        }
        EXPECT_EQ(count, 4 + sanitized.size());
    }

    // Test Text logging appending to file
    logger.setFileFormat(LogFormat::Text);
    EXPECT_FALSE(logger.setLogFile(tempDir.string()));
    EXPECT_TRUE(logger.setLogFile(logFilePath.string()));
    IMAGINE_LOG_INFO("Text format test message");
    logger.closeLogFile();

    {
        std::ifstream ifs(logFilePath);
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        EXPECT_NE(content.find("Text format test message"), std::string::npos);
        EXPECT_NE(content.find("[INFO ]"), std::string::npos);
    }

    // Cleanup and restore
    std::filesystem::remove_all(tempDir, ec);
    logger.setConsoleEnabled(origConsole);
    logger.setFileFormat(origFormat);
    logger.setLevel(origLevel);
}

TEST(LoggerTest, UnicodeLoggingAndFilePath) {
    auto& logger = Logger::instance();
    auto origLevel = logger.level();
    auto origFormat = logger.fileFormat();
    auto origConsole = logger.isConsoleEnabled();

    auto tempDir = std::filesystem::temp_directory_path() /
        ("imagine_unicode_log_test_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(tempDir);

    // Unicode folder and file path (Cyrillic and German characters)
    auto unicodeSubdir = tempDir / pathFromUtf8("Логи_Папка");
    auto logFilePath = unicodeSubdir / pathFromUtf8("журнал_Nürnberg.log");
    std::string logFilePathUtf8 = pathToUtf8(logFilePath);

    logger.setLevel(LogLevel::Debug);
    logger.setConsoleEnabled(true);
    logger.setFileFormat(LogFormat::Text);

    // Verify setLogFile works with UTF-8 path and creates parent directory
    EXPECT_TRUE(logger.setLogFile(logFilePathUtf8));

    // Log messages containing Cyrillic, umlauts, CJK
    const std::string cyrillicMsg = "Face scan error for media ID 10472 (Коломна/P1160040_filtered.tif): Failed to decode";
    const std::string germanMsg = "Face scan unable to open image file for media ID 23850 (D:\\Foto\\Nürnberg/DSC_7926.NEF)";
    const std::string cjkMsg = "Tokyo: 東京, Emoji: 🌟";

    logger.log(LogLevel::Error, "service.cpp", 2604, cyrillicMsg);
    logger.log(LogLevel::Error, "service.cpp", 2685, germanMsg);
    logger.log(LogLevel::Info, "service.cpp", 2700, cjkMsg);

    logger.closeLogFile();

    // Verify file content has preserved the exact UTF-8 byte sequences
    {
        std::ifstream ifs(pathFromUtf8(logFilePathUtf8), std::ios::binary);
        ASSERT_TRUE(ifs.is_open());
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        EXPECT_NE(content.find(cyrillicMsg), std::string::npos);
        EXPECT_NE(content.find(germanMsg), std::string::npos);
        EXPECT_NE(content.find(cjkMsg), std::string::npos);
        EXPECT_NE(content.find("[service.cpp:2604]"), std::string::npos);
    }

    // Now test JSON format with Unicode log path
    logger.setFileFormat(LogFormat::Json);
    EXPECT_TRUE(logger.setLogFile(logFilePathUtf8));
    logger.log(LogLevel::Error, "service.cpp", 2604, cyrillicMsg);
    logger.log(LogLevel::Info, "service.cpp", 2700, cjkMsg);
    logger.closeLogFile();

    {
        std::ifstream ifs(pathFromUtf8(logFilePathUtf8), std::ios::binary);
        ASSERT_TRUE(ifs.is_open());
        std::string line;
        bool foundCyrillic = false;
        bool foundCjk = false;
        while (std::getline(ifs, line)) {
            if (line.empty()) continue;
            auto parsed = nlohmann::json::parse(line, nullptr, false);
            if (parsed.is_object() && parsed.contains("msg")) {
                if (parsed["msg"] == cyrillicMsg) foundCyrillic = true;
                if (parsed["msg"] == cjkMsg) foundCjk = true;
            }
        }
        EXPECT_TRUE(foundCyrillic);
        EXPECT_TRUE(foundCjk);
    }

    // Cleanup and restore
    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
    logger.setConsoleEnabled(origConsole);
    logger.setFileFormat(origFormat);
    logger.setLevel(origLevel);
}

TEST(LoggerTest, RotationStrategyThreeFilesDeleteOld) {
    auto& logger = Logger::instance();
    auto origLevel = logger.level();
    auto origFormat = logger.fileFormat();
    auto origConsole = logger.isConsoleEnabled();
    auto origMaxFileSize = logger.maxFileSize();
    auto origMaxFiles = logger.maxFiles();

    logger.setLevel(LogLevel::Info);
    logger.setConsoleEnabled(false);
    logger.setFileFormat(LogFormat::Text);

    auto tempDir = std::filesystem::temp_directory_path() /
        ("imagine_rotation_test_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(tempDir);
    auto logPath = tempDir / "rotate.log";
    std::string logPathStr = pathToUtf8(logPath);

    // Default configuration should be 10MB and 3 files
    EXPECT_EQ(Logger::kDefaultMaxLogFileSize, 10 * 1024 * 1024);
    EXPECT_EQ(Logger::kDefaultMaxLogFiles, 3);

    // Set rotation with maxFileSize = 300 bytes and maxFiles = 3
    const size_t maxBytes = 300;
    const size_t maxFiles = 3;
    ASSERT_TRUE(logger.setLogFile(logPathStr, maxBytes, maxFiles));
    EXPECT_EQ(logger.maxFileSize(), maxBytes);
    EXPECT_EQ(logger.maxFiles(), maxFiles);
    EXPECT_TRUE(logger.isLogFileOpen());
    EXPECT_EQ(logger.logFilePath(), logPathStr);

    auto backup1 = tempDir / "rotate.log.1";
    auto backup2 = tempDir / "rotate.log.2";
    auto backup3 = tempDir / "rotate.log.3";
    auto backup4 = tempDir / "rotate.log.4";

    std::error_code ec;

    // Write log batch 1 (under 300 bytes)
    for (int i = 0; i < 3; ++i) {
        IMAGINE_LOG_INFO("Batch 1 message " + std::to_string(i));
    }
    EXPECT_TRUE(std::filesystem::exists(logPath, ec));
    EXPECT_FALSE(std::filesystem::exists(backup1, ec));

    // Write enough logs to trigger rotation 1 -> backup1 created (bounded loop)
    int iters = 0;
    while (!std::filesystem::exists(backup1, ec) && ++iters < 200) {
        IMAGINE_LOG_INFO("Trigger rotation 1 filling up file with some text");
    }
    ASSERT_TRUE(std::filesystem::exists(backup1, ec)) << "Timeout waiting for backup1";
    EXPECT_TRUE(std::filesystem::exists(logPath, ec));
    EXPECT_FALSE(std::filesystem::exists(backup2, ec));

    // Write enough logs to trigger rotation 2 -> backup2 created (bounded loop)
    iters = 0;
    while (!std::filesystem::exists(backup2, ec) && ++iters < 200) {
        IMAGINE_LOG_INFO("Trigger rotation 2 filling up file with more text");
    }
    ASSERT_TRUE(std::filesystem::exists(backup2, ec)) << "Timeout waiting for backup2";
    EXPECT_TRUE(std::filesystem::exists(logPath, ec));
    EXPECT_TRUE(std::filesystem::exists(backup1, ec));
    EXPECT_FALSE(std::filesystem::exists(backup3, ec));

    // Write enough logs to trigger rotation 3 -> backup3 created (bounded loop)
    iters = 0;
    while (!std::filesystem::exists(backup3, ec) && ++iters < 200) {
        IMAGINE_LOG_INFO("Trigger rotation 3 filling up file with even more text");
    }
    ASSERT_TRUE(std::filesystem::exists(backup3, ec)) << "Timeout waiting for backup3";
    EXPECT_TRUE(std::filesystem::exists(logPath, ec));
    EXPECT_TRUE(std::filesystem::exists(backup1, ec));
    EXPECT_TRUE(std::filesystem::exists(backup2, ec));
    EXPECT_FALSE(std::filesystem::exists(backup4, ec));

    // Capture contents of backup3 and backup2 before 4th rotation
    std::string b3ContentBefore;
    {
        std::ifstream ifs(backup3);
        b3ContentBefore = std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }
    std::string b2ContentBefore;
    {
        std::ifstream ifs(backup2);
        b2ContentBefore = std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }

    // Now write messages until rotation 4 occurs (backup3 changes) (bounded loop)
    bool rotated4 = false;
    for (int counter = 0; counter < 200; ++counter) {
        std::string currentB3;
        {
            std::ifstream ifs(backup3);
            currentB3 = std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        }
        if (currentB3 != b3ContentBefore) {
            // Exactly one rotation happened: backup2 was shifted to backup3 and old backup3 was deleted
            EXPECT_EQ(currentB3, b2ContentBefore);
            rotated4 = true;
            break;
        }
        IMAGINE_LOG_INFO("Single step to trigger rotation 4: " + std::to_string(counter));
    }
    ASSERT_TRUE(rotated4) << "Timeout waiting for rotation 4";

    // Now write many more messages to trigger numerous additional rotations
    for (int i = 0; i < 100; ++i) {
        IMAGINE_LOG_INFO("Lots of logs after 4th rotation " + std::to_string(i));
    }

    // Verify backup4 still does NOT exist (old deleted)
    EXPECT_FALSE(std::filesystem::exists(backup4, ec));
    EXPECT_TRUE(std::filesystem::exists(backup3, ec));
    EXPECT_TRUE(std::filesystem::exists(backup2, ec));
    EXPECT_TRUE(std::filesystem::exists(backup1, ec));
    EXPECT_TRUE(std::filesystem::exists(logPath, ec));

    logger.closeLogFile();
    EXPECT_FALSE(logger.isLogFileOpen());
    EXPECT_TRUE(logger.logFilePath().empty());

    // Clean up
    std::filesystem::remove_all(tempDir, ec);
    logger.setConsoleEnabled(origConsole);
    logger.setFileFormat(origFormat);
    logger.setRotation(origMaxFileSize, origMaxFiles);
    logger.setLevel(origLevel);
}

TEST(LoggerTest, RotateOnOpenWhenOverLimit) {
    auto& logger = Logger::instance();
    auto origLevel = logger.level();
    auto origFormat = logger.fileFormat();
    auto origConsole = logger.isConsoleEnabled();
    auto origMaxFileSize = logger.maxFileSize();
    auto origMaxFiles = logger.maxFiles();

    logger.setLevel(LogLevel::Info);
    logger.setConsoleEnabled(false);

    auto tempDir = std::filesystem::temp_directory_path() /
        ("imagine_rotate_open_test_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(tempDir);
    auto logPath = tempDir / "over_limit.log";
    auto backup1 = tempDir / "over_limit.log.1";

    // Create a pre-existing file with 500 bytes
    {
        std::ofstream ofs(logPath);
        ofs << std::string(500, 'X');
    }

    // Open with maxFileSize = 300 bytes
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::exists(logPath, ec));
    EXPECT_FALSE(std::filesystem::exists(backup1, ec));

    ASSERT_TRUE(logger.setLogFile(pathToUtf8(logPath), 300, 3));
    // It should have rotated immediately on open because existing file > 300 bytes
    EXPECT_TRUE(std::filesystem::exists(backup1, ec));
    EXPECT_EQ(std::filesystem::file_size(backup1, ec), 500);

    logger.closeLogFile();
    std::filesystem::remove_all(tempDir, ec);
    logger.setConsoleEnabled(origConsole);
    logger.setFileFormat(origFormat);
    logger.setRotation(origMaxFileSize, origMaxFiles);
    logger.setLevel(origLevel);
}

TEST(LoggerTest, SetRotationAndGetters) {
    auto& logger = Logger::instance();
    logger.setRotation(5 * 1024 * 1024, 5);
    EXPECT_EQ(logger.maxFileSize(), 5 * 1024 * 1024);
    EXPECT_EQ(logger.maxFiles(), 5);

    // Restore defaults
    logger.setRotation(Logger::kDefaultMaxLogFileSize, Logger::kDefaultMaxLogFiles);
    EXPECT_EQ(logger.maxFileSize(), 10 * 1024 * 1024);
    EXPECT_EQ(logger.maxFiles(), 3);
}

TEST(LoggerTest, LogFileNotCreatedWhenClosed) {
    auto& logger = Logger::instance();
    logger.closeLogFile();
    EXPECT_FALSE(logger.isLogFileOpen());
    EXPECT_TRUE(logger.logFilePath().empty());
}

TEST(LoggerTest, PruneStaleBackupsWhenMaxFilesReduced) {
    auto& logger = Logger::instance();
    auto origLevel = logger.level();
    auto origFormat = logger.fileFormat();
    auto origConsole = logger.isConsoleEnabled();
    auto origMaxFileSize = logger.maxFileSize();
    auto origMaxFiles = logger.maxFiles();

    auto tempDir = std::filesystem::temp_directory_path() /
        ("imagine_prune_test_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(tempDir);
    auto logPath = tempDir / "prune.log";

    // Create stale rotated files up to .15
    for (int i = 1; i <= 15; ++i) {
        std::ofstream ofs(tempDir / ("prune.log." + std::to_string(i)));
        ofs << "dummy backup " << i;
    }
    // Also create unrelated files to ensure they are not pruned
    {
        std::ofstream ofs(tempDir / "other.log.10");
        ofs << "unrelated";
    }

    // Call setLogFile with maxFiles = 3
    ASSERT_TRUE(logger.setLogFile(pathToUtf8(logPath), 10 * 1024 * 1024, 3));

    std::error_code ec;
    // .1, .2, .3 should still exist
    EXPECT_TRUE(std::filesystem::exists(tempDir / "prune.log.1", ec));
    EXPECT_TRUE(std::filesystem::exists(tempDir / "prune.log.2", ec));
    EXPECT_TRUE(std::filesystem::exists(tempDir / "prune.log.3", ec));

    // .4 through .15 should have been pruned
    for (int i = 4; i <= 15; ++i) {
        EXPECT_FALSE(std::filesystem::exists(tempDir / ("prune.log." + std::to_string(i)), ec));
    }
    // Unrelated file should remain
    EXPECT_TRUE(std::filesystem::exists(tempDir / "other.log.10", ec));

    logger.closeLogFile();
    std::filesystem::remove_all(tempDir, ec);
    logger.setConsoleEnabled(origConsole);
    logger.setFileFormat(origFormat);
    logger.setRotation(origMaxFileSize, origMaxFiles);
    logger.setLevel(origLevel);
}

TEST(TypesTest, ExifDataJsonSerialization) {
    ExifData original;
    original.camera_make = "Nikon";
    original.camera_model = "Z9";
    original.lens = "Nikkor 24-70mm";
    original.date_taken = 1750000000;
    original.date_taken_str = "2026:06:15 12:00:00";
    original.exposure_time = 0.001;
    original.f_number = 4.0;
    original.iso = 400;
    original.focal_length = 35.0;
    original.orientation = 6;
    original.has_gps = true;
    original.latitude = 48.8566;
    original.longitude = 2.3522;
    original.altitude = 35.5;

    nlohmann::json j = original;
    ExifData deserialized = j.get<ExifData>();

    EXPECT_EQ(deserialized.camera_make, "Nikon");
    EXPECT_EQ(deserialized.camera_model, "Z9");
    EXPECT_EQ(deserialized.lens, "Nikkor 24-70mm");
    EXPECT_EQ(deserialized.date_taken, 1750000000);
    EXPECT_EQ(deserialized.date_taken_str, "2026:06:15 12:00:00");
    EXPECT_DOUBLE_EQ(deserialized.exposure_time, 0.001);
    EXPECT_DOUBLE_EQ(deserialized.f_number, 4.0);
    EXPECT_EQ(deserialized.iso, 400);
    EXPECT_DOUBLE_EQ(deserialized.focal_length, 35.0);
    EXPECT_EQ(deserialized.orientation, 6);
    EXPECT_TRUE(deserialized.has_gps);
    EXPECT_DOUBLE_EQ(deserialized.latitude, 48.8566);
    EXPECT_DOUBLE_EQ(deserialized.longitude, 2.3522);
    EXPECT_DOUBLE_EQ(deserialized.altitude, 35.5);

    // Test from empty JSON object (defaults)
    nlohmann::json emptyJ = nlohmann::json::object();
    ExifData defData = emptyJ.get<ExifData>();
    EXPECT_EQ(defData.camera_make, "");
    EXPECT_EQ(defData.orientation, 1);
    EXPECT_FALSE(defData.has_gps);
}

TEST(TypesTest, TagJsonSerialization) {
    Tag t1;
    t1.id = 10;
    t1.name = "Landscape";
    t1.category = "places";
    t1.parent_id = 5;
    t1.media_count = 42;

    nlohmann::json j1 = t1;
    EXPECT_EQ(j1["parent_id"], 5);

    Tag d1 = j1.get<Tag>();
    EXPECT_EQ(d1.id, 10);
    EXPECT_EQ(d1.name, "Landscape");
    EXPECT_EQ(d1.category, "places");
    ASSERT_TRUE(d1.parent_id.has_value());
    EXPECT_EQ(*d1.parent_id, 5);
    EXPECT_EQ(d1.media_count, 42);

    // Without parent_id
    Tag t2;
    t2.id = 11;
    t2.name = "Travel";
    t2.category = "keyword";
    t2.parent_id = std::nullopt;

    nlohmann::json j2 = t2;
    EXPECT_TRUE(j2["parent_id"].is_null());

    Tag d2 = j2.get<Tag>();
    EXPECT_FALSE(d2.parent_id.has_value());

    // From empty json
    nlohmann::json emptyJ = nlohmann::json::object();
    Tag d3 = emptyJ.get<Tag>();
    EXPECT_EQ(d3.id, 0);
    EXPECT_EQ(d3.category, "keyword");
    EXPECT_FALSE(d3.parent_id.has_value());
}

TEST(TypesTest, MediaItemJsonSerialization) {
    MediaItem m;
    m.id = 101;
    m.file_path = "/media/test.jpg";
    m.file_name = "test.jpg";
    m.file_size = 50000;
    m.file_modified_time = 1700000000;
    m.content_hash = "sha256abc";
    m.width = 1920;
    m.height = 1080;
    m.date_taken = 1700000000;
    m.rating = 3;
    m.flag = FlagState::Pick;
    m.thumb_small = "/thumbs/sm.jpg";
    m.thumb_large = "/thumbs/lg.jpg";
    m.created_at = 1700001000;
    m.updated_at = 1700002000;

    Tag tag;
    tag.id = 1;
    tag.name = "Sunny";
    m.tags.push_back(tag);

    nlohmann::json j = m;
    MediaItem d = j.get<MediaItem>();

    EXPECT_EQ(d.id, 101);
    EXPECT_EQ(d.file_path, "/media/test.jpg");
    EXPECT_EQ(d.file_name, "test.jpg");
    EXPECT_EQ(d.file_size, 50000);
    EXPECT_EQ(d.file_modified_time, 1700000000);
    EXPECT_EQ(d.content_hash, "sha256abc");
    EXPECT_EQ(d.width, 1920);
    EXPECT_EQ(d.height, 1080);
    EXPECT_EQ(d.rating, 3);
    EXPECT_EQ(d.flag, FlagState::Pick);
    EXPECT_EQ(d.thumb_small, "/thumbs/sm.jpg");
    EXPECT_EQ(d.thumb_large, "/thumbs/lg.jpg");
    EXPECT_EQ(d.created_at, 1700001000);
    EXPECT_EQ(d.updated_at, 1700002000);
    ASSERT_EQ(d.tags.size(), 1u);
    EXPECT_EQ(d.tags[0].name, "Sunny");

    // From empty json
    nlohmann::json emptyJ = nlohmann::json::object();
    MediaItem dEmpty = emptyJ.get<MediaItem>();
    EXPECT_EQ(dEmpty.id, 0);
    EXPECT_EQ(dEmpty.flag, FlagState::Unflagged);
    EXPECT_TRUE(dEmpty.tags.empty());
}

TEST(TypesTest, AlbumJsonSerialization) {
    Album a1;
    a1.id = 20;
    a1.name = "Favorites";
    a1.description = "Top shots";
    a1.is_smart = true;
    a1.query_json = "{\"rating\": 5}";
    a1.cover_media_id = 999;
    a1.item_count = 15;
    a1.created_at = 1700000000;

    nlohmann::json j1 = a1;
    EXPECT_EQ(j1["cover_media_id"], 999);

    Album d1 = j1.get<Album>();
    EXPECT_EQ(d1.id, 20);
    EXPECT_EQ(d1.name, "Favorites");
    EXPECT_EQ(d1.description, "Top shots");
    EXPECT_TRUE(d1.is_smart);
    EXPECT_EQ(d1.query_json, "{\"rating\": 5}");
    ASSERT_TRUE(d1.cover_media_id.has_value());
    EXPECT_EQ(*d1.cover_media_id, 999);
    EXPECT_EQ(d1.item_count, 15);
    EXPECT_EQ(d1.created_at, 1700000000);

    // Without cover_media_id
    Album a2;
    a2.id = 21;
    a2.name = "All";
    a2.cover_media_id = std::nullopt;

    nlohmann::json j2 = a2;
    EXPECT_TRUE(j2["cover_media_id"].is_null());
    Album d2 = j2.get<Album>();
    EXPECT_FALSE(d2.cover_media_id.has_value());

    // From empty json
    nlohmann::json emptyJ = nlohmann::json::object();
    Album d3 = emptyJ.get<Album>();
    EXPECT_EQ(d3.id, 0);
    EXPECT_FALSE(d3.is_smart);
    EXPECT_FALSE(d3.cover_media_id.has_value());
}

TEST(TypesTest, CatalogStatsAndTimelineJsonSerialization) {
    CatalogStats stats;
    stats.total_media = 500;
    stats.total_picks = 120;
    stats.total_rejects = 30;
    stats.total_not_rejects = 470;
    stats.total_unrated = 150;
    stats.total_size_bytes = 104857600;
    stats.total_tags = 25;
    stats.total_albums = 4;
    stats.earliest_date = 1600000000;
    stats.latest_date = 1750000000;

    nlohmann::json js = stats;
    CatalogStats ds = js.get<CatalogStats>();
    EXPECT_EQ(ds.total_media, 500);
    EXPECT_EQ(ds.total_picks, 120);
    EXPECT_EQ(ds.total_rejects, 30);
    EXPECT_EQ(ds.total_not_rejects, 470);
    EXPECT_EQ(ds.total_unrated, 150);
    EXPECT_EQ(ds.total_size_bytes, 104857600);
    EXPECT_EQ(ds.total_tags, 25);
    EXPECT_EQ(ds.total_albums, 4);
    EXPECT_EQ(ds.earliest_date, 1600000000);
    EXPECT_EQ(ds.latest_date, 1750000000);

    nlohmann::json emptyJs = nlohmann::json::object();
    CatalogStats dsEmpty = emptyJs.get<CatalogStats>();
    EXPECT_EQ(dsEmpty.total_media, 0);
    EXPECT_EQ(dsEmpty.total_picks, 0);
    EXPECT_EQ(dsEmpty.total_rejects, 0);
    EXPECT_EQ(dsEmpty.total_not_rejects, 0);
    EXPECT_EQ(dsEmpty.total_unrated, 0);

    TimelineEntry entry;
    entry.year = 2026;
    entry.month = 9;
    entry.count = 42;

    nlohmann::json jt = entry;
    TimelineEntry dt = jt.get<TimelineEntry>();
    EXPECT_EQ(dt.year, 2026);
    EXPECT_EQ(dt.month, 9);
    EXPECT_EQ(dt.count, 42);

    nlohmann::json emptyJt = nlohmann::json::object();
    TimelineEntry dtEmpty = emptyJt.get<TimelineEntry>();
    EXPECT_EQ(dtEmpty.year, 0);
    EXPECT_EQ(dtEmpty.month, 0);
    EXPECT_EQ(dtEmpty.count, 0);
}

TEST(ErrorTest, ResultConstructedWithStatusOkInvariant) {
    // MED-01: Result(Status::ok()) must not create a split-brain state
    Result<int> res(Status::ok());
    EXPECT_FALSE(res.isOk());
    EXPECT_FALSE(static_cast<bool>(res));
    EXPECT_FALSE(res.status().isOk());
    EXPECT_EQ(res.status().code(), StatusCode::InternalError);
    EXPECT_NE(res.status().message().find("Result<T> constructed with Status::ok()"), std::string::npos);
    EXPECT_THROW((void)res.value(), std::runtime_error);

    Result<std::string> failFactory = Result<std::string>::failure(Status::ok());
    EXPECT_FALSE(failFactory.isOk());
    EXPECT_FALSE(failFactory.status().isOk());
    EXPECT_EQ(failFactory.status().code(), StatusCode::InternalError);
}

TEST(ErrorTest, ResultValueOrRvalueMoveOnly) {
    // MED-05: Result<T>::valueOr() && overload supports move-only types
    auto ptr = std::make_unique<int>(123);
    Result<std::unique_ptr<int>> okRes(std::move(ptr));
    auto val = std::move(okRes).valueOr(nullptr);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(*val, 123);

    Result<std::unique_ptr<int>> errRes(Status::notFound("missing"));
    auto fallback = std::make_unique<int>(456);
    auto valFallback = std::move(errRes).valueOr(std::move(fallback));
    ASSERT_NE(valFallback, nullptr);
    EXPECT_EQ(*valFallback, 456);
}

TEST(LoggerTest, LoggerSingletonCopyMoveDeleted) {
    // LOW-02: Logger copy/move explicitly deleted
    static_assert(!std::is_copy_constructible_v<Logger>);
    static_assert(!std::is_copy_assignable_v<Logger>);
    static_assert(!std::is_move_constructible_v<Logger>);
    static_assert(!std::is_move_assignable_v<Logger>);
}

TEST(LoggerTest, ShortCircuitEvaluationWhenLevelDisabled) {
    // MED-02: IMAGINE_LOG_* macros must not evaluate arguments when level is disabled
    auto& logger = Logger::instance();
    auto origLevel = logger.level();
    logger.setLevel(LogLevel::Warn);

    EXPECT_FALSE(logger.shouldLog(LogLevel::Debug));
    EXPECT_FALSE(logger.shouldLog(LogLevel::Info));
    EXPECT_TRUE(logger.shouldLog(LogLevel::Warn));
    EXPECT_TRUE(logger.shouldLog(LogLevel::Error));

    int sideEffects = 0;
    auto expensiveFunc = [&sideEffects]() -> std::string {
        ++sideEffects;
        return "expensive output";
    };

    IMAGINE_LOG_DEBUG("Debug: " + expensiveFunc());
    IMAGINE_LOG_INFO("Info: " + expensiveFunc());
    EXPECT_EQ(sideEffects, 0);

    IMAGINE_LOG_WARN("Warn: " + expensiveFunc());
    EXPECT_EQ(sideEffects, 1);

    logger.setLevel(origLevel);
}

TEST(LoggerTest, TextLogEscapingInjection) {
    // MED-04: formatText escapes newlines, carriage returns, and control chars
    auto& logger = Logger::instance();
    auto origLevel = logger.level();
    auto origFormat = logger.fileFormat();
    auto origConsole = logger.isConsoleEnabled();

    auto tempDir = std::filesystem::temp_directory_path() /
        ("imagine_injection_test_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(tempDir);
    auto logFilePath = tempDir / "inject.log";

    logger.setLevel(LogLevel::Info);
    logger.setConsoleEnabled(false);
    logger.setFileFormat(LogFormat::Text);
    ASSERT_TRUE(logger.setLogFile(pathToUtf8(logFilePath)));

    const std::string malicious = "user input\n[INFO ] [fake.cpp:1] forged admin log\r\x1b[31minjected";
    IMAGINE_LOG_INFO(malicious);
    logger.closeLogFile();

    {
        std::ifstream ifs(logFilePath, std::ios::binary);
        ASSERT_TRUE(ifs.is_open());
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        // Raw newline from malicious string must NOT create a separate log line
        EXPECT_EQ(content.find("user input\n[INFO ]"), std::string::npos);
        EXPECT_NE(content.find("user input\\n[INFO ] [fake.cpp:1] forged admin log\\r\\x1b[31minjected"), std::string::npos);
    }

    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
    logger.setConsoleEnabled(origConsole);
    logger.setFileFormat(origFormat);
    logger.setLevel(origLevel);
}

TEST(LoggerTest, PruneBackupsWithUnicodeSiblingFilesAndMaxFilesZero) {
    // HIGH-03 & MED-03: Pruning handles Unicode sibling files without throwing std::system_error,
    // and maxFiles == 0 prunes old backups.
    auto& logger = Logger::instance();
    auto origLevel = logger.level();
    auto origFormat = logger.fileFormat();
    auto origConsole = logger.isConsoleEnabled();
    auto origMaxFileSize = logger.maxFileSize();
    auto origMaxFiles = logger.maxFiles();

    auto tempDir = std::filesystem::temp_directory_path() /
        ("imagine_unicode_prune_test_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(tempDir);
    auto logFilePath = tempDir / "app.log";

    // Create sibling files with Unicode characters (Cyrillic, CJK, Emoji)
    {
        std::ofstream ofs1(tempDir / pathFromUtf8("Логи_Папка.txt"));
        ofs1 << "test";
        std::ofstream ofs2(tempDir / pathFromUtf8("東京_backup.txt"));
        ofs2 << "test";
    }

    // Create stale backups
    for (int i = 1; i <= 5; ++i) {
        std::ofstream ofs(tempDir / ("app.log." + std::to_string(i)));
        ofs << "backup " << i;
    }

    // Opening with maxFiles = 0 should prune all old backups (index > 0)
    ASSERT_TRUE(logger.setLogFile(pathToUtf8(logFilePath), 1024 * 1024, 0));

    std::error_code ec;
    for (int i = 1; i <= 5; ++i) {
        EXPECT_FALSE(std::filesystem::exists(tempDir / ("app.log." + std::to_string(i)), ec));
    }
    // Unicode siblings must still be intact
    EXPECT_TRUE(std::filesystem::exists(tempDir / pathFromUtf8("Логи_Папка.txt"), ec));
    EXPECT_TRUE(std::filesystem::exists(tempDir / pathFromUtf8("東京_backup.txt"), ec));

    // Now test setRotation pruning immediately
    for (int i = 1; i <= 5; ++i) {
        std::ofstream ofs(tempDir / ("app.log." + std::to_string(i)));
        ofs << "new backup " << i;
    }
    logger.setRotation(1024 * 1024, 2);
    // .1, .2 remain, .3, .4, .5 pruned
    EXPECT_TRUE(std::filesystem::exists(tempDir / "app.log.1", ec));
    EXPECT_TRUE(std::filesystem::exists(tempDir / "app.log.2", ec));
    EXPECT_FALSE(std::filesystem::exists(tempDir / "app.log.3", ec));
    EXPECT_FALSE(std::filesystem::exists(tempDir / "app.log.4", ec));
    EXPECT_FALSE(std::filesystem::exists(tempDir / "app.log.5", ec));

    logger.closeLogFile();
    std::filesystem::remove_all(tempDir, ec);
    logger.setConsoleEnabled(origConsole);
    logger.setFileFormat(origFormat);
    logger.setRotation(origMaxFileSize, origMaxFiles);
    logger.setLevel(origLevel);
}

TEST(TypesTest, SanitizeUtf8Rfc3629Strictness) {
    // HIGH-01: sanitizeUtf8 enforces RFC 3629 shortest-form and surrogate bounds
    const std::string replacement = "\xef\xbf\xbd";

    // 2-byte overlongs (0xC0, 0xC1)
    EXPECT_EQ(sanitizeUtf8(std::string("\xc0\x80", 2)), replacement + replacement);
    EXPECT_EQ(sanitizeUtf8(std::string("\xc1\xbf", 2)), replacement + replacement);

    // Valid 2-byte UTF-8 starts at 0xC2
    EXPECT_EQ(sanitizeUtf8("\xc2\xa9"), "\xc2\xa9"); // ©

    // 3-byte overlongs (0xE0 with byte 2 < 0xA0)
    EXPECT_EQ(sanitizeUtf8(std::string("\xe0\x80\x80", 3)), replacement + replacement + replacement);
    EXPECT_EQ(sanitizeUtf8(std::string("\xe0\x9f\xbf", 3)), replacement + replacement + replacement);
    EXPECT_EQ(sanitizeUtf8("\xe0\xa0\x80"), "\xe0\xa0\x80"); // valid lowest 3-byte

    // UTF-16 surrogate halves (0xED with byte 2 >= 0xA0)
    EXPECT_EQ(sanitizeUtf8(std::string("\xed\xa0\x80", 3)), replacement + replacement + replacement); // U+D800
    EXPECT_EQ(sanitizeUtf8(std::string("\xed\xbf\xbf", 3)), replacement + replacement + replacement); // U+DFFF
    EXPECT_EQ(sanitizeUtf8("\xed\x9f\xbf"), "\xed\x9f\xbf"); // valid just before surrogates (U+D7FF)

    // 4-byte overlongs (0xF0 with byte 2 < 0x90)
    EXPECT_EQ(sanitizeUtf8(std::string("\xf0\x80\x80\x80", 4)), replacement + replacement + replacement + replacement);
    EXPECT_EQ(sanitizeUtf8(std::string("\xf0\x8f\xbf\xbf", 4)), replacement + replacement + replacement + replacement);
    EXPECT_EQ(sanitizeUtf8("\xf0\x90\x80\x80"), "\xf0\x90\x80\x80"); // valid lowest 4-byte (U+10000)

    // Codepoints > U+10FFFF (0xF4 with byte 2 >= 0x90, 0xF5..0xFF)
    EXPECT_EQ(sanitizeUtf8(std::string("\xf4\x90\x80\x80", 4)), replacement + replacement + replacement + replacement);
    EXPECT_EQ(sanitizeUtf8(std::string("\xf5\x80\x80\x80", 4)), replacement + replacement + replacement + replacement);
    EXPECT_EQ(sanitizeUtf8("\xf4\x8f\xbf\xbf"), "\xf4\x8f\xbf\xbf"); // valid maximum codepoint U+10FFFF
}

TEST(TypesTest, StripExtendedPrefixAndUnc) {
    // HIGH-02 & LOW-03: stripExtendedPrefix bounds check and UNC normalization
#if defined(_WIN32)
    EXPECT_EQ(stripExtendedPrefix(std::filesystem::path(L"\\\\?\\C:\\Users")).wstring(), L"C:\\Users");
    // Short path of length 5: must not read past ws.size()
    EXPECT_EQ(stripExtendedPrefix(std::filesystem::path(L"\\\\?\\C")).wstring(), L"\\\\?\\C");
    // UNC path
    EXPECT_EQ(stripExtendedPrefix(std::filesystem::path(L"\\\\?\\UNC\\server\\share\\photo.jpg")).wstring(),
              L"\\\\server\\share\\photo.jpg");
#else
    EXPECT_EQ(stripExtendedPrefix(std::filesystem::path("/home/user")).string(), "/home/user");
#endif
}

TEST(TypesTest, PathFromUtf8StringView) {
    // MED-06: pathFromUtf8 accepts string_view without temporary std::string
    std::string_view sv = "photos/summer2026/beach.jpg";
    auto p = pathFromUtf8(sv);
    EXPECT_EQ(pathToUtf8(p), "photos/summer2026/beach.jpg");
}

TEST(TypesTest, JsonSafeNullsAndBadTypes) {
    // HIGH-04: from_json handles nulls, unexpected types, and bad enum values safely
    nlohmann::json jExif = {
        {"camera_make", nullptr},
        {"camera_model", nullptr},
        {"date_taken", nullptr},
        {"exposure_time", nullptr},
        {"has_gps", nullptr}
    };
    ExifData exif = jExif.get<ExifData>();
    EXPECT_EQ(exif.camera_make, "");
    EXPECT_EQ(exif.date_taken, 0);
    EXPECT_DOUBLE_EQ(exif.exposure_time, 0.0);
    EXPECT_FALSE(exif.has_gps);

    nlohmann::json jMedia = {
        {"id", nullptr},
        {"file_path", nullptr},
        {"flag", 42}, // invalid enum value
        {"exif", nullptr},
        {"tags", nullptr},
        {"caption", nullptr},
        {"duration", nullptr}
    };
    MediaItem m;
    m.flag = FlagState::Pick;
    Tag sampleTag;
    sampleTag.id = 1;
    sampleTag.name = "Existing";
    m.tags.push_back(sampleTag);
    from_json(jMedia, m);

    EXPECT_EQ(m.id, 0);
    EXPECT_EQ(m.file_path, "");
    EXPECT_EQ(m.flag, FlagState::Unflagged); // clamped to Unflagged
    EXPECT_TRUE(m.tags.empty());             // reset when null
    EXPECT_EQ(m.exif.camera_make, "");

    nlohmann::json jTag = {
        {"id", nullptr},
        {"name", nullptr},
        {"parent_id", nullptr},
        {"cover_media_id", "not_an_int"},
        {"first_date", nullptr}
    };
    Tag t = jTag.get<Tag>();
    EXPECT_EQ(t.id, 0);
    EXPECT_EQ(t.name, "");
    EXPECT_FALSE(t.parent_id.has_value());
    EXPECT_FALSE(t.cover_media_id.has_value());
    EXPECT_FALSE(t.first_date.has_value());

    nlohmann::json jAlbum = {
        {"id", nullptr},
        {"name", nullptr},
        {"is_smart", nullptr},
        {"cover_media_id", nullptr}
    };
    Album a = jAlbum.get<Album>();
    EXPECT_EQ(a.id, 0);
    EXPECT_EQ(a.name, "");
    EXPECT_FALSE(a.is_smart);
    EXPECT_FALSE(a.cover_media_id.has_value());
}
