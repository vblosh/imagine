# Face detection requirements: SCRFD with C++

Status: proposed requirements based on the local photo benchmark of 3 October 2026.

## Purpose and scope

Detect all candidate human faces in local photos using an InsightFace SCRFD model
executed through ONNX Runtime's C++ API. Return face rectangles, confidence scores,
and five facial landmarks for downstream display, alignment, or recognition.

This specification is independent of the current application implementation.
Face detection does not assign a person's identity. Identity matching and embedding
extraction are separate stages and are outside these detection requirements.

## Selected model and runtime

- Use the **SCRFD-10GF** detector supplied as `buffalo_l/det_10g.onnx` in the tested
  InsightFace model pack. This is SCRFD, not RetinaFace.
- Run inference through **ONNX Runtime C++**, without a Python process or an
  embedded Python interpreter in the deployed detection pipeline.
- Support the CPU execution provider as the baseline. Support the CUDA execution
  provider for compatible NVIDIA GPUs.
- Use ONNX Runtime **1.30.0** for the Imagine implementation, as explicitly
  selected by the user. **1.21.0** remains the measured pilot reference version;
  validate compatibility and outputs again for 1.30.0.
- Package or acquire the model separately from application logic. Record the
  exact model file, checksum, runtime version, and detector settings with results.
- Confirm that the chosen model weights permit the intended distribution and
  use. Do not infer model-weight permissions from the library's source-code license.

## Findings from the local benchmark

The pilot used 24 photos from the Adobe Photoshop Elements catalog, containing
72 named face regions. Images were EXIF-oriented and resized to a maximum long
side of 640 pixels. The tested SCRFD configuration used a 640 × 640 inference
canvas and a detection confidence threshold of 0.5.

| Observation | Measured result |
|---|---|
| Named regions containing a detected face | **72/72**, checked using face-presence correspondence and visual review |
| Total detections across the 24 photos | 114 |
| Observed detections on depictions | One mannequin and two poster faces |
| Mean CPU detection time per image | **64.02 ms** |
| Mean GPU detection time per image | **15.39 ms** |

The host had an AMD Ryzen 7 8845HS and an NVIDIA GeForce RTX 2080 with 8 GB VRAM.
ONNX Runtime requested four CPU threads. Timings were measured through the
InsightFace Python integration with ONNX Runtime, not a native C++ detector.
They are reference measurements, not a C++ performance guarantee. They exclude
file decoding and cold model startup and include detector preprocessing,
postprocessing, and transfers needed to return CPU results. Models were warmed
up before three measured passes per image.

SCRFD found small, untagged people that the tested RetinaFace configuration
missed. It also detected poster faces. It should therefore be treated as a
candidate-face detector, not a reliable classifier of actual people versus
photographs, illustrations, or mannequins.

The 72 regions are partial annotations, not an exhaustive inventory of visible
people. One broad Adobe region covers overlapping faces. Four group photos are
from the same session. Consequently, the pilot does not establish overall
precision, recall on all visible faces, or recognition accuracy. The 100% result
means a face was found in each tagged region, not that every person was found.

## Functional requirements

### Input and preprocessing

1. Accept decoded local images and define their pixel format explicitly. Apply
   EXIF orientation before detection, or require the caller to supply an already
   oriented image and document that contract.
2. Preserve the image aspect ratio when preparing the 640 × 640 model input.
   Match the selected InsightFace SCRFD implementation's resize, padding,
   channel ordering, normalization, and tensor layout exactly.
3. Track the complete image-to-model transformation. Map returned rectangles
   and landmarks back into the coordinates of the oriented source image.
4. Keep the initial reproducible configuration at a 640 × 640 canvas and
   confidence threshold **0.5**. Make confidence and suppression settings
   explicit, versioned configuration. Validate any changed resolution or
   threshold separately; the pilot findings do not automatically apply to it.

### Inference and postprocessing

1. Load and reuse the ONNX model session across photos. Do not reload the model
   for each image.
2. Port or reproduce SCRFD's output decoding and non-maximum suppression for
   the exact selected model. Do not assume its tensors contain final rectangles.
3. Verify tensor names, shapes, output scales, landmark ordering, and suppression
   behavior against the pinned reference implementation before implementation
   acceptance. This document does not prescribe unverified tensor constants.
4. Return every candidate surviving the configured confidence filtering and
   suppression. Do not select only the largest face or assume one face per image.
5. Return an empty collection for a successfully processed image with no faces.
   Distinguish this from model-loading, decoding, or inference failure.
6. Preserve candidates for downstream review even when they may depict a face
   on a poster or mannequin. Any depiction filter must be a separately evaluated
   feature, rather than an assumed capability of SCRFD.

### C++ result contract

Each result must contain:

- A face rectangle in oriented source-image coordinates, with documented units
  and a consistent convention for right and bottom boundaries.
- A model confidence score. Treat it as a detector score, not a calibrated
  probability that the face belongs to a real person.
- Five landmark coordinates in the same coordinate system, with an explicitly
  documented ordering compatible with downstream face alignment.

The operation must also report the actual execution device/provider, effective
settings, model identity, and any failure or fallback. Validate finite coordinates
and scores and handle edge-of-image detections consistently.

## CPU and GPU behavior

1. CPU detection must work without CUDA installed.
2. GPU mode must verify that the CUDA execution provider initialized and that
   model computation actually runs on the GPU. A GPU request alone is not proof
   of GPU execution.
3. Define fallback behavior explicitly. A strict GPU request must fail clearly
   if CUDA is unusable. An automatic-device mode may use CPU, but must report
   that choice and its reason.
4. Account for legitimate CPU support operations in a CUDA session. Provider
   validation must establish GPU model execution rather than require every
   graph operation to execute on CUDA.
5. Pin and validate runtime dependencies as a set. The successful Windows pilot
   used ONNX Runtime 1.21.0, CUDA 12 runtime libraries, and cuDNN 9.8.0.87.
   An earlier attempt with cuDNN 9.27 failed and fell back to CPU; those timings
   were discarded. This is an observed configuration issue, not a claim about
   every installation of that cuDNN version.
6. Keep inference local. After model and runtime installation, processing must
   not require uploading photos or contacting an external inference service.

## Quality acceptance

The primary product question is **whether the detector found the actual faces**.
Agreement with Adobe's rectangle size or shape is not the primary acceptance
metric.

1. Compare native C++ outputs against the pinned InsightFace reference using
   identical decoded pixels and settings. Check detection counts, confidence
   scores, rectangle and landmark coordinates with documented numerical
   tolerances. Investigate differences near thresholds.
2. Reproduce detection in all 72 named regions of the original pilot, with visual
   inspection of ambiguous regions. Center-inside-region correspondence may
   assist review, but cannot establish correctness on its own.
3. Confirm multi-face behavior on the eleven-person group examples. The tested
   SCRFD configuration detected all eleven faces in those examples.
4. Preserve visual regression cases for small faces, profiles, sunglasses,
   children, low light, overlapping faces, and poster/mannequin detections.
5. Before claiming broader detection quality, evaluate an independent sample
   with every visible human face annotated, including photos with no people.
   Report missed people, spurious detections, duplicate detections, and detections
   on depictions separately. Set product acceptance thresholds from that
   evaluation; do not infer them from the partial Adobe labels.

## Performance acceptance

Measure the native C++ implementation on both CPU and GPU with the same prepared
images, model, and settings as the reference. Report warm mean, median, and p95
detection latency, plus cold initialization time and peak memory separately.

Include preprocessing, output decoding, suppression, and required CPU/GPU
transfers in detection latency. Exclude photo-file decoding from that measurement
and report it separately when measuring the complete application workflow.
Synchronize GPU work before ending a timing interval.

Use **64.02 ms CPU** and **15.39 ms GPU** as baseline comparison points on the
measured host, not hard limits for other hardware. A material regression requires
an explanation, such as differing preprocessing, provider selection, threading,
or input size. Establish production latency targets after measuring native C++.

## Implementation deliverables

- A C++ detector component using ONNX Runtime with the result contract above.
- A small runnable C++ example that processes an image, emits structured
  detections, and optionally writes a visual overlay.
- Reproducible model/runtime setup instructions and dependency versions.
- CPU and CUDA verification, reference-output comparisons, and visual quality
  results on the pilot and subsequent independently annotated evaluation set.

## Existing evidence and implementation references

- Local report: `docs/face-recognition-comparison.md`.
- Raw pilot predictions: `.imagine/facebench/results/insightface-cpu.json` and
  `.imagine/facebench/results/insightface-gpu.json`.
- Visual audit: `.imagine/facebench/quality/detection-quality.html`.
- Model selection: the InsightFace model-zoo documentation and SCRFD ONNX
  implementation associated with the tested InsightFace 0.7.3 package. Consult
  the exact pinned implementation when defining the C++ tensor contract.

The local evidence files may be ignored by version control and are not a substitute
for distributing a reproducible, appropriately licensed evaluation fixture.
