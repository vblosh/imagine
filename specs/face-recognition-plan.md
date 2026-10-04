# Face detection and recognition implementation plan

Status: implemented; validation evidence and evaluation limits are recorded in `../docs/face-analysis-validation.md`. Based on `face-detection-scrfd.md` and the product decisions of 3 October 2026.

## Product decisions

- Local C++ inference only: InsightFace 0.7.3 buffalo_l `det_10g.onnx` (SCRFD-10GF) and `w600k_r50.onnx` (ArcFace ResNet-50, 512-dimensional embeddings).
- Manual scans of selected photos or the entire catalog. One background scan per catalog; progress, cancellation, retry, and restart interruption reporting.
- Users name faces and review suggestions to known people. No automatic identity assignment, unknown-person clustering, video inference, or rectangle editing.
- Existing people tags represent identities. Add dedicated face/embedding storage and preserve manual tag ownership independently of face-derived ownership.
- User-supplied models, CPU baseline, optional verified CUDA, identity correction, dismissal and restoration.

## Inference/runtime

Add optional ONNX Runtime C++ 1.30.0 integration (explicit user override). ONNX Runtime 1.21.0 remains the historical pilot reference only. Preserve builds without the runtime. Verify model compatibility and record SHA-256 identities. Port exact InsightFace 0.7.3 SCRFD preprocessing, decoding, NMS, landmark ordering, similarity alignment, and ArcFace preprocessing. Use 640x640 detector input, confidence 0.5, NMS 0.4 (reference default), normalized 512-float embeddings. Reuse sessions across photos. Apply all eight EXIF orientations consistently with previews. Coordinates use oriented source pixels and exclusive right/bottom boundaries. Empty successful detections differ from failures; embedding failure retains a candidate face.

Support cpu, auto, and strict cuda. Verify actual GPU model execution by initialization profiling; report providers/fallback per model. No automatic weight download or inference network requests. Document runtime dependencies and separate weight-use rights (InsightFace supplied models are non-commercial research only; user-supplied weights do not remove this restriction).

## Persistence and matching

Migration v5 adds analysis jobs and per-photo status, faces and review revisions, embeddings, rejected identities, and manual tag provenance. Existing links migrate as manual. Store content hash, oriented dimensions, model hashes, settings, runtime/providers, pipeline version, and errors. Photo commits are atomic; cancellation occurs between photos. Restart marks unfinished jobs interrupted; unchanged completed results are reused.

Confirmed nondismissed faces are exemplars; photo-level people tags alone are not. Rank each identity by maximum cosine similarity over compatible exemplars; offer up to three matches at a provisional configurable threshold of 0.5. Scores are not probabilities. Acceptance/direct naming adds an exemplar. Rejection suppresses the face/person pair. Changing exemplars updates suggestions.

Face-derived photo tags exist while at least one confirmed nondismissed face has that identity. Clearing the final face removes face ownership only. Manual tag removal clears matching face identities and rejects those suggestions; tag deletion clears identity references, not detections. Original overwrite invalidates face geometry/embeddings and preserves manual tags. Forced reanalysis resets affected face reviews only after explicit UI confirmation; recognizer replacement refreshes embeddings without silently changing identities.

## Frozen browser API contract

All paths use `/api/faces`. Mutations use existing API-token authentication and JSON error responses. IDs are positive integers; requests reject invalid/unknown IDs, non-people tags, incompatible revisions, and overlapping jobs. Embeddings never leave the backend.

- `GET /status`: `{built, ready, error, config:{device, confidence, nms_threshold, match_threshold}, runtime:{detector_checksum, recognizer_checksum, runtime_version, detector_provider, recognizer_provider, fallback_reason, pipeline_version}, job:null|Job}`.
- `POST /jobs`: `{scope:"selected"|"catalog", media_ids?:[id], force:false}`; returns Job with HTTP 202. Maximum 1000 selected IDs. `GET /jobs/:id` returns Job; `POST /jobs/:id/cancel` requests cancellation.
- Job: `{id, state, total, processed, skipped, failed, remaining, error}`. States: running, cancelling, cancelled, completed, interrupted, failed.
- `GET /media/:id`: `{media_id, state, error, width, height, faces:[Face]}`.
- `GET /review?offset=0&limit=50`: `{items:[Face], total}`; unnamed nondismissed faces, limit 1..200.
- Face: `{id, media_id, revision, x, y, width, height, score, landmarks:[{x,y}], person_tag_id:null|id, person_name, dismissed, embedding_error, suggestions:[{tag_id,name,score}], crop_url}`.
- `POST /:id/identity`: `{revision, tag_id:null|id}` or `{revision,name}` creates/uses a people tag. Returns updated Face.
- `POST /:id/accept`: `{revision,tag_id}` accepts a currently eligible suggestion. `POST /:id/reject`: `{revision,tag_id}` records rejection. Return updated Face.
- `POST /:id/dismiss`: `{revision,dismissed:true|false}`. Return updated Face.
- `GET /:id/crop`: JPEG crop from valid oriented original, cache keyed by source and face revision; reject stale geometry.

Inference shared C++ contract: `include/imagine/faces/engine.hpp`. Backend owns lifecycle and serializes access to Engine. Server setup uses local environment configuration (`IMAGINE_FACE_MODELS`, `IMAGINE_FACE_DEVICE`, optional match threshold); expose setup errors in status, not runtime configuration writes from the browser.

## User interface

Add a Faces button on the toolbar that opens the analysis dialog with selected-photo/catalog scope, progress, cancellation, retry, and access to existing results. Closing continues polling. Reanalysis warnings use the application's own modal styling. After a scan, open a face-crop grid scoped to that job's photos. Combine confirmed identities and their strongest matching suggestions into one group per person. Keep each person group together on one page, allowing large groups to exceed the usual page size. Keep unnamed and dismissed groups separate. One click accepts only eligible suggested faces and counts distinct photos. Select face cards independently of the underlying photo selection. A shared grid side panel supplies Choose a person, Name this face, Save name, and Dismiss selection for all selected faces; preserve correction and restoration access. Keep inspector face information and oriented overlays, and remove its scan button and duplicate naming controls. Report partial bulk failures and retain failed selections. Refresh tags/counts after writes. Localize English, Spanish, German, Russian, Chinese, with keyboard/mobile behavior and unavailable/no-face/partial-failure states. Migration v6 adds job-to-photo membership for job-scoped results without changing existing face records.

## Delivery and validation

Use three gpt-6-luna subagents at xhigh: inference/runtime; persistence/jobs/API; UI/localization. Primary agent integrates shared files, reviews, builds with Ninja, and validates. Provide standalone C++ image-to-JSON/overlay example, setup guide, schema/API docs, and reference comparison/benchmark tooling.

Test feature-off/on builds; CPU without CUDA; strict CUDA and auto fallback. Test exact transforms, NMS, alignment, malformed inputs, multiple/zero faces, finite embedding checks, migration, provenance, corrections/rejections, stale edits, jobs/cancellation/restart, API validation/auth, and UI review/progress. Run full C++/Playwright suites after focused tests. Real-model numerical comparison tests must use explicit fixtures and skip clearly if prerequisites are absent. Report tolerances and threshold-adjacent differences.

Referenced pilot evidence is absent from this checkout but recovered locally in the sibling `../facebench` checkout (`data`, `results`, `quality`, `models/provenance.json`). Use explicit fixture paths without adding private photos or model weights to version control. Reproduce outputs before claiming 72-region/eleven-person reproduction. Recognition needs independently identity-labeled evaluation. Measure warm mean/median/p95, cold startup, memory and separate decoding. Do not claim model-quality/GPU/performance acceptance without actual evidence.
