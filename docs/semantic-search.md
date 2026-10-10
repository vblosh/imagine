# Semantic AI Search Guide

Imagine includes a local, privacy-first **Semantic AI Search** engine powered by OpenAI's **CLIP (Contrastive Language-Image Pre-Training)** model via ONNX Runtime. 

Unlike traditional keyword search—which only matches filenames, EXIF metadata, and manually added tags—semantic search understands the **visual and conceptual content** of your photos. You can search your catalog using natural language descriptions or find visually similar photos with a single click.

---

## Table of Contents
1. [Key Features](#1-key-features)
2. [Prerequisites & Model Setup](#2-prerequisites--model-setup)
3. [Building the Search Index](#3-building-the-search-index)
4. [Using Natural Language Search](#4-using-natural-language-search)
5. [Using "Find Similar" (Reverse Image Search)](#5-using-find-similar-reverse-image-search)
6. [Combining Semantic Search with Filters](#6-combining-semantic-search-with-filters)
7. [REST API Reference](#7-rest-api-reference)
8. [Frequently Asked Questions (FAQ)](#8-frequently-asked-questions-faq)

---

## 1. Key Features

- **Natural Language Queries**: Search for descriptive concepts, moods, and objects—such as *"golden retriever puppy on green grass"*, *"snowy mountain peak at sunset"*, *"vintage red convertible"*, or *"birthday party with balloons"*.
- **Visual Similarity ("Find Similar")**: Select any photo and immediately find other visually and conceptually similar images in your collection.
- **100% Offline & Private**: All image processing and vector inference run locally on your device via ONNX Runtime. No photos or search queries are ever uploaded to the cloud or third-party servers.
- **Fast In-Memory Vector Search**: Normalized 512-dimensional vector embeddings are stored in SQLite and indexed in memory for sub-10ms cosine similarity searches across thousands of photos.
- **Combined Filtering**: Combine semantic queries with existing catalog filters, including star ratings (1–5 stars), media types, and date ranges via the timeline scrubber.
- **Non-Destructive**: Your original photos and files on disk are never altered. Embeddings are stored safely in `catalog.db`.

---

## 2. Prerequisites & Model Setup

Semantic search requires local CLIP ONNX model files. When Imagine is built with ONNX Runtime enabled, the CLIP engine checks for models on startup.

### Model Directory Location

By default, Imagine looks for model files in:
- **Linux / macOS**: `~/.cache/imagine/clip_models/`
- **Windows**: `%LOCALAPPDATA%\imagine\clip_models\` (or `<catalog_dir>/clip_models/`)

You can customize this location at any time by setting the `IMAGINE_CLIP_MODEL_DIR` environment variable:
```bash
export IMAGINE_CLIP_MODEL_DIR=/path/to/my/clip_models
```

### Required Files

Place the following files in your `clip_models` directory:

| Filename | Description |
| :--- | :--- |
| `image_encoder.onnx` | Visual encoder model (e.g. CLIP ViT-B/32 image model) |
| `text_encoder.onnx` | Textual encoder model (e.g. CLIP ViT-B/32 text model) |
| `vocab.json` | Tokenizer vocabulary dictionary |
| `merges.txt` | BPE tokenizer subword merges table |
| `model_manifest.json` *(optional)* | Model metadata and dimension specifications |

#### Optional `model_manifest.json` Example:
```json
{
  "model_id": "clip-vit-base-patch32",
  "revision": "v1",
  "vector_dim": 512,
  "preprocess": {
    "width": 224,
    "height": 224
  }
}
```

### Hardware Acceleration

By default, inference runs efficiently on CPU threads (`std::thread::hardware_concurrency()`). If your system has an NVIDIA GPU with CUDA support, you can enable CUDA execution by setting:
```bash
export IMAGINE_CLIP_DEVICE=cuda
```

### Lazy Loading & Memory Management

To minimize server baseline memory usage, Imagine employs **lazy on-demand loading and automatic idle unloading** for the CLIP engine:
- **Startup Probe**: On server boot, Imagine validates model directory presence, manifest metadata, and tokenizer tables without creating heavy ONNX Runtime sessions (~0 MB overhead).
- **On-Demand Loading**: The ONNX model sessions (~300–600 MB) are loaded into RAM only when a semantic natural-language query executes or a photo indexing job runs.
- **Idle Unload Watchdog**: Models automatically unload from RAM and reclaim process working set memory after a period of inactivity. This is configured via `IMAGINE_CLIP_IDLE_UNLOAD_SEC` (default: `30` seconds; set to `0` for immediate unload).
- **Zero Overhead for Reverse Image Search**: "Find Similar" queries operate directly on precomputed SQLite vector embeddings in the in-memory index; ONNX model weights remain unloaded.

---

## 3. Building the Search Index

Before semantic search can find photos, your catalog's photos must be indexed. During indexing, Imagine generates a 512-dimensional vector embedding for each photo and saves it in the catalog database.

### Rebuilding the Index from the Web UI

1. Open Imagine in your browser and click the **Settings** icon (gear `⚙️`) in the top navigation bar.
2. In the Settings dialog, navigate to the **Semantic Search** section.
3. Review the status:
   - `Ready (X vectors indexed, model: ...)`: Models are installed and ready.
   - `Model unavailable or offline`: Model files are missing from the model directory.
4. Click **Rebuild Search Index**.
5. The **Build Search Index** dialog opens, displaying a live progress bar, file counters, and current status:
   - As photos are analyzed, the progress bar updates in real time.
   - You can click **Cancel** or press **Escape** at any time to safely interrupt the scan. Any photos indexed up to that point remain saved in the catalog.
6. When indexing completes, a notification toast reports the total number of indexed photos.

> [!TIP]
> Indexing is incremental: photos that have already been indexed with the current model version will be skipped automatically on subsequent runs unless a forced re-index is requested.

---

## 4. Using Natural Language Search

Once the index is built, you can search your library using natural descriptive language.

### Toggling Semantic Search

1. Locate the main **Search Box** in the top navigation bar.
2. On the right side of the search input, you will see the **Semantic AI Toggle** button (Sparkle / AI icon):
   - **Active (Blue)**: Natural language semantic AI search is enabled.
   - **Inactive (Default)**: Standard text search (matching filenames, tags, and camera metadata) is enabled.
3. Click the icon or press the toggle to activate Semantic Search.

### Writing Effective Semantic Queries

CLIP models understand descriptive visual concepts, actions, styles, lighting, and relationships. Try queries such as:

- **Objects & Animals**:
  - `a playful golden retriever in the grass`
  - `vintage red sports car on asphalt`
  - `steaming cup of coffee on a wooden desk`
- **Scenery & Nature**:
  - `dramatic sunset over ocean waves`
  - `snow covered pine trees on a misty morning`
  - `winding trail through dense green forest`
- **Atmosphere & Style**:
  - `cozy living room with warm fireplace lighting`
  - `neon-lit city street at night in the rain`
  - `black and white architecture photography`
- **Events & People**:
  - `birthday celebration with cake and candles`
  - `friends laughing around a campfire`
  - `bride and groom wedding portrait`

### Executing Queries & Typing Debounce

When Semantic AI search is enabled:
- **Immediate Search on Enter**: Press **Enter** at any time to execute the search immediately without waiting.
- **2-Second Debounce for Sentence Queries**: Natural language queries often involve composing multi-word sentences and descriptions. To avoid executing heavy neural network text embeddings repeatedly on natural typing pauses between words, Imagine applies a **2-second debounce** timer (`SEMANTIC_SEARCH_DEBOUNCE_MS = 2000`). If you pause typing for 2 seconds, the search proceeds automatically.
- **Fast Standard Keyword Search**: When Semantic AI search is toggled off, the search box switches back to standard metadata filtering with a snappy 500ms debounce.

### Search Results

Results are displayed in the media grid sorted by semantic similarity score, with the most relevant visual matches appearing first.

To clear your query and return to the complete catalog, click the **✕** button on the right side of the search input.

---

## 5. Using "Find Similar" (Reverse Image Search)

If you find a photo you like and want to see other photos in your collection with similar visual style, composition, colors, or subject matter:

1. Click on the photo in the media grid to select it.
2. Look at the **Inspector Panel** on the right side of the screen.
3. In the **Actions** section, click **Find Similar** (search icon `🔍`).
4. The media grid instantly re-renders to display the most visually similar photos from your catalog, ordered by visual embedding proximity.
5. Click the **✕** button in the search bar at any time to exit similarity mode.

---

## 6. Combining Semantic Search with Filters

Semantic search operates seamlessly alongside Imagine's catalog filters:

- **Rating Filter**: Star ratings (e.g. 3★, 4★, 5★) filter semantic search results so you only see high-rated photos matching the query.
- **Timeline Histogram**: Click any year or month in the timeline bar at the bottom to constrain the semantic search to photos taken in that time period.
- **Picks & Rejects**: Use the sidebar to restrict results to Picked (`P`) photos or exclude Rejected (`X`) photos.

---

## 7. REST API Reference

You can also interact with the semantic search engine programmatically using Imagine's REST API.

### `GET /api/semantic/status`
Returns the operational status of the CLIP engine and current index size.

**Response:**
```json
{
  "built": true,
  "ready": true,
  "modelId": "clip-vit-base-patch32",
  "indexSize": 2540,
  "vectorDim": 512,
  "executionProvider": "cpu"
}
```

### `POST /api/semantic/search`
Searches the catalog using natural language text.

**Request Body:**
```json
{
  "query": "vintage car in the snow",
  "limit": 50,
  "filters": {
    "ratingMin": 3,
    "dateFrom": 1704067200,
    "dateTo": 1735689600,
    "mediaType": "photo"
  }
}
```

**Response:**
```json
{
  "model_id": "clip-vit-base-patch32",
  "duration_ms": 3.82,
  "items": [
    { "media_id": 142, "score": 0.312 },
    { "media_id": 89, "score": 0.284 }
  ],
  "media_items": [ /* Full MediaItem JSON records */ ]
}
```

### `POST /api/semantic/similar/:id`
Finds photos visually similar to a given photo ID.

**Request Body:**
```json
{
  "limit": 25
}
```

### `POST /api/semantic/jobs`
Starts an asynchronous background indexing job.

**Request Body:**
```json
{
  "scope": "catalog",
  "force": false
}
```

**Response:**
```json
{
  "status": "ok",
  "job_id": 1
}
```

### `GET /api/semantic/jobs/:id`
Retrieves progress and status for an indexing job.

**Response:**
```json
{
  "job_id": 1,
  "status": "running",
  "total": 500,
  "processed": 175,
  "failed": 0,
  "remaining": 325
}
```

### `POST /api/semantic/jobs/:id/cancel`
Cancels an ongoing background indexing job.

---

## 8. Frequently Asked Questions (FAQ)

### Q: Why is the Semantic AI toggle disabled or showing a missing models notice?
**A**: This indicates that the CLIP ONNX model files are not present in the model directory. Verify that `image_encoder.onnx`, `text_encoder.onnx`, `vocab.json`, and `merges.txt` are placed in `~/.cache/imagine/clip_models` or the folder set in `IMAGINE_CLIP_MODEL_DIR`.

### Q: Does semantic search require an active internet connection?
**A**: No. The ONNX Runtime model runs entirely on your local CPU or GPU. No network calls or cloud connections are made.

### Q: Can I search for videos using Semantic Search?
**A**: Currently, semantic indexing analyzes static photos (JPEG, PNG, WebP, BMP). Videos can be filtered using metadata, tags, and albums.

### Q: Does rebuilding the search index overwrite my photo ratings or tags?
**A**: No. Rebuilding the search index only updates vector embeddings in the `semantic_index_state` table. All user metadata, star ratings, albums, tags, and date changes are completely preserved.
