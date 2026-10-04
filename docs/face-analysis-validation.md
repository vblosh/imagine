# Face analysis validation — 4 October 2026

Imagine uses InsightFace 0.7.3's buffalo_l detector and recognizer through native
ONNX Runtime **1.30.0**. Validation used the operator's local model files and
the recovered 24-image pilot in the sibling `facebench` checkout. Private
photos, embeddings, weights, and generated reports remain outside version
control. Build and reference commands are in [the runtime guide](face-analysis-runtime.md).

## Functional validation

- The complete native suite passed **164 tests** with real-model fixtures, including schema
  migration, all eight EXIF orientations, API validation/authentication, tag
  ownership, review revisions, cancellation, restart recovery, source changes,
  and protecting reviewed identities against incompatible detector replacement.
- Review regressions cover restoring a changed source without losing face IDs,
  revisions or tag ownership, preserving prior results after failed attempts,
  superseding interrupted jobs, forced recognizer resets, and finite cosine
  thresholds across the supported -1 through 1 interval.
- The independent build without ONNX Runtime passed **163 tests**, with the two
  real-model integration tests explicitly skipped because inference was disabled.
- The complete Playwright suite passed **232 tests**, including the toolbar
  workflow, styled warning cancellation, automatic grouped-grid opening,
  desktop/mobile layout, UTF-8 name limits, overlays, and bulk acceptance of
  1,002 faces with a revision failure retained for review. The review regression
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

These measurements predate the grouped-grid update. The grid now loads one
compatible confirmed-example snapshot per request and reuses it across that
page's faces. Cosine comparisons remain linear in example count for each face;
the snapshot removes repeated SQLite reads, but its performance has not been
measured at these catalog sizes.
These synthetic scale measurements do not evaluate identity discrimination.
Short local requests vary with HTTP/OS scheduling; their baseline overhead can
outweigh the comparison work, as the first two rows show. Earlier runs measured
about 16 ms at 1,000 examples and 20 ms at 10,000 examples.
