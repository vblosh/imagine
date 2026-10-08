import { state } from './state.js';
import { dom, showToast } from './dom.js';
import { api } from './api.js';
import { t } from './i18n.js';
import { getTimelinePeriodBounds } from './timeline-range.js';
import { showModal, hideModal } from './modals.js';

let activeJobId = null;
let activeJobPollTimer = null;

export async function checkSemanticStatus() {
    try {
        const status = await api.get('/api/semantic/status');
        state.semanticSearchAvailable = Boolean(status.ready);
        state.semanticIndexSize = status.indexSize || 0;
        state.semanticModelId = status.modelId || '';
        state.semanticBuilt = Boolean(status.built);
        updateSemanticToggleVisibility();
        updateSettingsSemanticStatus();
        return status;
    } catch (e) {
        state.semanticSearchAvailable = false;
        state.semanticBuilt = false;
        updateSemanticToggleVisibility();
        updateSettingsSemanticStatus();
        return null;
    }
}

export function updateSemanticToggleVisibility() {
    if (dom.semanticToggle) {
        const isSupported = Boolean(state.semanticSearchAvailable || state.semanticBuilt);
        dom.semanticToggle.style.display = isSupported ? 'inline-flex' : 'none';
        if (!state.semanticSearchAvailable) {
            state.semanticSearchEnabled = false;
            dom.semanticToggle.disabled = true;
            dom.semanticToggle.setAttribute('aria-disabled', 'true');
            dom.semanticToggle.classList.remove('active');
            dom.semanticToggle.classList.add('disabled-toggle');
            dom.semanticToggle.setAttribute('aria-pressed', 'false');
            dom.semanticToggle.title = t('semantic_models_missing_title') || 'Semantic AI search (models not installed)';
        } else {
            dom.semanticToggle.disabled = false;
            dom.semanticToggle.removeAttribute('aria-disabled');
            dom.semanticToggle.classList.remove('disabled-toggle');
            dom.semanticToggle.classList.toggle('active', Boolean(state.semanticSearchEnabled));
            dom.semanticToggle.setAttribute('aria-pressed', String(Boolean(state.semanticSearchEnabled)));
            dom.semanticToggle.title = t('semantic_toggle_title') || 'Toggle semantic AI search';
        }
    }
}

export function updateSettingsSemanticStatus() {
    if (dom.settingsSemanticStatus) {
        if (!state.semanticBuilt) {
            dom.settingsSemanticStatus.textContent = 'Status: Semantic search engine not built in this binary';
        } else if (!state.semanticSearchAvailable) {
            dom.settingsSemanticStatus.textContent = `Status: Model unavailable or offline (${state.semanticIndexSize} vectors indexed)`;
        } else {
            dom.settingsSemanticStatus.textContent = `Status: Ready (${state.semanticIndexSize} vectors indexed, model: ${state.semanticModelId})`;
        }
    }
}

export async function performSemanticSearch(query, limit = 100, options = {}) {
    if (!query || !state.semanticSearchAvailable) return null;
    
    const filters = {};
    if (state.activeMediaType && state.activeMediaType !== 'all') {
        filters.mediaType = state.activeMediaType;
    }
    if (state.activeTimelinePeriod) {
        const bounds = getTimelinePeriodBounds(state.activeTimelinePeriod);
        if (bounds && bounds.from) filters.dateFrom = bounds.from;
        if (bounds && bounds.to) filters.dateTo = bounds.to;
    }
    if (state.ratingFilter && state.ratingFilter > 0) {
        filters.ratingMin = state.ratingFilter;
    }
    
    try {
        const result = await api.post('/api/semantic/search', {
            query, limit, filters
        }, options);
        return result;
    } catch (e) {
        if (e.name === 'AbortError') throw e;
        console.error('Semantic search failed:', e);
        showToast('Semantic search failed: ' + (e.message || e), 'error');
        return null;
    }
}

export async function findSimilarMedia(mediaId, limit = 50, options = {}) {
    if (!mediaId || !state.semanticSearchAvailable) return null;
    try {
        const result = await api.post(`/api/semantic/similar/${mediaId}`, { limit }, options);
        return result;
    } catch (e) {
        if (e.name === 'AbortError') throw e;
        console.error('Find similar failed:', e);
        showToast('Find similar failed: ' + (e.message || e), 'error');
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

function selectedPhotoIds() {
    return Array.from(state.selectedIds)
        .map(Number)
        .filter(id => Number.isInteger(id) && id > 0)
        .filter(id => {
            const item = (state.mediaItems || []).find(media => Number(media.id) === id);
            return item && (!item.media_type || item.media_type === 'photo');
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
        if (!state.semanticBuilt) {
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

    const errorMessage = semanticSubmissionError || (hasJob && currentSemanticJob.error ? String(currentSemanticJob.error) : '');
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
    const scope = dom.semanticScanScope;
    if (scope) scope.value = semanticDialogSelectedIds.length ? 'selected' : 'catalog';
    currentSemanticRequest = semanticDialogSelectedIds.length
        ? { scope: 'selected', media_ids: [...semanticDialogSelectedIds] }
        : { scope: 'catalog', media_ids: [] };
    if (dom.semanticForceReindex) dom.semanticForceReindex.checked = false;
    renderSemanticJobState();
    const defaultFocus = (currentSemanticJob ? dom.closeSemanticIndexModalBtn : dom.semanticScanScope) || dom.cancelSemanticJobBtn;
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

export async function submitSemanticJob(request) {
    currentSemanticRequest = request;
    semanticSubmissionError = '';
    const body = { scope: request.scope, force: request.force === true };
    if (request.scope === 'selected') body.media_ids = request.media_ids;

    setJobButton(dom.semanticJobStartBtn, true, true);
    if (dom.semanticJobError) {
        dom.semanticJobError.textContent = '';
        dom.semanticJobError.hidden = true;
    }
    try {
        const result = await api.post('/api/semantic/jobs', body);
        if (!result || !result.job_id) throw new Error(result?.error || 'Failed to start indexing job');
        currentSemanticJob = {
            id: result.job_id,
            state: 'running',
            scope: request.scope,
            total: 0,
            processed: 0,
            skipped: 0,
            failed: 0,
            remaining: 0
        };
        renderSemanticJobState();
        scheduleJobPoll(0);
    } catch (err) {
        semanticSubmissionError = t('semantic_job_start_error', { error: err.message });
        renderSemanticJobState();
    }
}

function scheduleJobPoll(delay = 1000) {
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
            currentSemanticJob = result;
            renderSemanticJobState();
            if (TERMINAL_JOB_STATES.has(currentSemanticJob.state)) {
                if (currentSemanticJob.state === 'completed') {
                    const msg = t('semantic_search_indexing_complete', { count: currentSemanticJob.processed }) || `Indexed ${currentSemanticJob.processed} photos.`;
                    showToast(msg, 'info');
                }
                await checkSemanticStatus();
            } else {
                scheduleJobPoll();
            }
        } else {
            scheduleJobPoll(2000);
        }
    } catch (err) {
        if (dom.semanticJobError) {
            dom.semanticJobError.textContent = err.message;
            dom.semanticJobError.hidden = false;
        }
        scheduleJobPoll(3000);
    } finally {
        semanticJobPollInFlight = false;
    }
}

export async function cancelSemanticScan() {
    if (!currentSemanticJob || TERMINAL_JOB_STATES.has(currentSemanticJob.state)) return;
    setJobButton(dom.semanticJobStopBtn, true, true);
    try {
        await api.post(`/api/semantic/jobs/${encodeURIComponent(currentSemanticJob.id)}/cancel`, {});
        currentSemanticJob = { ...currentSemanticJob, state: 'cancelling' };
        renderSemanticJobState();
        scheduleJobPoll(0);
    } catch (err) {
        if (dom.semanticJobError) {
            dom.semanticJobError.textContent = t('semantic_job_cancel_error', { error: err.message });
            dom.semanticJobError.hidden = false;
        }
        renderSemanticJobState();
    }
}

export async function runSemanticIndexing(force = false) {
    openSemanticIndexModal();
    if (dom.semanticForceReindex) dom.semanticForceReindex.checked = Boolean(force);
    if (state.semanticSearchAvailable) {
        await startSemanticScan();
    }
}

export async function cancelCurrentSemanticJob() {
    await cancelSemanticScan();
    closeSemanticIndexModal();
}

export async function startSemanticIndexing(scope = 'catalog', mediaIds = [], force = false) {
    try {
        const result = await api.post('/api/semantic/jobs', {
            scope, media_ids: mediaIds, force
        });
        if (result && result.job_id) {
            return result.job_id;
        }
        return null;
    } catch (e) {
        showToast('Failed to start indexing: ' + (e.message || e), 'error');
        return null;
    }
}

export async function getSemanticJobProgress(jobId) {
    try {
        return await api.get(`/api/semantic/jobs/${jobId}`);
    } catch (e) {
        return null;
    }
}

export async function cancelSemanticJob(jobId) {
    try {
        await api.post(`/api/semantic/jobs/${jobId}/cancel`);
    } catch (e) {
        showToast('Failed to cancel job: ' + (e.message || e), 'error');
    }
}
