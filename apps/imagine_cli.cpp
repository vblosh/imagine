#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <csignal>
#include <chrono>
#include <thread>
#include <filesystem>
#include "imagine/core/catalog.hpp"
#include "imagine/core/query.hpp"
#include "imagine/server/web_server.hpp"
#include "imagine/thumbnail/cache.hpp"
#include "imagine/common/logger.hpp"

namespace {

static imagine::server::WebServer* g_serverInstance{nullptr};

void sigHandler(int) {
    std::cout << "\nShutting down server...\n";
    if (g_serverInstance) {
        g_serverInstance->stop();
    }
}

std::string formatBytes(int64_t bytes) {
    if (bytes <= 0) return "0 B";
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unitIdx = 0;
    double d = static_cast<double>(bytes);
    while (d >= 1024.0 && unitIdx < 4) {
        d /= 1024.0;
        unitIdx++;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << d << " " << units[unitIdx];
    return oss.str();
}

std::string formatUnixTime(int64_t sec) {
    if (sec <= 0) return "Unknown";
    std::chrono::sys_seconds tp{std::chrono::seconds{sec}};
    std::chrono::sys_days sd = std::chrono::floor<std::chrono::days>(tp);
    std::chrono::year_month_day ymd{sd};
    auto timeOfDay = tp - sd;
    auto h = std::chrono::duration_cast<std::chrono::hours>(timeOfDay);
    auto m = std::chrono::duration_cast<std::chrono::minutes>(timeOfDay - h);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u %02d:%02d",
                  static_cast<int>(ymd.year()),
                  static_cast<unsigned>(ymd.month()),
                  static_cast<unsigned>(ymd.day()),
                  static_cast<int>(h.count()),
                  static_cast<int>(m.count()));
    return std::string(buf);
}

void printHelp() {
    std::cout << R"(
Imagine Photo Organizer CLI
Usage:
  imagine <command> [options]

Commands:
  import <path>       Import photos from a folder into the catalog
  serve               Start the embedded HTTP web server and REST API
  relocate            Make photo and thumbnail paths in catalog relative
  list                List and query photos in the catalog
  stats               Display catalog statistics and metrics
  tag <id> <name>     Attach a keyword tag to a photo
  geotag <id> <lat> <lon> Set GPS coordinates for a photo
  delete <id...>      Delete one or more photos from the catalog
  --help, -h          Show this help message

Options for 'import':
  --catalog <db>      Path to SQLite catalog database (default: catalog.db)
  --photos-dir <dir>  Base root directory for photos (default: <path>)
  --thumbs-dir <dir>  Path to thumbnail cache directory (default: system cache)
  --thumbs <dir>      Alias for --thumbs-dir
  --recursive         Recursively scan subdirectories (default: true)
  --no-recursive      Do not scan subdirectories
  --threads <N>       Number of worker threads (default: hardware concurrency)

Options for 'serve':
  --catalog <db>      Path to SQLite catalog database (default: catalog.db)
  --photos-dir <dir>  Path to photos directory (default: current/catalog directory)
  --thumbs-dir <dir>  Path to thumbnail cache directory (default: system cache)
  --thumbs <dir>      Alias for --thumbs-dir
  --host <ip>         Host address to bind to (default: 0.0.0.0)
  --port <port>       Port number to listen on (default: 8080)
  --web-dir <dir>     Path to directory containing web UI assets (default: web)

Options for 'relocate':
  --catalog <db>      Path to SQLite catalog database (default: catalog.db)
  --photos-dir <dir>  Base root directory to make photo paths relative to
  --thumbs-dir <dir>  Base root directory to make thumbnail paths relative to
  --thumbs <dir>      Alias for --thumbs-dir

Options for 'list':
  --catalog <db>      Path to SQLite catalog database (default: catalog.db)
  --rating <N>        Filter by minimum star rating (0-5)
  --search <text>     Search keyword in filename, camera, lens, or tags
  --gps               Filter only photos with GPS coordinates
  --limit <N>         Maximum items to display (default: 50)
  --offset <N>        Offset pagination (default: 0)

Options for 'geotag':
  --catalog <db>      Path to SQLite catalog database (default: catalog.db)
  --altitude <alt>    Altitude in meters (default: 0.0)
  --clear             Remove GPS coordinates from photo

Options for 'tag':
  --catalog <db>      Path to SQLite catalog database (default: catalog.db)
  --category <cat>    Tag category: keyword, people, places, events (default: keyword)

Options for 'delete':
  --catalog <db>      Path to SQLite catalog database (default: catalog.db)
  --yes, -y           Skip confirmation prompt
  --rejected          Delete all photos marked as Rejected

Options for 'stats':
  --catalog <db>      Path to SQLite catalog database (default: catalog.db)
)" << std::endl;
}

int handleImport(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Error: 'import' requires a directory path.\n";
        std::cerr << "Usage: imagine import <path> [--catalog <db>] [--thumbs <dir>] [--recursive] [--threads N]\n";
        return 1;
    }

    std::string path = argv[2];
    std::string catalogDb = "catalog.db";
    std::string photosDir = "";
    std::string thumbsDir = "";
    bool recursive = true;
    size_t threads = std::max(1u, std::thread::hardware_concurrency());

    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--catalog" && i + 1 < argc) {
            catalogDb = argv[++i];
        } else if (arg == "--photos-dir" && i + 1 < argc) {
            photosDir = argv[++i];
        } else if ((arg == "--thumbs-dir" || arg == "--thumbs") && i + 1 < argc) {
            thumbsDir = argv[++i];
        } else if (arg == "--threads" && i + 1 < argc) {
            threads = static_cast<size_t>(std::stoul(argv[++i]));
        } else if (arg == "--recursive") {
            recursive = true;
        } else if (arg == "--no-recursive") {
            recursive = false;
        }
    }

    if (!std::filesystem::exists(path)) {
        std::cerr << "Error: Directory does not exist: " << path << "\n";
        return 1;
    }

    if (photosDir.empty()) {
        photosDir = path;
    } else {
        std::string normPath = imagine::core::Importer::normalizePath(path);
        std::string normRoot = imagine::core::Importer::normalizePath(photosDir);
        if (normPath != normRoot && !imagine::core::Importer::isInsideRootDir(normPath, normRoot)) {
            std::cerr << "Error: Directory '" << path << "' is outside photos root directory '" << photosDir << "'\n";
            return 1;
        }
    }

    imagine::core::Catalog catalog(threads);
    auto status = catalog.open(catalogDb, thumbsDir, photosDir);
    if (!status.isOk()) {
        std::cerr << "Error opening catalog: " << status.message() << "\n";
        return 1;
    }

    std::cout << "Starting import of '" << path << "' using " << threads << " threads...\n";

    auto start = std::chrono::steady_clock::now();
    auto res = catalog.importDirectory(path, recursive, [](const imagine::core::ImportProgress& p) {
        std::cout << "\r[Processed: " << p.processed_files.load()
                  << " / " << p.total_files.load()
                  << " | Imported: " << p.imported_files.load()
                  << " | Skipped: " << p.skipped_files.load()
                  << " | Failed: " << p.failed_files.load() << "] "
                  << std::flush;
    });

    auto end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "\n";
    if (!res.isOk()) {
        std::cerr << "Import failed: " << res.status().message() << "\n";
        return 1;
    }

    const auto& prog = res.value();
    std::cout << "\n================ Import Summary ================\n"
              << "  Total Scanned: " << prog.total_files.load() << "\n"
              << "  Imported:      " << prog.imported_files.load() << "\n"
              << "  Skipped:       " << prog.skipped_files.load() << "\n"
              << "  Failed:        " << prog.failed_files.load() << "\n"
              << "  Time Elapsed:  " << (elapsed / 1000.0) << " seconds\n"
              << "================================================\n";

    return 0;
}

int handleServe(int argc, char** argv) {
    std::string catalogDb = "catalog.db";
    std::string photosDir = "";
    std::string thumbsDir = "";
    std::string host = "0.0.0.0";
    int port = 8080;
    std::string webDir = "web";

    const char* envPhotos = std::getenv("IMAGINE_PHOTOS_DIR");
    if (envPhotos && *envPhotos) {
        photosDir = envPhotos;
    }
    const char* envThumbs = std::getenv("IMAGINE_THUMBS_DIR");
    if (envThumbs && *envThumbs) {
        thumbsDir = envThumbs;
    }

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--catalog" && i + 1 < argc) {
            catalogDb = argv[++i];
        } else if (arg == "--photos-dir" && i + 1 < argc) {
            photosDir = argv[++i];
        } else if ((arg == "--thumbs-dir" || arg == "--thumbs") && i + 1 < argc) {
            thumbsDir = argv[++i];
        } else if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--web-dir" && i + 1 < argc) {
            webDir = argv[++i];
        }
    }


    imagine::core::Catalog catalog;
    auto status = catalog.open(catalogDb, thumbsDir, photosDir);
    if (!status.isOk()) {
        std::cerr << "Error opening catalog database: " << status.message() << "\n";
        return 1;
    }

    imagine::server::WebServer webServer(catalog, webDir);
    g_serverInstance = &webServer;

    std::signal(SIGINT, sigHandler);
    std::signal(SIGTERM, sigHandler);

    std::cout << R"(
======================================================
  IMAGINE Photo Organizer Web Server
======================================================
  URL:        http://)" << (host == "0.0.0.0" ? "localhost" : host) << ":" << port << R"(
  Web Dir:    )" << webDir << R"(
  Catalog DB: )" << catalogDb << R"(
  Photos Dir: )" << (photosDir.empty() ? "(none / relative to catalog)" : photosDir) << R"(
  Cache Dir:  )" << (thumbsDir.empty() ? imagine::thumbnail::Cache::defaultCacheDir() : thumbsDir) << R"(
======================================================
  Press Ctrl+C to stop the server.
)" << std::endl;

    auto startStatus = webServer.run(host, port, webDir);
    if (!startStatus.isOk()) {
        std::cerr << "Server error: " << startStatus.message() << "\n";
        return 1;
    }

    return 0;
}

int handleList(int argc, char** argv) {
    std::string catalogDb = "catalog.db";
    imagine::core::QueryCriteria criteria;
    criteria.limit = 50;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--catalog" && i + 1 < argc) {
            catalogDb = argv[++i];
        } else if (arg == "--rating" && i + 1 < argc) {
            criteria.min_rating = std::stoi(argv[++i]);
        } else if (arg == "--search" && i + 1 < argc) {
            criteria.search_text = argv[++i];
        } else if (arg == "--gps") {
            criteria.has_gps = true;
        } else if (arg == "--limit" && i + 1 < argc) {
            criteria.limit = std::stoi(argv[++i]);
        } else if (arg == "--offset" && i + 1 < argc) {
            criteria.offset = std::stoi(argv[++i]);
        }
    }

    imagine::core::Catalog catalog;
    auto status = catalog.open(catalogDb);
    if (!status.isOk()) {
        std::cerr << "Error opening catalog database: " << status.message() << "\n";
        return 1;
    }

    auto res = catalog.query(criteria);
    if (!res.isOk()) {
        std::cerr << "Query failed: " << res.status().message() << "\n";
        return 1;
    }

    const auto& qr = res.value();
    std::cout << "\nFound " << qr.total_count << " photos (showing " << qr.items.size() << "):\n\n";

    bool showGps = criteria.has_gps.value_or(false);
    std::cout << std::setfill(' ') << std::left
              << std::setw(6)  << "ID"
              << std::setw(30) << "File Name"
              << std::setw(8)  << "Rating"
              << std::setw(8)  << "Flag"
              << std::setw(18) << "Date Taken"
              << std::setw(12) << "Size";
    if (showGps) {
        std::cout << std::setw(24) << "GPS (Lat, Lon)";
    }
    std::cout << "Camera\n";
    std::cout << std::string(showGps ? 124 : 100, '-') << "\n";

    for (const auto& item : qr.items) {
        std::string flagStr = (item.flag == imagine::FlagState::Pick) ? "Pick"
                            : (item.flag == imagine::FlagState::Reject) ? "Reject" : "-";
        std::string stars = (item.rating > 0) ? (std::to_string(item.rating) + "*") : "-";
        std::string camera = item.exif.camera_make.empty() ? "-"
                           : (item.exif.camera_make + " " + item.exif.camera_model);

        std::string fname = item.file_name;
        if (fname.size() > 28) {
            fname = fname.substr(0, 25) + "...";
        }

        std::cout << std::left
                  << std::setw(6)  << item.id
                  << std::setw(30) << fname
                  << std::setw(8)  << stars
                  << std::setw(8)  << flagStr
                  << std::setw(18) << formatUnixTime(item.date_taken)
                  << std::setw(12) << formatBytes(item.file_size);
        if (showGps) {
            std::string gpsStr = "-";
            if (item.exif.has_gps) {
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(4) << item.exif.latitude << ", " << item.exif.longitude;
                gpsStr = ss.str();
            }
            std::cout << std::setw(24) << gpsStr;
        }
        std::cout << camera << "\n";
    }
    std::cout << "\n";
    return 0;
}

int handleStats(int argc, char** argv) {
    std::string catalogDb = "catalog.db";

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--catalog" && i + 1 < argc) {
            catalogDb = argv[++i];
        }
    }

    imagine::core::Catalog catalog;
    auto status = catalog.open(catalogDb);
    if (!status.isOk()) {
        std::cerr << "Error opening catalog database: " << status.message() << "\n";
        return 1;
    }

    auto statsRes = catalog.getStats();
    if (!statsRes.isOk()) {
        std::cerr << "Failed to retrieve statistics: " << statsRes.status().message() << "\n";
        return 1;
    }

    const auto& s = statsRes.value();
    std::cout << "\n============= Catalog Statistics =============\n"
              << "  Catalog Database:   " << catalogDb << "\n"
              << "  Total Photos:       " << s.total_media << "\n"
              << "  Total Disk Size:    " << formatBytes(s.total_size_bytes) << "\n"
              << "  Total Tags:         " << s.total_tags << "\n"
              << "  Total Albums:       " << s.total_albums << "\n"
              << "  Earliest Photo:     " << formatUnixTime(s.earliest_date) << "\n"
              << "  Latest Photo:       " << formatUnixTime(s.latest_date) << "\n"
              << "==============================================\n\n";

    return 0;
}

int handleTag(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Error: 'tag' requires <media_id> and <tag_name>.\n";
        std::cerr << "Usage: imagine tag <media_id> <tag_name> [--category <cat>] [--catalog <db>]\n";
        return 1;
    }

    imagine::MediaId mediaId = std::stoll(argv[2]);
    std::string tagName = argv[3];
    std::string category = "keyword";
    std::string catalogDb = "catalog.db";

    for (int i = 4; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--category" && i + 1 < argc) {
            category = argv[++i];
        } else if (arg == "--catalog" && i + 1 < argc) {
            catalogDb = argv[++i];
        }
    }

    imagine::core::Catalog catalog;
    auto status = catalog.open(catalogDb);
    if (!status.isOk()) {
        std::cerr << "Error opening catalog database: " << status.message() << "\n";
        return 1;
    }

    auto tagStatus = catalog.addTag(mediaId, tagName, category);
    if (!tagStatus.isOk()) {
        std::cerr << "Failed to add tag: " << tagStatus.message() << "\n";
        return 1;
    }

    std::cout << "Successfully added tag '" << tagName << "' (" << category
              << ") to media ID " << mediaId << ".\n";
    return 0;
}

int handleDelete(int argc, char** argv) {
    std::string catalogDb = "catalog.db";
    bool skipConfirm = false;
    bool deleteRejected = false;
    std::vector<imagine::MediaId> targetIds;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--catalog" && i + 1 < argc) {
            catalogDb = argv[++i];
        } else if (arg == "--yes" || arg == "-y") {
            skipConfirm = true;
        } else if (arg == "--rejected") {
            deleteRejected = true;
        } else if (!arg.empty() && arg[0] != '-') {
            try {
                targetIds.push_back(std::stoll(arg));
            } catch (...) {
                std::cerr << "Error: Invalid media ID: '" << arg << "'. Must be a numeric ID.\n";
                return 1;
            }
        } else {
            std::cerr << "Unknown option: '" << arg << "'\n";
            return 1;
        }
    }

    if (targetIds.empty() && !deleteRejected) {
        std::cerr << "Error: 'delete' requires at least one <media_id> or the --rejected flag.\n";
        std::cerr << "Usage: imagine delete <id...> [--catalog <db>] [--yes] [--rejected]\n";
        return 1;
    }

    imagine::core::Catalog catalog;
    auto status = catalog.open(catalogDb);
    if (!status.isOk()) {
        std::cerr << "Error opening catalog database: " << status.message() << "\n";
        return 1;
    }

    if (deleteRejected) {
        imagine::core::QueryCriteria criteria;
        criteria.flag = imagine::FlagState::Reject;
        criteria.limit = 100000;
        auto qRes = catalog.query(criteria);
        if (!qRes.isOk()) {
            std::cerr << "Error querying rejected photos: " << qRes.status().message() << "\n";
            return 1;
        }
        for (const auto& item : qRes.value().items) {
            targetIds.push_back(item.id);
        }
        if (targetIds.empty()) {
            std::cout << "No photos with 'Reject' flag found in catalog.\n";
            return 0;
        }
    }

    if (!skipConfirm) {
        std::cout << "Are you sure you want to delete " << targetIds.size()
                  << " photo(s) from catalog database '" << catalogDb << "'? [y/N]: ";
        std::string ans;
        if (!std::getline(std::cin, ans) || (ans != "y" && ans != "Y" && ans != "yes" && ans != "YES")) {
            std::cout << "Operation cancelled.\n";
            return 0;
        }
    }

    int successCount = 0;
    int failCount = 0;
    for (auto id : targetIds) {
        auto delStatus = catalog.deleteMedia(id);
        if (delStatus.isOk()) {
            successCount++;
        } else {
            failCount++;
            std::cerr << "Failed to delete photo ID " << id << ": " << delStatus.message() << "\n";
        }
    }

    std::cout << "Successfully deleted " << successCount << " photo(s) from catalog.";
    if (failCount > 0) {
        std::cout << " (" << failCount << " failed)";
    }
    std::cout << "\n";

    return (failCount == 0) ? 0 : 1;
}

int handleGeotag(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Error: 'geotag' requires photo ID. Usage: imagine geotag <id> <lat> <lon> [--altitude <alt>] [--catalog <db>] or imagine geotag <id> --clear\n";
        return 1;
    }

    imagine::MediaId id = 0;
    try {
        id = std::stoll(argv[2]);
    } catch (...) {
        std::cerr << "Error: Invalid photo ID: '" << argv[2] << "'. Must be a numeric ID.\n";
        return 1;
    }

    std::string catalogDb = "catalog.db";
    bool clearGps = false;
    double lat = 0.0;
    double lon = 0.0;
    double alt = 0.0;
    bool hasCoords = false;

    int argIdx = 3;
    if (argIdx < argc && argv[argIdx][0] != '-') {
        try {
            lat = std::stod(argv[argIdx++]);
            if (argIdx < argc && argv[argIdx][0] != '-') {
                lon = std::stod(argv[argIdx++]);
                hasCoords = true;
            }
        } catch (...) {
            std::cerr << "Error: Invalid latitude/longitude values.\n";
            return 1;
        }
    }

    for (int i = argIdx; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--catalog" && i + 1 < argc) {
            catalogDb = argv[++i];
        } else if (arg == "--altitude" && i + 1 < argc) {
            try {
                alt = std::stod(argv[++i]);
            } catch (...) {}
        } else if (arg == "--clear") {
            clearGps = true;
        }
    }

    if (!clearGps && !hasCoords) {
        std::cerr << "Error: Latitude and longitude must be provided, or specify --clear.\n";
        std::cerr << "Usage: imagine geotag <id> <lat> <lon> [--altitude <alt>] [--catalog <db>] or imagine geotag <id> --clear\n";
        return 1;
    }

    imagine::core::Catalog catalog;
    auto status = catalog.open(catalogDb);
    if (!status.isOk()) {
        std::cerr << "Error opening catalog database: " << status.message() << "\n";
        return 1;
    }

    bool hasGps = !clearGps;
    auto s = catalog.setGps(id, hasGps, lat, lon, alt);
    if (!s.isOk()) {
        std::cerr << "Error updating GPS for photo ID " << id << ": " << s.message() << "\n";
        return 1;
    }

    if (clearGps) {
        std::cout << "Cleared GPS coordinates for photo ID " << id << ".\n";
    } else {
        std::cout << "Updated GPS for photo ID " << id << ": " << lat << ", " << lon << " (" << alt << " m)\n";
    }
    return 0;
}

int handleRelocate(int argc, char** argv) {
    std::string catalogDb = "catalog.db";
    std::string photosDir = "";
    std::string thumbsDir = "";

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--catalog" && i + 1 < argc) {
            catalogDb = argv[++i];
        } else if (arg == "--photos-dir" && i + 1 < argc) {
            photosDir = argv[++i];
        } else if ((arg == "--thumbs-dir" || arg == "--thumbs") && i + 1 < argc) {
            thumbsDir = argv[++i];
        }
    }

    if (photosDir.empty() && thumbsDir.empty()) {
        std::cerr << "Error: 'relocate' requires --photos-dir or --thumbs-dir.\n";
        std::cerr << "Usage: imagine relocate [--catalog <db>] --photos-dir <dir> [--thumbs-dir <dir>]\n";
        return 1;
    }

    imagine::core::Catalog catalog;
    auto status = catalog.open(catalogDb, thumbsDir, photosDir);
    if (!status.isOk()) {
        std::cerr << "Error opening catalog: " << status.message() << "\n";
        return 1;
    }

    auto res = catalog.makePathsRelative(photosDir, thumbsDir);
    if (!res.isOk()) {
        std::cerr << "Relocation failed: " << res.status().message() << "\n";
        return 1;
    }

    std::cout << "Successfully updated " << res.value() << " media item(s) in catalog database to relative paths.\n";
    return 0;
}

} // anonymous namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printHelp();
        return 0;
    }

    std::string command = argv[1];

    if (command == "--help" || command == "-h" || command == "help") {
        printHelp();
        return 0;
    } else if (command == "import") {
        return handleImport(argc, argv);
    } else if (command == "serve") {
        return handleServe(argc, argv);
    } else if (command == "relocate") {
        return handleRelocate(argc, argv);
    } else if (command == "list") {
        return handleList(argc, argv);
    } else if (command == "stats") {
        return handleStats(argc, argv);
    } else if (command == "tag") {
        return handleTag(argc, argv);
    } else if (command == "geotag") {
        return handleGeotag(argc, argv);
    } else if (command == "delete" || command == "remove") {
        return handleDelete(argc, argv);
    } else {
        std::cerr << "Unknown command: '" << command << "'\n";
        printHelp();
        return 1;
    }
}
