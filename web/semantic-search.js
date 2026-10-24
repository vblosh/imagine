import { state } from './state.js';
import { dom, showToast } from './dom.js';
import { api } from './api.js';
import { t } from './i18n.js';
import { showModal, hideModal } from './modals.js';

export const SEMANTIC_SEARCH_DEBOUNCE_MS = 2000;
import {
    checkSemanticStatus,
    updateSemanticToggleVisibility,
    updateSettingsSemanticStatus
} from './semantic-status.js';

export {
    checkSemanticStatus,
    updateSemanticToggleVisibility,
    updateSettingsSemanticStatus
};

export async function performSemanticSearch(query, limit = 100, options = {}) {
    if (!query || !state.semanticSearchAvailable) return null;
    
    const { filters = {}, ...fetchOptions } = options;
    const activeFilters = { ...filters };
    if (!activeFilters.media_type && !activeFilters.mediaType) {
        activeFilters.media_type = 'photo';
    }
    
    try {
        const result = await api.post('/api/semantic/search', {
            query, limit, filters: activeFilters
        }, fetchOptions);
        return result;
    } catch (e) {
        if (e.name === 'AbortError') throw e;
        console.error('Semantic search failed:', e);
        showToast(t('toast_semantic_search_failed', { error: e.message || e }), 'error');
        return null;
    }
}

export async function findSimilarMedia(mediaId, limit = 50, options = {}) {
    if (!mediaId || !state.semanticSearchAvailable) return null;
    const { ...fetchOptions } = options;
    try {
        const result = await api.post(`/api/semantic/similar/${encodeURIComponent(mediaId)}`, { limit }, fetchOptions);
        return result;
    } catch (e) {
        if (e.name === 'AbortError') throw e;
        console.error('Find similar failed:', e);
        showToast(t('toast_find_similar_failed', { error: e.message || e }), 'error');
        return null;
    }
}

const TERMINAL_JOB_STATES = new Set(['completed', 'failed', 'cancelled']);

let currentSemanticJob = null;
let currentSemanticRequest = null;
let semanticDialogSelectedIds = [];
let semanticJobPollTimer = null;
let semanticJobPollInFlight = false;
let semanticSubmissionError = '';
let semanticCancelError = '';

function selectedPhotoIds() {
    const mediaMap = new Map((state.mediaItems || []).map(item => [Number(item.id), item]));
    return Array.from(state.selectedIds)
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

export function renderSemanticJobState() {
    const hasJob = Boolean(currentSemanticJob);
    const initial = !hasJob && Boolean(currentSemanticRequest);
    const running = hasJob && !TERMINAL_JOB_STATES.has(currentSemanticJob.state);

    const notice = dom.semanticJobNotice;
    const stateNode = dom.semanticJobState;
    const scopeWrap = dom.semanticScanScopeWrap;
    const progressWrap = dom.semanticJobProgressWrap;
    const progress = dom.semanticJobProgress;
    const counts = dom.semanticJobCounts;
    const forceOption = dom.semanticForceOption;
    const error = dom.semanticJobError;

    if (notice) {
        let noticeMsg = '';
        if (state.semanticConnectionError) {
            noticeMsg = t('semantic_status_connection_error') || 'Status: Could not connect to semantic search service';
        } else if (!state.semanticBuilt) {
            noticeMsg = t('semantic_engine_not_built') || 'Status: Semantic search engine not built in this binary';
        } else if (!state.semanticSearchAvailable) {
            noticeMsg = t('semantic_models_missing_notice') || 'CLIP models are not installed. Download models to the models directory to enable semantic search.';
        }
        notice.textContent = noticeMsg;
        notice.hidden = !noticeMsg;
    }

    if (stateNode) {
        if (initial) {
            const count = currentSemanticRequest.scope === 'selected' ? currentSemanticRequest.media_ids.length : null;
            stateNode.textContent = currentSemanticRequest.scope === 'catalog'
                ? t('semantic_start_catalog_prompt')
                : t('semantic_start_selected_prompt', { count });
        } else if (hasJob) {
            const stateKey = `semantic_job_state_${currentSemanticJob.state}`;
            stateNode.textContent = t(stateKey) || `Semantic indexing is ${currentSemanticJob.state}.`;
        } else {
            stateNode.textContent = '';
        }
    }

    if (progressWrap) progressWrap.hidden = !hasJob;
    if (hasJob) {
        const total = Math.max(0, Number(currentSemanticJob.total) || 0);
        const processed = Math.max(0, Number(currentSemanticJob.processed) || 0);
        const skipped = Math.max(0, Number(currentSemanticJob.skipped) || 0);
        const failed = Math.max(0, Number(currentSemanticJob.failed) || 0);
        const remaining = Math.max(0, Number(currentSemanticJob.remaining) || 0);
        const done = Math.min(total, processed + skipped + failed);
        if (progress) progress.value = total ? Math.round(done * 100 / total) : 0;
        if (counts) counts.textContent = t('semantic_job_counts', { processed, skipped, failed, remaining, total });
    }

    const errorMessage = semanticSubmissionError || semanticCancelError || (hasJob && currentSemanticJob.error ? String(currentSemanticJob.error) : '');
    if (error) {
        error.textContent = errorMessage;
        error.hidden = !errorMessage;
    }

    if (scopeWrap) scopeWrap.hidden = !initial;
    if (forceOption) forceOption.hidden = !initial;
    if (dom.semanticSelectedScopeOption) {
        dom.semanticSelectedScopeOption.disabled = semanticDialogSelectedIds.length === 0;
    }

    setJobButton(dom.semanticJobStartBtn, initial, !state.semanticSearchAvailable);
    setJobButton(dom.semanticJobStopBtn, running, currentSemanticJob && currentSemanticJob.state === 'cancelling');
    setJobButton(dom.cancelSemanticJobBtn, true);
}

export function onSemanticScopeChange() {
    const scope = dom.semanticScanScope?.value || 'catalog';
    currentSemanticRequest = scope === 'selected'
        ? { scope, media_ids: [...semanticDialogSelectedIds] }
        : { scope, media_ids: [] };
    renderSemanticJobState();
}

export function openSemanticIndexModal() {
    if (currentSemanticJob && !TERMINAL_JOB_STATES.has(currentSemanticJob.state)) {
        renderSemanticJobState();
        showModal(dom.semanticIndexModal, dom.closeSemanticIndexModalBtn);
        return;
    }
    semanticDialogSelectedIds = selectedPhotoIds();
    currentSemanticJob = null;
    semanticSubmissionError = '';
    semanticCancelError = '';
    const scope = dom.semanticScanScope;
    if (scope) scope.value = semanticDialogSelectedIds.length ? 'selected' : 'catalog';
    currentSemanticRequest = semanticDialogSelectedIds.length
        ? { scope: 'selected', media_ids: [...semanticDialogSelectedIds] }
        : { scope: 'catalog', media_ids: [] };
    if (dom.semanticForceReindex) dom.semanticForceReindex.checked = false;
    renderSemanticJobState();
    const defaultFocus = dom.semanticScanScope || dom.cancelSemanticJobBtn;
    showModal(dom.semanticIndexModal, defaultFocus);
}

export function closeSemanticIndexModal() {
    if (dom.semanticIndexModal) {
        hideModal(dom.semanticIndexModal);
    }
}

export async function startSemanticScan() {
    const scope = dom.semanticScanScope?.value || 'catalog';
    currentSemanticRequest = scope === 'selected'
        ? { scope, media_ids: [...semanticDialogSelectedIds] }
        : { scope, media_ids: [] };
    if (!currentSemanticRequest || !state.semanticSearchAvailable) return;

    const force = Boolean(dom.semanticForceReindex?.checked);
    const request = { ...currentSemanticRequest, force };
    await submitSemanticJob(request);
}

export async function startSemanticIndexing(scope = 'catalog', mediaIds = [], force = false) {
    try {
        const body = { scope, force: Boolean(force) };
        if (scope === 'selected' || (Array.isArray(mediaIds) && mediaIds.length > 0)) {
            body.media_ids = mediaIds;
        }
        const result = await api.post('/api/semantic/jobs', body);
        if (!result || !result.job_id) {
            throw new Error(result?.error || 'Failed to start indexing job');
        }
        currentSemanticRequest = { scope, media_ids: mediaIds, force: Boolean(force) };
        currentSemanticJob = {
            id: result.job_id,
            state: 'running',
            scope,
            total: 0,
            processed: 0,
            skipped: 0,
            failed: 0,
            remaining: 0
        };
        semanticSubmissionError = '';
        semanticCancelError = '';
        semanticJobPollErrorCount = 0;
        renderSemanticJobState();
        scheduleJobPoll(0);
        return result.job_id;
    } catch (e) {
        semanticSubmissionError = t('semantic_job_start_error', { error: e.message || e });
        renderSemanticJobState();
        showToast(t('toast_failed_start_indexing', { error: e.message || e }), 'error');
        return null;
    }
}

export async function submitSemanticJob(request) {
    setJobButton(dom.semanticJobStartBtn, true, true);
    if (dom.semanticJobError) {
        dom.semanticJobError.textContent = '';
        dom.semanticJobError.hidden = true;
    }
    return await startSemanticIndexing(request.scope, request.media_ids || [], request.force === true);
}

const MAX_SEMANTIC_POLL_ERRORS = 5;
const BASE_POLL_DELAY_MS = 1000;
const BASE_ERROR_DELAY_MS = 2000;
const MAX_POLL_DELAY_MS = 30000;

let semanticJobPollErrorCount = 0;

function scheduleJobPoll(delay = BASE_POLL_DELAY_MS) {
    if (semanticJobPollTimer) window.clearTimeout(semanticJobPollTimer);
    if (!currentSemanticJob || TERMINAL_JOB_STATES.has(currentSemanticJob.state)) return;
    semanticJobPollTimer = window.setTimeout(pollSemanticJob, delay);
}

async function pollSemanticJob() {
    if (!currentSemanticJob || TERMINAL_JOB_STATES.has(currentSemanticJob.state) || semanticJobPollInFlight) return;
    semanticJobPollInFlight = true;
    try {
        const result = await api.get(`/api/semantic/jobs/${encodeURIComponent(currentSemanticJob.id)}`);
        if (result && result.id) {
            semanticJobPollErrorCount = 0;
            currentSemanticJob = result;
            renderSemanticJobState();
            if (TERMINAL_JOB_STATES.has(currentSemanticJob.state)) {
                if (currentSemanticJob.state === 'completed') {
                    const msg = t('semantic_search_indexing_complete', { count: currentSemanticJob.processed }) || `Indexed ${currentSemanticJob.processed} photos.`;
                    showToast(msg, 'info');
                }
                await checkSemanticStatus();
            } else {
                scheduleJobPoll(BASE_POLL_DELAY_MS);
            }
        } else {
            semanticJobPollErrorCount++;
            if (semanticJobPollErrorCount >= MAX_SEMANTIC_POLL_ERRORS) {
                const errMsg = t('semantic_job_poll_failed', { error: 'Job not found or invalid response' });
                if (dom.semanticJobError) {
                    dom.semanticJobError.textContent = errMsg;
                    dom.semanticJobError.hidden = false;
                }
                showToast(errMsg, 'error');
                return;
            }
            const delay = Math.min(MAX_POLL_DELAY_MS, BASE_ERROR_DELAY_MS * Math.pow(2, semanticJobPollErrorCount - 1));
            scheduleJobPoll(delay);
        }
    } catch (err) {
        semanticJobPollErrorCount++;
        const errMsg = semanticJobPollErrorCount >= MAX_SEMANTIC_POLL_ERRORS
            ? (t('semantic_job_poll_failed', { error: err.message }) || err.message)
            : err.message;
        if (dom.semanticJobError) {
            dom.semanticJobError.textContent = errMsg;
            dom.semanticJobError.hidden = false;
        }
        if (semanticJobPollErrorCount >= MAX_SEMANTIC_POLL_ERRORS) {
            showToast(errMsg, 'error');
            return;
        }
        const delay = Math.min(MAX_POLL_DELAY_MS, BASE_ERROR_DELAY_MS * Math.pow(2, semanticJobPollErrorCount - 1));
        scheduleJobPoll(delay);
    } finally {
        semanticJobPollInFlight = false;
    }
}

export async function cancelSemanticJob(jobId) {
    if (!jobId) return false;
    const isCurrent = currentSemanticJob && String(currentSemanticJob.id) === String(jobId);
    if (isCurrent) {
        setJobButton(dom.semanticJobStopBtn, true, true);
    }
    try {
        await api.post(`/api/semantic/jobs/${encodeURIComponent(jobId)}/cancel`, {});
        if (isCurrent) {
            semanticCancelError = '';
            currentSemanticJob = { ...currentSemanticJob, state: 'cancelling' };
            renderSemanticJobState();
            semanticJobPollErrorCount = 0;
            scheduleJobPoll(0);
        }
        return true;
    } catch (err) {
        if (isCurrent) {
            semanticCancelError = t('semantic_job_cancel_error', { error: err.message });
            renderSemanticJobState();
        }
        showToast(t('semantic_job_cancel_error', { error: err.message }), 'error');
        return false;
    }
}

export async function cancelSemanticScan() {
    if (!currentSemanticJob || TERMINAL_JOB_STATES.has(currentSemanticJob.state)) return true;
    return await cancelSemanticJob(currentSemanticJob.id);
}

export async function runSemanticIndexing(force = false) {
    if (currentSemanticJob && !TERMINAL_JOB_STATES.has(currentSemanticJob.state)) {
        openSemanticIndexModal();
        return;
    }
    openSemanticIndexModal();
    if (dom.semanticForceReindex) dom.semanticForceReindex.checked = Boolean(force);
    if (state.semanticSearchAvailable) {
        await startSemanticScan();
    }
}

export async function cancelCurrentSemanticJob() {
    const success = await cancelSemanticScan();
    if (success) {
        closeSemanticIndexModal();
    }
}

export async function getSemanticJobProgress(jobId) {
    if (!jobId) return null;
    try {
        const result = await api.get(`/api/semantic/jobs/${encodeURIComponent(jobId)}`);
        if (result && currentSemanticJob && String(currentSemanticJob.id) === String(jobId)) {
            currentSemanticJob = result;
            renderSemanticJobState();
        }
        return result;
    } catch (e) {
        return null;
    }
}

export function getCurrentSemanticJob() {
    return currentSemanticJob;
}

export function getSemanticJobPollErrorCount() {
    return semanticJobPollErrorCount;
}
