# Smart Tags — Local Zero-Shot Auto-Labeling: Implementation Plan

> Status: Proposed  
> Scope: Local AI feature for the Imagine photo organizer  
> Last updated: 2026-10-20

## 1. Summary and Recommendation

The [Best Free Google Photos Alternatives in 2026 comparison](https://imagesensor-info.com/blog/en/102/#toc2) uses **face recognition** and **search** as the AI differentiators: Immich and PhotoPrism have face recognition built in, digiKam runs it locally, Ente groups faces on-device, and Immich combines timeline, search, and faces.

Imagine already ships both capabilities:

- InsightFace SCRFD + ArcFace face detection/recognition with named people and review queues.
- CLIP ViT-B/32 semantic search and visual similarity over an in-memory vector index.

The missing Google Photos-like capability is **automatic, browsable content labels**: "Beach", "Dog", "Mountain", "Wedding", "Food", "Sunset", and similar. PhotoPrism users expect labels; Google Photos users expect "Things".

### Recommendation

Add **Smart Tags** — local, zero-shot scene/object/activity labels derived from the CLIP image embeddings Imagine already indexes.

Key properties:

- **No new model download.** Reuses the installed CLIP image and text encoders.
- **No image re-encoding.** Scores existing `semantic_index_state.embedding` vectors.
- **100% local.** No cloud calls, telemetry, or external services.
- **Composable.** Tags are normal keyword tags, so the existing sidebar filter, search box, inspector, albums, and rating/date filters continue to work.
- **Incremental.** Per-media state tracks content hash, model revision, and taxonomy version, so repeat runs skip unchanged photos.
- **Reversible.** Removing an AI tag records a rejection; clearing Smart Tags removes only AI-owned assignments.

## 2. Alternatives Considered

| Feature | Assessment | Decision |
|---|---|---|
| **Near-duplicate / best-shot culling** | Valuable and matches the article's migration advice about checking duplicates. Mostly perceptual hashing plus sharpness/quality heuristics rather than a model feature. | Defer; CLIP similarity can seed it later. |
| **OCR / text-in-photo search** | High value for receipts, documents, and screenshots. Requires a new OCR model, runtime, and preprocessing pipeline. | Defer; larger footprint. |
| **AI captions / VLM descriptions** | Best natural-language descriptions, but adds a large generative model and conflicts with the lightweight local-first design. | Defer. |
| **Memories / curated highlights** | No new model, but mostly product design and depends on quality/label signals. | Defer; Smart Tags provides the signal. |
| **Smart Tags (this plan)** | Reuses the existing CLIP index, no new downloads, fills the auto-label gap, and is easy to remove or tune. | **Implement first.** |

## 3. Scope and Non-Goals

### In scope

- Automatic scene/object/activity labels from the existing CLIP index.
- Incremental and forced tagging jobs with progress and cancellation.
- Tags stored as normal `keyword` tags with AI provenance.
- Manual removal with rejection memory so deleted labels do not reappear.
- Clearing all AI tags without touching manual or face tags.
- Settings status, generation modal, sidebar/inspector AI markers.
- English canonical label names in the initial taxonomy.
- A compiled-in taxonomy with an optional JSON override for tuning.

### Out of scope

- New model downloads, OCR, caption generation, or generative editing.
- A fifth top-level "Things" tab; labels live under the existing Keywords sidebar group.
- Per-label enable/disable UI (the taxonomy file covers this).
- Localized label names in v1; canonical English names are stored and displayed.
- Video/audio labeling; photos only.
- Auto-running semantic indexing as part of a smart-tag job.

## 4. User Experience

### Settings

**Settings → Smart Tags** shows one of:

- `Status: Smart tagging engine not built in this binary`
- `Status: CLIP model unavailable`
- `Status: Build the search index first (0 photos indexed)`
- `Status: N photos labeled · M labels (index: K photos)`
- `Status: Could not connect to smart tag service`
- `Status: Taxonomy error: <message>`

Buttons:

- **Generate Smart Tags** — opens the job modal.
- **Remove Smart Tags** — confirms and clears AI-owned tags.

### Generation modal

Mirrors the existing semantic-index modal:

- Scope: selected photos or entire catalog.
- Force re-label option.
- Progress bar and counts.
- Error area.
- Start / Cancel / Close.

State text:

- Generating smart tags…
- Cancelling after the current photo…
- Smart tags are up to date.
- Smart tagging was cancelled.
- Smart tagging stopped before it finished.
- Smart tagging failed.

Counts:

```text
{processed} labeled · {skipped} skipped · {failed} failed · {remaining} remaining of {total}
```

### Browsing

- Labels appear in the existing **Keywords** sidebar group.
- Clicking a label filters the grid using the existing tag filter.
- Labels appear in the inspector with a small **AI** marker.
- Typing a label in the normal search box finds it because tag names are already searched.
- Removing an AI tag from a photo records a rejection; future runs skip it.
- A manually re-added tag clears the rejection and becomes manual.

## 5. Architecture and Data Flow

```text
semantic_index_state.embedding (already built by semantic indexing)
        +
taxonomy prompts ──► CLIP text encoder ──► mean-pooled, normalized label vectors
        │
        ▼
cosine score per label ──► threshold + top-k + per-group cap
        │
        ▼
media_tags + media_tag_provenance(source='clip')
        │
        ├─► sidebar keyword tags / tag filter / text search
        ├─► inspector tags with AI marker
        └─► smart_tag_state (incremental skip) + smart_tag_rejections
```

The tag job iterates existing vectors, not images, so a large catalog is scored with matrix-style dot products. Tag writes are batched (for example, 200 photos per transaction) to avoid per-photo fsyncs.

The CLIP text encoder loads on demand and is kept alive during the job by the existing idle-unload guard. Label vectors are cached in memory keyed by model ID + revision + taxonomy version so reruns skip text encoding.

## 6. Schema v8 and Provenance

Update `Schema::CurrentVersion` to `8` and add `applyMigrationV8` in `src/db/schema.cpp` / `include/imagine/db/schema.hpp`.

```sql
ALTER TABLE media_tag_provenance ADD COLUMN source TEXT NOT NULL DEFAULT 'manual';
UPDATE media_tag_provenance SET source = 'face' WHERE manual = 0;
CREATE INDEX IF NOT EXISTS idx_media_tag_provenance_source
    ON media_tag_provenance(source, media_id);

ALTER TABLE semantic_jobs ADD COLUMN kind TEXT NOT NULL DEFAULT 'index';
CREATE INDEX IF NOT EXISTS idx_semantic_jobs_kind ON semantic_jobs(kind, state);

CREATE TABLE IF NOT EXISTS smart_tag_state (
    media_id         INTEGER PRIMARY KEY REFERENCES media_items(id) ON DELETE CASCADE,
    content_hash     TEXT NOT NULL,
    model_id         TEXT NOT NULL,
    model_revision   TEXT NOT NULL,
    taxonomy_version TEXT NOT NULL,
    label_count      INTEGER NOT NULL DEFAULT 0,
    updated_at       INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS smart_tag_rejections (
    media_id    INTEGER NOT NULL REFERENCES media_items(id) ON DELETE CASCADE,
    tag_id      INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
    rejected_at INTEGER NOT NULL,
    PRIMARY KEY(media_id, tag_id)
);
```

### Provenance source values

| `source` | `manual` | Meaning |
|---|---|---|
| `manual` | `1` | User-created or user-claimed assignment. |
| `face` | `0` | Face-recognition ownership (including legacy rows backfilled by migration). |
| `clip` | `0` | Smart Tags ownership. |

### Provenance rules

- `addTagToMedia`: on conflict set `manual=1, source='manual'`; delete the matching `smart_tag_rejections` row.
- `replaceTagAssignments`: set `source='manual'` for inserted/updated pairs and clear rejections; a user batch action must never remain marked as `clip`.
- `syncFaceTagOwnership` in `src/faces/service.cpp`:
  - insert new ownership as `manual=0, source='face'`;
  - delete on unassign only when `source != 'clip'` (legacy rows with `manual=0` still count as face/derived);
  - never delete a `clip`-owned tag when a face is unassigned.
- `removeTagFromMedia`: read `source`; when `source='clip'`, insert `smart_tag_rejections(media_id, tag_id)` before deleting.
- `invalidateFaceAnalysis` (media-changed path): keep deleting `manual=0` tags (covers face and clip), and additionally delete `smart_tag_state` and `smart_tag_rejections` for that media.
- `deleteTag`: existing FK cascades remove assignments and rejections; leave `smart_tag_state` intact so a globally deleted label is not immediately recreated by an incremental run. A forced run or taxonomy-version change can restore it; document this.

## 7. CatalogDb and Tag Payload Changes

### CatalogDb additions

In `include/imagine/db/catalog_db.hpp`:

```cpp
struct SmartTagRecord {
    MediaId mediaId{0};
    std::vector<TagId> tagIds;
    std::string contentHash;
};

Status replaceSmartTags(const std::vector<SmartTagRecord>& records,
                        const std::string& modelId,
                        const std::string& modelRevision,
                        const std::string& taxonomyVersion);
Status clearSmartTags();
```

`replaceSmartTags` per record:

1. Delete `media_tags` for tag IDs owned by `source='clip'` for that media.
2. Delete those `source='clip'` provenance rows.
3. Insert new `media_tags` and `ON CONFLICT DO NOTHING` provenance rows (`manual=0`, `source='clip'`) so manual ownership is never downgraded.
4. Upsert `smart_tag_state` with content hash, model, revision, taxonomy version, and `label_count`.

`clearSmartTags` deletes all `clip` assignments/provenance plus `smart_tag_state` and `smart_tag_rejections`.

### Tag payload

Extend `Tag` in `include/imagine/common/types.hpp` / `src/common/types.cpp`:

```cpp
std::string source{"manual"}; // per-media provenance
int64_t ai_count{0};          // tag-list metadata
```

- `getTagsForMedia` and `getTagsForMediaBatch` add `LEFT JOIN media_tag_provenance` and select `COALESCE(source,'manual')`.
- `getAllTags` adds `COALESCE(SUM(CASE WHEN p.source='clip' THEN 1 ELSE 0 END),0) AS ai_count`.
- `to_json` / `from_json` include both fields; extra JSON fields are backward compatible.

## 8. Taxonomy and Scoring

Add `include/imagine/clip/smart_tags.hpp` and `src/clip/smart_tags.cpp`.

### Taxonomy model

```cpp
struct TagLabel {
    std::string id;          // stable slug, e.g. "beach"
    std::string name;        // tag name, e.g. "Beach"
    std::string group;       // "Nature", "Animals", ...
    float threshold{0.24f};
    std::vector<std::string> prompts;
};

struct TagTaxonomy {
    std::string version;     // FNV-1a of canonical label definitions
    std::vector<TagLabel> labels;
};
```

- The default taxonomy is a compiled-in JSON string; no runtime asset path issues.
- Optional override: `IMAGINE_SMART_TAGS_TAXONOMY=/path/to/taxonomy.json`.
- Validation: unique IDs, unique names, non-empty prompts, thresholds in `(0,1]`, at least one label.
- Load once at service construction; report parse errors through status. Changing the file requires a restart.
- Curated taxonomy: 75 labels in 9 groups — Nature, Animals, Food & Drink, People & Events, Places & Travel, Architecture & Landmarks, Activities, Objects & Home, Mood & Weather. Each label has 2–3 prompts and a per-label threshold. Dedicated Architecture & Landmarks group prevents category crowding under per-group caps.

### Taxonomy JSON example

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
    }
  ]
}
```

### Scoring

```cpp
struct SmartTagOptions {
    float minScore{0.23f};
    int maxLabels{3};
    int maxPerGroup{2};
};

std::vector<LabelScore> selectSmartLabels(
    const std::vector<float>& imageEmbedding,
    const std::vector<std::vector<float>>& labelEmbeddings,
    const std::vector<TagLabel>& labels,
    const SmartTagOptions& options);
```

- Label embedding = mean of prompt embeddings, then L2-normalize.
- Score = dot product with the normalized image embedding.
- Require `score >= max(options.minScore, label.threshold)`.
- Sort descending, tie-break by label ID; greedily take up to `maxLabels`, with at most `maxPerGroup` from one group.
- Environment overrides:
  - `IMAGINE_SMART_TAGS_MIN_SCORE` (default `0.23`)
  - `IMAGINE_SMART_TAGS_MAX_LABELS` (default `3`)
  - `IMAGINE_SMART_TAGS_MAX_PER_GROUP` (default `2`)

All values are clamped to safe ranges.

## 9. CLIP Service Jobs, Status, and Clear

Reuse `semantic_jobs` with `kind='tags'`:

- One CLIP worker, so semantic indexing and smart tagging cannot run simultaneously.
- The existing unique active-job index and restart handling (`running`/`cancelling` → `interrupted`) apply automatically.
- Existing semantic jobs default to `kind='index'`.

### Service API

Add to `include/imagine/clip/service.hpp`:

```cpp
nlohmann::json smartTagStatus() const;
Status startSmartTagJob(const std::string& scope,
                        const std::vector<MediaId>& mediaIds,
                        bool force,
                        int64_t& jobId);
Status clearSmartTags();
Result<nlohmann::json> getJob(int64_t jobId,
                              const std::string& expectedKind = "") const;
Status cancelJob(int64_t jobId,
                 const std::string& expectedKind = "");
```

### `runTagJob` flow

1. Validate engine ready, taxonomy valid, and index non-empty.
2. Ensure tag rows exist for every label via `createOrGetTag(name, "keyword")`; map label ID → tag ID.
3. Load label text embeddings; cache them in `Impl` keyed by model ID + revision + taxonomy version. Approximately 450 KB for 225 prompts.
4. Load candidate content hashes and `smart_tag_state` rows for the current model/taxonomy.
5. Load all `smart_tag_rejections` into an in-memory set.
6. For each candidate:
   - Skip if state matches content hash/model/revision/taxonomy unless `force`;
   - fetch vectors in batches from the in-memory index;
   - apply rejections and compute selected labels;
   - accumulate `SmartTagRecord`s;
   - flush every 200 records with `replaceSmartTags`.
7. Update job counts per batch, not per photo.
8. On batch failure, retry records individually so one deleted media does not fail the whole job; count genuine failures.
9. Finish as `completed`, `cancelled`, or `failed`; write `error` on failure.

### Status JSON

```json
{
  "built": true,
  "ready": true,
  "taxonomyReady": true,
  "taxonomyError": "",
  "taxonomyVersion": "<hash>",
  "labelCount": 56,
  "indexedPhotos": 1200,
  "labeledPhotos": 900,
  "tagAssignments": 2300,
  "modelId": "clip-vit-b32"
}
```

Counts respect the current model/revision and `content_hash = media_items.content_hash`; stale index rows are excluded.

`clearSmartTags()` returns a conflict when a job is running.

## 10. REST API

Add `registerSmartTagRoutes` to `include/imagine/server/api_router.hpp` / `src/server/api_router.cpp`.

| Method | Path | Behavior |
|---|---|---|
| `GET` | `/api/smart-tags/status` | Status JSON above; `200` even when unavailable. |
| `POST` | `/api/smart-tags/jobs` | `{scope:"catalog"\|"selected", media_ids?, force?}`; validates like semantic jobs; `400` when engine/taxonomy/index unavailable; returns `{job_id,status}`. |
| `GET` | `/api/smart-tags/jobs/:id` | Job JSON with `kind`, `total/processed/skipped/failed/remaining/error`; `404` unless `kind='tags'`. |
| `POST` | `/api/smart-tags/jobs/:id/cancel` | Cancels a tags job only. |
| `POST` | `/api/smart-tags/clear` | Removes all `clip` assignments/state/rejections; conflict while a job runs. |

Validation rules:

- `scope` is `catalog` or `selected`.
- `media_ids` is allowed only with `selected`; IDs must be positive integers without duplicates.
- `force` is boolean.
- Job start requires `ready=true`, `taxonomyReady=true`, and `indexedPhotos > 0`.
- `getJob` / `cancelJob` pass `expectedKind="tags"` for these routes.
- Existing semantic routes pass `expectedKind="index"`; behavior is unchanged for existing clients.

Existing media/tag responses automatically include `source` and `ai_count`.

## 11. Web UI and i18n

### New modules

- `web/smart-tags-status.js` — status loading and settings status text; imports only `state`, `dom`, `api`, and `i18n` to prevent import cycles.
- `web/smart-tags.js` — modal workflow, polling, start/cancel/clear; imports `smart-tags-status.js`, `modals.js`, and `loadMetadata` / `loadMedia` from `views.js`.

### State

Add to `web/state.js`:

```js
smartTagsAvailable: false,
smartTagsBuilt: false,
smartTagsTaxonomyReady: false,
smartTagIndexedPhotos: 0,
smartTagLabeledPhotos: 0,
smartTagLabelCount: 0,
smartTagsConnectionError: false,
```

### DOM and HTML

Add to `web/dom.js` and `web/index.html`:

- Settings section:
  - `#settingsSmartTagsStatus`
  - `#settingsSmartTagsBtn`
  - `#settingsClearSmartTagsBtn`
- Modal `#smartTagsModal`, reusing `face-job-modal` / `face-job-dialog` classes:
  - `#smartTagsJobNotice`
  - `#smartTagsJobState`
  - `#smartTagsScanScope`
  - `#smartTagsJobProgress`
  - `#smartTagsJobCounts`
  - `#smartTagsForce`
  - `#smartTagsJobError`
  - `#smartTagsJobStartBtn`
  - `#smartTagsJobStopBtn`
  - `#closeSmartTagsJobBtn`

Scope options reuse `face_scope_selected` / `face_scope_catalog`; close/cancel reuse existing keys.

### Behavior

- `app.js` calls `checkSmartTagStatus()` at init; `openSettingsModal` refreshes it.
- Start is disabled when unavailable or when `indexedPhotos === 0`.
- On job completion: refresh status, `loadMetadata()`, and `loadMedia()` so sidebar counts and the grid update.
- Clear button confirms, calls `/api/smart-tags/clear`, then refreshes.
- `web/sidebar.js` renders a small `AI` marker when `tag.ai_count > 0`.
- `web/inspector.js` renders `AI` and a tooltip when `tag.source === 'clip'`.
- `web/app.css` adds `.tag-ai-marker` styling.

### i18n

Update all five locales in `web/locales/` with approximately 20 keys:

- titles and settings hints;
- status states (ready, index empty, offline, not built, connection error, taxonomy error);
- scope and force labels;
- job states and counts;
- start/cancel/complete/error messages;
- AI badge and tooltip;
- clear confirmation and success/error messages.

`npm run check:i18n` must pass.

## 12. Edge Cases and Failure Modes

- **No ONNX Runtime / no CLIP models:** status reports `built/ready=false`; start returns `400`; UI disabled with a clear notice.
- **No semantic index:** start returns `400` "Build the search index first"; UI shows that state.
- **Selected scope includes unindexed/non-photo IDs:** counted as skipped, not failed.
- **Stale embeddings:** candidate query requires `semantic_index_state.content_hash = media_items.content_hash`; stale rows are not scored.
- **Manual tag with the same name:** preserved; AI insertion never downgrades `manual=1`.
- **User removes an AI tag:** rejection recorded; forced and incremental runs skip it.
- **User manually re-adds:** rejection cleared and provenance becomes manual.
- **Media changed/re-imported:** invalidation removes clip tags, state, and rejections.
- **Media deleted mid-job:** FK failure isolated per record; job continues and counts failures.
- **Concurrent jobs:** single active-job index and `jobRunning` prevent semantic indexing and tagging from overlapping.
- **Server restart mid-job:** existing startup logic marks `running`/`cancelling` jobs `interrupted`.
- **Tag deleted globally:** assignments cascade; state remains so incremental runs do not immediately recreate it. Documented; force/taxonomy change can restore it.
- **Taxonomy parse error:** status exposes `taxonomyError`; jobs fail with a clear message.
- **Text encode failure:** job fails; no partial labels from that run.
- **CLIP vector dimension mismatch:** job fails with a clear error.

## 13. Tests

### C++ unit tests

`tests/db_test.cpp`:

- migration v8 creates the new tables/columns and backfills `manual=0` rows to `source='face'`;
- `replaceSmartTags` preserves manual and face tags, removes old clip tags, and upserts state;
- removing a clip tag records a rejection;
- `clearSmartTags` removes only clip ownership;
- media-change invalidation also clears clip tags/state/rejections.

`tests/clip_test.cpp`:

- embedded taxonomy is valid, unique, versioned, and stable;
- invalid taxonomy JSON/duplicates/empty prompts are rejected;
- `selectSmartLabels` enforces threshold, top-k, group cap, and deterministic tie-break;
- mean-pooling and normalization of prompt vectors;
- `smartTagStatus()` when the engine is not ready;
- `startSmartTagJob` rejects when no index/models are available.

`tests/server_test.cpp`:

- `/api/smart-tags/status` reports unavailable state without models;
- job payload validation (scope, `media_ids`, duplicates);
- tags job IDs are not accessible through semantic job routes;
- `/api/smart-tags/clear` keeps manual/face tags;
- media JSON exposes `source` and tag lists expose `ai_count`.

### UI tests

Add `tests/ui/test_smart_tags.py`:

- Settings shows the Smart Tags section and status text;
- clicking Generate opens the modal; Start is disabled without models/index;
- close/Escape behavior and focus handling;
- English and one non-English label render correctly.

Existing UI fixtures start at schema v1 and let the server migrate, so no fixture schema change is needed. AI badge behavior is covered by server JSON tests rather than seeding pre-migration provenance.

### Manual QA

- Copy a real v7 catalog; start the server; verify manual/face tags are unchanged after migration.
- Build/reuse a CLIP index, generate Smart Tags, inspect sidebar/inspector, and click a label filter.
- Remove one AI label; force a rerun; confirm it is not re-added.
- Clear Smart Tags; confirm manual and face tags remain.
- Cancel a job mid-run; restart; confirm the `interrupted` state.
- Spot-check approximately 50 photos across taxonomy groups; if precision is poor, raise thresholds or trim the taxonomy via the JSON override before shipping.

## 14. Documentation Updates

- `docs/smart-tags.md` (new): how it works, prerequisites, UI flow, API endpoints, taxonomy JSON schema, environment variables, tuning, limitations, and privacy.
- `README.md`: feature bullet under Key Features and a Smart Tags subsection under CLIP.
- `docs/USER_GUIDE.md`: user workflow and REST API additions.
- `docs/dev/database_schema.md`: new columns/tables and provenance source semantics.

## 15. Acceptance Criteria

- `cmake --build build --target imagine_tests` succeeds with ONNX Runtime both enabled and disabled.
- `./build/imagine_tests` passes, including all existing face/semantic tests.
- `npm test` passes: ESLint, cycle check, and i18n key parity.
- `pytest tests/ui -k "smart_tags or settings or i18n"` passes.
- Migration from a v7 catalog preserves all existing tags; manual tags remain manual and face tags remain face-owned.
- With CLIP models and a non-empty index: a catalog job labels photos, tags appear in the Keywords sidebar and inspector with the AI marker, clicking a tag filters the grid, and the normal search box finds labels.
- A second incremental run skips already-labeled photos and creates no duplicates.
- Removing an AI tag records a rejection; a forced rerun does not restore it.
- Clearing Smart Tags leaves manual and face tags intact.
- Cancel and restart behavior matches semantic indexing.

## 16. Assumptions and Risks

### Assumptions

- Users who want Smart Tags have already built the CLIP index; the feature does not auto-index.
- English canonical tag names are acceptable for v1.
- `keyword` is the right tag category; no new top-level tab.
- The CLIP text encoder is available whenever the image encoder is available.

### Risks and mitigations

- **CLIP B/32 precision:** mitigated by a conservative taxonomy, per-label thresholds, top-3, per-group cap, easy removal, rejection memory, and a JSON override for tuning. If spot-checks are weak, reduce the default taxonomy to high-confidence labels rather than lowering thresholds.
- **Provenance regression:** mitigated by migration backfill, `source != 'clip'` legacy handling in face ownership, and explicit tests that face unassignment never deletes clip tags.
- **Tag clutter:** mitigated by AI markers, the existing collapsible/searchable keyword group, and a one-click clear.
- **Job/table coupling:** `semantic_jobs.kind` is additive and existing semantic jobs default to `index`; all old IDs remain valid.
- **Label-vector encoding cost:** approximately 225 text encodes at job start; cached per model/revision/taxonomy so repeat runs are cheap.

## 17. Implementation Order

1. Schema v8, provenance source, `smart_tag_state`/rejections, CatalogDb methods, Tag JSON fields, face ownership fix.
2. Taxonomy parsing, scoring, and pure unit tests.
3. `clip::Service` status, label-vector cache, tag job, clear, and kind-aware job access.
4. HTTP routes and server tests.
5. Web status module, modal workflow, settings section, AI markers, i18n, and UI tests.
6. Docs, README, schema docs, manual QA, and full CI run.

## 18. File Map

| Area | Files |
|---|---|
| Schema/DB | `include/imagine/db/schema.hpp`, `src/db/schema.cpp`, `include/imagine/db/catalog_db.hpp`, `src/db/catalog_db.cpp` |
| Types | `include/imagine/common/types.hpp`, `src/common/types.cpp` |
| AI engine | `include/imagine/clip/smart_tags.hpp` (new), `src/clip/smart_tags.cpp` (new), `include/imagine/clip/service.hpp`, `src/clip/service.cpp`, `src/faces/service.cpp` |
| HTTP | `include/imagine/server/api_router.hpp`, `src/server/api_router.cpp` |
| Build | `CMakeLists.txt` |
| Web | `web/index.html`, `web/app.js`, `web/dom.js`, `web/state.js`, `web/sidebar.js`, `web/inspector.js`, `web/modals.js`, `web/app.css`, `web/smart-tags.js` (new), `web/smart-tags-status.js` (new), `web/locales/` |
| Tests | `tests/db_test.cpp`, `tests/clip_test.cpp`, `tests/server_test.cpp`, `tests/ui/test_smart_tags.py` (new) |
| Docs | `docs/smart-tags.md` (new), `README.md`, `docs/USER_GUIDE.md`, `docs/dev/database_schema.md` |
