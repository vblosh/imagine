# Smart Tags — Local Zero-Shot Auto-Labeling Guide

Imagine features **Smart Tags**, an on-device, zero-shot photo auto-labeling engine powered by OpenAI's **CLIP (Contrastive Language-Image Pre-Training)** ViT-B/32 neural network.

Smart Tags automatically categorizes your photo collection with descriptive scene, object, activity, and mood labels—such as *"Beach"*, *"Dog"*, *"Mountain"*, *"Wedding"*, *"Sunset"*, or *"Coffee"*—without requiring any manual tagging effort, without downloading additional models, and while maintaining 100% on-device privacy.

---

## Table of Contents
1. [Overview & Motivation](#1-overview--motivation)
2. [How It Works](#2-how-it-works)
   - [Scoring Pre-Computed Embeddings](#scoring-pre-computed-embeddings)
   - [Mean-Pooled Prompt Vectors](#mean-pooled-prompt-vectors)
   - [Cosine Similarity & Selection Heuristics](#cosine-similarity--selection-heuristics)
3. [User Experience & Workflows](#3-user-experience--workflows)
   - [Prerequisites](#prerequisites)
   - [Settings & Status Indicators](#settings--status-indicators)
   - [Generating Smart Tags](#generating-smart-tags)
   - [Browsing & Tag Filtering](#browsing--tag-filtering)
   - [Inspector Panel & AI Badges](#inspector-panel--ai-badges)
   - [Rejection Memory](#rejection-memory)
   - [Clearing Smart Tags](#clearing-smart-tags)
4. [Taxonomy & Customization](#4-taxonomy--customization)
   - [Built-In Taxonomy](#built-in-taxonomy)
   - [Custom Taxonomy JSON Override](#custom-taxonomy-json-override)
   - [Taxonomy JSON Schema](#taxonomy-json-schema)
   - [Environment Configuration](#environment-configuration)
5. [REST API Reference](#5-rest-api-reference)
   - [`GET /api/smart-tags/status`](#get-apismart-tagsstatus)
   - [`POST /api/smart-tags/jobs`](#post-apismart-tagsjobs)
   - [`GET /api/smart-tags/jobs/:id`](#get-apismart-tagsjobsid)
   - [`POST /api/smart-tags/jobs/:id/cancel`](#post-apismart-tagsjobsidcancel)
   - [`POST /api/smart-tags/clear`](#post-apismart-tagsclear)
6. [Privacy, Performance & Resource Usage](#6-privacy-performance--resource-usage)
7. [Edge Cases & FAQs](#7-edge-cases--faqs)

---

## 1. Overview & Motivation

Popular cloud photo services like Google Photos and Apple Photos group photos by visual concepts ("Things", "Places", "Scenes"). Open-source organizers often struggle to provide this experience without complex machine learning pipelines or heavy multi-gigabyte classification models.

Imagine solves this cleanly by repurposing the **CLIP ViT-B/32 vector embeddings** that you already compute for semantic natural language search:

- **Zero New Downloads**: Reuses the CLIP image and text ONNX encoders (`image_encoder.onnx`, `text_encoder.onnx`, `vocab.json`, `merges.txt`) already present on your system.
- **Zero Image Re-Encoding**: Scores existing image vectors stored in `semantic_index_state.embedding`. Thousands of photos can be classified in seconds using in-memory matrix dot products.
- **100% On-Device Privacy**: No images, embeddings, or tags are ever transmitted to third-party services or cloud providers. All classification runs locally on your CPU or GPU.
- **Native Catalog Integration**: Smart tags are stored as standard `keyword` category tags. They work seamlessly with existing sidebar tag filters, search queries, album assignments, and rating filters.
- **Intelligent Rejection Memory**: If you remove an inaccurate AI tag from a photo, Imagine remembers your decision and will **never** re-apply that tag to that photo in future runs.
- **Safe & Fully Reversible**: A dedicated provenance tracking system (`source = 'clip'`) guarantees that removing or clearing smart tags will never alter user-created manual tags or face-recognition tags.

---

## 2. How It Works

### Architecture Data Flow

```text
┌─────────────────────────────────────────────────────────────┐
│  semantic_index_state.embedding (pre-computed by indexing)  │
└──────────────────────────────┬──────────────────────────────┘
                               │
                               ▼
┌──────────────────────────────┴──────────────────────────────┐
│  Taxonomy Prompts ──► CLIP Text Encoder ──► Mean Normalized │
│  ("a photo of a dog", "a pet puppy playing")   Label Vectors │
└──────────────────────────────┬──────────────────────────────┘
                               │
                               ▼
┌──────────────────────────────┴──────────────────────────────┐
│  In-Memory Cosine Similarity Dot Products                    │
│  score = dot(image_vec, label_vec)                          │
│  filter: score >= max(options.minScore, label.threshold)    │
│  rank: descending by score, deterministic ID tie-break      │
│  greedy select: top-k (max 3), per-group cap (max 2)        │
└──────────────────────────────┬──────────────────────────────┘
                               │
                               ▼
┌──────────────────────────────┴──────────────────────────────┐
│  Database Update (Batched 200 items / transaction)          │
│  - media_tags (standard tag links)                          │
│  - media_tag_provenance (source = 'clip', manual = 0)       │
│  - smart_tag_state (media_id, content_hash, taxonomy_ver)   │
│  - respects smart_tag_rejections (skips rejected tags)      │
└─────────────────────────────────────────────────────────────┘
```

### Scoring Pre-Computed Embeddings

Unlike traditional image classifiers that load raw image files, resize pixels, and execute a convolutional or transformer backbone for every single image, Smart Tags performs zero visual inference during labeling. It reads the 512-dimensional L2-normalized image embeddings already computed during CLIP semantic indexing.

### Mean-Pooled Prompt Vectors

To maximize zero-shot classification accuracy and avoid prompt brittleness, each taxonomy label defines 2 to 4 descriptive English prompts:

```text
Label: Dog
Prompts:
  1. "a photo of a dog"
  2. "a pet dog"
  3. "a puppy playing"
```

During startup or before a labeling job runs:
1. Each prompt is tokenized using Imagine's internal Byte Pair Encoding (BPE) tokenizer.
2. The CLIP text encoder generates a 512-dimensional vector embedding for each prompt.
3. The prompt vectors for each label are averaged together (**mean pooling**):
   $$\mathbf{v}_{\text{mean}} = \frac{1}{N} \sum_{i=1}^{N} \mathbf{v}_i$$
4. The resulting pooled vector is L2-normalized:
   $$\mathbf{v}_{\text{label}} = \frac{\mathbf{v}_{\text{mean}}}{\|\mathbf{v}_{\text{mean}}\|_2}$$

These precomputed label vectors are cached in memory keyed by `model_id + revision + taxonomy_version` (~450 KB for 225 prompts across 75 labels), eliminating redundant text encoding across runs.

### Cosine Similarity & Selection Heuristics

Because both the image embeddings and the label embeddings are unit vectors ($\|\mathbf{u}\|_2 = 1$), their cosine similarity equals their dot product:

$$\text{score} = \mathbf{u}_{\text{image}} \cdot \mathbf{v}_{\text{label}}$$

Label selection applies four filtering criteria:
1. **Threshold Test**: A candidate label must satisfy:
   $$\text{score} \ge \max(\text{minScore}, \text{label.threshold})$$
   where `minScore` defaults to `0.23` (configurable via `IMAGINE_SMART_TAGS_MIN_SCORE`) and `label.threshold` is the label-specific baseline (e.g. `0.24` for Beach, `0.25` for Dog).
2. **Rejection Check**: If the pair `(media_id, tag_id)` exists in `smart_tag_rejections`, the label is immediately skipped.
3. **Deterministic Ranking**: Qualifying candidates are sorted descending by score; ties are resolved deterministically by comparing label IDs alphabetically.
4. **Top-K and Group Capping**:
   - Greedily selects up to `maxLabels` total tags per photo (default: `3`, configurable via `IMAGINE_SMART_TAGS_MAX_LABELS`).
   - Caps tags from any single taxonomy group to `maxPerGroup` (default: `2`, configurable via `IMAGINE_SMART_TAGS_MAX_PER_GROUP`). For example, a photo will not receive three different animal tags; at most two animal tags are assigned, leaving room for a setting or activity tag.

---

## 3. User Experience & Workflows

### Prerequisites

Before generating Smart Tags, your photos must have their CLIP semantic vectors indexed:
1. Ensure CLIP models are placed in `~/.cache/imagine/clip_models` (or `IMAGINE_CLIP_MODEL_DIR`).
2. Build the semantic index from **Settings → Semantic Search → Rebuild Search Index**.

> [!NOTE]
> If you attempt to run Smart Tags before indexing, the status reports:
> `Status: Build the search index first (0 photos indexed)` and the Generate button is disabled.

### Settings & Status Indicators

Open **Settings** (gear icon `⚙️` in the top bar) and navigate to the **Smart Tags** section:

| Status Message | Meaning & Recommended Action |
|---|---|
| `Status: N photos labeled · M labels (index: K photos)` | Engine is ready. Displays labeled photo count, active labels count, and indexed photo count. |
| `Status: Build the search index first (0 photos indexed)` | CLIP models are loaded, but photos have not been indexed yet. Rebuild the search index first. |
| `Status: CLIP model unavailable` | CLIP model files (`image_encoder.onnx`, `text_encoder.onnx`, etc.) are missing from the model directory. |
| `Status: Smart tagging engine not built in this binary` | Imagine was compiled without ONNX Runtime support (`-DIMAGINE_ENABLE_FACE_ANALYSIS=OFF`). |
| `Status: Could not connect to smart tag service` | REST API connection failed or server is temporarily unreachable. |
| `Status: Taxonomy error: <message>` | Custom taxonomy JSON override failed schema validation or parsing. |

### Generating Smart Tags

Click **Generate Smart Tags** in the Settings dialog to open the job modal:

```
+-------------------------------------------------------------+
| Generate Smart Tags                                     [X] |
|-------------------------------------------------------------|
| Scan Scope:                                                 |
| [ Entire Catalog (1,200 indexed photos)                   v] |
|                                                             |
| [ ] Force re-label (re-evaluate previously labeled photos)  |
|                                                             |
| [========================>               ] 68%              |
| 816 labeled · 12 skipped · 0 failed · 372 remaining of 1,200 |
|                                                             |
| Status: Generating smart tags...                            |
|-------------------------------------------------------------|
|                             [ Cancel ]  [ Start Tagging ]   |
+-------------------------------------------------------------+
```

1. **Select Scope**:
   - **Entire Catalog**: Labels all indexed photos in the database.
   - **Selected Photos**: Labels only currently selected photos in the media grid.
2. **Force Re-label**:
   - **Unchecked (Incremental, recommended)**: Skips photos whose `content_hash`, `model_id`, `model_revision`, and `taxonomy_version` match the existing `smart_tag_state`. Runs in fractions of a second for up-to-date collections.
   - **Checked (Forced)**: Re-scores all photos against the taxonomy, updating labels to reflect any changes in thresholds or settings. Respects rejection memory.
3. **Execution & Real-Time Progress**:
   - The modal tracks processed, skipped, failed, and remaining counts.
   - Safe cancellation: Click **Cancel** at any time. The engine finishes the current batch, commits it to SQLite, and stops cleanly without corrupting catalog state.

### Browsing & Tag Filtering

Auto-generated tags are added as normal `keyword` tags:
- **Sidebar Integration**: Smart tags appear under the **Keywords** accordion group in the left sidebar.
- **AI Indicator**: Tags created by Smart Tags display a small **AI** badge in the sidebar to distinguish them from user-created tags.
- **Instant Filtering**: Click any smart tag (e.g. *Beach*, *Sunset*, *Dog*) to filter the media grid instantly.
- **Search Box**: Typing a smart tag name into the main search box matches all photos tagged with that keyword.

### Inspector Panel & AI Badges

When a photo is selected, the **Inspector Panel** on the right shows all attached tags:
- Smart tags are styled with a distinctive **AI badge**.
- Hovering over the badge displays the tooltip: `Smart Tag (AI-generated)`.
- User-created manual tags and face-recognition tags do not have this badge.

### Rejection Memory

If Smart Tags incorrectly labels a photo (e.g. a plush toy misidentified as *Dog*):
1. In the Inspector panel, click the **✕** button on the `Dog` tag.
2. The tag is detached from the photo.
3. Imagine records an entry in `smart_tag_rejections`:
   ```sql
   INSERT INTO smart_tag_rejections (media_id, tag_id, rejected_at) VALUES (?, ?, ?);
   ```
4. **Persistent Memory**: On future labeling runs—even forced catalog re-scans—the engine checks `smart_tag_rejections` and will **never re-assign that tag to that photo**.

> [!TIP]
> **Claiming or Restoring a Rejected Tag**:
> If you manually add a tag back to a photo using the Inspector's tag input, Imagine automatically deletes the rejection record from `smart_tag_rejections` and promotes the tag's provenance to `manual`.

### Clearing Smart Tags

If you ever wish to reset or remove all AI-assigned labels:
1. Open **Settings → Smart Tags**.
2. Click **Remove Smart Tags**.
3. A confirmation prompt appears:
   > *"Are you sure you want to remove all AI-generated smart tags? Manual tags and face tags will not be affected."*
4. Confirming executes `POST /api/smart-tags/clear`:
   - Deletes all tag assignments where `source = 'clip'`.
   - Clears `smart_tag_state` and `smart_tag_rejections`.
   - Leaves all user-created manual tags (`source = 'manual'`) and face tags (`source = 'face'`) completely intact.

---

## 4. Taxonomy & Customization

### Built-In Taxonomy

Imagine ships with an embedded, carefully curated taxonomy of **75 labels** across **9 semantic groups**. Each label includes multiple natural phrasing prompts and tuned threshold values to maximize precision:

| Group | Included Labels |
|---|---|
| **Nature** (12) | Beach, Mountains, Forest, Lake, River, Ocean, Sunset, Sunrise, Snow, Flowers, Sky, Clouds |
| **Animals** (7) | Dog, Cat, Bird, Horse, Wildlife, Fish, Insect |
| **Food & Drink** (6) | Food, Coffee, Wine, Beer, Cake, Fruit |
| **People & Events** (6) | Wedding, Birthday, Concert, Portrait, Party, Baby |
| **Places & Travel** (7) | City, Street, Airplane, Car, Bicycle, Boat, Train |
| **Architecture & Landmarks** (19) | Architecture, Bridge, Church, Cathedral, Castle, Fortress, Museum, Palace, Ruins, Temple, Mosque, Monument, Skyscraper, Tower, Lighthouse, Plaza, Library, Stained Glass, Arch & Colonnade |
| **Activities** (6) | Camping, Hiking, Swimming, Skiing, Running, Cycling |
| **Objects & Home** (6) | Book, Instrument, Art, Garden, Interior, Furniture |
| **Mood & Weather** (6) | Night, Autumn, Spring, Summer, Winter, Rain |

### Custom Taxonomy JSON Override

You can override the built-in taxonomy by creating a custom JSON file and pointing the `IMAGINE_SMART_TAGS_TAXONOMY` environment variable to it:

```bash
export IMAGINE_SMART_TAGS_TAXONOMY=/etc/imagine/my_taxonomy.json
./build/imagine serve --catalog catalog.db
```

### Taxonomy JSON Schema

The taxonomy file must be a valid JSON object matching the following structure:

```json
{
  "version": 1,
  "labels": [
    {
      "id": "beach",
      "name": "Beach",
      "group": "Nature",
      "threshold": 0.24,
      "prompts": [
        "a photo of a beach",
        "sand and ocean waves",
        "people relaxing on a sandy beach"
      ]
    },
    {
      "id": "dog",
      "name": "Dog",
      "group": "Animals",
      "threshold": 0.25,
      "prompts": [
        "a photo of a dog",
        "a pet dog",
        "a puppy playing"
      ]
    },
    {
      "id": "coffee",
      "name": "Coffee",
      "group": "Food & Drink",
      "threshold": 0.26,
      "prompts": [
        "a cup of coffee",
        "latte art in a coffee mug",
        "espresso cup on a wooden table"
      ]
    }
  ]
}
```

#### Field Specifications:
- `version` (`integer`, required): Taxonomy format version (must be `1`).
- `labels` (`array`, required): List of label definition objects:
  - `id` (`string`, required): Unique lower-case ASCII slug (e.g. `"golden_gate_bridge"`).
  - `name` (`string`, required): Tag display name created in the catalog (e.g. `"Golden Gate Bridge"`). Must be unique across labels.
  - `group` (`string`, required): Semantic grouping category used for per-group quota caps (e.g. `"Places"`, `"Nature"`).
  - `threshold` (`number`, required): Float in `(0.0, 1.0]`. Minimum cosine similarity score required for assignment. Recommended range: `0.22`–`0.28`.
  - `prompts` (`array of strings`, required): 1 to 10 descriptive English phrases representing the visual concept.

#### Validation Rules:
- IDs and names must be strictly unique.
- Prompts array must not be empty, and individual prompt strings cannot be blank.
- Any validation failure sets `taxonomyReady = false` and records the error details in `taxonomyError`, visible in Settings.

### Environment Configuration

Configure Smart Tags behavior using environment variables:

| Variable | Default | Valid Range | Description |
|---|---|---|---|
| `IMAGINE_SMART_TAGS_TAXONOMY` | *(none)* | File path | Path to custom JSON taxonomy override file. |
| `IMAGINE_SMART_TAGS_MIN_SCORE` | `0.23` | `0.0`–`1.0` | Global minimum cosine similarity score cutoff. |
| `IMAGINE_SMART_TAGS_MAX_LABELS` | `3` | `1`–`20` | Maximum number of smart tags assigned to a single photo. |
| `IMAGINE_SMART_TAGS_MAX_PER_GROUP` | `2` | `1`–`10` | Maximum smart tags assigned from any single taxonomy group. |

---

## 5. REST API Reference

All mutating endpoints (`POST`) require authentication if `IMAGINE_API_TOKEN` is configured on the server.

### `GET /api/smart-tags/status`

Returns the operational status, index coverage, and taxonomy metrics.

**Response (`200 OK`):**
```json
{
  "built": true,
  "ready": true,
  "taxonomyReady": true,
  "taxonomyError": "",
  "taxonomyVersion": "b47c8f2a1e0d4a9f",
  "labelCount": 75,
  "indexedPhotos": 1200,
  "labeledPhotos": 900,
  "tagAssignments": 2300,
  "modelId": "clip-vit-base-patch32"
}
```

- `built`: `true` if compiled with ONNX Runtime support.
- `ready`: `true` if CLIP models are loaded and ready.
- `taxonomyReady`: `true` if default or custom taxonomy parsed successfully.
- `taxonomyVersion`: Unique hash of current taxonomy definition.
- `indexedPhotos`: Count of photos with valid CLIP vector embeddings.
- `labeledPhotos`: Count of photos labeled by the current model and taxonomy version.
- `tagAssignments`: Total active tag attachments with `source = 'clip'`.

---

### `POST /api/smart-tags/jobs`

Starts a background smart-tagging job.

**Request Body:**
```json
{
  "scope": "catalog",
  "force": false
}
```

Or for selected photos:
```json
{
  "scope": "selected",
  "media_ids": [101, 102, 103],
  "force": true
}
```

**Parameters:**
- `scope` (`string`, required): `"catalog"` or `"selected"`.
- `media_ids` (`array of integers`, optional): Required when `scope` is `"selected"`. Must be positive integers without duplicates.
- `force` (`boolean`, optional, default: `false`): When `true`, re-scores previously labeled photos.

**Response (`200 OK`):**
```json
{
  "job_id": 14,
  "status": "ok"
}
```

**Error Responses:**
- `400 Bad Request`: If engine is not ready, taxonomy has errors, or index is empty (`"Build the search index first"`).
- `409 Conflict`: If another semantic indexing or smart tagging job is already active.

---

### `GET /api/smart-tags/jobs/:id`

Retrieves status and progress counters for a tagging job.

**Response (`200 OK`):**
```json
{
  "id": 14,
  "kind": "tags",
  "state": "running",
  "scope": "catalog",
  "total": 1200,
  "processed": 800,
  "skipped": 20,
  "failed": 0,
  "remaining": 380,
  "error": "",
  "created_at": 1729480000,
  "updated_at": 1729480035
}
```

- `state`: `"running"`, `"cancelling"`, `"completed"`, `"cancelled"`, `"interrupted"`, or `"failed"`.
- Returns `404 Not Found` if the job ID belongs to a semantic indexing job (`kind = 'index'`).

---

### `POST /api/smart-tags/jobs/:id/cancel`

Requests cancellation of an ongoing smart-tagging job.

**Response (`200 OK`):**
```json
{
  "status": "ok"
}
```

---

### `POST /api/smart-tags/clear`

Removes all AI-assigned tags, state records, and rejection history from the database.

**Response (`200 OK`):**
```json
{
  "status": "ok"
}
```

**Error Responses:**
- `409 Conflict`: If a smart-tagging or semantic-indexing job is actively running.

---

## 6. Privacy, Performance & Resource Usage

### 100% On-Device Privacy
Smart Tags adheres to Imagine's strict local-first privacy standard:
- No photos or metadata are ever transmitted off your computer.
- No third-party classification services, APIs, or telemetry are used.

### High-Throughput Matrix Math
Because classification operates on pre-computed vectors:
- **No Image Decoding**: Avoids decoding JPEGs or rendering bitmaps.
- **In-Memory Dot Products**: Vector scoring is performed via highly optimized BLAS / vector inner products in RAM. An entire library of 50,000 photos can be evaluated in seconds.
- **Batched Database Writes**: Tag assignments are committed in batches (e.g. 200 photos per transaction), preventing disk thrashing and SQLite write lock contention.

### Zero Idle Footprint
- Precomputed prompt vectors consume approximately 350 KB in RAM.
- The heavy CLIP text encoder session loads on-demand when a job begins and unloads automatically after idle inactivity (`IMAGINE_CLIP_IDLE_UNLOAD_SEC`).

---

## 7. Edge Cases & FAQs

### Q: What happens if I manually created a tag named "Beach" before running Smart Tags?
**A**: Imagine preserves your manual tag! If Smart Tags determines a photo should be tagged with *Beach*:
- If the photo already has *Beach* with `manual = 1`, the manual provenance is preserved and never downgraded to `clip`.
- If the photo does not have *Beach*, the tag is attached with `source = 'clip'` and `manual = 0`.
- The tag entity itself in the `tags` table is shared.

### Q: If I delete an AI tag, will it come back on the next scan?
**A**: No. Removing an AI tag writes a record to `smart_tag_rejections`. Even if you run a forced catalog re-scan, the engine respects your rejection and skips that tag.

### Q: How do I un-reject a tag if I change my mind?
**A**: Simply attach the tag manually via the Inspector panel. Manually adding the tag removes the rejection record from `smart_tag_rejections` and marks the tag as manual (`source = 'manual'`).

### Q: What happens if I edit or rotate an original photo file?
**A**: When Imagine detects that a file's content has changed (new SHA-256 hash):
1. Semantic search embeddings are invalidated.
2. AI-generated tags and `smart_tag_state` for that item are cleared.
3. Once the photo is re-indexed by semantic search, Smart Tags will evaluate the newly updated visual content.

### Q: Does clearing Smart Tags remove my face tags?
**A**: No. Smart Tags uses strict provenance tracking (`source = 'clip'`). Face tags have `source = 'face'` and manual tags have `source = 'manual'`. Clearing smart tags deletes **only** rows where `source = 'clip'`.
