# Deduplicate — Developer Documentation

This document is the developer design reference for the planned **Deduplicate** feature in Imagine: exact byte-level duplicate detection plus CLIP visual near-duplicate detection, keeper ranking, metadata merge-before-delete, and a guided web review flow. It names only verified touchpoints; nothing described here is implemented yet.

---

## 1. Feature Scope

### 1.1 Two-pass detection model

The feature runs two complementary passes over a user-selected scope (sidebar source, current filter, or explicit selection):

1. **Exact pass (SHA-256).** Groups rows whose `(content_hash, file_size)` pair is identical. `content_hash` is the existing SHA-256 hex column populated at import; no new hashing pipeline is needed. Two files with the same hash but different sizes never group (guards against a truncated write that reused a hash slot). This pass needs no CLIP model and must work offline.
2. **Visual pass (CLIP).** Uses the existing CLIP embedding index (`include/imagine/clip/service.hpp`: `search` / `findSimilar`) to cluster near-identical renders: same scene re-exported at different sizes, re-encoded JPEGs, screenshot-of-photo copies. Default similarity threshold is approximately `>= 0.97` (tunable in the scan modal), further constrained by a `date_taken` window so photos taken months apart never cluster even when embeddings are close (e.g. product shots on the same white background).

Exact groups are computed first and pinned: members of an exact group are never re-clustered by the visual pass, and a dismissed pair (Section 2) suppresses both passes.

### 1.2 Keeper scoring (`rankKeeper`)

Each duplicate group elects one **keeper**; every other member is a removal candidate. Scoring is deterministic and applied in order:

1. **Flag status.** Any flagged (favorite/pick) photo outranks every unflagged photo.
2. **Resolution.** Larger `width * height` wins; unknown (`0`) dimensions lose.
3. **File size.** Larger `file_size` wins (proxy for less compression).
4. **Metadata completeness.** More complete record wins: non-empty title/description (mapped to `caption` for photos and `audio_title` for audio in `MediaItem`), presence of GPS (`exif.has_gps`), tag count, and a `date_taken != 0` bonus (negative pre-1970 timestamps are valid and earn the bonus; only `0` means undated). The merge step (Section 2) preserves `date_taken != 0` semantics when copying fields.
5. **Path cleanliness.** A clean library path (inside a managed import root, whole path segment / token matching for `copy`, `export`, `tmp`, `temp`, `(1)` without penalizing words like `copyright.jpg`, and fewer percent-encoded sequences like `%20`) outranks a messy one.
6. **Earliest-id tiebreak.** Lowest `MediaId` wins, so repeated scans are stable.

The modal lets the reviewer swap the keeper (see Section 4); the swap is a UI-only override recorded in the resolve request, not a rescore.

### 1.3 Merge-before-delete invariant

Deletion never discards knowledge. Before any `Catalog::deleteMedia` call the resolver must merge losing members into the keeper: union of tags, union of album memberships (preserving positions, Section 2), and best-effort fill of empty metadata fields (title/caption, GPS, and `date_taken` only when keeper's date is empty `0`), preserving the keeper's existing `date_taken != 0` (negative pre-1970 timestamps are valid non-zero dates and are never overwritten). Only after the merge commits does the delete run. A group resolves to zero deletions when the user keeps all members.

### 1.4 Non-goals

- No automatic background deletion; every delete is an explicit user confirmation.
- No cross-catalog dedup; scope is one open catalog database.
- No new hash algorithm; SHA-256 `content_hash` is reused as-is.
- No changes to the import pipeline in this plan.

---

## 2. Backend Design (Database + Core)

### 2.1 Current state (verified)

- `include/imagine/db/schema.hpp` declares `Schema::CurrentVersion = 7`; `src/db/schema.cpp` implements `applyMigrationV1` through `applyMigrationV7`. The plan adds **Migration V8** there.
- `include/imagine/db/catalog_db.hpp` exposes `insertMedia`, `getMediaById` / `getMediaByPath` / `getMediaByHash`, `deleteMedia`, `runInTransaction`, plus tags/albums APIs. It has **no dedup methods yet**.
- `Catalog::deleteMedia(MediaId, bool)` at `src/core/catalog.cpp:723` delegates to `db_->deleteMedia`; it is the single choke point all resolves must flow through.
- `include/imagine/clip/service.hpp` exposes `search`, `findSimilar`, `removeMedia`, `startJob`, `status`. There is **no dedup scan helper yet**.
- `include/imagine/core/` contains `catalog`, `event_suggestions`, `import_tags`, `importer`, `query` only: **`include/imagine/core/deduplicator.hpp` is MISSING** and is created by this plan.

### 2.2 Migration V8 (`src/db/schema.cpp` + `include/imagine/db/schema.hpp`)

Bump `Schema::CurrentVersion` to `8` and add `applyMigrationV8`:

```sql
-- Exact-group lookup. idx_media_hash already exists on media_items(content_hash);
-- V8 ensures/renames to this partial index, which skips empty-hash rows (legacy).
CREATE INDEX IF NOT EXISTS idx_media_content_hash
  ON media_items(content_hash) WHERE content_hash != '';

-- Persistent user dismissals. Canonical ordering a < b keeps each pair unique.
CREATE TABLE IF NOT EXISTS duplicate_dismissals (
  media_id_a INTEGER NOT NULL REFERENCES media_items(id) ON DELETE CASCADE,
  media_id_b INTEGER NOT NULL REFERENCES media_items(id) ON DELETE CASCADE,
  dismissed_at INTEGER NOT NULL,
  PRIMARY KEY (media_id_a, media_id_b),
  CHECK (media_id_a < media_id_b)
);
CREATE INDEX IF NOT EXISTS idx_dup_dismissals_b ON duplicate_dismissals(media_id_b);
```

Migration must be idempotent (`IF NOT EXISTS`) and backfills nothing: existing `media_items` rows already carry `content_hash`. Downgrade path is drop-table / drop-index only.

### 2.3 `CatalogDb` additions (`include/imagine/db/catalog_db.hpp`)

| Method | Purpose |
|---|---|
| `findExactDuplicateGroups(mediaIds)` | Groups `media_items WHERE content_hash != ''` by `(content_hash, file_size)` with `HAVING COUNT(*) > 1`, restricted to the scan scope when `mediaIds` is non-empty. Returns groups largest-first. |
| `mergeMediaMetadata(keeperId, sourceId)` | Field-level merge inside the caller transaction: copy empty title/description/GPS/`date_taken` from source to keeper (never overwrite the keeper when keeper `date_taken != 0`, and never write `0` over a real or negative value); union tags via existing tag APIs using `getTagsForMediaBatch` for one batched read; union album memberships preserving positions. |
| `dismissDuplicatePair(a, b)` | Canonicalize `a < b`, `INSERT OR IGNORE` one row with `dismissed_at` unix time. |
| `getDismissedDuplicatePairs()` | Return the full `(a, b)` set for scan-time exclusion. Small table; loaded once per scan into an `unordered_set` of combined 64-bit keys. |

Transaction discipline: the scan path is read-only. The resolve path wraps each group (merge every loser, then delete) in one transaction per group using the existing `runInTransaction` plus savepoint pattern, so one bad group aborts only itself. Tag reads use `getTagsForMediaBatch`, never N single-row lookups.

### 2.4 `Deduplicator` engine (new `include/imagine/core/deduplicator.hpp`)

```cpp
enum class DuplicateMatchType { kExact, kVisual };
struct DeduplicatorOptions {
  double visualThreshold = 0.97;        // cosine similarity floor
  int64_t dateTakenWindowSec = 86400;  // max |dtA - dtB|; 0 = no window
  std::vector<MediaId> scopeIds;        // empty = whole catalog
};
struct DuplicateGroup {
  DuplicateMatchType type;
  std::vector<MediaId> memberIds;  // [0] is the ranked keeper
  double minSimilarity = 1.0;      // 1.0 for exact groups
};
struct DeduplicatorScanResult {
  std::vector<DuplicateGroup> groups;
  int mediaScanned = 0;
};
```

- **Architectural scan placement recorded decision:** Visual scan and Union-Find graph clustering (`DisjointSet`) are co-located in `ClipService::scanVisualDuplicates` (`src/clip/service.cpp`), where vector embeddings and similarity queries live, delegating keeper ranking to `imagine::core::isBetterKeeper`. `Deduplicator` in `include/imagine/core/deduplicator.hpp` defines the canonical domain types (`DuplicateMatchType`, `DuplicateCandidateOptions` / `DeduplicatorOptions`, `DuplicateGroup`, `DuplicateScanResult` / `DeduplicatorScanResult`), `rankKeeper`, `isBetterKeeper`, and `sortDuplicateGroup`. Exact duplicate scanning is implemented by `CatalogDb::findExactDuplicateGroups`.
- `rankKeeper(members, metadata)` implements the Section 1.2 ordering; pure function, unit-testable without a database.
- Visual scan fetches candidate pairs via `ClipService::findSimilar` above threshold, drops pairs failing the `date_taken` window (window check skipped only when either side `== 0`: undated photos cluster on visual score alone; negative pre-1970 timestamps are valid and participate in the window), drops dismissed pairs, then **Union-Find** clusters the surviving edges. Exact-group members are removed from the visual candidate set before clustering so the two passes never overlap.
- Result ordering: exact groups first (largest-first), then visual groups. `mediaScanned` counts in-scope rows examined.

### 2.5 `Catalog::deleteMedia` safeguards (`src/core/catalog.cpp:723`)

The resolve path must call `Catalog::deleteMedia`, never `CatalogDb::deleteMedia` directly, so these safeguards apply:

- **Shared-render thumbnail check.** Before unlinking files, compare the victim `content_hash` against remaining rows: if another row shares the hash, keep the shared thumbnail bytes.
- **Hardlink check.** Use `std::filesystem::equivalent(victimPath, keeperPath)`: when equivalent (hardlinks / same inode), delete the directory entry only, never truncate shared data.
- **CLIP eviction.** Call the `clipService.removeMedia(victimId)` API so the vector index cannot return ghosts.
- **Album position preserve/compact.** Carry the victim album positions to the keeper where missing, then compact the album ordering so no gaps remain.
- **Encoding discipline.** Convert paths with the existing `pathFromUtf8` / `pathToUtf8` helpers; never pass raw UTF-8 to `std::filesystem` on Windows.
- **`date_taken != 0` discipline.** Merge and delete must never overwrite the keeper when keeper `date_taken != 0`, and must never write `0` over a real or negative (pre-1970) timestamp; filtered queries keep their existing `!= 0` guard (negative timestamps are valid, never `date_taken > 0`).

---

## 3. REST API (`include/imagine/server/api_router.hpp`)

### 3.1 Current state

`ApiRouter` has `registerMedia` and related `register*Routes` including Face and Semantic routes, but **no duplicates routes**. Closest existing endpoints: `DELETE /api/media/:id`, `POST /api/media/batch-delete`, `POST /api/semantic/similar/:id`. The three endpoints below are new and live under `/api/media/duplicates/`.

### 3.2 `POST /api/media/duplicates/scan`

Request:

```json
{ "media_ids": [12, 13, 14], "visual_threshold": 0.97, "date_window_sec": 86400 }
```

- `media_ids`: optional scope; empty or absent means whole catalog. When present, length is validated against the existing `kMaxBatchSize` batch cap (same constant as batch-delete); over-cap returns `400`.
- `visual_threshold`: optional; must satisfy `isfinite(v) && 0.0 < v && v <= 1.0`, else `400`.
- Handler runs `Deduplicator` exact plus visual scans on the scope, hydrates member thumbnails and metadata via the existing `getMediaById` batch path, and returns groups with the keeper first.

Response (`200`):

```json
{
  "groups": [
    { "type": "exact", "keeper": 12, "members": [12, 13], "min_similarity": 1.0 },
    { "type": "visual", "keeper": 14, "members": [14, 15], "min_similarity": 0.982 }
  ],
  "media_scanned": 1200
}
```

### 3.3 `POST /api/media/duplicates/resolve`

```json
{ "groups": [{ "keeper": 12, "remove": [13] }] }
```

For each group, in one transaction per group: `mergeMediaMetadata(keeper, loser)` for every loser, then `Catalog::deleteMedia(loser)` (which evicts the CLIP `VectorIndex` entry via `clipService.removeMedia`), then compact album positions. Returns per-group `{ keeper, removed, merged_tags }`. Unknown IDs yield per-group `404` entries without aborting sibling groups.

### 3.4 `POST /api/media/duplicates/dismiss`

```json
{ "pairs": [[12, 15]] }
```

Canonicalizes each pair (`a < b`) and calls `dismissDuplicatePair`. Future scans exclude them via `getDismissedDuplicatePairs`. Returns `{ "dismissed": 1 }`. Dismissal is never destructive and has no delete API in this plan (reversible only by removing the pair row).

---

## 4. Frontend Design (`web/`)

### 4.1 Current state

`web/` contains `app.js`, `views.js`, `sidebar.js`, `modals.js`, `auto-events.js`; locales `en` / `de` / `es` / `ru` / `zh`; `styles/` holds 16 files including `variables.css` and **no `deduplicate.css`**; `scripts/check-cycles.js` and `scripts/check-i18n-parity.js` enforce module and locale hygiene. There is **no dedup modal and no `dedup` references anywhere in `web/`**: every file below is new or additive.

### 4.2 New module `web/deduplicate.js`

- Owns the scan state machine: `idle -> scanning -> review -> resolving -> done`, plus `error` with retry.
- **Import discipline:** must not directly import grid, inspector, or loupe modules (cycle risk). Instead it exports `setDeduplicateActionHooks({ openLoupe, revealInGrid })`, which `initViews` in `views.js` wires once at startup. `scripts/check-cycles.js` must keep passing.
- Calls the three Section 3 endpoints with `fetch`, reuses the existing auth and error envelope, and never constructs URLs outside `/api/media/duplicates/` except thumbnail URLs already received from scan results.

### 4.3 Styles: `web/styles/deduplicate.css`

New stylesheet imported by the existing `app.css` bundle. All colors, spacing, radii, and shadows come from `styles/variables.css` app tokens: no hardcoded palette. Card grid, keeper badge, similarity pill, and footer bar reuse the same tokens as the existing modal styles so dark and light themes follow automatically.

### 4.4 Entry points

1. **Sidebar** (`web/sidebar.js`, additive only): a *Duplicates* item with a badge count (exact-group count after the last scan). Clicking opens the dedup modal in its config state.
2. **Selection toolbar** (additive only): *Find duplicates in selection* appears when at least 2 photos are selected; it opens the modal pre-scoped to those `media_ids`.

### 4.5 Modal flow (rendered by `deduplicate.js`, shell helpers in `modals.js`)

1. **Config step.** Threshold slider (default `0.97`), date-window select, scope label, *Scan* button. Posts to `/scan`.
2. **Summary step.** Group count, reclaimable bytes (sum of non-keeper `file_size`), *Resolve all keepers* and *Dismiss all* bulk actions behind confirms.
3. **Group cards.** Side-by-side thumbnails, keeper badge on `members[0]`, similarity pill (`100%` for exact, percent for visual), per-card *Keep this one instead* (keeper-swap, UI-only reorder recorded in the resolve request), per-member *Loupe* button routed through the injected `openLoupe` hook, per-member *Reveal* routed through `revealInGrid`.
4. **Metadata pills.** Per member: resolution, file size, `date_taken` (hidden only when `== 0`; negative pre-1970 dates display normally), flag state, tag count. Pills expose the keeper-score inputs so the ranking is auditable.
5. **Footer.** *Dismiss pair* (calls `/dismiss`), *Delete losers* (calls `/resolve` for this group after confirm), *Close*. After resolve, the card collapses with an undo hint pointing at the existing trash/restore flow where applicable.

### 4.6 Keyboard, a11y, and XSS hygiene

- `modals.js` owns the modal stack; `keyboard.js` Escape handling closes topmost (config closes, review steps back to summary). `deduplicate.js` registers and unregisters its Escape consumer with the same helpers as other modals.
- Every user-controlled string (title, path, tag name) is rendered through the existing `escapeHtml` helper; thumbnails use `src` assignment, never `innerHTML` interpolation. No `eval` or `new Function`.
- Locale keys are all `dedup_*` in each of the five locale files (`en`, `de`, `es`, `ru`, `zh`); `scripts/check-i18n-parity.js` must pass with zero missing or extra keys.

---

## 5. Edge Cases, Testing Roadmap, and Touchpoint Index

### 5.1 Edge-case table

| # | Case | Handling |
|---|---|---|
| E1 | `content_hash` NULL (legacy rows) | Excluded from exact pass by the partial index; visual pass only. |
| E2 | Same hash, different `file_size` | Never grouped (truncation guard). |
| E3 | Hardlinked copies | `std::filesystem::equivalent` check; entry-only delete. |
| E4 | Shared thumbnails | Shared-`content_hash` check keeps thumbnail bytes while references remain. |
| E5 | `date_taken == 0` (undated) | No scoring bonus; window check skipped for that pair; merge never writes `0` over a real or negative value. Negative pre-1970 timestamps are valid throughout. |
| E6 | Dismissed pair reappears in CLIP results | Excluded by the `getDismissedDuplicatePairs` set before Union-Find. |
| E7 | Exact members inside visual candidates | Removed before Union-Find; passes never overlap. |
| E8 | Threshold `NaN` / `Inf` / out of range | `400` via `isfinite` plus range check. |
| E9 | Scope over `kMaxBatchSize` | `400`; client chunks or narrows scope. |
| E10 | Keeper deleted concurrently | Per-group `404`; sibling groups still resolve; card refreshes. |
| E11 | Non-UTF8 / Windows paths | `pathFromUtf8` / `pathToUtf8` conversions at the filesystem boundary. |
| E12 | CLIP model absent | Visual pass returns `[]`; exact pass still works offline. |
| E13 | XSS in titles, paths, tags | `escapeHtml` on all interpolated strings. |
| E14 | JS module cycle | No grid/inspector/loupe imports; `setDeduplicateActionHooks` via `initViews`; `check-cycles` gate. |
| E15 | Missing locale keys | `dedup_*` keys in all 5 locales; `check-i18n-parity` gate. |

### 5.2 Phased roadmap

- **Phase 1: DB + core (+ `db_test`, `catalog_test`).** Migration V8, `CatalogDb::{findExactDuplicateGroups, mergeMediaMetadata, dismissDuplicatePair, getDismissedDuplicatePairs}`, `Deduplicator` structs plus `rankKeeper` and sort algorithms, `Catalog::deleteMedia` safeguard tests (hardlink, shared thumbnail, album compact, `date_taken != 0`). Visual Union-Find clustering is co-located in `ClipService::scanVisualDuplicates`.
- **Phase 2: CLIP + API (+ `server_test`, `clip_test`).** Visual scan wiring via `findSimilar`, the three `/api/media/duplicates/*` routes in `ApiRouter`, `kMaxBatchSize` and `isfinite` validation tests, merge-then-`deleteMedia` plus `VectorIndex` eviction tests.
- **Phase 3: Web + E2E (`tests/ui/test_deduplicate.py`).** `web/deduplicate.js`, `web/styles/deduplicate.css`, sidebar plus selection entries, modal flow, keeper-swap, loupe hook, `escapeHtml` checks, five-locale `dedup_*` keys, `check-i18n-parity` plus `check-cycles` gates, Playwright E2E scan/review/resolve/dismiss run.

### 5.3 File touchpoint index (create vs. modify)

| File | Change |
|---|---|
| `include/imagine/db/schema.hpp`, `src/db/schema.cpp` | Modify: version `7 -> 8`, add `applyMigrationV8`. |
| `include/imagine/db/catalog_db.hpp` (+ impl) | Modify: add the four dedup methods. |
| `include/imagine/core/deduplicator.hpp` (+ `src/core/deduplicator.cpp`) | **Create** (directory has no dedup file today). |
| `src/core/catalog.cpp` (`Catalog::deleteMedia`) | Modify: thumbnail, hardlink, album, encoding guards. |
| `include/imagine/clip/service.hpp` (+ impl) | Modify: add scan helper used by the visual pass. |
| `include/imagine/server/api_router.hpp` (+ impl) | Modify: register the three duplicates routes. |
| `web/deduplicate.js` | **Create** (no dedup module exists). |
| `web/styles/deduplicate.css` | **Create** (no such stylesheet among the 16). |
| `web/sidebar.js`, `web/views.js` (`initViews`), `web/modals.js`, `web/keyboard.js`, `web/app.css` | Modify: entries, hooks, stack/Escape, bundle import. |
| `web/locales` (`en`, `de`, `es`, `ru`, `zh`) | Modify: add `dedup_*` keys. |
| `scripts/check-cycles.js`, `scripts/check-i18n-parity.js` | Gates only (no change expected). |
| `docs/dev/deduplicate.md` (this file) | **Create**: the sole artifact of this task. |
| `docs/dev/database_schema.md`, `docs/dev/map_view.md` | Reference only: header-convention sources. |

