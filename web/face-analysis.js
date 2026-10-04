/** Browser controls for local face analysis and human identity review. */

import { api, escapeHtml } from './api.js';
import { dom, showToast } from './dom.js';
import { state } from './state.js';
import { t } from './i18n.js';
import { loadMedia, loadMetadata } from './media-grid.js';

const GRID_PAGE_SIZE = 200;
const GRID_API_PAGE_SIZE = 1000;
const FACE_BATCH_SIZE = 1000;
const JOB_REQUEST_KEY = 'imagine_face_job_request';
const TERMINAL_JOB_STATES = new Set(['cancelled', 'completed', 'interrupted', 'failed']);
const faceById = new Map();
const inspectorFaceById = new Map();
const faceOverlayContexts = new WeakMap();
const faceOverlayTrackers = new WeakMap();
const faceOverlayNodes = new Set();

let faceStatus = null;
let currentJob = null;
let currentRequest = null;
let submissionError = '';
let jobPollTimer = null;
let jobPollInFlight = false;
let scanDialogSelectedIds = [];
let gridFaces = [];
let gridTotal = 0;
let gridPage = 0;
let gridLoadToken = 0;
let gridViewToken = 0;
let gridJobId = null;
let gridIsLoading = false;
let gridActionBusy = false;
let gridSelection = new Set();
let gridRangeAnchor = null;
let gridLastClicked = null;
let gridModalReturnFocus = null;
let scanModalReturnFocus = null;
let forcePendingRequest = null;
let currentInspectorMediaId = null;
let inspectorRequestId = 0;
let busyFaces = new Set();
let lastAutoOpenedJobId = null;
let faceOverlayResizeListenerBound = false;

const byId = id => document.getElementById(id);

function utf8ByteLength(value) {
  let bytes = 0;
  for (const character of String(value)) {
    const codePoint = character.codePointAt(0);
    bytes += codePoint <= 0x7f ? 1 : codePoint <= 0x7ff ? 2 : codePoint <= 0xffff ? 3 : 4;
  }
  return bytes;
}

function isFaceReady() {
  return Boolean(faceStatus && faceStatus.built === true && faceStatus.ready === true);
}

function faceJobNeedsRetry(job = currentJob) {
  return Boolean(job && ((Number(job.remaining) || 0) > 0 || (Number(job.failed) || 0) > 0));
}

function faceSetupMessage() {
  if (!faceStatus) return t('face_status_unavailable');
  if (faceStatus.built !== true) return t('face_unavailable');
  if (faceStatus.ready !== true) {
    return String(faceStatus.error || t('face_unavailable'));
  }
  return '';
}

function showSetupNotice(node, message) {
  if (!node) return;
  node.textContent = message || '';
  node.hidden = !message;
}

function updateAnalyzeAvailability() {
  const message = faceSetupMessage();
  showSetupNotice(byId('faceGridNotice'), message);
  showSetupNotice(byId('faceJobNotice'), message);
  if (byId('faceJobStartBtn')) byId('faceJobStartBtn').disabled = !isFaceReady();
}

async function refreshFaceStatus() {
  try {
    faceStatus = await api.get('/api/faces/status');
  } catch (_) {
    faceStatus = { built: false, ready: false, error: t('face_status_unavailable') };
  }
  updateAnalyzeAvailability();
  updatePeopleReviewCount();
  const job = faceStatus && faceStatus.job;
  if (job && job.id) {
    currentJob = job;
    const needsRetry = (Number(job.remaining) || 0) > 0 || (Number(job.failed) || 0) > 0;
    // The in-memory request is persisted for this browser, but a restarted
    // server may report an interrupted job created by another session. The
    // API intentionally does not expose the original ID list, so offer the
    // safe catalog retry path for any unfinished work in that case.
    currentRequest = readSavedJobRequest(job.id) || (job.state && TERMINAL_JOB_STATES.has(job.state)
      ? (needsRetry ? { scope: 'catalog', media_ids: [] } : null)
      : { scope: 'catalog', media_ids: [] });
    renderJobState();
    if (!TERMINAL_JOB_STATES.has(job.state)) {
      openJobModal();
      scheduleJobPoll(0);
    } else if ((Number(job.remaining) || 0) > 0 || (Number(job.failed) || 0) > 0) {
      openJobModal();
    }
  }
}

function readSavedJobRequest(jobId) {
  try {
    const saved = JSON.parse(localStorage.getItem(JOB_REQUEST_KEY) || 'null');
    return saved && String(saved.job_id) === String(jobId) ? saved.request : null;
  } catch (_) {
    return null;
  }
}

function saveJobRequest(jobId, request) {
  try {
    localStorage.setItem(JOB_REQUEST_KEY, JSON.stringify({ job_id: jobId, request }));
  } catch (_) {}
}

function clearSavedJobRequest(jobId) {
  try {
    const saved = JSON.parse(localStorage.getItem(JOB_REQUEST_KEY) || 'null');
    if (!jobId || (saved && String(saved.job_id) === String(jobId))) {
      localStorage.removeItem(JOB_REQUEST_KEY);
    }
  } catch (_) {}
}

function openJobModal() {
  const modal = byId('faceJobModal');
  if (!modal) return;
  if (modal.style.display !== 'flex') scanModalReturnFocus = document.activeElement;
  modal.style.display = 'flex';
  renderJobState();
  const focusTarget = currentJob ? byId('faceJobCloseBtn') : byId('faceScanScope');
  focusTarget?.focus();
}

function closeJobModal(restoreFocus = true) {
  const modal = byId('faceJobModal');
  if (modal) modal.style.display = 'none';
  if (restoreFocus && scanModalReturnFocus && typeof scanModalReturnFocus.focus === 'function' && document.contains(scanModalReturnFocus)) {
    scanModalReturnFocus.focus();
  }
  scanModalReturnFocus = null;
}

function setJobButton(button, visible, disabled = false) {
  if (!button) return;
  button.hidden = !visible;
  button.disabled = disabled;
}

function renderJobState() {
  updateAnalyzeAvailability();
  const hasJob = Boolean(currentJob);
  const initial = !hasJob && Boolean(currentRequest);
  const running = hasJob && !TERMINAL_JOB_STATES.has(currentJob.state);
  const stateNode = byId('faceJobState');
  const progressWrap = byId('faceJobProgressWrap');
  const progress = byId('faceJobProgress');
  const counts = byId('faceJobCounts');
  const error = byId('faceJobError');
  const forceOption = byId('faceForceOption');
  const scopeLabel = byId('faceScanScopeWrap');

  if (initial) {
    const count = currentRequest.scope === 'selected' ? currentRequest.media_ids.length : null;
    stateNode.textContent = currentRequest.scope === 'catalog'
      ? t('face_start_catalog_prompt')
      : t('face_start_selected_prompt', { count });
  } else if (hasJob) {
    const stateKey = `face_job_state_${currentJob.state}`;
    stateNode.textContent = t(stateKey);
  } else {
    stateNode.textContent = '';
  }

  if (progressWrap) progressWrap.hidden = !hasJob;
  if (hasJob) {
    const total = Math.max(0, Number(currentJob.total) || 0);
    const processed = Math.max(0, Number(currentJob.processed) || 0);
    const skipped = Math.max(0, Number(currentJob.skipped) || 0);
    const failed = Math.max(0, Number(currentJob.failed) || 0);
    const remaining = Math.max(0, Number(currentJob.remaining) || 0);
    const done = Math.min(total, processed + skipped + failed);
    if (progress) progress.value = total ? Math.round(done * 100 / total) : 0;
    if (counts) counts.textContent = t('face_job_counts', { processed, skipped, failed, remaining, total });
  }

  const errorMessage = submissionError || (hasJob && currentJob.error ? String(currentJob.error) : '');
  if (error) {
    error.textContent = errorMessage;
    error.hidden = !errorMessage;
  }
  if (forceOption) forceOption.hidden = !initial;
  if (scopeLabel) scopeLabel.hidden = !initial;
  if (byId('faceSelectedScopeOption')) {
    byId('faceSelectedScopeOption').disabled = scanDialogSelectedIds.length === 0;
  }

  setJobButton(byId('faceJobCancelBtn'), initial);
  setJobButton(byId('faceJobStartBtn'), initial, !isFaceReady());
  setJobButton(byId('faceJobStopBtn'), running, currentJob && currentJob.state === 'cancelling');
  const canRetry = hasJob && !running && Boolean(currentRequest)
    && ((Number(currentJob.remaining) || 0) > 0 || (Number(currentJob.failed) || 0) > 0);
  setJobButton(byId('faceJobRetryBtn'), canRetry, !isFaceReady());
  setJobButton(byId('faceJobDoneBtn'), hasJob && !running);

  setJobButton(byId('faceViewFacesBtn'), !running || Boolean(currentJob));
}

function selectedPhotoIds() {
  return Array.from(state.selectedIds)
    .map(Number)
    .filter(id => Number.isInteger(id) && id > 0)
    .filter(id => {
      const item = (state.mediaItems || []).find(media => Number(media.id) === id);
      return item && (!item.media_type || item.media_type === 'photo');
    });
}

function openFaceAnalysisDialog() {
  if (currentJob && (!TERMINAL_JOB_STATES.has(currentJob.state) || faceJobNeedsRetry(currentJob))) {
    openJobModal();
    return;
  }
  scanDialogSelectedIds = selectedPhotoIds();
  currentJob = null;
  const scope = byId('faceScanScope');
  if (scope) scope.value = scanDialogSelectedIds.length ? 'selected' : 'catalog';
  currentRequest = scanDialogSelectedIds.length
    ? { scope: 'selected', media_ids: [...scanDialogSelectedIds] }
    : { scope: 'catalog', media_ids: [] };
  if (byId('faceForceReanalysis')) byId('faceForceReanalysis').checked = false;
  openJobModal();
}

async function startFaceJob(retry = false) {
  if (!retry) {
    const scope = byId('faceScanScope')?.value || 'catalog';
    currentRequest = scope === 'selected'
      ? { scope, media_ids: [...scanDialogSelectedIds] }
      : { scope, media_ids: [] };
  }
  if (!currentRequest || !isFaceReady()) return;
  const force = !retry && Boolean(byId('faceForceReanalysis')?.checked);
  const request = { ...currentRequest, force };
  if (force && !retry) {
    forcePendingRequest = request;
    openForceWarningModal();
    return;
  }
  await submitFaceJob(request);
}

async function submitFaceJob(request) {
  currentRequest = request;
  submissionError = '';
  const body = { scope: request.scope, force: request.force === true };
  if (request.scope === 'selected') body.media_ids = request.media_ids;

  setJobButton(byId('faceJobStartBtn'), true, true);
  const error = byId('faceJobError');
  if (error) { error.textContent = ''; error.hidden = true; }
  try {
    const result = await api.post('/api/faces/jobs', body);
    currentJob = result && result.job ? result.job : result;
    if (!currentJob || !currentJob.id) throw new Error(t('face_status_unavailable'));
    saveJobRequest(currentJob.id, request);
    renderJobState();
    if (TERMINAL_JOB_STATES.has(currentJob.state)) {
      if (currentJob.state === 'completed') handleCompletedJob(currentJob);
    } else {
      scheduleJobPoll(0);
    }
  } catch (err) {
    submissionError = t('face_job_start_error', { error: err.message });
    renderJobState();
  }
}

function openForceWarningModal() {
  const modal = byId('faceForceWarningModal');
  if (!modal) return submitFaceJob(forcePendingRequest);
  modal.style.display = 'flex';
  byId('faceForceCancelBtn')?.focus();
}

function closeForceWarningModal() {
  const modal = byId('faceForceWarningModal');
  if (modal) modal.style.display = 'none';
  forcePendingRequest = null;
  byId('faceJobStartBtn')?.focus();
}

async function confirmForceWarning() {
  const request = forcePendingRequest;
  if (!request) return closeForceWarningModal();
  const modal = byId('faceForceWarningModal');
  if (modal) modal.style.display = 'none';
  forcePendingRequest = null;
  await submitFaceJob(request);
}

function scheduleJobPoll(delay = 1300) {
  if (jobPollTimer) window.clearTimeout(jobPollTimer);
  if (!currentJob || TERMINAL_JOB_STATES.has(currentJob.state)) return;
  jobPollTimer = window.setTimeout(pollFaceJob, delay);
}

async function pollFaceJob() {
  if (!currentJob || TERMINAL_JOB_STATES.has(currentJob.state) || jobPollInFlight) return;
  jobPollInFlight = true;
  try {
    const result = await api.get(`/api/faces/jobs/${encodeURIComponent(currentJob.id)}`);
    currentJob = result && result.job ? result.job : result;
    renderJobState();
    if (TERMINAL_JOB_STATES.has(currentJob.state)) {
      if (currentJob.state === 'completed') handleCompletedJob(currentJob);
      if (currentInspectorMediaId) refreshInspectorFaces(currentInspectorMediaId, { force: true });
    } else {
      scheduleJobPoll();
    }
  } catch (err) {
    const error = byId('faceJobError');
    if (error) {
      error.textContent = err.message;
      error.hidden = false;
    }
    scheduleJobPoll(3000);
  } finally {
    jobPollInFlight = false;
  }
}

async function cancelFaceJob() {
  if (!currentJob || TERMINAL_JOB_STATES.has(currentJob.state)) return;
  setJobButton(byId('faceJobStopBtn'), true, true);
  try {
    const result = await api.post(`/api/faces/jobs/${encodeURIComponent(currentJob.id)}/cancel`, {});
    currentJob = result && result.job ? result.job : (result && result.id ? result : { ...currentJob, state: 'cancelling' });
    renderJobState();
    scheduleJobPoll(0);
  } catch (err) {
    const error = byId('faceJobError');
    if (error) {
      error.textContent = t('face_job_cancel_error', { error: err.message });
      error.hidden = false;
    }
    renderJobState();
  }
}

async function retryFaceJob() {
  if (!currentRequest) return;
  const retryRequest = { ...currentRequest, force: false };
  if (retryRequest.scope === 'selected') retryRequest.media_ids = [...retryRequest.media_ids];
  currentRequest = retryRequest;
  currentJob = null;
  if (byId('faceForceReanalysis')) byId('faceForceReanalysis').checked = false;
  await startFaceJob(true);
}

function peopleTags() {
  return (state.tags || []).filter(tag => String(tag.category || 'keyword').toLowerCase() === 'people');
}

function safeCropUrl(face) {
  const fallback = `/api/faces/${encodeURIComponent(Number(face.id) || 0)}/crop?revision=${encodeURIComponent(Number(face.revision) || 0)}`;
  const candidate = typeof face.crop_url === 'string' ? face.crop_url : '';
  if (candidate.startsWith('/') && !candidate.startsWith('//')) return candidate;
  return fallback;
}

function faceCardMarkup(face) {
  const id = Number(face.id);
  if (!Number.isInteger(id) || id <= 0) return '';
  faceById.set(String(id), face);
  const currentName = face.person_name || t('face_name_person');
  const image = `<img class="face-card-crop" src="${escapeHtml(safeCropUrl(face))}" alt="${escapeHtml(t('face_crop_alt', { name: currentName }))}" loading="lazy">`;
  const mediaLabel = t('face_open_photo', { id: Number(face.media_id) || '' });
  const suggestion = !face.dismissed && Array.isArray(face.suggestions) ? face.suggestions[0] : null;
  const removeLabel = face.person_tag_id != null ? t('face_clear_identity') : t('face_dismiss');
  const removeButton = !face.dismissed
    ? `<button type="button" class="face-card-remove" data-face-remove data-face-id="${id}" title="${escapeHtml(removeLabel)}" aria-label="${escapeHtml(removeLabel)}"${busyFaces.has(String(id)) ? ' disabled' : ''}>×</button>`
    : '';
  const detail = face.dismissed ? t('face_dismissed')
    : face.person_tag_id != null ? t('face_confirmed_identity')
      : suggestion ? t('face_suggested_person', { name: suggestion.name || '' })
        : t('face_unnamed');
  return `<article class="face-card" data-face-id="${id}">
    ${removeButton}<div class="face-card-image-row">${image}<div class="face-card-copy"><div class="face-card-title">${escapeHtml(currentName)}</div><div class="face-card-meta">${escapeHtml(mediaLabel)}</div><div class="face-card-meta">${escapeHtml(detail)}</div></div></div>
  </article>`;
}

function renderFaceCards(container, faces) {
  if (!container) return;
  inspectorFaceById.clear();
  for (const face of faces || []) inspectorFaceById.set(String(face.id), face);
  container.innerHTML = (faces || []).map(face => faceCardMarkup(face)).join('');
}

function updateFaceOverlayGeometry(svg) {
  const context = faceOverlayContexts.get(svg);
  if (!context) return;
  const { image, wrapper, mediaData, enabled } = context;
  const width = Number(mediaData && mediaData.width);
  const height = Number(mediaData && mediaData.height);
  if (!(enabled && width > 0 && height > 0 && svg.childElementCount > 0 && image && wrapper)) {
    svg.setAttribute('hidden', '');
    return;
  }
  const imageRect = image.getBoundingClientRect();
  const wrapperRect = wrapper.getBoundingClientRect();
  if (!(imageRect.width > 0 && imageRect.height > 0 && wrapperRect.width > 0 && wrapperRect.height > 0)) {
    svg.setAttribute('hidden', '');
    return;
  }
  svg.setAttribute('viewBox', `0 0 ${width} ${height}`);
  svg.setAttribute('preserveAspectRatio', 'xMidYMid meet');
  svg.style.inset = 'auto';
  svg.style.left = `${imageRect.left - wrapperRect.left}px`;
  svg.style.top = `${imageRect.top - wrapperRect.top}px`;
  svg.style.right = 'auto';
  svg.style.bottom = 'auto';
  svg.style.width = `${imageRect.width}px`;
  svg.style.height = `${imageRect.height}px`;
  svg.style.transform = '';
  svg.removeAttribute('hidden');
}

function scheduleFaceOverlayGeometry(svg, followTransform = false) {
  const tracker = faceOverlayTrackers.get(svg);
  updateFaceOverlayGeometry(svg);
  if (!tracker || tracker.frame) return;
  const started = performance.now();
  const tick = now => {
    updateFaceOverlayGeometry(svg);
    if (followTransform && now - started < 240) {
      tracker.frame = requestAnimationFrame(tick);
    } else {
      tracker.frame = 0;
    }
  };
  if (followTransform) tracker.frame = requestAnimationFrame(tick);
}

function ensureFaceOverlayTracking(svg, image, wrapper, followTransform = false) {
  if (!svg || !image || !wrapper) return;
  faceOverlayNodes.add(svg);
  if (!faceOverlayResizeListenerBound) {
    window.addEventListener('resize', () => {
      for (const node of faceOverlayNodes) scheduleFaceOverlayGeometry(node);
    });
    faceOverlayResizeListenerBound = true;
  }
  const previous = faceOverlayTrackers.get(svg);
  if (previous && previous.image === image && previous.wrapper === wrapper) return;
  previous?.resizeObserver?.disconnect();
  previous?.mutationObserver?.disconnect();
  const tracker = { image, wrapper, frame: 0, resizeObserver: null, mutationObserver: null };
  const onLoad = () => scheduleFaceOverlayGeometry(svg, followTransform);
  image.addEventListener('load', onLoad);
  image.addEventListener('transitionend', event => {
    if (event.propertyName === 'transform') scheduleFaceOverlayGeometry(svg);
  });
  if (typeof ResizeObserver !== 'undefined') {
    tracker.resizeObserver = new ResizeObserver(() => scheduleFaceOverlayGeometry(svg));
    tracker.resizeObserver.observe(image);
    tracker.resizeObserver.observe(wrapper);
  }
  if (followTransform && typeof MutationObserver !== 'undefined') {
    tracker.mutationObserver = new MutationObserver(() => scheduleFaceOverlayGeometry(svg, true));
    tracker.mutationObserver.observe(image, { attributes: true, attributeFilter: ['style'] });
  }
  faceOverlayTrackers.set(svg, tracker);
}

function renderFaceOverlay(svg, mediaData, enabled, image, wrapper, followTransform = false) {
  if (!svg) return;
  svg.replaceChildren();
  faceOverlayContexts.set(svg, { mediaData, enabled, image, wrapper });
  ensureFaceOverlayTracking(svg, image, wrapper, followTransform);
  const width = Number(mediaData && mediaData.width);
  const height = Number(mediaData && mediaData.height);
  const faces = Array.isArray(mediaData && mediaData.faces) ? mediaData.faces : [];
  if (width > 0 && height > 0) {
    svg.setAttribute('viewBox', `0 0 ${width} ${height}`);
    svg.setAttribute('preserveAspectRatio', 'xMidYMid meet');
    for (const face of faces) {
      const x = Number(face.x);
      const y = Number(face.y);
      const faceWidth = Number(face.width);
      const faceHeight = Number(face.height);
      if (face.dismissed || ![x, y, faceWidth, faceHeight].every(Number.isFinite) || faceWidth <= 0 || faceHeight <= 0) continue;
      const rect = document.createElementNS('http://www.w3.org/2000/svg', 'rect');
      rect.setAttribute('class', 'face-box');
      rect.setAttribute('x', String(x));
      rect.setAttribute('y', String(y));
      rect.setAttribute('width', String(faceWidth));
      rect.setAttribute('height', String(faceHeight));
      const title = document.createElementNS('http://www.w3.org/2000/svg', 'title');
      title.textContent = face.person_name || t('face_name_person');
      rect.appendChild(title);
      svg.appendChild(rect);
    }
  }
  scheduleFaceOverlayGeometry(svg, followTransform);
}

async function refreshInspectorFaces(mediaId, { force = false } = {}) {
  const section = byId('inspectorFacesSection');
  const list = byId('inspectorFaceList');
  const status = byId('inspectorFaceState');
  const toggle = byId('inspectorFaceOverlayToggle');
  const image = byId('inspectorImg');
  const wrapper = byId('inspectorPreviewWrapper');
  const id = Number(mediaId);
  if (!Number.isInteger(id) || id <= 0 || !section) return;
  currentInspectorMediaId = id;
  section.style.display = 'block';
  if (!force && list?.dataset.mediaId === String(id) && list.dataset.loaded === 'true') return;
  const requestId = ++inspectorRequestId;
  if (status) status.textContent = t('face_loading');
  if (list) {
    list.dataset.mediaId = String(id);
    list.dataset.loaded = 'false';
  }
  try {
    const data = await api.get(`/api/faces/media/${id}`);
    if (requestId !== inspectorRequestId || currentInspectorMediaId !== id) return;
    const faces = Array.isArray(data.faces) ? data.faces : [];
    faces.forEach(face => faceById.set(String(face.id), face));
    if (list) {
      renderFaceCards(list, faces, 'inspector');
      list.dataset.loaded = 'true';
    }
    const stateName = String(data.state || '').toLowerCase();
    if (status) {
      if (data.error) status.textContent = t('face_failed', { error: data.error });
      else if (faces.length) status.textContent = t('face_count', { count: faces.length });
      else if (['complete', 'completed', 'done', 'succeeded'].includes(stateName)) status.textContent = t('face_no_faces');
      else if (['failed', 'error'].includes(stateName)) status.textContent = t('face_failed', { error: data.error || t('face_status_unavailable') });
      else status.textContent = t('face_not_analyzed');
    }
    renderFaceOverlay(byId('inspectorFaceOverlay'), data, Boolean(toggle?.checked), image, wrapper);
  } catch (err) {
    if (requestId !== inspectorRequestId || currentInspectorMediaId !== id) return;
    inspectorFaceById.clear();
    if (list) { list.replaceChildren(); list.dataset.loaded = 'false'; }
    if (status) status.textContent = t('face_failed', { error: err.message });
    renderFaceOverlay(byId('inspectorFaceOverlay'), null, false, image, wrapper);
  }
}

async function removeInspectorFace(event) {
  const button = event.target.closest('[data-face-remove][data-face-id]');
  if (!button || button.disabled) return;
  const id = Number(button.dataset.faceId);
  const key = String(id);
  const face = inspectorFaceById.get(key);
  const list = byId('inspectorFaceList');
  const mediaId = Number(face && face.media_id);
  if (!Number.isInteger(id) || id <= 0 || !face || face.dismissed || busyFaces.has(key) ||
      !Number.isInteger(mediaId) || mediaId <= 0 || Number(list?.dataset.mediaId) !== mediaId) return;

  const clearIdentity = face.person_tag_id != null;
  const action = clearIdentity ? 'identity' : 'dismiss';
  const body = clearIdentity
    ? { revision: Number(face.revision), tag_id: null }
    : { revision: Number(face.revision), dismissed: true };
  busyFaces.add(key);
  button.disabled = true;
  try {
    const path = `/api/faces/${encodeURIComponent(id)}/${action}`;
    const updated = await api.post(path, body);
    busyFaces.delete(key);
    try {
      await refreshAfterFaceChanges([{ ...face, ...(updated || {}), media_id: Number(updated?.media_id) || mediaId }]);
    } catch (_) {
      if (currentInspectorMediaId === mediaId) await refreshInspectorFaces(mediaId, { force: true });
    }
    try { await refreshOpenFaceGrid(); } catch (_) {}
  } catch (err) {
    busyFaces.delete(key);
    if (currentInspectorMediaId === mediaId) {
      try { await refreshInspectorFaces(mediaId, { force: true }); } catch (_) {}
    }
    try { await refreshOpenFaceGrid(); } catch (_) {}
    showToast(t('face_action_error', { error: err.message }), 'error');
  } finally {
    busyFaces.delete(key);
    if (button.isConnected) button.disabled = false;
  }
}

function clearInspectorFaces() {
  inspectorRequestId += 1;
  currentInspectorMediaId = null;
  inspectorFaceById.clear();
  if (byId('inspectorFacesSection')) byId('inspectorFacesSection').style.display = 'none';
  if (byId('inspectorFaceList')) {
    byId('inspectorFaceList').replaceChildren();
    byId('inspectorFaceList').dataset.mediaId = '';
    byId('inspectorFaceList').dataset.loaded = 'false';
  }
  renderFaceOverlay(byId('inspectorFaceOverlay'), null, false, byId('inspectorImg'), byId('inspectorPreviewWrapper'));
}

function faceGridGroupInfo(face) {
  if (face.dismissed) return { key: 'dismissed', type: 'dismissed', title: t('face_grid_group_dismissed') };
  const suggestion = Array.isArray(face.suggestions) ? face.suggestions[0] : null;
  const confirmedTagId = Number(face.person_tag_id);
  const suggestedTagId = Number(suggestion?.tag_id);
  const tagId = confirmedTagId > 0 ? confirmedTagId : suggestedTagId > 0 ? suggestedTagId : 0;
  if (tagId > 0) {
    const title = peopleTags().find(tag => Number(tag.id) === tagId)?.name
      || face.person_name || suggestion?.name;
    return { key: `person:${tagId}`, type: 'person', title: title || t('face_person_unknown'), tagId };
  }
  if (suggestion && suggestion.name) {
    return { key: `suggestion-name:${String(suggestion.name).toLocaleLowerCase()}`, type: 'suggestion-name', title: suggestion.name };
  }
  return { key: 'unnamed', type: 'unnamed', title: t('face_grid_group_unnamed') };
}

function eligibleSuggestionFaces(faces, tagId) {
  return (faces || []).filter(face => !face.dismissed
    && face.person_tag_id == null
    && Number(face.suggestions?.[0]?.tag_id) === Number(tagId));
}

function distinctMediaCount(faces) {
  return new Set((faces || []).map(face => Number(face.media_id)).filter(id => id > 0)).size;
}

function faceGridPageGroups() {
  const groupMap = new Map();
  for (const face of filteredGridFaces()) {
    const info = faceGridGroupInfo(face);
    if (!groupMap.has(info.key)) groupMap.set(info.key, { ...info, faces: [] });
    groupMap.get(info.key).faces.push(face);
  }
  const rank = { person: 0, 'suggestion-name': 1, unnamed: 2, dismissed: 3 };
  const groups = Array.from(groupMap.values());
  for (const group of groups) {
    group.faces.sort((a, b) => (Number(a.media_id) - Number(b.media_id)) || (Number(a.id) - Number(b.id)));
  }
  groups.sort((a, b) => (rank[a.type] - rank[b.type]) || a.title.localeCompare(b.title));

  const pages = [];
  let page = [];
  let pageSize = 0;
  const finishPage = () => {
    if (!page.length) return;
    pages.push(page);
    page = [];
    pageSize = 0;
  };
  for (const group of groups) {
    if (group.type === 'person') {
      if (pageSize && pageSize + group.faces.length > GRID_PAGE_SIZE) finishPage();
      page.push({ ...group, visibleFaces: group.faces, totalFaces: group.faces.length });
      pageSize += group.faces.length;
      if (pageSize >= GRID_PAGE_SIZE) finishPage();
      continue;
    }
    let offset = 0;
    while (offset < group.faces.length) {
      if (pageSize >= GRID_PAGE_SIZE) finishPage();
      const take = Math.min(GRID_PAGE_SIZE - pageSize, group.faces.length - offset);
      page.push({ ...group, visibleFaces: group.faces.slice(offset, offset + take), totalFaces: group.faces.length });
      pageSize += take;
      offset += take;
      if (pageSize >= GRID_PAGE_SIZE) finishPage();
    }
  }
  finishPage();
  return pages;
}

function faceGridCardMarkup(face) {
  const id = Number(face.id);
  const group = faceGridGroupInfo(face);
  const suggestion = !face.dismissed && Array.isArray(face.suggestions) ? face.suggestions[0] : null;
  const title = face.person_name || (suggestion && suggestion.name) || t('face_name_person');
  const meta = face.dismissed ? t('face_dismissed')
    : face.person_tag_id != null ? t('face_confirmed_identity')
      : suggestion ? t('face_similarity_score', { score: Number(suggestion.score || 0).toFixed(2) })
        : t('face_unnamed');
  const selected = gridSelection.has(String(id));
  const alt = t('face_crop_alt', { name: title });
  return `<label class="face-grid-card${selected ? ' selected' : ''}" data-face-id="${id}" data-group-key="${escapeHtml(group.key)}">
    <input class="face-grid-select" type="checkbox" value="${id}" aria-label="${escapeHtml(t('face_grid_select_face', { name: title }))}" ${selected ? 'checked' : ''}>
    <img class="face-grid-crop" src="${escapeHtml(safeCropUrl(face))}" alt="${escapeHtml(alt)}" loading="lazy">
    <span class="face-grid-name">${escapeHtml(title)}</span>
    <span class="face-grid-meta">${escapeHtml(t('face_open_photo', { id: Number(face.media_id) || '' }))}</span>
    <span class="face-grid-meta">${escapeHtml(meta)}</span>
  </label>`;
}

function filteredGridFaces() {
  const showNamed = byId('faceGridShowNamed')?.checked ?? true;
  const showUnnamed = byId('faceGridShowUnnamed')?.checked ?? true;
  const confirmedPersonId = byId('faceGridConfirmedPersonFilter')?.value || '';
  return gridFaces.filter(face => {
    if (confirmedPersonId && String(face.person_tag_id ?? '') !== confirmedPersonId) return false;
    return face.person_tag_id != null ? showNamed : showUnnamed;
  });
}

function currentGridPageFaces() {
  return (faceGridPageGroups()[gridPage] || []).flatMap(group => group.visibleFaces);
}

function renderFaceGrid() {
  const list = byId('faceGridList');
  const empty = byId('faceGridEmpty');
  const pagination = byId('faceGridPagination');
  if (!list) return;
  const filtered = filteredGridFaces();
  const pages = faceGridPageGroups();
  const pageCount = Math.max(1, pages.length);
  gridPage = Math.min(gridPage, pageCount - 1);
  const pageGroups = pages[gridPage] || [];
  list.innerHTML = pageGroups.map(group => {
    const acceptFaces = group.type === 'person' ? eligibleSuggestionFaces(group.faces, group.tagId) : [];
    const photoCount = distinctMediaCount(acceptFaces);
    const disabled = gridIsLoading || gridActionBusy ? 'disabled' : '';
    const groupAction = group.type === 'person' && group.tagId && acceptFaces.length
      ? `<button type="button" class="btn btn-primary btn-sm" data-face-group-accept="${escapeHtml(group.key)}" ${disabled}>${escapeHtml(t('face_grid_accept_group', { count: photoCount }))}</button>`
      : group.type === 'dismissed'
        ? `<button type="button" class="btn btn-secondary btn-sm" data-face-group-restore="${escapeHtml(group.key)}" ${disabled}>${escapeHtml(t('face_grid_restore_group', { count: group.totalFaces }))}</button>`
        : '';
    return `<section class="face-grid-group" data-group-key="${escapeHtml(group.key)}">
      <div class="face-grid-group-header"><h4>${escapeHtml(group.title)}</h4><span class="face-grid-group-count">${escapeHtml(t('face_grid_group_count', { count: group.totalFaces }))}</span>${groupAction}</div>
      <div class="face-grid-group-cards">${group.visibleFaces.map(faceGridCardMarkup).join('')}</div>
    </section>`;
  }).join('');
  if (empty) {
    empty.textContent = filtered.length ? '' : t('face_grid_empty');
    empty.hidden = filtered.length > 0 || gridIsLoading;
  }
  if (pagination) pagination.hidden = pageCount <= 1 || gridIsLoading;
  const pageStatus = byId('faceGridPageStatus');
  if (pageStatus) pageStatus.textContent = t('face_review_page', { page: gridPage + 1, pages: pageCount, count: filtered.length });
  const prev = byId('faceGridPrevBtn');
  const next = byId('faceGridNextBtn');
  if (prev) prev.disabled = gridPage <= 0 || gridIsLoading;
  if (next) next.disabled = gridPage + 1 >= pageCount || gridIsLoading;
  const count = byId('faceGridCount');
  if (count) count.textContent = t('face_grid_loaded_count', { loaded: filtered.length, total: gridTotal });
  updateGridSelectionUi();
}

function selectedGridFaces() {
  return filteredGridFaces().filter(face => gridSelection.has(String(face.id)));
}

function updateGridSelectionUi() {
  const selected = selectedGridFaces();
  const selectedCount = byId('faceGridSelectedCount');
  if (selectedCount) selectedCount.textContent = t('face_grid_selected_count', { count: selected.length });
  const actionScope = byId('faceGridActionScope');
  if (actionScope) actionScope.textContent = selected.length ? t('face_grid_action_scope', { count: selected.length }) : '';
  const filtered = filteredGridFaces();
  const allSelected = filtered.length > 0 && selected.length === filtered.length;
  const selectAll = byId('faceGridSelectAllBtn');
  if (selectAll) {
    selectAll.textContent = t(allSelected ? 'face_grid_deselect_all_loaded' : 'face_grid_select_all_loaded', { count: filtered.length });
    selectAll.disabled = !filtered.length || gridIsLoading || gridActionBusy;
  }
  const selectedIds = new Set(selected.map(face => String(face.id)));
  byId('faceGridList')?.querySelectorAll('.face-grid-card').forEach(card => {
    const active = selectedIds.has(card.dataset.faceId);
    card.classList.toggle('selected', active);
    const checkbox = card.querySelector('.face-grid-select');
    if (checkbox) {
      checkbox.checked = active;
      checkbox.disabled = gridIsLoading || gridActionBusy;
    }
  });
  byId('faceGridList')?.querySelectorAll('[data-face-group-accept], [data-face-group-restore]').forEach(button => {
    button.disabled = gridIsLoading || gridActionBusy;
  });
  const hasSelection = selected.length > 0;
  const suggestionTargets = selected.filter(face => !face.dismissed && face.person_tag_id == null && Number(face.suggestions?.[0]?.tag_id) > 0);
  const suggestionIds = suggestionTargets.map(face => Number(face.suggestions[0].tag_id));
  const commonSuggestion = suggestionTargets.length > 0 && suggestionIds.every(id => id === suggestionIds[0]);
  const hasIdentity = selected.some(face => face.person_tag_id != null);
  const hasRejectedSuggestion = suggestionTargets.length > 0;
  const hasDismissed = selected.some(face => face.dismissed);
  for (const id of ['faceGridApplyNameBtn', 'faceGridDismissBtn']) {
    const button = byId(id);
    if (button) button.disabled = !hasSelection || gridIsLoading || gridActionBusy;
  }
  const setEnabled = (id, enabled) => {
    const button = byId(id);
    if (button) button.disabled = !enabled || gridIsLoading || gridActionBusy;
  };
  setEnabled('faceGridAcceptBtn', commonSuggestion);
  setEnabled('faceGridClearBtn', hasIdentity);
  setEnabled('faceGridRejectBtn', hasRejectedSuggestion);
  setEnabled('faceGridRestoreBtn', hasDismissed);
  const panel = byId('faceGridSidePanel');
  if (panel) panel.setAttribute('aria-disabled', hasSelection ? 'false' : 'true');
  for (const button of byId('faceGridSidePanel')?.querySelectorAll('button') || []) {
    if (gridIsLoading || gridActionBusy) button.disabled = true;
  }
  const input = byId('faceGridNameInput');
  if (input) input.disabled = gridIsLoading || gridActionBusy;
  const personSelect = byId('faceGridPersonSelect');
  if (personSelect) personSelect.disabled = gridIsLoading || gridActionBusy;
  const confirmedPersonFilter = byId('faceGridConfirmedPersonFilter');
  if (confirmedPersonFilter) confirmedPersonFilter.disabled = gridIsLoading || gridActionBusy || confirmedPersonFilter.options.length <= 1;
  for (const id of ['faceGridShowNamed', 'faceGridShowUnnamed']) {
    byId(id)?.toggleAttribute('disabled', gridIsLoading || gridActionBusy);
  }
}

function updateFaceGridLoading(loading) {
  gridIsLoading = loading;
  const stateNode = byId('faceGridState');
  if (loading && stateNode && !stateNode.textContent) stateNode.textContent = t('face_grid_loading');
  byId('faceGridIncludeDismissed')?.toggleAttribute('disabled', loading || gridActionBusy);
  updateGridSelectionUi();
}

async function loadFaceGridPages({ jobId = gridJobId, includeDismissed = byId('faceGridIncludeDismissed')?.checked || false, preserveSelection = false, preservePage = false } = {}) {
  const token = ++gridLoadToken;
  const previousSelection = preserveSelection ? new Set(gridSelection) : new Set();
  const previousPage = preservePage ? gridPage : 0;
  gridIsLoading = true;
  if (!preserveSelection) {
    gridFaces = [];
    gridTotal = 0;
    gridPage = 0;
  }
  renderFaceGrid();
  const stateNode = byId('faceGridState');
  if (stateNode) stateNode.textContent = t('face_grid_loading');
  const retry = byId('faceGridRetryBtn');
  if (retry) retry.hidden = true;
  const error = byId('faceGridActionError');
  if (error) { error.textContent = ''; error.hidden = true; }
  const empty = byId('faceGridEmpty');
  if (empty) empty.hidden = true;
  byId('faceGridIncludeDismissed')?.toggleAttribute('disabled', true);
  try {
    const all = [];
    let offset = 0;
    let total = null;
    while (total === null || all.length < total) {
      const params = { offset, limit: GRID_API_PAGE_SIZE, include_dismissed: Boolean(includeDismissed) };
      if (jobId !== null && jobId !== undefined && jobId !== '') params.job_id = jobId;
      const result = await api.get('/api/faces/grid', params);
      if (token !== gridLoadToken || byId('faceGridModal')?.style.display !== 'flex') return;
      const items = Array.isArray(result.items) ? result.items : [];
      if (total === null) total = Math.max(0, Number(result.total) || 0);
      all.push(...items);
      offset += items.length;
      if (stateNode) stateNode.textContent = t('face_grid_loading_progress', { loaded: all.length, total });
      if (items.length === 0 && all.length < total) throw new Error(t('face_grid_incomplete_load', { loaded: all.length, total }));
      if (all.length >= total) break;
    }
    if (token !== gridLoadToken) return;
    gridFaces = all;
    gridTotal = total || 0;
    populateFaceGridConfirmedPeople();
    faceById.clear();
    for (const face of gridFaces) faceById.set(String(face.id), face);
    gridSelection = new Set(Array.from(previousSelection).filter(id => gridFaces.some(face => String(face.id) === id)));
    gridPage = Math.min(previousPage, Math.max(0, faceGridPageGroups().length - 1));
    if (stateNode) stateNode.textContent = '';
    if (retry) retry.hidden = true;
    renderFaceGrid();
  } catch (err) {
    if (token !== gridLoadToken) return;
    if (stateNode) stateNode.textContent = t('face_grid_load_error', { error: err.message });
    if (retry) retry.hidden = false;
    if (empty) empty.hidden = true;
  } finally {
    if (token === gridLoadToken) {
      updateFaceGridLoading(false);
      renderFaceGrid();
    }
  }
}

function openFaceGrid({ jobId = null, includeDismissed = false, title = null } = {}) {
  const modal = byId('faceGridModal');
  if (!modal) return;
  if (modal.style.display !== 'flex') gridModalReturnFocus = document.activeElement;
  gridViewToken += 1;
  gridJobId = jobId;
  gridSelection.clear();
  gridRangeAnchor = null;
  gridPage = 0;
  const includeDismissedToggle = byId('faceGridIncludeDismissed');
  if (includeDismissedToggle) includeDismissedToggle.checked = includeDismissed;
  const confirmedPersonFilter = byId('faceGridConfirmedPersonFilter');
  if (confirmedPersonFilter) confirmedPersonFilter.value = '';
  const subtitle = byId('faceGridSubtitle');
  if (subtitle) subtitle.textContent = title || (jobId ? t('face_grid_job_scope', { job: jobId }) : t('face_grid_catalog_scope'));
  modal.style.display = 'flex';
  renderGridJobIssue(jobId);
  byId('faceGridCloseBtn')?.focus();
  updateAnalyzeAvailability();
  populateFaceGridPeople();
  loadFaceGridPages({ jobId, includeDismissed });
}

function closeFaceGrid(restoreFocus = true) {
  const modal = byId('faceGridModal');
  if (modal) modal.style.display = 'none';
  gridLoadToken += 1;
  gridViewToken += 1;
  gridIsLoading = false;
  if (restoreFocus && gridModalReturnFocus && typeof gridModalReturnFocus.focus === 'function' && document.contains(gridModalReturnFocus)) {
    gridModalReturnFocus.focus();
  }
  gridModalReturnFocus = null;
}

function populateFaceGridPeople() {
  const select = byId('faceGridPersonSelect');
  if (!select) return;
  const selected = select.value;
  select.innerHTML = `<option value="">${escapeHtml(t('face_choose_person'))}</option>` + peopleTags()
    .map(tag => `<option value="${Number(tag.id)}">${escapeHtml(tag.name)}</option>`).join('');
  if (Array.from(select.options).some(option => option.value === selected)) select.value = selected;
}

function populateFaceGridConfirmedPeople() {
  const select = byId('faceGridConfirmedPersonFilter');
  if (!select) return;
  const selected = select.value;
  const namesById = new Map(peopleTags().map(tag => [String(Number(tag.id)), tag.name]));
  const confirmedPeople = new Map();
  for (const face of gridFaces) {
    if (face.person_tag_id == null) continue;
    const id = Number(face.person_tag_id);
    if (!Number.isInteger(id) || id <= 0) continue;
    const key = String(id);
    const name = namesById.get(key) || face.person_name;
    if (name) confirmedPeople.set(key, name);
  }
  const options = Array.from(confirmedPeople, ([id, name]) => ({ id, name }))
    .sort((a, b) => a.name.localeCompare(b.name));
  select.innerHTML = `<option value="">${escapeHtml(t('face_grid_all_confirmed_people'))}</option>`
    + options.map(person => `<option value="${escapeHtml(person.id)}">${escapeHtml(person.name)}</option>`).join('');
  if (options.some(person => person.id === selected)) select.value = selected;
}

function renderGridJobIssue(jobId) {
  const issue = byId('faceGridJobIssue');
  const message = byId('faceGridJobIssueText');
  const retry = byId('faceGridJobRetryBtn');
  const matchesJob = currentJob && jobId !== null && String(currentJob.id) === String(jobId);
  const visible = Boolean(matchesJob && faceJobNeedsRetry(currentJob));
  if (issue) issue.hidden = !visible;
  if (message) {
    message.textContent = visible
      ? t('face_job_finished_with_failures', {
        failed: Number(currentJob.failed) || 0,
        remaining: Number(currentJob.remaining) || 0
      })
      : '';
  }
  if (retry) retry.hidden = !visible;
}

function openFailedJobFromGrid() {
  if (!currentJob || !faceJobNeedsRetry(currentJob)) return;
  const returnFocus = gridModalReturnFocus;
  closeFaceGrid(false);
  openJobModal();
  if (returnFocus) scanModalReturnFocus = returnFocus;
}

async function openFaceGridFromScanDialog() {
  const returnFocus = scanModalReturnFocus;
  closeJobModal(false);
  openFaceGrid({ jobId: null, includeDismissed: false });
  if (returnFocus && byId('faceGridModal')) gridModalReturnFocus = returnFocus;
}

async function openPeopleFaceReview() {
  openFaceGrid({ jobId: null, includeDismissed: false });
}

async function updatePeopleReviewCount() {
  const count = byId('faceReviewCount');
  if (!count) return;
  try {
    const result = await api.get('/api/faces/review', { offset: 0, limit: 1 });
    count.textContent = String(Math.max(0, Number(result.total) || 0));
  } catch (_) {
    count.textContent = '0';
  }
}

function handleGridSelectionClick(event) {
  if (gridIsLoading || gridActionBusy) return;
  const card = event.target.closest('.face-grid-card');
  if (!card) return;
  gridLastClicked = { id: card.dataset.faceId, shiftKey: event.shiftKey };
}

function handleGridSelectionChange(event) {
  if (gridIsLoading || gridActionBusy) return;
  const checkbox = event.target.closest('.face-grid-select');
  if (!checkbox) return;
  const id = String(checkbox.value);
  const pageIds = currentGridPageFaces().map(face => String(face.id));
  const targetIndex = pageIds.indexOf(id);
  const anchorIndex = gridRangeAnchor === null ? -1 : pageIds.indexOf(String(gridRangeAnchor));
  if (gridLastClicked?.id === id && gridLastClicked.shiftKey && anchorIndex >= 0 && targetIndex >= 0) {
    const low = Math.min(anchorIndex, targetIndex);
    const high = Math.max(anchorIndex, targetIndex);
    for (const rangeId of pageIds.slice(low, high + 1)) {
      if (checkbox.checked) gridSelection.add(rangeId);
      else gridSelection.delete(rangeId);
    }
  } else if (checkbox.checked) {
    gridSelection.add(id);
  } else {
    gridSelection.delete(id);
  }
  if (!(gridLastClicked?.shiftKey)) gridRangeAnchor = id;
  gridLastClicked = null;
  updateGridSelectionUi();
}

async function refreshAfterFaceChanges(updatedFaces) {
  const affectedIds = new Set((updatedFaces || []).map(face => Number(face.media_id)).filter(id => id > 0));
  try {
    await loadMetadata();
    // Re-run the active server-side media query so changed People tags, counts,
    // sorting and pagination are reflected in the current view. loadMedia
    // reconciles selection against the refreshed result without restoring a
    // stale selection snapshot or overwriting changes made while awaiting.
    await loadMedia();
  } catch (_) {
    // Metadata/media refresh is best-effort; keep face review available if it fails.
  } finally {
    await updatePeopleReviewCount();
    populateFaceGridPeople();
    if (currentInspectorMediaId && affectedIds.has(currentInspectorMediaId)) {
      await refreshInspectorFaces(currentInspectorMediaId, { force: true });
    }
  }
}

async function refreshOpenFaceGrid() {
  const modal = byId('faceGridModal');
  if (modal?.style.display !== 'flex') return;
  const viewToken = gridViewToken;
  const jobId = gridJobId;
  const includeDismissed = Boolean(byId('faceGridIncludeDismissed')?.checked);
  await loadFaceGridPages({ jobId, includeDismissed, preserveSelection: true, preservePage: true });
  if (viewToken !== gridViewToken || modal.style.display !== 'flex') return;
}

async function applyFaceBatch(action, faces, fields = {}) {
  const targetFaces = Array.from(new Map((faces || []).map(face => [String(face.id), face])).values());
  if (!targetFaces.length || gridActionBusy) return;
  const actionViewToken = gridViewToken;
  const actionJobId = gridJobId;
  const actionIncludeDismissed = Boolean(byId('faceGridIncludeDismissed')?.checked);
  const actionPage = gridPage;
  gridActionBusy = true;
  updateGridSelectionUi();
  const stateNode = byId('faceGridState');
  if (stateNode) stateNode.textContent = t('face_grid_applying', { count: targetFaces.length });
  const successFaces = [];
  const failures = [];
  try {
    for (let offset = 0; offset < targetFaces.length; offset += FACE_BATCH_SIZE) {
      const chunk = targetFaces.slice(offset, offset + FACE_BATCH_SIZE);
      const body = { action, faces: chunk.map(face => ({ id: Number(face.id), revision: Number(face.revision) })), ...fields };
      try {
        const result = await api.post('/api/faces/batch', body);
        successFaces.push(...(Array.isArray(result.updated) ? result.updated : []));
        failures.push(...(Array.isArray(result.failed) ? result.failed : []));
      } catch (err) {
        failures.push(...chunk.map(face => ({ id: Number(face.id), error: err.message })));
      }
    }
    if (successFaces.length) await refreshAfterFaceChanges(successFaces);
    populateFaceGridPeople();
    if (actionViewToken === gridViewToken && byId('faceGridModal')?.style.display === 'flex') {
      gridSelection = new Set(failures.map(item => String(item.id)));
      gridPage = actionPage;
      await loadFaceGridPages({ jobId: actionJobId, includeDismissed: actionIncludeDismissed, preserveSelection: true, preservePage: true });
      if (actionViewToken === gridViewToken && byId('faceGridModal')?.style.display === 'flex') {
        showGridActionResult(successFaces.length, failures);
      }
    }
    return { successCount: successFaces.length, failures };
  } finally {
    if (stateNode && stateNode.textContent === t('face_grid_applying', { count: targetFaces.length })) stateNode.textContent = '';
    gridActionBusy = false;
    byId('faceGridIncludeDismissed')?.toggleAttribute('disabled', gridIsLoading);
    updateGridSelectionUi();
    renderFaceGrid();
  }
}

function showGridActionResult(successCount, failures) {
  const error = byId('faceGridActionError');
  if (!error) return;
  if (failures.length) {
    const firstError = failures.find(item => item.error)?.error || '';
    error.textContent = t('face_grid_partial_error', { succeeded: successCount, failed: failures.length, error: firstError });
    error.hidden = false;
  } else {
    error.textContent = '';
    error.hidden = true;
    const state = byId('faceGridState');
    if (state) state.textContent = t('face_grid_action_success', { count: successCount });
  }
}

async function rejectSelectedSuggestions(faces) {
  const targetFaces = faces.filter(face => !face.dismissed && face.person_tag_id == null && Number(face.suggestions?.[0]?.tag_id) > 0);
  if (!targetFaces.length || gridActionBusy) return;
  const actionViewToken = gridViewToken;
  const actionJobId = gridJobId;
  const actionIncludeDismissed = Boolean(byId('faceGridIncludeDismissed')?.checked);
  const actionPage = gridPage;
  gridActionBusy = true;
  updateGridSelectionUi();
  const stateNode = byId('faceGridState');
  if (stateNode) stateNode.textContent = t('face_grid_applying', { count: targetFaces.length });
  const updated = [];
  const failures = [];
  try {
    for (let offset = 0; offset < targetFaces.length; offset += 24) {
      const batch = targetFaces.slice(offset, offset + 24);
      const outcomes = await Promise.all(batch.map(async face => {
        try {
          const response = await api.post(`/api/faces/${encodeURIComponent(Number(face.id))}/reject`, {
            revision: Number(face.revision), tag_id: Number(face.suggestions[0].tag_id)
          });
          return { updated: response && response.face ? response.face : response };
        } catch (err) {
          return { failed: { id: Number(face.id), error: err.message } };
        }
      }));
      updated.push(...outcomes.map(item => item.updated).filter(Boolean));
      failures.push(...outcomes.map(item => item.failed).filter(Boolean));
    }
    if (updated.length) await refreshAfterFaceChanges(updated);
    populateFaceGridPeople();
    if (actionViewToken === gridViewToken && byId('faceGridModal')?.style.display === 'flex') {
      gridSelection = new Set(failures.map(item => String(item.id)));
      gridPage = actionPage;
      await loadFaceGridPages({ jobId: actionJobId, includeDismissed: actionIncludeDismissed, preserveSelection: true, preservePage: true });
      if (actionViewToken === gridViewToken && byId('faceGridModal')?.style.display === 'flex') {
        showGridActionResult(updated.length, failures);
      }
    }
  } finally {
    if (stateNode && stateNode.textContent === t('face_grid_applying', { count: targetFaces.length })) stateNode.textContent = '';
    gridActionBusy = false;
    byId('faceGridIncludeDismissed')?.toggleAttribute('disabled', gridIsLoading);
    updateGridSelectionUi();
    renderFaceGrid();
  }
}

async function applySelectedIdentity() {
  const selected = selectedGridFaces();
  if (!selected.length) return;
  const tagId = byId('faceGridPersonSelect')?.value || '';
  const name = byId('faceGridNameInput')?.value.trim() || '';
  if (!tagId && !name) {
    showToast(t('face_name_required'), 'error');
    byId('faceGridNameInput')?.focus();
    return;
  }
  if (!tagId && utf8ByteLength(name) > 100) {
    showToast(t('face_name_too_long'), 'error');
    byId('faceGridNameInput')?.focus();
    return;
  }
  const result = await applyFaceBatch('identity', selected, tagId ? { tag_id: Number(tagId) } : { name });
  if (result?.failures.length === 0 && byId('faceGridNameInput')) byId('faceGridNameInput').value = '';
}

function applySelectedSuggestion() {
  const selected = selectedGridFaces();
  const eligible = selected.filter(face => !face.dismissed && face.person_tag_id == null && Number(face.suggestions?.[0]?.tag_id) > 0);
  const tagId = Number(eligible[0]?.suggestions?.[0]?.tag_id);
  if (!eligible.length || !tagId || eligible.some(face => Number(face.suggestions?.[0]?.tag_id) !== tagId)) return;
  applyFaceBatch('accept', eligible, { tag_id: tagId });
}

function toggleAllLoadedFaces() {
  const filtered = filteredGridFaces();
  if (!filtered.length || gridActionBusy || gridIsLoading) return;
  if (selectedGridFaces().length === filtered.length) gridSelection.clear();
  else gridSelection = new Set(filtered.map(face => String(face.id)));
  updateGridSelectionUi();
}

function acceptSuggestionGroup(groupKey) {
  const group = faceGridGroupInfo(filteredGridFaces().find(face => faceGridGroupInfo(face).key === groupKey) || {});
  const tagId = Number(group.tagId);
  if (!tagId) return;
  const faces = eligibleSuggestionFaces(filteredGridFaces().filter(face => faceGridGroupInfo(face).key === groupKey), tagId);
  if (!faces.length) return;
  applyFaceBatch('accept', faces, { tag_id: tagId });
}

function restoreDismissedGroup(groupKey) {
  const faces = filteredGridFaces().filter(face => faceGridGroupInfo(face).key === groupKey && face.dismissed);
  applyFaceBatch('dismiss', faces, { dismissed: false });
}

function clearSelectedIdentity() {
  applyFaceBatch('identity', selectedGridFaces().filter(face => face.person_tag_id != null), { tag_id: null });
}

function dismissSelected(dismissed) {
  const selected = selectedGridFaces().filter(face => Boolean(face.dismissed) !== dismissed);
  applyFaceBatch('dismiss', selected, { dismissed });
}

function updateGridPage(delta) {
  const pages = Math.max(1, faceGridPageGroups().length);
  gridPage = Math.max(0, Math.min(pages - 1, gridPage + delta));
  gridRangeAnchor = null;
  renderFaceGrid();
}

async function handleCompletedJob(job) {
  const jobId = String(job.id);
  if (lastAutoOpenedJobId === jobId) return;
  lastAutoOpenedJobId = jobId;
  currentJob = job;
  if (faceJobNeedsRetry(job)) {
    showToast(t('face_job_finished_with_failures', {
      failed: Number(job.failed) || 0,
      remaining: Number(job.remaining) || 0
    }), 'error');
  } else {
    showToast(t('face_job_finished'), 'success');
    clearSavedJobRequest(job.id);
  }
  const returnFocus = scanModalReturnFocus;
  closeJobModal(false);
  openFaceGrid({ jobId: job.id, includeDismissed: false });
  renderGridJobIssue(job.id);
  if (returnFocus && byId('faceGridModal')) gridModalReturnFocus = returnFocus;
}

function trapModalFocus(event, modal, close) {
  if (event.key === 'Escape') {
    event.preventDefault();
    event.stopPropagation();
    close();
    return;
  }
  if (event.key !== 'Tab') return;
  const focusable = Array.from(modal.querySelectorAll('button:not([hidden]):not(:disabled), input:not([hidden]):not(:disabled), select:not([hidden]):not(:disabled), [tabindex="0"]'))
    .filter(element => element.getClientRects().length > 0);
  if (!focusable.length) return;
  const first = focusable[0];
  const last = focusable[focusable.length - 1];
  if (event.shiftKey && document.activeElement === first) {
    event.preventDefault();
    last.focus();
  } else if (!event.shiftKey && document.activeElement === last) {
    event.preventDefault();
    first.focus();
  }
}

async function refreshLoupeFaces(mediaId) {
  const toggle = byId('loupeFaceOverlayToggle');
  const svg = byId('loupeFaceOverlay');
  const image = byId('loupeImg');
  const wrapper = byId('loupeImageViewport');
  const id = Number(mediaId);
  if (!toggle?.checked || !Number.isInteger(id) || id <= 0) {
    renderFaceOverlay(svg, null, false, image, wrapper, true);
    return;
  }
  try {
    const data = await api.get(`/api/faces/media/${id}`);
    if (!toggle.checked || Number(state.mediaItems[state.loupeIndex]?.id) !== id) return;
    renderFaceOverlay(svg, data, true, image, wrapper, true);
  } catch (_) {
    renderFaceOverlay(svg, null, false, image, wrapper, true);
  }
}

function setupLoupeOverlay() {
  const toggle = byId('loupeFaceOverlayToggle');
  const img = byId('loupeImg');
  const overlay = byId('loupeFaceOverlay');
  const inspectorImg = byId('inspectorImg');
  const inspectorOverlay = byId('inspectorFaceOverlay');
  ensureFaceOverlayTracking(inspectorOverlay, inspectorImg, byId('inspectorPreviewWrapper'));
  ensureFaceOverlayTracking(overlay, img, byId('loupeImageViewport'), true);
  toggle?.addEventListener('change', () => {
    const id = state.loupeIndex >= 0 ? state.mediaItems[state.loupeIndex]?.id : null;
    refreshLoupeFaces(id);
  });
  window.addEventListener('imagine:loupeChanged', event => {
    refreshLoupeFaces(event.detail && event.detail.mediaId);
  });
}

function bindFaceActions() {
  for (const id of ['faceToolbarBtn', 'faceCategoryToolbarBtn']) byId(id)?.addEventListener('click', openFaceAnalysisDialog);
  byId('faceReviewBtn')?.addEventListener('click', openPeopleFaceReview);
  byId('faceViewFacesBtn')?.addEventListener('click', openFaceGridFromScanDialog);
  byId('faceScanScope')?.addEventListener('change', renderJobState);
  byId('faceForceReanalysis')?.addEventListener('change', renderJobState);
  byId('faceJobCloseBtn')?.addEventListener('click', closeJobModal);
  byId('faceJobCancelBtn')?.addEventListener('click', closeJobModal);
  byId('faceJobBackdrop')?.addEventListener('click', closeJobModal);
  byId('faceJobStartBtn')?.addEventListener('click', () => startFaceJob(false));
  byId('faceJobStopBtn')?.addEventListener('click', cancelFaceJob);
  byId('faceJobRetryBtn')?.addEventListener('click', retryFaceJob);
  byId('faceJobDoneBtn')?.addEventListener('click', closeJobModal);
  byId('faceForceCancelBtn')?.addEventListener('click', closeForceWarningModal);
  byId('faceForceWarningBackdrop')?.addEventListener('click', closeForceWarningModal);
  byId('faceForceConfirmBtn')?.addEventListener('click', confirmForceWarning);

  byId('faceGridCloseBtn')?.addEventListener('click', () => closeFaceGrid());
  byId('faceGridDoneBtn')?.addEventListener('click', () => closeFaceGrid());
  byId('faceGridBackdrop')?.addEventListener('click', () => closeFaceGrid());
  byId('faceGridRetryBtn')?.addEventListener('click', () => loadFaceGridPages({ jobId: gridJobId, includeDismissed: Boolean(byId('faceGridIncludeDismissed')?.checked), preserveSelection: true, preservePage: true }));
  byId('faceGridPrevBtn')?.addEventListener('click', () => updateGridPage(-1));
  byId('faceGridNextBtn')?.addEventListener('click', () => updateGridPage(1));
  byId('faceGridSelectAllBtn')?.addEventListener('click', toggleAllLoadedFaces);
  byId('faceGridJobRetryBtn')?.addEventListener('click', openFailedJobFromGrid);
  byId('faceGridIncludeDismissed')?.addEventListener('change', () => loadFaceGridPages({ jobId: gridJobId, includeDismissed: Boolean(byId('faceGridIncludeDismissed')?.checked), preserveSelection: true, preservePage: true }));
  byId('faceGridConfirmedPersonFilter')?.addEventListener('change', () => {
    const visibleIds = new Set(filteredGridFaces().map(face => String(face.id)));
    gridSelection = new Set(Array.from(gridSelection).filter(id => visibleIds.has(id)));
    gridPage = 0;
    gridRangeAnchor = null;
    renderFaceGrid();
  });
  for (const id of ['faceGridShowNamed', 'faceGridShowUnnamed']) {
    byId(id)?.addEventListener('change', () => {
      const visibleIds = new Set(filteredGridFaces().map(face => String(face.id)));
      gridSelection = new Set(Array.from(gridSelection).filter(id => visibleIds.has(id)));
      gridPage = 0;
      gridRangeAnchor = null;
      renderFaceGrid();
    });
  }
  byId('faceGridList')?.addEventListener('click', handleGridSelectionClick);
  byId('faceGridList')?.addEventListener('change', handleGridSelectionChange);
  byId('faceGridList')?.addEventListener('click', event => {
    const accept = event.target.closest('[data-face-group-accept]');
    if (accept && !accept.disabled) {
      acceptSuggestionGroup(accept.dataset.faceGroupAccept);
      return;
    }
    const restore = event.target.closest('[data-face-group-restore]');
    if (restore && !restore.disabled) restoreDismissedGroup(restore.dataset.faceGroupRestore);
  });
  byId('faceGridApplyNameBtn')?.addEventListener('click', applySelectedIdentity);
  byId('faceGridNameInput')?.addEventListener('keydown', event => {
    if (event.key === 'Enter') {
      event.preventDefault();
      applySelectedIdentity();
    }
  });
  byId('faceGridAcceptBtn')?.addEventListener('click', applySelectedSuggestion);
  byId('faceGridClearBtn')?.addEventListener('click', clearSelectedIdentity);
  byId('faceGridRejectBtn')?.addEventListener('click', () => rejectSelectedSuggestions(selectedGridFaces()));
  byId('faceGridDismissBtn')?.addEventListener('click', () => dismissSelected(true));
  byId('faceGridRestoreBtn')?.addEventListener('click', () => dismissSelected(false));

  byId('inspectorFaceList')?.addEventListener('click', removeInspectorFace);
  byId('inspectorFaceOverlayToggle')?.addEventListener('change', async () => {
    if (currentInspectorMediaId) await refreshInspectorFaces(currentInspectorMediaId, { force: true });
  });
  byId('faceJobModal')?.addEventListener('keydown', event => trapModalFocus(event, byId('faceJobModal'), closeJobModal));
  byId('faceForceWarningModal')?.addEventListener('keydown', event => trapModalFocus(event, byId('faceForceWarningModal'), closeForceWarningModal));
  byId('faceGridModal')?.addEventListener('keydown', event => trapModalFocus(event, byId('faceGridModal'), () => closeFaceGrid()));

  window.addEventListener('imagine:inspectorMediaChanged', event => {
    const detail = event.detail || {};
    const item = detail.item;
    if (item && item.media_type !== 'photo') {
      clearInspectorFaces();
      return;
    }
    if (!detail.mediaId) {
      clearInspectorFaces();
      return;
    }
    refreshInspectorFaces(detail.mediaId);
  });
  window.addEventListener('imagine:languageChanged', () => {
    renderJobState();
    if (currentInspectorMediaId) refreshInspectorFaces(currentInspectorMediaId, { force: true });
    if (byId('faceGridModal')?.style.display === 'flex') {
      populateFaceGridPeople();
      populateFaceGridConfirmedPeople();
      renderFaceGrid();
    }
    updatePeopleReviewCount();
  });
  setupLoupeOverlay();
}

export function initFaceAnalysis() {
  bindFaceActions();
  refreshFaceStatus();
  updatePeopleReviewCount();
}

export function getFaceAnalysisStatus() {
  return faceStatus;
}
