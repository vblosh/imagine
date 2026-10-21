/**
 * IMAGINE Photo Organizer - Smart Tags Workflow & Job Management
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

import { checkSmartTagStatus, setSmartTagStatusCallback } from './smart-tags-status.js';
import { showModal, hideModal } from './modals.js';
import { api } from './api.js';
import { state } from './state.js';
import { dom, showToast } from './dom.js';
import { t } from './i18n.js';
import { loadMetadata, loadMedia } from './views.js';

const TERMINAL_JOB_STATES = new Set(['completed', 'failed', 'cancelled', 'interrupted']);
const POLL_INTERVAL_MS = 500;

let currentSmartTagsJob = null;
let currentSmartTagsRequest = null;
let smartTagsDialogSelectedIds = [];
let smartTagsJobPollTimer = null;
let smartTagsJobPollInFlight = false;
let smartTagsPollErrorCount = 0;
let smartTagsSubmissionError = '';

setSmartTagStatusCallback(() => {
  if (dom.smartTagsModal && !dom.smartTagsModal.hidden) {
    renderSmartTagsJobState();
  }
});
let smartTagsCancelError = '';

export function getCurrentSmartTagsJob() {
  return currentSmartTagsJob;
}

function selectedPhotoIds() {
  const mediaMap = new Map((state.mediaItems || []).map(item => [Number(item.id), item]));
  const ids = state.selectedMediaIds || state.selectedIds || new Set();
  return Array.from(ids)
    .map(Number)
    .filter(id => Number.isInteger(id) && id > 0)
    .filter(id => {
      const item = mediaMap.get(id);
      if (!item) return true;
      return !item.media_type || item.media_type === 'photo';
    });
}

function setJobButton(button, visible, disabled = false) {
  if (!button) return;
  button.hidden = !visible;
  button.style.display = visible ? '' : 'none';
  button.disabled = disabled;
}

export function renderSmartTagsJobState() {
  const hasJob = Boolean(currentSmartTagsJob);
  const initial = !hasJob && Boolean(currentSmartTagsRequest);
  const running = hasJob && !TERMINAL_JOB_STATES.has(currentSmartTagsJob.state);

  const notice = dom.smartTagsJobNotice;
  const stateNode = dom.smartTagsJobState;
  const scopeWrap = dom.smartTagsScanScopeWrap;
  const progressWrap = dom.smartTagsJobProgressWrap;
  const progress = dom.smartTagsJobProgress;
  const counts = dom.smartTagsJobCounts;
  const forceOption = dom.smartTagsForceOption;
  const error = dom.smartTagsJobError;

  if (notice) {
    let noticeMsg = '';
    if (state.smartTagsConnectionError) {
      noticeMsg = t('smart_tags_connection_error') || 'Status: Could not connect to smart tag service';
    } else if (!state.smartTagsBuilt) {
      noticeMsg = t('smart_tags_engine_not_built') || 'Status: Smart tagging engine not built in this binary';
    } else if (!state.smartTagsAvailable) {
      noticeMsg = t('smart_tags_model_unavailable') || 'Status: CLIP model unavailable';
    } else if (!state.smartTagsTaxonomyReady) {
      noticeMsg = state.smartTagsTaxonomyError
        ? (t('smart_tags_taxonomy_error', { error: state.smartTagsTaxonomyError }) || `Status: Taxonomy error: ${state.smartTagsTaxonomyError}`)
        : (t('smart_tags_taxonomy_error', { error: '' }) || 'Status: Taxonomy error');
    } else if (state.smartTagIndexedPhotos === 0) {
      noticeMsg = t('smart_tags_index_empty', { count: 0 }) || 'Status: Build the search index first (0 photos indexed)';
    }
    notice.textContent = noticeMsg;
    notice.hidden = !noticeMsg;
  }

  if (stateNode) {
    if (initial) {
      const count = currentSmartTagsRequest.scope === 'selected' ? currentSmartTagsRequest.media_ids.length : null;
      stateNode.textContent = currentSmartTagsRequest.scope === 'catalog'
        ? (t('smart_tags_start_catalog_prompt') || 'Generate smart tags for photos across the catalog using CLIP embeddings.')
        : (t('smart_tags_start_selected_prompt', { count }) || `Generate smart tags for ${count} selected photos using CLIP embeddings.`);
    } else if (hasJob) {
      const stateKey = `smart_tags_job_${currentSmartTagsJob.state}`;
      const fallbackMsg = currentSmartTagsJob.state === 'running'
        ? 'Generating smart tags…'
        : currentSmartTagsJob.state === 'cancelling'
        ? 'Cancelling after the current photo…'
        : currentSmartTagsJob.state === 'completed'
        ? 'Smart tags are up to date.'
        : currentSmartTagsJob.state === 'cancelled'
        ? 'Smart tagging was cancelled.'
        : currentSmartTagsJob.state === 'interrupted'
        ? 'Smart tagging stopped before it finished.'
        : 'Smart tagging failed.';
      stateNode.textContent = t(stateKey) || fallbackMsg;
    } else {
      stateNode.textContent = '';
    }
  }

  if (progressWrap) progressWrap.hidden = !hasJob;
  if (hasJob) {
    const total = Math.max(0, Number(currentSmartTagsJob.total) || 0);
    const processed = Math.max(0, Number(currentSmartTagsJob.processed) || 0);
    const skipped = Math.max(0, Number(currentSmartTagsJob.skipped) || 0);
    const failed = Math.max(0, Number(currentSmartTagsJob.failed) || 0);
    const remaining = Math.max(0, Number(currentSmartTagsJob.remaining) || 0);
    const done = Math.min(total, processed + skipped + failed);
    if (progress) progress.value = total ? Math.round((done * 100) / total) : 0;
    if (counts) {
      counts.textContent = t('smart_tags_job_counts', { processed, skipped, failed, remaining, total }) ||
        `${processed} labeled · ${skipped} skipped · ${failed} failed · ${remaining} remaining of ${total}`;
    }
  }

  const errorMessage = smartTagsSubmissionError || smartTagsCancelError || (hasJob && currentSmartTagsJob.error ? String(currentSmartTagsJob.error) : '');
  if (error) {
    error.textContent = errorMessage;
    error.hidden = !errorMessage;
  }

  if (scopeWrap) scopeWrap.hidden = !initial;
  if (forceOption) forceOption.hidden = !initial;
  if (dom.smartTagsSelectedScopeOption) {
    dom.smartTagsSelectedScopeOption.disabled = smartTagsDialogSelectedIds.length === 0;
  }

  const cannotStart = !state.smartTagsAvailable ||
    !state.smartTagsBuilt ||
    !state.smartTagsTaxonomyReady ||
    state.smartTagIndexedPhotos === 0 ||
    Boolean(currentSmartTagsRequest?.scope === 'selected' && smartTagsDialogSelectedIds.length === 0);

  setJobButton(dom.smartTagsJobStartBtn, initial, cannotStart);
  setJobButton(dom.smartTagsJobStopBtn, running, currentSmartTagsJob && currentSmartTagsJob.state === 'cancelling');
  setJobButton(dom.cancelSmartTagsJobBtn, true, false);
}

export function onSmartTagsScopeChange() {
  const scope = dom.smartTagsScanScope?.value || 'catalog';
  currentSmartTagsRequest = scope === 'selected'
    ? { scope, media_ids: [...smartTagsDialogSelectedIds] }
    : { scope, media_ids: [] };
  renderSmartTagsJobState();
}

export function openSmartTagsModal() {
  if (currentSmartTagsJob && !TERMINAL_JOB_STATES.has(currentSmartTagsJob.state)) {
    renderSmartTagsJobState();
    showModal(dom.smartTagsModal, dom.closeSmartTagsJobBtn);
    return;
  }

  smartTagsDialogSelectedIds = selectedPhotoIds();
  currentSmartTagsJob = null;
  smartTagsSubmissionError = '';
  smartTagsCancelError = '';

  const scope = dom.smartTagsScanScope;
  if (scope) scope.value = smartTagsDialogSelectedIds.length ? 'selected' : 'catalog';
  currentSmartTagsRequest = smartTagsDialogSelectedIds.length
    ? { scope: 'selected', media_ids: [...smartTagsDialogSelectedIds] }
    : { scope: 'catalog', media_ids: [] };

  if (dom.smartTagsForce) dom.smartTagsForce.checked = false;
  renderSmartTagsJobState();

  const defaultFocus = dom.smartTagsScanScope || dom.cancelSmartTagsJobBtn || dom.closeSmartTagsJobBtn;
  showModal(dom.smartTagsModal, defaultFocus);
}

export function closeSmartTagsModal() {
  if (dom.smartTagsModal) {
    hideModal(dom.smartTagsModal);
  }
}

function scheduleJobPoll(delay = POLL_INTERVAL_MS) {
  if (smartTagsJobPollTimer) window.clearTimeout(smartTagsJobPollTimer);
  if (!currentSmartTagsJob || TERMINAL_JOB_STATES.has(currentSmartTagsJob.state)) return;
  smartTagsJobPollTimer = window.setTimeout(pollSmartTagsJob, delay);
}

async function pollSmartTagsJob() {
  if (!currentSmartTagsJob || TERMINAL_JOB_STATES.has(currentSmartTagsJob.state) || smartTagsJobPollInFlight) return;
  const pollJobId = currentSmartTagsJob.id;
  smartTagsJobPollInFlight = true;

  try {
    const result = await api.get(`/api/smart-tags/jobs/${encodeURIComponent(pollJobId)}`);
    if (!currentSmartTagsJob || currentSmartTagsJob.id !== pollJobId) {
      return;
    }
    smartTagsPollErrorCount = 0;
    if (result && result.id) {
      currentSmartTagsJob = result;
      renderSmartTagsJobState();

      if (TERMINAL_JOB_STATES.has(currentSmartTagsJob.state)) {
        if (currentSmartTagsJob.state === 'completed') {
          const msg = t('toast_smart_tags_complete', { count: currentSmartTagsJob.processed || 0 }) ||
            `Smart tagging complete: ${currentSmartTagsJob.processed || 0} photos labeled.`;
          showToast(msg, 'info');
        }
        await checkSmartTagStatus();
        if (currentSmartTagsJob.state === 'completed' || (currentSmartTagsJob.processed || 0) > 0) {
          loadMetadata();
          loadMedia();
        }
      } else {
        scheduleJobPoll(POLL_INTERVAL_MS);
      }
    } else {
      scheduleJobPoll(POLL_INTERVAL_MS);
    }
  } catch (err) {
    if (!currentSmartTagsJob || currentSmartTagsJob.id !== pollJobId) {
      return;
    }
    const isNotFound = err && (err.status === 404 || (err.message && err.message.toLowerCase().includes('not found')));
    smartTagsPollErrorCount++;
    const tooManyErrors = smartTagsPollErrorCount >= 5;

    currentSmartTagsJob.error = err.message || String(err);
    if (isNotFound || tooManyErrors) {
      currentSmartTagsJob.state = 'failed';
    }
    renderSmartTagsJobState();

    if (!isNotFound && !tooManyErrors) {
      scheduleJobPoll(POLL_INTERVAL_MS * 2);
    }
  } finally {
    smartTagsJobPollInFlight = false;
  }
}

export async function startSmartTagsJob() {
  const scope = dom.smartTagsScanScope?.value || 'catalog';
  currentSmartTagsRequest = scope === 'selected'
    ? { scope, media_ids: [...smartTagsDialogSelectedIds] }
    : { scope, media_ids: [] };

  if (!currentSmartTagsRequest || !state.smartTagsAvailable || !state.smartTagsTaxonomyReady || !state.smartTagsBuilt || state.smartTagIndexedPhotos === 0) return;

  if (smartTagsJobPollTimer) {
    window.clearTimeout(smartTagsJobPollTimer);
    smartTagsJobPollTimer = null;
  }
  smartTagsPollErrorCount = 0;

  const force = Boolean(dom.smartTagsForce?.checked);
  setJobButton(dom.smartTagsJobStartBtn, true, true);
  if (dom.smartTagsJobError) {
    dom.smartTagsJobError.textContent = '';
    dom.smartTagsJobError.hidden = true;
  }

  try {
    const body = { scope, force };
    if (scope === 'selected' || (Array.isArray(currentSmartTagsRequest.media_ids) && currentSmartTagsRequest.media_ids.length > 0)) {
      body.media_ids = currentSmartTagsRequest.media_ids;
    }
    const result = await api.post('/api/smart-tags/jobs', body);
    if (!result || !result.job_id) {
      throw new Error(result?.error || 'Failed to start smart tags job');
    }

    currentSmartTagsJob = {
      id: result.job_id,
      state: 'running',
      scope,
      total: 0,
      processed: 0,
      skipped: 0,
      failed: 0,
      remaining: 0
    };
    smartTagsSubmissionError = '';
    smartTagsCancelError = '';
    renderSmartTagsJobState();
    scheduleJobPoll(0);
    return result.job_id;
  } catch (e) {
    smartTagsSubmissionError = t('toast_smart_tags_start_failed', { error: e.message || e }) || `Failed to start smart tagging: ${e.message || e}`;
    renderSmartTagsJobState();
    showToast(smartTagsSubmissionError, 'error');
    return null;
  }
}

export async function cancelSmartTagsJob(jobId) {
  const targetId = jobId || (currentSmartTagsJob && currentSmartTagsJob.id);
  if (!targetId) return false;

  const isCurrent = currentSmartTagsJob && String(currentSmartTagsJob.id) === String(targetId);
  if (isCurrent) {
    setJobButton(dom.smartTagsJobStopBtn, true, true);
  }

  try {
    await api.post(`/api/smart-tags/jobs/${encodeURIComponent(targetId)}/cancel`, {});
    if (isCurrent) {
      smartTagsCancelError = '';
      currentSmartTagsJob = { ...currentSmartTagsJob, state: 'cancelling' };
      renderSmartTagsJobState();
      scheduleJobPoll(0);
    }
    return true;
  } catch (err) {
    if (isCurrent) {
      smartTagsCancelError = t('toast_smart_tags_cancel_failed', { error: err.message || err }) || `Failed to cancel smart tagging: ${err.message || err}`;
      renderSmartTagsJobState();
    }
    showToast(t('toast_smart_tags_cancel_failed', { error: err.message || err }) || `Failed to cancel smart tagging: ${err.message || err}`, 'error');
    return false;
  }
}

export function openClearSmartTagsModal() {
  if (!dom.clearSmartTagsModal) return;
  if (dom.clearSmartTagsPromptText) {
    dom.clearSmartTagsPromptText.textContent = t('smart_tags_clear_confirm') || 'Are you sure you want to remove all smart tags? Manual and face tags will not be affected.';
  }
  if (dom.clearSmartTagsWarningText) {
    dom.clearSmartTagsWarningText.textContent = t('smart_tags_clear_warning') || 'This action removes all automatically generated smart tags from the catalog database. The original files on disk will not be deleted.';
  }
  showModal(dom.clearSmartTagsModal, dom.cancelClearSmartTagsBtn || dom.confirmClearSmartTagsBtn);
}

export function closeClearSmartTagsModal() {
  if (!dom.clearSmartTagsModal) return;
  hideModal(dom.clearSmartTagsModal);
}

export async function submitClearSmartTags() {
  closeClearSmartTagsModal();

  try {
    await api.post('/api/smart-tags/clear', {});
    showToast(t('smart_tags_clear_success') || 'Smart tags removed successfully.', 'info');
    await checkSmartTagStatus();
    loadMetadata();
    loadMedia();
  } catch (err) {
    console.error('Failed to clear smart tags:', err);
    showToast(t('smart_tags_clear_failed', { error: err.message || err }) || `Failed to remove smart tags: ${err.message || err}`, 'error');
  }
}

export async function clearSmartTags() {
  openClearSmartTagsModal();
}

// Global escape key handler for Smart Tags modals
window.addEventListener('keydown', (e) => {
  if (e.key === 'Escape') {
    if (dom.clearSmartTagsModal && dom.clearSmartTagsModal.style.display === 'flex') {
      e.preventDefault();
      e.stopPropagation();
      closeClearSmartTagsModal();
      return;
    }
    if (dom.smartTagsModal && dom.smartTagsModal.style.display === 'flex') {
      e.preventDefault();
      e.stopPropagation();
      closeSmartTagsModal();
    }
  }
}, true);
