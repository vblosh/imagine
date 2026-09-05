# Imagine — Photo Organizer Engine & UI

[![CI](https://github.com/vblosh/imagine/actions/workflows/ci.yml/badge.svg)](https://github.com/vblosh/imagine/actions/workflows/ci.yml)

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
# Run all tests (unit tests and Playwright UI tests):
ctest --test-dir build --output-on-failure

# Or run unit tests directly:
./build/imagine_tests

# Or run the deterministic UI test suite directly:
./tests/ui/run_ui_tests.sh
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
├── docs/
│   ├── USER_GUIDE.md           # Comprehensive user guide, CLI & REST API reference
│   └── dev/                    # Developer documentation & UI test plans
├── include/imagine/            # Public C++20 header architecture
│   ├── common/                 # Types, Error, Logger
│   ├── concurrency/            # ThreadPool (std::jthread)
│   ├── core/                   # Importer, Query Engine, Catalog Facade
│   ├── db/                     # SQLite RAII Connection, Schema, CatalogDb
│   ├── metadata/               # SHA-256 Hasher, EXIF Reader
│   ├── server/                 # REST API Router, Web Server, MIME Types
│   └── thumbnail/              # Resizer, Rotator, Disk Cache
├── src/                        # Implementations for all C++ library modules
├── apps/
│   └── imagine_cli.cpp         # CLI application (import, serve, list, stats, tag)
├── web/                        # Photoshop Elements-styled modular SPA
│   ├── index.html              # HTML5 application entry point
│   ├── app.css                 # Dark theme stylesheet & CSS variables
│   ├── app.js                  # Application bootstrap & lifecycle wiring
│   ├── api.js                  # REST API client & response schema normalization
│   ├── state.js                # Central reactive application state & GPS cache
│   ├── dom.js                  # DOM element caching, validation & toast alerts
│   ├── media-grid.js           # Media grid rendering, card selection & pagination
│   ├── inspector.js            # Metadata inspector & resizable side panels
│   ├── map-view.js             # Leaflet map, spatial clustering & photo pinning
│   ├── loupe.js                # Fullscreen viewer with pan/zoom & slideshow
│   ├── modals.js               # Modal dialogs (Import, Album, Tag, Delete)
│   ├── keyboard.js             # Keyboard shortcuts & priority Escape manager
│   └── vendor/leaflet/         # Embedded Leaflet mapping assets
├── tests/                      # Automated test suites
│   ├── ui/                     # 110 deterministic Playwright UI tests (18 test suites)
│   ├── fixtures/               # Test image assets (JPEG with EXIF, etc.)
│   ├── web_server_benchmark.py # Web server concurrency benchmark script
│   └── *.cpp                   # 81 C++ unit tests across 14 test suites
└── sample_photos/              # Sample images for testing & quick evaluation
```
