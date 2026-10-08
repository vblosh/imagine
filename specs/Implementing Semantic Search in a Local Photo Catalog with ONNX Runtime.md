# Implementing Semantic Search in a Local Photo Catalog with ONNX Runtime

## Recommended design

Because the application already embeds ONNX Runtime, the cleanest design is to add **two compatible ONNX encoders**—one for images and one for text—plus a local vector index. A CLIP-family model is appropriate because its image and text encoders map both modalities into the same vector space; normalized image and text vectors can therefore be compared using cosine similarity or an equivalent dot product.[^1][^2]

For a self-hosted C++/JavaScript catalog with fewer than 100,000 photos, the recommended first implementation is:

- **C++ core:** image decoding, EXIF orientation, model preprocessing, ONNX Runtime inference, L2 normalization, incremental indexing, and nearest-neighbor search.
- **SQLite:** authoritative photo metadata, index status, model version, hashes, and optionally vectors.
- **Vector search:** start with exact SIMD search or Faiss `IndexFlatIP`; add HNSW only if measured query latency requires it.
- **JavaScript UI:** send a textual query and structured filters to the C++ backend, then render ranked photo IDs from the existing catalog.
- **Model:** begin with CLIP ViT-B/32 for a small, fast prototype; if German and Russian queries are important, benchmark a multilingual CLIP text encoder whose vectors are explicitly aligned with the same CLIP image encoder.[^3]

The key invariant is: **never mix embeddings from incompatible model revisions, dimensions, preprocessing pipelines, or normalization rules in one index**.

## Search pipeline

```text
Catalog import/change detector
        |
        v
Decode photo -> apply EXIF orientation -> RGB resize/crop/normalize
        |
        v
ONNX Runtime image encoder -> L2-normalized float vector
        |
        +--> vector index (photo_id -> embedding)
        +--> SQLite state (model_version, content_hash, indexed_at)

Search box -> tokenize text -> ONNX Runtime text encoder -> L2 normalize
        |
        v
nearest-neighbor search + metadata filters -> ranked photo IDs -> JS gallery
```

CLIP exposes separate image and text feature paths, and its similarity scores are based on cosine similarity between their features. In practice, export or acquire separate `image_encoder.onnx` and `text_encoder.onnx` graphs so bulk image indexing and interactive text queries do not execute unnecessary parts of a combined graph.[^1]

## Component choices

| Component | Recommended first choice | Why |
|---|---|---|
| Inference | Existing ONNX Runtime C++ integration | The C++ API is a thin RAII-style wrapper over the C API and supports CPU and hardware execution providers.[^4][^5] |
| Embedding model | CLIP ViT-B/32 baseline | Well-supported, 512-dimensional output in common exports, and practical for offline CPU use; verify every exported graph rather than assuming tensor names or dimensions.[^6][^7] |
| Multilingual queries | `clip-ViT-B-32-multilingual-v1` text encoder paired with its documented CLIP image encoder | Its model card describes a shared image/text space for more than 50 languages, including German and Russian.[^3] |
| Embedded vector store | Faiss `IndexFlatIP`, or a custom contiguous float matrix plus SIMD | Faiss is C++, supports dot-product search, and cosine search over normalized vectors.[^8] Exact search avoids approximate-recall surprises at this scale. |
| SQLite-only alternative | `sqlite-vec` | Pure C, portable, integrates metadata and vectors, but remains pre-v1 and warns of breaking changes.[^9] |
| Server-style alternative | Local Qdrant | Useful if advanced metadata filtering, facets, HNSW, or multiple clients are needed; JSON payloads can be filtered alongside vectors.[^10][^11] |

For fewer than 100,000 512-dimensional float32 vectors, raw vector storage is about **195 MiB** (`100000 x 512 x 4`), excluding index and metadata overhead. That is modest on a desktop, making exact search a sensible correctness-first baseline. A 768-dimensional index would use about 293 MiB, while 1024 dimensions would use about 391 MiB.

## Data model

Keep catalog metadata and embedding lifecycle state in SQLite even when Faiss owns the searchable vectors:

```sql
CREATE TABLE semantic_index_state (
    photo_id       INTEGER PRIMARY KEY,
    content_hash   BLOB NOT NULL,
    model_id       TEXT NOT NULL,
    model_revision TEXT NOT NULL,
    preprocess_id  TEXT NOT NULL,
    vector_dim     INTEGER NOT NULL,
    indexed_at     TEXT NOT NULL,
    error_text     TEXT,
    FOREIGN KEY(photo_id) REFERENCES photos(id) ON DELETE CASCADE
);

CREATE TABLE semantic_models (
    model_id          TEXT PRIMARY KEY,
    revision          TEXT NOT NULL,
    image_model_path  TEXT NOT NULL,
    text_model_path   TEXT NOT NULL,
    vector_dim        INTEGER NOT NULL,
    preprocess_json   TEXT NOT NULL,
    tokenizer_json    TEXT NOT NULL,
    created_at        TEXT NOT NULL
);
```

Use the catalog's stable numeric `photo_id` as the vector-index ID. Persist the Faiss index with an atomic temp-file rename, and treat SQLite as the source of truth for detecting missing, deleted, stale, or model-mismatched vectors.

If vectors are stored inside SQLite, use a BLOB containing packed `float32` values rather than JSON. `sqlite-vec` supports float, int8, and binary vectors in virtual tables and can place non-vector metadata alongside them.[^9]

## ONNX model contract

Before writing the production wrapper, inspect both graphs and record their exact contracts:

```text
image_encoder.onnx
  input:  pixel_values  float32 [batch, 3, height, width]
  output: image_embeds  float32 [batch, dimension]

text_encoder.onnx
  input:  input_ids      int64 [batch, sequence]
  input:  attention_mask int64 [batch, sequence]   # if graph requires it
  output: text_embeds     float32 [batch, dimension]
```

Do not hard-code these names until reading them through `Ort::Session::GetInputNameAllocated`, `GetOutputNameAllocated`, and tensor type/shape metadata. Community exports vary: some emit projected embeddings, some expose hidden states, some combine both towers, and some already normalize outputs.

Both outputs must have the same dimension. Normalize each output vector yourself unless the exported graph contract guarantees normalization:

```cpp
void l2Normalize(std::span<float> v) {
    double sum = 0.0;
    for (float x : v) sum += static_cast<double>(x) * x;
    const float inv = 1.0f / std::sqrt(std::max(sum, 1e-24));
    for (float& x : v) x *= inv;
}
```

After normalization, cosine similarity is simply the inner product. Faiss explicitly supports cosine similarity as a dot product over normalized vectors.[^8]

## Preprocessing correctness

Most poor semantic-search implementations fail in preprocessing rather than vector search. The C++ image pipeline must reproduce the model processor exactly:

1. Decode the image and convert to RGB.
2. Apply EXIF orientation before resizing.
3. Resize according to the processor's interpolation and shortest-edge policy.
4. Center-crop to the model input dimensions if required.
5. Convert channels to float in the required range.
6. Normalize each channel with the model's configured mean and standard deviation.
7. Arrange memory as NCHW or NHWC exactly as declared by the graph.

For standard Hugging Face CLIP processing, the documented image mean begins with `[0.48145466, 0.4578275, 0.40821073]`; use the checked-in processor configuration for all values rather than copying constants from an unrelated model. Tokenization is equally model-specific: bundle the exact vocabulary, merges/tokenizer files, special-token IDs, padding behavior, and maximum sequence length with the application.[^12]

Create a parity test during model export: run 20-50 fixture images and text queries through the reference Python implementation and through C++ ONNX Runtime, then compare normalized vectors and top-k rankings. This test should block model or preprocessing upgrades when similarity drifts beyond an agreed tolerance.

## C++ session structure

Create one long-lived session per encoder. ONNX Runtime supports graph optimization, configurable intra-op threading, and several hardware-specific execution providers.[^13][^14][^5]

```cpp
class ClipEncoder {
public:
    ClipEncoder(const std::filesystem::path& imageModel,
                const std::filesystem::path& textModel,
                int intraThreads)
        : env_(ORT_LOGGING_LEVEL_WARNING, "photo-semantic"),
          imageSession_(env_, imageModel.c_str(), makeOptions(intraThreads)),
          textSession_(env_, textModel.c_str(), makeOptions(intraThreads)) {}

    std::vector<float> encodeImage(const DecodedImage& image);
    std::vector<float> encodeText(std::string_view query);

private:
    static Ort::SessionOptions makeOptions(int threads) {
        Ort::SessionOptions o;
        o.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        o.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        if (threads > 0) o.SetIntraOpNumThreads(threads);
        return o;
    }

    Ort::Env env_;
    Ort::Session imageSession_;
    Ort::Session textSession_;
};
```

The production implementation should also:

- Cache input/output names and verified shapes once at startup.
- Reuse input and output buffers where possible.
- Warm both sessions with one dummy inference after construction.
- Avoid constructing a session for each photo or query.
- Batch background image inference, while keeping text queries batch size 1 for low latency.
- Use a bounded work queue so indexing cannot starve thumbnail generation or the UI.
- Record inference errors per photo and continue the catalog job.

ONNX Runtime's default CPU provider uses intra-op parallelism inside operators; if the thread count is left at zero, it generally uses physical cores, while graph execution remains sequential unless parallel execution is selected. For CLIP-like mostly linear graphs, start with sequential graph execution and benchmark intra-op counts such as 1, 2, 4, and physical-core count rather than assuming all cores are optimal.[^13]

## Image inference sketch

```cpp
std::vector<float> ClipEncoder::encodeImage(const DecodedImage& source) {
    TensorBuffer input = preprocessClipImage(source); // aligned float32 NCHW
    std::array<int64_t, 4> shape{1, 3, input.height, input.width};

    auto mem = Ort::MemoryInfo::CreateCpu(
        OrtArenaAllocator, OrtMemTypeDefault);
    auto tensor = Ort::Value::CreateTensor<float>(
        mem, input.data.data(), input.data.size(),
        shape.data(), shape.size());

    const char* inputNames[]  = {imageInputName_.c_str()};
    const char* outputNames[] = {imageOutputName_.c_str()};

    auto outputs = imageSession_.Run(
        Ort::RunOptions{nullptr},
        inputNames, &tensor, 1,
        outputNames, 1);

    auto info = outputs.GetTensorTypeAndShapeInfo();
    const size_t count = info.GetElementCount();
    const float* ptr = outputs.GetTensorData<float>();

    std::vector<float> embedding(ptr, ptr + count);
    l2Normalize(embedding);
    return embedding;
}
```

The text path follows the same pattern but creates one or more `int64` tensors from the tokenizer output. Validate at startup that both encoder outputs have the configured dimension and reject indexing if they differ.

## Index construction

With Faiss exact inner-product search:

```cpp
class PhotoVectorIndex {
public:
    explicit PhotoVectorIndex(int dimension)
        : flat_(dimension), ids_(&flat_) {}

    void add(int64_t photoId, const std::vector<float>& normalized) {
        if (normalized.size() != static_cast<size_t>(flat_.d))
            throw std::invalid_argument("embedding dimension mismatch");
        ids_.add_with_ids(1, normalized.data(), &photoId);
    }

    std::vector<SearchHit> search(std::spanst float> query, size_t k) {
        std::vector<float> scores(k);
        std::vector<faiss::idx_t> photoIds(k);
        ids_.search(1, query.data(), k, scores.data(), photoIds.data());

        std::vector<SearchHit> out;
        for (size_t i = 0; i < k && photoIds[i] >= 0; ++i)
            out.push_back({photoIds[i], scores[i]});
        return out;
    }

private:
    faiss::IndexFlatIP flat_;
    faiss::IndexIDMap2 ids_;
};
```

Faiss provides exact and approximate vector indexes, supports dot-product and Euclidean search, and is implemented in C++. For the initial implementation, exact search gives a reliable relevance baseline. If latency later becomes unacceptable, migrate behind the same interface to `IndexHNSWFlat`, then measure recall against `IndexFlatIP` using a held-out query set.[^15][^8]

### Updates and deletion

A robust incremental indexer should use this sequence:

1. Detect a new or modified catalog record using content hash, modification time, or both.
2. Generate the new embedding outside the index lock.
3. In one short critical section, remove the previous ID if present and add the replacement.
4. Update SQLite state only after the index update succeeds.
5. Periodically write an atomic snapshot and rebuild from SQLite if recovery detects inconsistency.

For exact flat indexes, deletion and frequent mutation may be simpler to handle with an append-only delta index plus periodic rebuild. At fewer than 100,000 photos, rebuilding the in-memory search index from persisted vectors is operationally simple.

## Search API

Expose a narrow backend endpoint rather than running model inference in the browser:

```http
POST /api/search/semantic
Content-Type: application/json

{
  "query": "red electric car in snow",
  "limit": 100,
  "filters": {
    "dateFrom": "2024-01-01",
    "dateTo": "2026-12-31",
    "ratingMin": 3,
    "cameraIds": [2, 7]
  }
}
```

```json
{
  "model": "clip-vit-b32-multilingual-v1@revision",
  "elapsedMs": 18,
  "items": [
    { "photoId": 8123, "score": 0.287 },
    { "photoId": 9914, "score": 0.274 }
  ]
}
```

Do not present the raw similarity value as a percentage or probability. It is useful for ranking, but its scale depends on the model, query, and catalog; calibrate any user-visible labels against manually judged examples.

## Metadata filtering

The simplest design is **filter first when the candidate set is small; over-fetch then filter when it is large**:

- For selective filters such as a narrow date range or one album, obtain candidate photo IDs from SQLite and exact-score only those vectors.
- For broad filters, request perhaps 5-10 times the visible result count from the vector index, filter using catalog metadata, and repeat with a larger candidate count if too few remain.
- For consistently complex filters, move the index to Qdrant or maintain filter-friendly bitsets beside the Faiss index.

Qdrant can store arbitrary JSON payload with vectors and apply filters during retrieval; its documentation recommends payload indexes for fields used frequently in filters. This capability is useful, but an extra server is usually unnecessary for a single-user catalog below 100,000 photos unless metadata-filter complexity justifies it.[^10][^11]

## Hybrid ranking

Semantic similarity should complement rather than replace the existing metadata and keyword search. A practical result score is:

```text
final = 0.70 * semantic_score
      + 0.15 * normalized_keyword_score
      + 0.10 * user_rating_boost
      + 0.05 * recency_or_album_boost
```

Treat these weights as starting hypotheses, not universal constants. Keep vector retrieval and reranking separate so weights can be changed without regenerating embeddings.

Useful structured interpretations include:

- `before:2024`, `after:2020`, `rating:>=4`, `camera:Sony` become metadata filters.
- The remaining phrase becomes the semantic text query.
- Quoted filename, tag, and person-name terms can retain exact-match boosts.
- Empty semantic text with filters becomes an ordinary catalog query.

## Similar-photo search

The same image index supports “find visually similar” with no text inference:

1. Retrieve the selected photo's normalized embedding.
2. Search the image index with that vector.
3. Exclude the source photo.
4. Optionally group near-duplicates or burst sequences.

CLIP works well for semantic similarity but may not be ideal for pixel-level duplicate detection; research has noted that generic CLIP representations can confuse visually different images with similar captions. Keep a separate perceptual hash or specialized visual descriptor for exact/near-duplicate detection, then use CLIP for conceptual similarity.[^16]

## Multilingual handling

Standard CLIP is strongest when queries resemble its training language distribution. For German, Russian, and English catalog use, either:

- use a multilingual CLIP model whose text tower is aligned to the chosen image tower;
- translate non-English queries locally to English, then run the standard text encoder; or
- index generated captions in multiple languages and combine text retrieval with image-vector retrieval.

The Sentence Transformers multilingual CLIP model card explicitly lists German and Russian among more than 50 aligned languages and states that its original CLIP image encoder is unchanged. Benchmark multilingual behavior on the actual catalog: queries such as “rotes Auto im Schnee,” “красная машина в снегу,” and “red car in snow” should retrieve substantially overlapping top results.[^3]

## ONNX Runtime optimization

Use a measurement-driven sequence:

1. **Correctness first:** FP32 CPU, exact vector search, fixed preprocessing, parity tests.
2. **Batch indexing:** benchmark image batches of 1, 4, 8, 16, and 32 under the real memory limit.
3. **Thread tuning:** test several intra-op counts; leave inter-op parallelism disabled unless profiling shows independent graph branches can benefit.
4. **Graph optimization:** enable `ORT_ENABLE_ALL`; optionally serialize an optimized model for faster startup. ONNX Runtime supports online and offline graph optimization, including simplification, fusion, and layout transformations.[^14]
5. **Execution providers:** if an NVIDIA GPU is present, prefer CUDA before CPU; ONNX Runtime assigns supported nodes according to provider priority.[^17][^5]
6. **Precision:** test FP16 on suitable GPUs and INT8 on CPU only after measuring retrieval quality.

ONNX Runtime's quantization documentation describes 8-bit conversion, recommends dynamic quantization generally for transformer models, and warns that quantization debugging is easier when model optimization is performed as a separate preprocessing step. Quantization must be evaluated by **retrieval metrics**, not merely tensor error: compare top-k overlap, recall on judged queries, indexing throughput, query latency, memory, and model size.[^18]

For NVIDIA deployment, ONNX Runtime's TensorRT provider should normally be registered before CUDA, with CUDA as fallback for unsupported nodes. This adds build and engine-cache complexity; use it only after CUDA profiling demonstrates a worthwhile bottleneck.[^19]

## Background indexing

Indexing should be resumable and unobtrusive:

```text
scan catalog
  -> enqueue only missing/stale model revisions
  -> decode/preprocess workers
  -> bounded inference batcher
  -> vector persistence/index writer
  -> commit semantic_index_state
  -> periodic snapshot/checkpoint
```

Recommended operational rules:

- Run indexing at low priority and pause when the user is editing or importing.
- Cap decoded-image memory separately from model-input memory.
- Persist progress every batch.
- Detect moved files by content identity rather than path alone.
- Invalidate vectors on model, tokenizer, preprocessing, or orientation-policy changes.
- Keep the old index active until the replacement index is complete, then switch via atomic rename or an index-generation pointer.

## Quality evaluation

Create a catalog-specific benchmark before tuning models. Record 50-200 realistic queries across objects, scenes, events, colors, activities, seasons, indoor/outdoor settings, and all target languages. For each query, mark relevant photos or at least judge a pooled top-20 set from competing configurations.

Track:

- Recall@10 and Recall@50.
- Precision@10 or nDCG@10 when relevance has grades.
- Median and p95 text-query embedding latency.
- Median and p95 nearest-neighbor latency.
- Images indexed per second.
- Peak memory during batch indexing.
- Top-k overlap versus the FP32 exact-search baseline.

Also include adversarial cases: text inside screenshots, abstract queries, small objects, black-and-white images, rotated photos, collages, and visually similar bursts. This reveals whether failures come from the model, preprocessing, metadata, or vector retrieval.

## Implementation sequence

### Phase 1: vertical slice

- Check in one image encoder, one text encoder, processor/tokenizer files, and a model manifest.
- Implement reference-compatible C++ preprocessing and tokenization.
- Create long-lived ONNX Runtime sessions.
- Index 1,000 representative photos into `IndexFlatIP`.
- Add one `/api/search/semantic` endpoint and a search mode in the JavaScript UI.
- Add Python-versus-C++ embedding parity tests.

### Phase 2: production indexing

- Add resumable background indexing and progress reporting.
- Add content/model revision invalidation.
- Persist vectors or a recoverable index snapshot.
- Handle updates, deletions, and atomic index replacement.
- Add metadata filtering and similar-photo search.

### Phase 3: optimize

- Benchmark batching and ONNX Runtime threads.
- Compare CPU FP32, CPU INT8, and available GPU providers.
- Evaluate multilingual models on German, Russian, and English query sets.
- Consider HNSW only when exact-search measurements justify the extra complexity.

## Common mistakes

- Using a generic text embedding model that is not aligned with the image encoder.
- Exporting a hidden-state tensor instead of the model's projected image/text embedding.
- Forgetting L2 normalization before inner-product search.
- Applying JPEG orientation after inference rather than before it.
- Using OpenCV's default BGR order when the processor expects RGB.
- Changing resize interpolation, crop policy, channel normalization, or tokenizer files without rebuilding the index.
- Recreating ONNX Runtime sessions for every item.
- Running too many parallel inference calls while each session already uses all CPU cores.
- Treating cosine scores as calibrated confidence percentages.
- Upgrading the model in place and mixing old and new vectors.

## Acceptance criteria

A release-ready implementation should satisfy all of the following:

- C++ and the reference exporter produce close normalized vectors for fixtures.
- Text and image encoders return equal dimensions and compatible embeddings.
- Index rebuild resumes safely after interruption.
- Deleted photos disappear from results; changed photos are re-embedded.
- Search works entirely offline.
- German, Russian, and English benchmark queries meet agreed relevance targets.
- Median and p95 latency are measured on target hardware.
- Exact FP32 search remains available as a regression oracle.
- Every persisted vector is traceable to a model revision and preprocessing manifest.

---

## References

1. [github.com · openai · CLIPGitHub - openai/CLIP: CLIP (Contrastive Language-Image...](https://github.com/openai/CLIP) - [Blog] [Paper] [Model Card] [Colab]

2. [docs.oracle.com · en · databaseONNX Pipeline Models: CLIP Multi-Modal Embedding](https://docs.oracle.com/en/database/oracle/machine-learning/oml4py/2-23ai/mlpug/onnx-pipeline-models-multi-modal-embedding.html) - The pipeline generator will generate two ONNX pipeline models for a pretrained CLIP model, distingui...

3. [sentence-transformers/clip-ViT-B-32-multilingual-v1 - Hugging Face](https://huggingface.co/sentence-transformers/clip-ViT-B-32-multilingual-v1) - We’re on a journey to advance and democratize artificial intelligence through open source and open s...

4. [onnxruntime.ai · docs · get-startedC++ | onnxruntime](https://onnxruntime.ai/docs/get-started/with-cpp.html) - The C++ API is a thin wrapper of the C API. Please refer to C API for more details. See Tutorials: A...

5. [ONNX Runtime Execution Providers](https://onnxruntime.ai/docs/execution-providers/) - ONNX Runtime works with different hardware acceleration libraries through its extensible Execution P...

6. [Pipelines](https://huggingface.co/docs/transformers.js/api/pipelines) - Feature extraction pipeline using no model head. This pipeline extracts the hidden states from the b...

7. [huggingface.co · sayantan47 · clip-vit-b32-onnxsayantan47/clip-vit-b32-onnx · Hugging Face](https://huggingface.co/sayantan47/clip-vit-b32-onnx) - It supports fast image-text similarity and zero-shot classification without requiring PyTorch or Ten...

8. [github.com · facebookresearch · faissGitHub - facebookresearch/faiss: A library for efficient...](https://github.com/facebookresearch/faiss) - Faiss is a library for efficient similarity search and clustering of dense vectors. It contains algo...

9. [GitHub - asg017/sqlite-vec: A vector search SQLite extension that ...](https://github.com/asg017/sqlite-vec) - Store and query float, int8, and binary vectors in vec0 virtual tables; Written in pure C, no depend...

10. [qdrant.tech · documentation · manage-dataPayload - Qdrant](https://qdrant.tech/documentation/manage-data/payload/) - Store JSON payloads alongside vectors in Qdrant, then use them for filtering, faceting, and result r...

11. [qdrant.tech · documentation · searchFiltering - Qdrant](https://qdrant.tech/documentation/search/filtering/) - Filter Qdrant search results with payload conditions on metadata and IDs, combining database-style c...

12. [CLIP - Hugging Face](https://huggingface.co/docs/transformers/en/model_doc/clip) - CLIP is a multimodal vision and language model motivated by overcoming the fixed number of object ca...

13. [Thread management | onnxruntime](https://onnxruntime.ai/docs/performance/tune-performance/threading.html) - Onnxruntime sessions utilize multi-threading to parallelize computation inside each operator. The in...

14. [onnxruntime.ai › docs › performanceGraph optimizations | onnxruntime](https://onnxruntime.ai/docs/performance/model-optimizations/graph-optimizations.html) - ONNX Runtime: cross-platform, high performance ML inferencing and training accelerator

15. [faiss.aiWelcome to Faiss Documentation](https://faiss.ai/) - Faiss is a library for efficient similarity search and clustering of dense vectors. It contains algo...

16. [arxiv.org · html · 2409Optimizing CLIP Models for Image Retrieval with Maintained Joint...](https://arxiv.org/html/2409.01936v1) - This paper addresses the challenge of optimizing CLIP models for various image-based similarity sear...

17. [CUDA Execution Provider](https://onnxruntime.ai/docs/execution-providers/CUDA-ExecutionProvider.html) - The CUDA Execution Provider supports the following configuration options. device_id. The device ID. ...

18. [onnxruntime.ai · model-optimizations · quantizationQuantize ONNX Models - onnxruntime](https://onnxruntime.ai/docs/performance/model-optimizations/quantization.html) - In order to leverage these optimizations, you need to optimize your models using the Transformer Mod...

19. [NVIDIA - TensorRT | onnxruntime](https://onnxruntime.ai/docs/execution-providers/TensorRT-ExecutionProvider.html) - TensorRT configurations can be set by execution provider options. It's useful when each model and in...

