# Face analysis runtime

Imagine uses InsightFace **0.7.3 `buffalo_l`** locally through ONNX Runtime C++ **1.30.0**. The detector is `det_10g.onnx` (SCRFD-10GF); the recognizer is `w600k_r50.onnx` (ArcFace ResNet-50, 512 features). The application never downloads model files or sends photos to an inference service.

## Runtime and model setup

Install the official [ONNX Runtime 1.30.0 release](https://github.com/microsoft/onnxruntime/releases/tag/v1.30.0) Windows x64 C++ SDK and unpack it so the SDK root contains `include/` and `lib/`. The CPU SDK asset is `onnxruntime-win-x64-1.30.0.zip`; the CUDA 12 asset is `onnxruntime-win-x64-gpu_cuda12-1.30.0.zip`. Imagine checks that the SDK headers expose API version 30 and that the runtime DLL reports exactly 1.30.0.

The [CUDA execution provider requirements](https://onnxruntime.ai/docs/execution-providers/CUDA-ExecutionProvider.html) list CUDA **12.8 or newer** and cuDNN **9.x** for the ONNX Runtime 1.30 CUDA 12 package. The default GPU package is built for CUDA 13.0; use the matching SDK and runtime libraries as a set. Add the CUDA and cuDNN `bin` folders to `PATH`. CPU mode does not require CUDA or cuDNN. In `auto` mode each model is profiled during startup, and its provider is accepted only if ONNX Runtime reports at least one model kernel on CUDA. If that check fails, Imagine rebuilds that session on CPU and reports the reason. `cuda` mode fails initialization when either model does not execute kernels on CUDA.

Place the two user-supplied files in a local model directory such as `C:/Models/buffalo_l/`:

```text
C:/Models/buffalo_l/det_10g.onnx
C:/Models/buffalo_l/w600k_r50.onnx
```

The pilot files had these SHA-256 values:

| Model file | SHA-256 |
|---|---|
| `det_10g.onnx` | `5838f7fe053675b1c7a08b633df49e7af5495cee0493c7dcf6697200b85b5b91` |
| `w600k_r50.onnx` | `4c06341c33c2ca1f86781dab0e829f88ad5b64be9fba56e56bc9ebdefc619e43` |

These values identify the pilot files. Imagine accepts structurally compatible user-supplied weights under the expected filenames and records their actual SHA-256 values; it does not download, replace, or redistribute model weights. InsightFace describes the supplied pretrained weights as restricted to non-commercial research use. Supplying weights locally does not change their terms. See [InsightFace 0.7.3 on PyPI](https://pypi.org/project/insightface/0.7.3/) and its model-use notes before deployment.

## Build and run

Face inference is off by default. Existing builds then include an engine status that explains the feature is unavailable. To enable the optional runtime:

```powershell
cmake -S . -B build -G Ninja `
  -DIMAGINE_ENABLE_FACE_ANALYSIS=ON `
  -DORT_ROOT=C:/SDK/onnxruntime-win-x64-1.30.0
cmake --build build --target imagine face_inference_example imagine_tests
```

Configure the server with local paths:

```powershell
$env:IMAGINE_FACE_MODELS = 'C:/Models/buffalo_l'
$env:IMAGINE_FACE_DEVICE = 'cpu' # cpu, auto, or cuda
$env:IMAGINE_FACE_IDLE_UNLOAD_SEC = '30' # seconds before unloading idle models from RAM (default 30; 0 for immediate unload)
```

The server uses lazy loading for inference models: at server boot only lightweight metadata and model file checksums are validated, keeping baseline process memory at ~3–12 MB. The ~400 MB ONNX Runtime models are loaded into memory on-demand only when a face scan job executes, and automatically unloaded when idle or completed. Non-scan operations (browsing the faces grid, reviewing suggestions, merging/naming people, displaying face crops) run directly against SQLite embeddings and crop thumbnails without loading the ONNX inference models.

The standalone C++ example emits JSON detections and normalized embeddings. An overlay is optional:

```powershell
build/face_inference_example.exe `
  --models C:/Models/buffalo_l `
  --image C:/Photos/photo.jpg `
  --device cpu `
  --warmups 3 `
  --repeats 20 `
  --overlay C:/Temp/faces.png
```

The example initializes the engine once, reports cold `initialization_ms`, separately reports `image_decode_ms`, runs warmups, then measures the requested repeats. Its JSON `performance.summary_ms` contains mean, median, and nearest-rank p95 for detector preprocessing, detector inference, decode, NMS, detection total, recognition, and full analysis. `performance.peak_working_set` reports the process peak working set on Windows. `Engine::lastTimings()` exposes the stage timings from the most recent call to C++ clients. With no benchmark flags, it still performs one analysis and emits one JSON document.

Rectangles use oriented source-image pixels and exclusive right/bottom edges. The five landmarks are ordered as image-left eye, image-right eye, nose, image-left mouth, and image-right mouth. A successful image with no faces returns an empty detection array. A face remains present if its embedding fails; its `embedding_error` is set and its embedding array is empty.

## Numerical reference

`tools/face_analysis/insightface_reference.py` calls the SCRFD, ArcFace, and alignment implementations from InsightFace 0.7.3. Install `onnxruntime==1.30.0`, NumPy, OpenCV, ONNX, and scikit-image for the reference tool only. It does not participate in Imagine's runtime. You can either install the InsightFace 0.7.3 package or use the three files extracted from the official [InsightFace 0.7.3 source distribution](https://pypi.org/project/insightface/0.7.3/).

```powershell
python tools/face_analysis/insightface_reference.py `
  --models C:/Models/buffalo_l `
  --image C:/Photos/lossless-reference.png `
  --device cpu `
  --reference-source build/face-deps/insightface-0.7.3 `
  --native-example build/face_inference_example.exe `
  --output C:/Temp/reference.json
```

The harness compares detection count, scores, rectangles, all five landmarks, and embedding cosine similarity. It requires matching embedding-error state and a 512-value, finite, approximately unit-normalized vector for every successful embedding. Its starting tolerances are 0.05 pixels for coordinates, `1e-4` for detector scores, and 0.9995 minimum cosine similarity for embeddings. Add `--warmups 3 --repeats 20` to have the native example include repeated-use stage timing statistics in the comparison JSON. Investigate any detection-count difference or candidate close to the 0.5 threshold. Use the same decoded pixels on both sides; a lossless PNG fixture avoids differences between JPEG decoders. The recovered pilot PNGs may be used from the sibling `facebench/data/images` folder without copying personal photos into this repository. They represent partial Adobe face annotations and do not measure recognition accuracy.

The C++ port follows InsightFace 0.7.3's 640 × 640 top-left black padding, aspect-preserving resize, RGB NCHW normalization, SCRFD 9-output ordering (strides 8/16/32, two anchors per location), score threshold 0.5, and greedy NMS with threshold 0.4 and inclusive overlap areas. ArcFace crops use the pinned five-landmark 112 × 112 similarity alignment and `[0,255]` to `[-1,1]` input normalization. Embeddings are finite-checked and L2-normalized before returning.
