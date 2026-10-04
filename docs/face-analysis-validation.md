# Face analysis validation — 4 October 2026

Imagine uses InsightFace 0.7.3's buffalo_l detector and recognizer through native
ONNX Runtime **1.30.0**. Validation used the operator's local model files and
the recovered 24-image pilot in the sibling `facebench` checkout. Private
photos, embeddings, weights, and generated reports remain outside version
control. Build and reference commands are in [the runtime guide](face-analysis-runtime.md).

## Functional validation

- The complete native suite passed **170 tests** with real-model fixtures, including schema
  migration, all eight EXIF orientations, API validation/authentication, tag
  ownership, review revisions, cancellation, restart recovery, source changes,
  and protecting reviewed identities against incompatible detector replacement.
- Review regressions cover restoring a changed source without losing face IDs,
  revisions or tag ownership, preserving prior results after failed attempts,
  superseding interrupted jobs, forced recognizer resets, and finite cosine
  thresholds across the supported -1 through 1 interval.
- The independent build without ONNX Runtime passed **169 tests**, with the two
  real-model integration tests explicitly skipped because inference was disabled.
- The complete Playwright suite passed **243 tests**, including the toolbar
  workflow, styled warning cancellation, automatic grouped-grid opening,
  desktop/mobile layout, UTF-8 name limits, overlays, and bulk acceptance of
  1,002 and 2,002 faces with a revision failure retained for review. Concurrent
  page loading, decoded crop retention, geometry changes, and grid refresh while
  metadata is pending are covered. Lazy-grid regressions cover 25,000 faces,
  bounded DOM and page caches, random scroll jumps, responsive layout, delayed
  pages, request cancellation, incomplete responses, retry and unloaded failures.
  Test-server logs use temporary files to avoid
  filling undrained Windows pipes; the shared PNG mock has a valid header CRC.
  The review regression
  run also covers HTTP 401/409 and network start failures, partial scan recovery,
  adding/removing People tags under an active filter, and small-image overlay
  bounds and box geometry through loading, zoom and viewport resize. Playwright
  ran in the permitted Windows environment; a focused recovery check also
  passed after reopening Faces and reloading the browser.
- A separate real-model workflow scanned two photos in an isolated catalog,
  loaded JPEG crops, named/cleared/dismissed/restored identities through the API,
  and named and accepted a grouped similarity suggestion through Chromium.
  The grouped workflow used models at `C:/Models/buffalo_l`. It used the
  actual application and models rather than mocked face responses.
- CPU works without CUDA DLLs. Automatic mode reported CPU fallback and its
  reason when CUDA dependencies were absent. Strict CUDA failed clearly in that
  environment. With the tested dependencies present, both models initialized
  CUDA and initialization profiling verified GPU model computation; CPU support
  operations are permitted.

## Numerical parity

The official InsightFace 0.7.3 SCRFD, ArcFace, and alignment source was used with
Python ONNX Runtime 1.30.0. Identical lossless pixels avoid JPEG decoder variation.

| Comparison | Images | Faces | Outcome |
|---|---:|---:|---|
| Native CPU vs reference CPU | 24 | 114 | Passed |
| Native CUDA vs reference CPU | 24 | 114 | Passed |
| Native CUDA vs reference CPU, upscaled 1280 × 854 lossless fixture | 1 | 1 | Passed |

Every comparison checked detection count, scores, boxes, five landmarks,
embedding error state, 512 finite normalized values, and embedding cosine.
Tolerances were **0.05 source pixels**, **0.0001 detector score**, and
**0.9995 minimum embedding cosine**. Minimum observed pilot cosines were
0.9997911 on CPU and 0.9997929 on CUDA. The upscaled fixture's cosine was
0.9999929. Multi-face pilot rows include the eleven-face group cases.

This establishes implementation parity, not independent model accuracy. The
pilot has only partial Adobe annotations. A new visual audit of all 72 named
regions and an independent, exhaustively annotated detection/identity set are
still required before broader quality or recognition-accuracy claims. The
default cosine cutoff of 0.5 remains provisional. A native overlay was inspected
on a representative pilot photo; faces on depictions remain review candidates.

## Runtime and matching measurements

The host used an AMD Ryzen 7 8845HS CPU and NVIDIA RTX 2080, driver 591.86.
The tested GPU dependencies were ONNX Runtime 1.30.0's CUDA 12 SDK,
CUDA runtime 12.8.90, cuBLAS 12.8.4.1, and cuDNN 9.8.0.87.
CPU inference uses four intra-op threads.

Native inference used all 24 prepared PNGs, three warmups and twenty measured
repeats per image. Each image used a fresh process and one reused engine. Each
image's JSON contains warm mean, median and p95. The table below aggregates
**per-image means** with equal image weight; its p95 is the p95 of those 24
means, not the pooled p95 of 480 runs.

| Stage | CPU mean / median / p95 of image means | CUDA mean / median / p95 of image means |
|---|---|---|
| Detection including resize, transfer, decode and NMS | 91.89 / 93.92 / 115.62 ms | 13.45 / 13.59 / 14.13 ms |
| Recognition including alignment, all faces in photo | 174.51 / 131.28 / 484.64 ms | 22.75 / 16.86 / 52.56 ms |
| Full analysis excluding image-file decoding | 266.40 / 224.74 / 600.26 ms | 36.19 / 30.21 / 66.25 ms |

Per-image warm detection p95 values ranged from 76.72–134.63 ms on CPU and
13.29–15.57 ms on CUDA. Average detector preprocessing was 5.14/5.11 ms
(CPU/CUDA), output decode 0.025/0.024 ms, and NMS 0.003/0.003 ms.
Average process initialization was 873.68/1300.68 ms and image decoding plus
orientation 7.35/7.72 ms. Initialization used fresh sessions but did not flush
the OS file cache. Maximum Windows process peak working set was 443.59/886.82
MiB; this is process RAM, not a measurement of total GPU memory.

Against the historical 1.21.0 pilot's 64.02 ms CPU / 15.39 ms GPU detection
baselines, the new 24-image CPU result is 43.5% slower and CUDA is 12.6% faster.
The CPU run varied from 71.75–117.99 ms across image means; the cause was not
measured and remains a performance investigation. A same-runtime, four-thread
reference check on a representative image measured 69.06 ms detection mean,
68.87 ms median and 71.00 ms p95, versus 71.75 ms native mean for that image
(3.9% higher). These runs distinguish a modest observed port overhead on that
fixture from the unresolved aggregate CPU variation; they do not establish a
cause for the historical regression. Production latency targets remain open.

Matching was measured through the real local review API with one unnamed face,
one identity, repeated valid fixture embeddings, and ten requests at each size.
These figures include SQLite reads, matching, serialization, and local HTTP.

| Confirmed examples | Mean | Median | p95 |
|---:|---:|---:|---:|
| 1 | 14.20 ms | 15.73 ms | 15.98 ms |
| 1,000 | 9.23 ms | 10.14 ms | 16.47 ms |
| 10,000 | 18.70 ms | 17.69 ms | 31.06 ms |
| 30,000 | 113.59 ms | 113.92 ms | 124.35 ms |

These measurements predate the grouped-grid performance changes below. Cosine
comparisons remain linear in example count for each unnamed face.
These synthetic scale measurements do not evaluate identity discrimination.
Short local requests vary with HTTP/OS scheduling; their baseline overhead can
outweigh the comparison work, as the first two rows show. Earlier runs measured
about 16 ms at 1,000 examples and 20 ms at 10,000 examples.

## Review-grid performance changes

The grid previously calculated suggestions for confirmed and dismissed faces,
although those suggestions were not used by the review UI. A catalog with
10,000 confirmed examples and 200 unnamed faces therefore performed matching
for all 10,200 faces. The grid now matches only active unnamed faces and loads
compatible examples and rejection records once per request. Batch acceptance
shares those reads across faces and adds each successful confirmation to its
request-local example set, preserving sequential acceptance behavior. It avoids
another match after assigning an identity. The exact cosine calculation,
maximum score per person, rejection exclusions and top-three cutoff are retained.

The first browser optimization cached grouping and filtering results, retained
decoded crop images when identity changes left geometry unchanged, and fetched
remaining API pages with bounded concurrency. The following lazy-loading change
replaces that full-grid fetch with compact group summaries and bounded card
pages for the visible scroll window. Group acceptance runs on the server and
includes unloaded eligible faces. The active photo query and metadata still
refresh after writes. Source/revision validation applies to every requested
crop; no persisted recognition cache or approximate matching is introduced.

The synthetic benchmark can be reproduced without models or user photos:

```powershell
py -3.10 scripts/benchmark-face-review.py --binary build/face-grid-preview/imagine.exe --examples 10000 --candidates 200 --accept 100 --runs 5 --output build/face-deps/review-benchmark.json
```

Each invocation creates an isolated catalog and verifies suggestions, accepted
identities and persisted revisions. It uses identical normalized 512-dimensional
vectors for one person. Measurements include local HTTP, SQLite, cosine matching
and JSON handling; they exclude inference, browser rendering and crop loading.
They measure review cost, not recognition accuracy or latency for every catalog.

On the same host, 200 unnamed faces and 10,000 confirmed examples produced:

| Workload | Unchanged build | Optimized build |
|---|---:|---:|
| Entire 10,200-face grid, API pages of up to 1,000 | 39,021 ms | 1,644 ms |
| Accept 100 suggestions, one batch request | 4,973 ms | 929 ms |

The unchanged build includes the current access-logging changes. Its full-grid
control is one measured load; the optimized full-grid figure is the median of
five warm loads. Acceptance figures are one request each, with assignments and
revisions checked independently in SQLite. These observed runs show about 24
times faster full-grid loading and 5 times faster acceptance, without claiming
percentile latency from a single acceptance sample. An older preview baseline
without the current logging changes took 53,651 ms for the grid and 7,348 ms
for acceptance; it is separate evidence rather than the controlled comparison.

An additional control kept the unchanged and optimized servers idle on
equivalent separate 10,000-example catalogs and alternated five candidate-page
requests per server serially. Loading only the 200 unnamed faces measured
821 ms unchanged (range 810–835 ms) and 837 ms optimized (793–846 ms).
The ranges overlap: this patch primarily removes unnecessary named-face
matching and repeated batch reads; it does not make exact comparison of many
unnamed faces with many confirmed examples constant-time.

## Lazy face-card loading

Review faces fetches compact group summaries, then requests 48-card pages for
the visible rows and a small buffer. The browser renders at most 96 cards and
keeps four card pages cached, plus metadata for explicitly selected faces.
Scrolling to a distant row requests its pages directly; it does not download
the intervening faces. Groups remain complete on the server, and their Accept
buttons count distinct photos and include eligible unloaded faces.

The server retains one compact indexed scope and up to four filtered snapshots
for ten minutes. It stores face IDs, review metadata and exact suggestion scores;
the matching baseline additionally retains active unnamed query vectors and
best-example scores. It does not cache full card JSON or confirmed-example vectors.
It reads matching inputs under the catalog
lock, computes exact cosine ranking outside that lock, and checks the database
generation before publishing the result. Local writes and external commits
invalidate snapshots conservatively. An expired or changed snapshot requires a
refresh; no stale snapshot can silently accept a changed group. Failed review
details use bounded 200-ID lookups and reuse a current index when available.

Whole-group acceptance also exposed a SQLite query-plan issue: counting a
photo's assigned faces could use the person index and scan every example of
that person. The ownership query now explicitly uses the existing per-media
index. Its count and tag-provenance behavior are unchanged.

Reproduce the API measurement with an isolated synthetic 25,000-photo catalog:

```powershell
py -3.10 scripts/benchmark-face-lazy-grid.py --binary build/face-grid-preview/imagine.exe --photos 25000 --candidates 200 --output build/face-deps/lazy-grid-benchmark.json
```

The catalog has 200 unnamed faces and 24,800 confirmed examples of one person,
using identical normalized 512-dimensional vectors. On this host:

| Request | Observed time | Response size |
|---|---:|---:|
| First group summary, including index construction | 2,193 ms | 270 bytes |
| Cached summary, median of five requests | 1.04 ms | 270 bytes |
| First 100-card page / distant confirmed page | 17.7 / 4.6 ms | 32,787 / 27,713 bytes |
| Accept all 200 suggestions, including unloaded faces | 47.4 ms | 50 bytes |

Only the first 100 candidate cards and a separate confirmed page were fetched;
100 accepted candidates were never loaded by the client. All 200 identities and
incremented revisions were checked independently in SQLite. Before the per-media
query-plan fix, the same lazy group-accept workload
took 15,154 ms. Acceptance and card-page figures are individual samples, not
percentile estimates. Measurements include local HTTP, SQLite, exact matching
and JSON; they exclude inference, image decoding, crops and browser rendering.
"First" means the first index request in a fresh server process, without
flushing the OS file cache. Catalogs with many unnamed faces and examples can
still spend substantial time computing their initial exact suggestions.

A separate Chromium run used the actual API with the same 25,000-photo catalog
and substituted only crop pixels. The first view requested one page and rendered
35 cards in 2,690 ms, including initial suggestion indexing. Jumping to the bottom
requested offset 24,960 directly. Returning to the top, resizing the dialog and
accepting all 200 suggestions, including unloaded faces, passed without browser
errors. These browser timings are individual observations; they include rendering
but do not measure real crop decoding. The separate real-model workflow above
also passed with the lazy review API and actual JPEG crops.

## Refresh after review edits

The initial lazy-grid benchmark used only 200 unnamed faces. A follow-up with
2,500 unnamed faces and 22,500 confirmed examples exposed repeated full matching:
after an unrelated external rating edit, the group summary took 21,977 ms;
dismissing one unnamed detection took 22,648 ms. Both unnecessarily repeated
the initial 22,392 ms matching pass. Confirming, clearing and rejecting a single
face also repeated that work.

The isolated refresh benchmark checks current group counts and persisted review
revisions after each operation:

```powershell
py -3.10 scripts/benchmark-face-review-refresh.py --binary build/face-grid-preview/imagine.exe --photos 25000 --candidates 2500 --output build/face-deps/review-refresh.json
```

Its vectors are synthetic and identical, with one person and one face per photo.
It measures individual API requests, not recognition accuracy or percentile
latency, and excludes inference, crop decoding and browser rendering.

The matcher now retains an immutable baseline independently of frozen review
snapshots. It compares current query vectors, checksum/pipeline, rejections and
SHA-256 exemplar fingerprints before reusing scores. Per-query state keeps at
most four person maxima and two example scores per retained person. Additions
update affected comparisons; removals promote a proven alternate when possible.
An uncertain alternate, omitted ranking boundary or name tie uses exact fallback.
The fresh cache is published only after the final database-stamp check. No
database schema, embedding format, recognition threshold or model is changed.

Observed refresh times for the same 25,000-photo / 2,500-unnamed workload:

| Change before summary refresh | Previous implementation | With matching reuse |
|---|---:|---:|
| Unrelated external rating | 21,977 ms | 401 ms |
| Dismiss one unnamed face | 22,648 ms | 372 ms |
| Confirm one face | 34,546 ms | 372 ms |
| Clear its identity | 36,521 ms | 399 ms |
| Reject one suggestion | 36,941 ms | 406 ms |

Dismissing/restoring the best confirmed example and naming/clearing a new person
also passed, with refreshes of 377–449 ms. A second run with **10,000 unnamed
faces and 15,000 confirmed examples** passed every operation, with refreshes of
384–445 ms and independently checked group counts and persisted revisions.
These are individual synthetic observations. Initial matching is still required:
the 10,000-unnamed run's first summary took 74,806 ms; this run overlapped the
browser regression suite during initial matching. Server restart and uncertain
large matching changes can still require a full calculation.

The mutation-parity native test compares every unnamed card and its group with
the uncached exact endpoint through rejections, exemplar removals/restoration,
relabeling, name ties and revisionless embedding changes. Browser coverage also
checks that rejection refresh starts while metadata is pending, with one summary
request rather than serially waiting for unrelated photo and People refreshes.
The actual-API 25,000-photo Chromium workflow also passed again after the matcher
change, with one initial card-page request, 35 rendered cards, a direct jump to
offset 24,960 and acceptance of all 200 suggestions including unloaded faces.
