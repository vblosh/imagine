# Imagine — Photo Organizer Engine & UI

[![CI](https://github.com/vblosh/imagine/actions/workflows/ci.yml/badge.svg)](https://github.com/vblosh/imagine/actions/workflows/ci.yml)

**Imagine** is a modern, high-performance C++20 photo catalog and organizer inspired by **Adobe Photoshop Elements Organizer**.

It features a multi-threaded cataloging engine, SQLite metadata store (with WAL mode), SHA-256 deduplication, dual-resolution thumbnail generation, rich search/query APIs, an interactive CLI, an embedded local Web UI with zero external runtime dependencies, and privacy-first local AI engines for **Face Recognition** (InsightFace SCRFD + ArcFace) and **Semantic AI Search & Visual Similarity** (OpenAI CLIP ViT).

> 📖 **Looking for full documentation?** Check out the [Comprehensive User Guide](docs/USER_GUIDE.md) for detailed tutorials, keyboard shortcuts, CLI options, REST API reference, and architecture details.

---

## Key Features

- **Photoshop Elements-Inspired Web and Mobile Web UI**:
  - **Media Grid**: Smooth, responsive thumbnail layout with dynamic zoom slider (120px–360px).
  - **Timeline Scrubber**: Interactive histogram of photos by year and month with range-filtering capabilities.
  - **Sidebars**: Collapsible Folders tree, Standard/Smart Albums, and Tag categories (People, Places, Events, Keywords).
  - **Map View & Geotagging**: Leaflet map view with spatial clustering, photo pinning, and reverse geocoding via OpenStreetMap Nominatim proxy.
  - **Auto Events**: Automatically suggest outings and events from selected photos using local timestamps, GPS coordinates, captions, and existing tags.
  - **Face Review & Naming**: Local face detection, cluster grouping, face crop cards, naming, and review suggestions using existing people tags.
  - **Semantic AI Search**: Natural language search and visual similarity ("Find Similar") powered by OpenAI CLIP via ONNX Runtime.
  - **Inspector Panel**: EXIF breakdown (camera, lens, shutter speed, aperture, ISO, focal length, GPS map link), 5-star ratings, pick/reject flags, face boxes, and tag editor.
  - **Quick Edit**: Non-destructive photo adjustments (crop, rotate, flip, tonal adjustments) directly in the browser.
  - **Fullscreen Loupe / Slideshow**: Keyboard-driven full-resolution viewer (0–5 star ratings, P/X flags, Arrow navigation, zoom & pan).
  - **Multilingual Support (i18n)**: 5 built-in languages (English, Spanish, German, Russian, Chinese) with instant switching, `localStorage` persistence, and URL parameter override (`?lang=...`).
- **Local Face Detection & Recognition Engine**:
  - **100% Offline & Private**: Zero external cloud calls, no telemetry, all processing stays strictly on your machine.
  - **InsightFace Pipeline**: Fast SCRFD-10GF face detection with 5 facial landmarks and ArcFace ResNet-50 producing 512-dimensional normalized embeddings.
  - **Smart Identity Suggestions**: Cosine similarity matching against previously confirmed people tags.
  - **Lazy Model Loading**: Zero overhead (~3–12 MB baseline) until a scan starts; automatic idle unloading (`IMAGINE_FACE_IDLE_UNLOAD_SEC`) frees model memory.
  - **Hardware Acceleration**: Multi-threaded CPU execution or NVIDIA CUDA GPU acceleration (`IMAGINE_FACE_DEVICE=cuda`).
- **CLIP Semantic AI Search & Visual Similarity Engine**:
  - **Natural Language Search**: Search your library using descriptive text (e.g. *"golden retriever puppy on grass"*, *"snowy mountain peak at sunset"*).
  - **Reverse Image Search ("Find Similar")**: Find visually and compositionally related photos with a single click.
  - **Dual ONNX Encoders**: CLIP ViT-B/32 image and text encoders with built-in Byte Pair Encoding (BPE) tokenizer.
  - **Fast In-Memory Vector Search**: Sub-10ms cosine similarity indexing directly in RAM with embeddings persisted in SQLite.
  - **Seamless Filter Fusion**: Combine semantic queries with star ratings, pick/reject flags, media types, and date ranges.
- **High-Throughput Importer**:
  - Multi-threaded scanning and processing pipeline (`std::jthread` thread pool).
  - Support for Photos (JPEG, PNG, WebP, TIFF, BMP), Videos (MP4, MOV, WebM, MKV, AVI, etc.), and Audio (MP3, FLAC, WAV, AAC, etc.).
  - Fast change-detection skipping unchanged files via modification timestamps.
  - Automatic SHA-256 cryptographic content hashing for deduplication.
  - Robust EXIF parsing with fallback to filesystem timestamps.
  - Dual-resolution JPEG thumbnail caching (256px small and 1024px large) with TurboJPEG acceleration when available.
- **SQLite Catalog Engine**:
  - RAII transactions, parameterized queries, and WAL mode for high concurrency.
  - Filter by ratings, pick/reject flags, date ranges, camera/lens, tags, and search text.
- **Self-Contained & Zero-Dependency**:
  - All third-party C/C++ libraries (`sqlite3`, `stb`, `easyexif`, `cpp-httplib`, `nlohmann_json`, `googletest`) are automatically managed via CMake `FetchContent`.
  - Built-in Web UI runs locally on any modern browser without Node.js, npm, or native GUI dependencies.

---

## Face Recognition Engine

Imagine includes an optional, privacy-first local face analysis and recognition pipeline based on **InsightFace 0.7.3 (`buffalo_l`)** using **ONNX Runtime C++ 1.30.0**.

### Pipeline Architecture
1. **Face Detection (SCRFD-10GF)**:
   - High-accuracy detection (`det_10g.onnx`) with 5 facial landmarks (image-left eye, image-right eye, nose, image-left mouth, image-right mouth).
   - Aspect-preserving resize with top-left black padding (640 × 640), RGB NCHW normalization, and greedy NMS (threshold: 0.4).
2. **Alignment & Landmark Transformation**:
   - Pinned 5-landmark similarity transform aligning face crops to standard 112 × 112 orientation.
3. **Face Recognition (ArcFace ResNet-50)**:
   - Deep feature extraction (`w600k_r50.onnx`) generating 512-dimensional L2-normalized feature vectors.
4. **Identity Matching & Suggestions**:
   - Cosine similarity matching against confirmed identities in the catalog.
   - Grouping faces into named people clusters and unconfirmed suggestion queues.

### Memory & Lifecycle Management
- **Startup Probe**: On boot, Imagine only validates model file availability and SHA-256 checksums (~3–12 MB memory footprint).
- **On-Demand Loading**: Heavy ONNX sessions (~400 MB) are initialized only when a scan job runs.
- **Idle Unload Watchdog**: Models automatically unload from RAM after inactivity (configurable via `IMAGINE_FACE_IDLE_UNLOAD_SEC`, default: 30s; set to `0` for immediate unload).
- **Zero Overhead for Review**: Browsing face grids, viewing face crop chips, and confirming names operate directly against SQLite and thumbnail caches without loading ONNX models.

### Standalone Face CLI Tool
Imagine includes a standalone benchmarking and verification CLI executable `face_inference_example`:
```bash
# Run detection and recognition on an image, generating an annotated overlay:
./build/face_inference_example \
  --models /path/to/buffalo_l \
  --image sample_photos/portrait.jpg \
  --device cpu \
  --overlay output_overlay.png
```

See the [Face Analysis Runtime Guide](docs/face-analysis-runtime.md), [Face Analysis REST API](docs/face-analysis-api.md), and [Face Analysis Validation Guide](docs/face-analysis-validation.md) for full details.

---

## CLIP Semantic Search Engine

Imagine provides local, privacy-first natural language photo search and reverse image similarity powered by OpenAI's **CLIP (Contrastive Language-Image Pre-Training)** via ONNX Runtime.

### Capabilities
- **Natural Language Text Search**: Find photos describing scenes, moods, objects, and activities (e.g. *"birthday cake with candles"*, *"vintage red sports car"*, *"golden retriever in the snow"*).
- **Reverse Image Search ("Find Similar")**: One-click visual discovery: select any photo and instantly locate visually and conceptually similar images across your collection.
- **Combined Queries**: Combine semantic search with star ratings (e.g. `rating >= 4`), pick/reject status, media types, and date ranges.
- **Fast In-Memory Vector Search**: Normalized 512-dimensional vector embeddings are indexed in memory for sub-10ms cosine similarity searches across thousands of photos.
- **Built-in Tokenizer**: Fast C++ Byte Pair Encoding (BPE) tokenizer parsing queries without external Python or Hugging Face runtimes.
- **Resource Management**: Reverse image search ("Find Similar") executes directly on indexed vectors with zero model overhead. The text encoder loads on-demand and unloads automatically when idle (`IMAGINE_CLIP_IDLE_UNLOAD_SEC`).

### Model Requirements & Locations
The CLIP engine looks for models in:
- **Linux / macOS**: `~/.cache/imagine/clip_models/`
- **Windows**: `%LOCALAPPDATA%\imagine\clip_models\` (or `<catalog_dir>/clip_models/`)
- Or customize via `IMAGINE_CLIP_MODEL_DIR=/path/to/clip_models`

Required files: `image_encoder.onnx`, `text_encoder.onnx`, `vocab.json`, and `merges.txt`.

For indexing instructions, API endpoints, and configuration, see the [Semantic AI Search Guide](docs/semantic-search.md).

---

## Quick Start

### 1. Build

#### Standard Build (CPU):
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

#### Build with Face Analysis & CLIP Semantic Search:
Requires [ONNX Runtime 1.30.0](https://github.com/microsoft/onnxruntime/releases/tag/v1.30.0) SDK:
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DIMAGINE_ENABLE_FACE_ANALYSIS=ON \
  -DORT_ROOT=/path/to/onnxruntime-1.30.0
cmake --build build -j$(nproc)
```

On Windows (PowerShell / Ninja):
```powershell
cmake -S . -B build -G Ninja `
  -DIMAGINE_ENABLE_FACE_ANALYSIS=ON `
  -DORT_ROOT=C:/SDK/onnxruntime-win-x64-1.30.0
cmake --build build --target imagine face_inference_example imagine_tests
```

#### Debug Build with AddressSanitizer & UndefinedBehaviorSanitizer:
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
cmake --build build -j$(nproc)
```

### 2. Run Tests

```bash
# Run all automated tests via CTest:
ctest --test-dir build --output-on-failure

# Or run C++ unit tests directly (14 test suites):
./build/imagine_tests

# Or run the deterministic Playwright UI test suite (28 test suites):
./tests/ui/run_ui_tests.sh

# Run reference numerical validation against Python InsightFace 0.7.3:
python tools/face_analysis/insightface_reference.py \
  --models /path/to/buffalo_l \
  --image tests/fixtures/test_reference.png \
  --native-example ./build/face_inference_example
```

### 3. Import Media

Import a folder of photos, videos, or audio into an Imagine catalog:
```bash
./build/imagine import /path/to/photos --catalog catalog.db
```

Options:
- `--photos-dir <dir>`: Base directory for relative media storage.
- `--thumbs-dir <dir>`: Custom thumbnail cache directory.
- `--threads <N>`: Worker thread count (default: hardware concurrency).
- `--no-recursive`: Disable scanning nested subfolders.

### 4. Start the Web Organizer

```bash
# Standard server launch:
./build/imagine serve --catalog catalog.db --port 8080

# Launch with Face Recognition and CLIP enabled:
IMAGINE_FACE_MODELS=/path/to/buffalo_l \
IMAGINE_CLIP_MODEL_DIR=/path/to/clip_models \
./build/imagine serve --catalog catalog.db --port 8080
```
Open [http://localhost:8080](http://localhost:8080) in your browser!

### 5. Query via CLI

```bash
# Display catalog statistics and metrics
./build/imagine stats --catalog catalog.db

# List media (filter by rating, type, GPS, or keyword search)
./build/imagine list --catalog catalog.db --rating 4 --type photo

# Tag media items
./build/imagine tag 1 "SummerVacation" --category events --catalog catalog.db

# Set GPS coordinates
./build/imagine geotag 1 37.7749 -122.4194 --catalog catalog.db

# Relocate media and thumbnail paths to relative paths
./build/imagine relocate --catalog catalog.db --photos-dir /path/to/photos

# Delete media from catalog
./build/imagine delete 1 2 3 --catalog catalog.db --yes
```

---

## Environment Variables Reference

Imagine can be configured using environment variables:

| Variable | Default | Description |
|---|---|---|
| `IMAGINE_CATALOG` / `IMAGINE_CATALOG_DB` | `catalog.db` | Path to SQLite catalog database |
| `IMAGINE_PHOTOS_DIR` | `.` | Root photos directory |
| `IMAGINE_THUMBS_DIR` | system cache | Path to thumbnail cache directory |
| `IMAGINE_HOST` | `0.0.0.0` | Host address to bind to |
| `IMAGINE_PORT` | `8080` | Port number to listen on |
| `IMAGINE_WEB_DIR` | `web` | Path to directory containing Web UI static assets |
| `IMAGINE_API_TOKEN` | *(none)* | Optional secret bearer token required for mutating API requests |
| `IMAGINE_LOG_FILE` | *(none)* | Optional path for JSON log output |
| `IMAGINE_LOG_LEVEL` | `info` | Logging verbosity: `debug`, `info`, `warn`, `error`, `none` |
| `IMAGINE_FACE_MODELS` | *(none)* | Path to directory containing `det_10g.onnx` and `w600k_r50.onnx` |
| `IMAGINE_FACE_DEVICE` | `cpu` | Face inference execution provider: `cpu`, `auto`, or `cuda` |
| `IMAGINE_FACE_IDLE_UNLOAD_SEC` | `30` | Seconds before unloading idle face models from RAM (`0` for immediate) |
| `IMAGINE_FACE_MATCH_THRESHOLD` | `0.5` | Cosine similarity threshold for face identity suggestions |
| `IMAGINE_CLIP_MODEL_DIR` | system cache | Path to directory containing CLIP ONNX models and tokenizer files |
| `IMAGINE_CLIP_DEVICE` | `cpu` | CLIP execution provider: `cpu` or `cuda` |
| `IMAGINE_CLIP_IDLE_UNLOAD_SEC` | `30` | Seconds before unloading idle CLIP models from RAM (`0` for immediate) |
| `IMAGINE_FFMPEG_PATH` | `ffmpeg` | Path to FFmpeg binary for video thumbnail extraction |

---

## Project Structure

```
imagine/
├── CMakeLists.txt              # Root build configuration (C++20, ONNX Runtime, Sanitizers)
├── cmake/                      # CMake modules & manifests
│   ├── Dependencies.cmake      # FetchContent (SQLite, STB, EasyEXIF, httplib, JSON, GTest)
│   └── app.manifest            # Windows application manifest (DPI awareness, visual styles)
├── docs/                       # Comprehensive documentation & developer guides
│   ├── USER_GUIDE.md           # User guide, tutorial, CLI options & REST API reference
│   ├── face-analysis-runtime.md# Face analysis runtime setup, model configuration & CPU/CUDA guides
│   ├── face-analysis-api.md    # Face analysis REST API endpoint specification
│   ├── face-analysis-validation.md # InsightFace reference parity validation protocol
│   ├── semantic-search.md      # CLIP Semantic AI search & visual similarity guide
│   └── dev/                    # Developer architectural documentation
│       ├── database_schema.md  # SQLite schema, tables, migrations (v1-v4), indexing
│       ├── map_view.md         # Map View Leaflet integration and reverse geocoding
│       └── ui_test_plan.md     # Deterministic UI test harness architecture
├── include/imagine/            # Public C++20 header architecture
│   ├── clip/                   # CLIP Semantic search engine & service
│   │   ├── engine.hpp          # ONNX Runtime CLIP ViT image/text inference engine
│   │   └── service.hpp         # Vector index, background indexing job & search service
│   ├── faces/                  # Face detection & recognition
│   │   ├── engine.hpp          # SCRFD detector & ArcFace recognizer inference engine
│   │   └── service.hpp         # Face clustering, identity suggestions & job management
│   ├── common/                 # Common utilities (Types, Error, Logger)
│   ├── concurrency/            # ThreadPool (std::jthread worker pool)
│   ├── core/                   # Core catalog facade, importer, query engine & event suggestions
│   ├── db/                     # SQLite RAII Connection, Schema, CatalogDb
│   ├── metadata/               # SHA-256 Hasher, EXIF Reader, Media Reader
│   ├── server/                 # REST API Router, Web Server, MIME Types
│   └── thumbnail/              # Resizer, Rotator, Disk Cache (TurboJPEG / STB)
├── src/                        # Implementations for all C++ library modules
│   ├── clip/                   # CLIP inference, BPE tokenizer & service implementations
│   ├── faces/                  # SCRFD / ArcFace pipeline & face service implementations
│   ├── common/                 # Logger & types implementations
│   ├── concurrency/            # Thread pool implementation
│   ├── core/                   # Catalog, importer, query & event suggestions implementations
│   ├── db/                     # SQLite connection, schema & database implementations
│   ├── metadata/               # EXIF reader, SHA-256 hasher & media reader implementations
│   ├── server/                 # REST API router & HTTP web server implementations
│   └── thumbnail/              # JPEG thumbnail generator & multi-tier cache implementations
├── apps/                       # Executable applications
│   ├── imagine_cli.cpp         # Main CLI application (import, serve, list, stats, tag, geotag, etc.)
│   └── face_inference_example.cpp # Standalone face detection & recognition benchmark tool
├── tools/                      # Migration, verification, and reference tools
│   ├── face_analysis/          # Face analysis reference scripts
│   │   └── insightface_reference.py # Python InsightFace 0.7.3 reference parity test harness
│   ├── migrate_pse.py          # Photoshop Elements catalog migration tool
│   ├── verify_migration.py     # Photoshop Elements migration verification tool
│   └── fix_orientations.py     # EXIF orientation repair utility
├── scripts/                    # Development & benchmarking scripts
│   ├── benchmark-face-review.py # Face review UI performance benchmark
│   ├── benchmark-face-lazy-grid.py # Face grid virtualization benchmark
│   └── benchmark-face-review-refresh.py # Face review refresh benchmark
├── web/                        # Photoshop Elements-styled modular SPA (Zero external dependencies)
│   ├── index.html              # HTML5 single-page application entry point
│   ├── app.css                 # Dark theme stylesheet & CSS custom properties
│   ├── app.js                  # Application bootstrap, routing & lifecycle wiring
│   ├── api.js                  # REST API client & response schema normalization
│   ├── state.js                # Central reactive application state & cache
│   ├── dom.js                  # DOM element caching, validation & toast alerts
│   ├── media-grid.js           # Media grid rendering, card selection & virtual pagination
│   ├── inspector.js            # Metadata inspector & resizable side panels
│   ├── map-view.js             # Leaflet map, spatial clustering & photo pinning
│   ├── loupe.js                # Fullscreen viewer with pan/zoom & slideshow
│   ├── modals.js               # Modal dialogs (Import, Album, Tag, Delete, Settings)
│   ├── face-analysis.js        # Face scan dialog, cluster review grid, naming & suggestions
│   ├── semantic-search.js      # Natural language search toggle, UI & vector index builder
│   ├── category-view.js        # People, Places, Events, and Albums view management
│   ├── quick-edit.js           # In-browser photo editor (crop, rotate, flip, tonal adjustments)
│   ├── auto-events.js          # Local metadata event suggestions and review dialog
│   ├── keyboard.js             # Keyboard shortcuts & priority Escape manager
│   ├── i18n.js                 # 5-language internationalization (EN, ES, DE, RU, ZH)
│   ├── persistence.js          # LocalStorage persistence for user preferences
│   ├── timeline-range.js       # Timeline histogram range slider control
│   ├── icons/                  # Web application icons and PWA assets
│   ├── manifest.webmanifest    # Progressive Web App manifest
│   └── vendor/leaflet/         # Embedded Leaflet mapping library assets
├── tests/                      # Automated test suites
│   ├── ui/                     # Deterministic Playwright UI test suites (28 suites)
│   ├── fixtures/               # Test image assets (JPEG with EXIF, lossless reference PNGs)
│   ├── web_server_benchmark.py # Web server concurrency benchmark script
│   └── *.cpp                   # C++ unit tests (14 test suites covering DB, server, CLIP, faces, etc.)
└── sample_photos/              # Sample images for testing & quick evaluation
```

---

## Documentation

- **[User Guide & Manual](docs/USER_GUIDE.md)**: Getting started, browsing, organizing, tagging, fullscreen loupe, inspector, CLI reference, and troubleshooting.
- **[Face Analysis Runtime Guide](docs/face-analysis-runtime.md)**: ONNX Runtime setup, model preparation, CPU and CUDA execution providers, and idle memory management.
- **[Face Analysis REST API](docs/face-analysis-api.md)**: Complete endpoint reference for face scan jobs, identity assignment, group review, and batch operations.
- **[Face Analysis Reference Validation](docs/face-analysis-validation.md)**: Technical protocol for validating numerical parity against Python InsightFace 0.7.3.
- **[Semantic AI Search Guide](docs/semantic-search.md)**: CLIP model installation, building the vector search index, natural language query examples, and visual similarity.
- **[Database Schema Documentation](docs/dev/database_schema.md)**: Relational schema architecture, tables, indexes, and migrations (v1 to v4).
- **[Map View & Geotagging Developer Guide](docs/dev/map_view.md)**: Leaflet map clustering, spatial queries, and reverse geocoding architecture.
- **[UI Test Plan](docs/dev/ui_test_plan.md)**: Playwright test architecture, test matrix, and deterministic UI harness.
