# Face analysis API and storage

Face analysis uses the local `buffalo_l` model files `det_10g.onnx` and
`w600k_r50.onnx`. The application never downloads model files. Configure
`IMAGINE_FACE_MODELS` to the directory containing both files and set
`IMAGINE_FACE_DEVICE` to `cpu`, `auto`, or `cuda`. `IMAGINE_FACE_MATCH_THRESHOLD`
sets the finite cosine suggestion cutoff from -1 through 1; its default is 0.5. The ONNX
Runtime build used for this feature is 1.30.0. A build without ONNX Runtime
reports `built: false` and rejects scan requests with HTTP 503.

The model files are supplied by the operator and their checksums are reported
by the status endpoint. InsightFace 0.7.3 describes its pretrained weights as
available for non-commercial research use. Local installation does not change
the model's use restrictions.

All routes are under `/api/faces`. Reads return JSON unless the route serves a
JPEG crop. Existing API-token authentication applies to every mutation. Face
embeddings remain in the catalog database and are never included in API
responses.

## Routes

| Method and path | Behavior |
|---|---|
| `GET /status` | Build/readiness, effective settings, model checksums, providers, fallback detail, and latest recoverable or retryable job. |
| `POST /jobs` | Starts a scan and returns HTTP 202 with a job. Body: `{"scope":"selected","media_ids":[12,13],"force":false}` or `{"scope":"catalog","force":false}`. Selected scans accept at most 1,000 unique photo IDs. |
| `GET /jobs/:id` | Reads persisted progress and state. |
| `POST /jobs/:id/cancel` | Requests cancellation between photos. |
| `GET /media/:id` | Reads analysis state, oriented dimensions, errors, and detections for a photo. |
| `GET /:id` | Reads one current face, including its revision and suggestions, for correction of an unloaded failed review. |
| `GET /lookup?ids=12,13` | Reads up to 200 unique positive face IDs, returning `{items,failed}` with individual missing-ID errors. Used to resolve unloaded review failures in bounded batches. |
| `GET /review?offset=0&limit=50` | Lists unnamed, nondismissed faces from completed analyses. The limit is 1–200. |
| `GET /grid?job_id=123&offset=0&limit=200&include_dismissed=false` | Lists completed faces for a scan job, or all completed faces when `job_id` is omitted. The limit is 1–1,000. |
| `GET /groups` | Compact, filtered person/unnamed/dismissed group summaries and a review snapshot; no face cards or embeddings. |
| `GET /group?key=person%3A42&snapshot=...&offset=0&limit=100` | A bounded page of face cards from one snapshot group. |
| `POST /groups/accept` | Accepts every eligible suggestion in the snapshot group, including unloaded cards. Body: `{"snapshot":"...","key":"person:42"}`. |
| `POST /:id/identity` | Assigns or clears a person: `{"revision":1,"tag_id":42}`, `{"revision":1,"name":"Ari"}`, or `{"revision":1,"tag_id":null}`. |
| `POST /:id/accept` | Accepts an eligible suggestion: `{"revision":1,"tag_id":42}`. |
| `POST /:id/reject` | Rejects an eligible suggestion with the same payload. |
| `POST /:id/dismiss` | Dismisses or restores a detection: `{"revision":1,"dismissed":true}`. |
| `POST /batch` | Applies one identity, suggestion acceptance, or dismissal action to up to 1,000 face revisions; returns per-face `updated` and `failed` arrays. |
| `GET /:id/crop?revision=1` | Serves a JPEG crop if the face revision and source image are still current. |

`GET /grid` accepts `include_dismissed=true` to include dismissed faces. A job-scoped grid contains only photos recorded for that job that currently have complete analysis. Historical jobs created before job-to-photo membership was recorded produce an empty scoped grid. With no `job_id`, the grid includes every photo with a complete analysis. Results are ordered by face ID.

For a lighter review-grid response, suggestions are omitted (returned as an empty array) for already assigned or dismissed faces. Suggestion-acceptance mutation responses also return an empty suggestions array after the update; refresh the media or review endpoint to retrieve suggestions for an unnamed, active face.

The review browser uses `/groups` and `/group` instead of downloading the entire
legacy `/grid`. Group queries accept optional `job_id`, `show_named` and
`show_unnamed` (both default true), `include_dismissed` (default false), and an
optional `person_tag_id` filter. The response is
`{snapshot,total,groups:[{key,type,tag_id,name,faces_count,photos_count,suggestion_faces_count,suggestion_photos_count}]}`.
`total` counts faces across the filtered groups. A person group combines
confirmed faces with unnamed faces whose strongest eligible suggestion is that
person. Each face belongs to one group. Other keys are `unnamed` and `dismissed`.
The suggestion button counts distinct photos, even when a photo has multiple
face detections.

The snapshot fixes group membership, scope, filters and captured review
revisions. `/group` returns `{snapshot,items,total,offset,limit}` for its bounded
card page; the limit is 1–200 and defaults to 100. Up to four filtered snapshots
are retained for the current indexed scope, each for ten minutes. Group acceptance requires mutation authentication and returns
`{updated_count,failed_count,failed:[{id,status,error}]}` without returning every
updated card. It preserves individual commit and partial-failure semantics.
Expired or invalidated snapshots return HTTP 409; obtain fresh summaries before
retrying. The bounded in-memory index is invalidated by local catalog writes
and committed changes made through another database connection. A separate
matching baseline survives snapshot invalidation. Rebuilding compares current
query vectors, pipelines, rejections and exemplar fingerprints, reuses unchanged
scores and updates affected comparisons. Uncertain rankings use exact fallback.
Active unnamed query vectors and a bounded number of best-person/example scores
remain in server memory; confirmed-example vectors are temporary build inputs.
The cache is not persisted or sent to the browser, and does not change the exact
matching algorithm.

`POST /batch` requires API-token authentication. Its body uses `action:"identity"`, `action:"accept"`, or `action:"dismiss"` and a `faces` array of `{id,revision}` objects. Identity takes one of `tag_id` (including `null` to clear) or `name`; accept takes `tag_id`; dismiss takes a boolean `dismissed`. Face IDs must be unique and positive. Structural validation errors return HTTP 400 for the request; per-face conflicts and validation failures appear in `failed` as `{id,status,error}` while successful faces appear in `updated`. The response is HTTP 200 even when some or all faces fail.

Face rectangles use oriented source-image pixels, floating-point coordinates,
and exclusive right and bottom boundaries. Landmarks use the same coordinate
space and order as the inference contract. Detector scores and cosine
similarities are scores, not probabilities. Suggestions include up to three
people tags above the configured cutoff; only explicit review actions assign an
identity. A supplied name is trimmed and limited to 100 UTF-8 bytes. A write
with an older face revision returns HTTP 409.

Job states are `running`, `cancelling`, `cancelled`, `completed`, `interrupted`,
and `failed`. Progress counts successful photos in `processed`, force-blocked
results and explicitly selected reusable items in `skipped`, errors in `failed`,
and pending work in `remaining`. A non-forced catalog scan queues only photos
without a completed analysis matching the catalog hash, model checksums,
pipeline version, and detector settings. Each photo result commits atomically.
If the process stops during a scan, startup marks the job `interrupted`; a later
catalog scan selects the remaining photos that still need analysis.

`GET /status` reports only the latest job. A newer successful job supersedes
older interruptions; the latest completed job remains available when it has
failed photos so the UI can offer a retry after reload.

## Catalog behavior

Schema migration v5 stores jobs, photo analysis metadata, detections, normalized
512-float embeddings, per-face rejection records, and tag-assignment
provenance. Per-photo metadata records source/model checksums, detector
settings, providers, ONNX Runtime version, and any CUDA fallback reason.
Existing media/tag links migrate as manual assignments. Confirmed faces derive
people tags for their photo; removing the last face assignment
removes only a face-owned tag. Removing a people tag from a photo clears matching
face identities and records those face/tag pairs as rejected. Deleting a person
tag clears face identities while retaining detections.

Schema migration v6 records the photo membership of each newly created scan job,
so job-scoped grids remain stable after a scan completes or is cancelled.

Changing a photo's content hash, dimensions, orientation, or media type through
the catalog invalidates its stored face results. Manual tags are retained.
Recognizer-only model changes refresh embeddings while retaining confirmed
identities and review decisions when detections remain compatible.

## Recognition features and examples

Each successful ArcFace feature is a finite, L2-normalized vector of 512
float32 values. `faces.embedding` stores it as a 2,048-byte SQLite BLOB;
`embedding_size` records its dimensions. Geometry, five landmarks, identity,
dismissal state, review revision, and any embedding error live in the same row.
`face_media_analysis` records source and model checksums, pipeline version,
runtime version, providers, device, fallback reason, and detector settings.
Failed embeddings remain empty and cannot become matching examples.

A confirmed, nondismissed face is an example for its people tag. An unnamed
face is compared with every eligible example from completed analyses using
cosine similarity. Each person's score is the maximum over their examples;
the top three people at or above the configured cutoff are suggested.
Eligibility requires the same recognizer checksum and pipeline version.
This does not retrain ArcFace or average a person's features. Clearing or
dismissing a face removes it from the example set, and deleting a person clears
their face identities. Rejected face/person pairs are excluded from suggestions.
Embeddings stay in the local catalog and are not included in browser responses.
Changing the detector, pipeline, or detector settings leaves completed reviewed
results in place and marks non-forced items skipped with a force-required error;
a forced replacement may reset those reviews. Reuse checks catalog metadata and does not read the original file. A physical file change
must be reimported through the catalog to invalidate stored results. Photos that
need inference are still checked against the catalog hash before and after
inference. A failed refresh retains the last successful analysis metadata,
embeddings, and review decisions until a replacement succeeds; the job records
the refresh failure separately, so restoring a prior source or model can reuse
the compatible committed results.
