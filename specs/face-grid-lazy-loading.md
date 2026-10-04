# Lazy loading for Review faces

## Goal

Open catalogs with 25,000 photos without downloading or rendering every face.
Retain one group per person and allow one click to accept every eligible
suggestion in a group, including faces not yet loaded in the browser.

## Design

1. Fetch compact group summaries with face counts and distinct-photo suggestion
   counts. Apply named, unnamed, dismissed and confirmed-person filters on the
   server. Preserve catalog and scan-job scopes.
2. Fetch small pages of cards only for visible or nearby content. Bound browser
   rendering and cancel obsolete requests when the dialog closes or its scope
   or filters change. Crop images continue to load lazily.
3. Build a bounded server review index once per catalog generation. Reuse it
   for summaries and card pages; invalidate it after local database mutations
   and commits made through another connection. Never return embeddings.
4. Accept a person group on the server using its current snapshot and review
   revisions. Count distinct photos on the button, update only eligible unnamed
   active faces, and preserve partial failures and manual tag ownership.
5. Keep selected-face actions, stale-edit conflicts, failed selections, active
   photo-query refreshes, keyboard access and the existing five languages.

Exact suggestion matching may still take time when an index first builds or
becomes stale. Lazy card loading reduces response size and browser work; it
does not change recognition quality or turn exact matching into constant-time
work. No model, embedding format or database schema change is required.

## Validation

- Native tests: group membership/counts, bounded paging, filter and job scope,
  whole-group acceptance beyond the loaded page, conflicts, authentication,
  rejection handling and cache invalidation after local and external writes.
- Browser tests: a 25,000-face group initially fetches and renders bounded
  content; moving through the grid loads additional pages; accepting the group
  affects unloaded faces; partial failures, filter changes and cancellation
  remain usable.
- Measure summary and card-page requests on an isolated synthetic 25,000-photo
  catalog. Distinguish cold index construction from cached page loading.
- Build with and without ONNX Runtime using Ninja and run the full native and
  browser suites after focused validation.

Implementation uses gpt-6-luna subagents at xhigh for backend, browser and
independent review, with integration and validation by the primary agent.

Implemented on 4 October 2026. Review faces uses 48-card pages, at most 96 rendered
cards and four cached pages; whole-group acceptance includes unloaded faces.
API contracts and measured 25,000-photo results are recorded in
[the API guide](../docs/face-analysis-api.md) and
[the validation report](../docs/face-analysis-validation.md).

## Repeated review refreshes

Catalog writes previously discarded all exact suggestion work. With thousands
of unnamed faces, even a rating edit or dismissing one detection could repeat
every query/example comparison. Rebuild frozen review snapshots after writes,
but reuse matching work when the actual recognition inputs are unchanged.
For small example-set changes, update only affected comparisons. Keep this cache
in server memory and use exact fallback whenever the retained candidates cannot
prove the current top three. Rejections, model/pipeline changes, external vector
edits and person-name tie ordering must remain correct.

Validate unrelated catalog edits, dismissal, confirmation, clearing and rejection
on an isolated 25,000-photo catalog with many unnamed faces, measuring the
summary refresh after each edit separately from the initial matching pass.

Implemented and validated on 4 October 2026: the 25,000-photo / 10,000-unnamed
synthetic catalog refreshed after each tested edit in 384–445 ms. The initial
matching pass and uncertain changes still use full exact matching. See the
validation report for workload limits, comparisons and regression results.
