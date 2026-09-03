# Imagine — Photo Organizer Engine & UI

**Imagine** is a modern, high-performance C++20 photo catalog and organizer inspired by **Adobe Photoshop Elements Organizer**.

It features a multi-threaded cataloging engine, SQLite metadata store (with WAL mode), SHA-256 deduplication, dual-resolution thumbnail generation, rich search/query APIs, an interactive CLI, and an embedded local Web UI with zero external runtime dependencies.

> 📖 **Looking for full documentation?** Check out the [Comprehensive User Guide](docs/USER_GUIDE.md) for detailed tutorials, keyboard shortcuts, CLI options, REST API reference, and architecture details.

---

## Key Features

- **Photoshop Elements-Inspired Web UI**:
  - **Media Grid**: Smooth, responsive thumbnail layout with dynamic zoom slider.
  - **Timeline Scrubber**: Interactive histogram of photos by year and month.
  - **Sidebars**: Collapsible Folders tree, Standard/Smart Albums, and Tag categories (People, Places, Events, Keywords).
  - **Inspector Panel**: EXIF breakdown (camera, lens, shutter speed, aperture, ISO, focal length, GPS map link), 5-star ratings, pick/reject flags, and tag editor.
  - **Fullscreen Loupe / Slideshow**: Keyboard-driven full-resolution viewer (0–5 star ratings, P/X flags, Arrow navigation).
- **High-Throughput Importer**:
  - Multi-threaded scanning and processing pipeline (`std::jthread` thread pool).
  - Fast change-detection skipping unchanged files.
  - Automatic SHA-256 cryptographic content hashing for deduplication.
  - Robust EXIF parsing with fallback to filesystem timestamps.
  - Dual-resolution JPEG thumbnail caching (256px small and 1024px large).
- **SQLite Catalog Engine**:
  - RAII transactions, parameterized queries, and WAL mode for high concurrency.
  - Filter by ratings, pick/reject flags, date ranges, camera/lens, tags, and search text.
- **Self-Contained & Zero-Dependency**:
  - All third-party C/C++ libraries (`sqlite3`, `stb`, `easyexif`, `cpp-httplib`, `nlohmann_json`, `googletest`) are automatically managed via CMake `FetchContent`.
  - Built-in Web UI runs locally on any browser without node/npm or native GUI packages.

---

## Quick Start

### 1. Build
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

To build with AddressSanitizer and UndefinedBehaviorSanitizer:
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
cmake --build build -j$(nproc)
```

### 2. Run Tests
```bash
ctest --test-dir build --output-on-failure
# Or directly:
./build/imagine_tests
```

### 3. Import Photos
```bash
./build/imagine import /path/to/photos --catalog catalog.db
```

### 4. Start the Web Organizer
```bash
./build/imagine serve --catalog catalog.db --port 8080
```
Open [http://localhost:8080](http://localhost:8080) in your browser!

### 5. Query via CLI
```bash
# Display catalog statistics
./build/imagine stats --catalog catalog.db

# List photos (with optional rating or search filter)
./build/imagine list --catalog catalog.db --rating 4

# Tag a photo
./build/imagine tag 1 "SummerVacation" --category events --catalog catalog.db
```

---

## Project Structure

```
imagine/
├── CMakeLists.txt              # Root build configuration
├── cmake/
│   └── Dependencies.cmake      # FetchContent for SQLite, STB, EasyEXIF, httplib, JSON, GTest
├── include/imagine/
│   ├── common/                 # Types, Error, Logger
│   ├── db/                     # SQLite RAII Connection, Schema, CatalogDb
│   ├── metadata/               # SHA-256 Hasher, EXIF Reader
│   ├── thumbnail/              # Resizer, Rotator, Disk Cache
│   ├── concurrency/            # ThreadPool
│   ├── core/                   # Importer, Query Engine, Catalog Facade
│   └── server/                 # REST API Router, Web Server
├── src/                        # Implementations for all library modules
├── apps/
│   └── imagine_cli.cpp         # CLI application (import, serve, list, stats, tag)
├── web/                        # Photoshop Elements-styled SPA (HTML, CSS, JS)
│   ├── index.html
│   ├── app.css
│   └── app.js
└── tests/                      # 30 Unit tests across 7 test suites
```
