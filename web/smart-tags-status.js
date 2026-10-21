/**
 * IMAGINE Photo Organizer - Smart Tags Status
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

import { state } from './state.js';
import { dom } from './dom.js';
import { api } from './api.js';
import { t } from './i18n.js';

let statusChangeCallback = null;
export function setSmartTagStatusCallback(fn) {
  statusChangeCallback = fn;
}

export function updateSettingsSmartTagsStatus() {
  if (dom.settingsSmartTagsStatus) {
    if (state.smartTagsConnectionError) {
      dom.settingsSmartTagsStatus.textContent = t('smart_tags_connection_error') || 'Status: Could not connect to smart tag service';
    } else if (!state.smartTagsBuilt) {
      dom.settingsSmartTagsStatus.textContent = t('smart_tags_engine_not_built') || 'Status: Smart tagging engine not built in this binary';
    } else if (!state.smartTagsAvailable) {
      dom.settingsSmartTagsStatus.textContent = t('smart_tags_model_unavailable') || 'Status: CLIP model unavailable';
    } else if (!state.smartTagsTaxonomyReady) {
      dom.settingsSmartTagsStatus.textContent = state.smartTagsTaxonomyError
        ? (t('smart_tags_taxonomy_error', { error: state.smartTagsTaxonomyError }) || `Status: Taxonomy error: ${state.smartTagsTaxonomyError}`)
        : (t('smart_tags_taxonomy_error', { error: '' }) || 'Status: Taxonomy error');
    } else if (state.smartTagIndexedPhotos === 0) {
      dom.settingsSmartTagsStatus.textContent = t('smart_tags_index_empty', { count: 0 }) || 'Status: Build the search index first (0 photos indexed)';
    } else {
      dom.settingsSmartTagsStatus.textContent = t('smart_tags_status_ready', {
        labeled: state.smartTagLabeledPhotos,
        labels: state.smartTagLabelCount,
        indexed: state.smartTagIndexedPhotos
      }) || `Status: ${state.smartTagLabeledPhotos} photos labeled · ${state.smartTagLabelCount} labels (index: ${state.smartTagIndexedPhotos} photos)`;
    }
  }

  if (dom.settingsSmartTagsBtn) {
    dom.settingsSmartTagsBtn.disabled = !state.smartTagsAvailable || !state.smartTagsTaxonomyReady || state.smartTagIndexedPhotos === 0;
  }
  if (dom.settingsClearSmartTagsBtn) {
    dom.settingsClearSmartTagsBtn.disabled = !state.smartTagsBuilt || (state.smartTagLabeledPhotos === 0 && (state.smartTagAssignments || 0) === 0);
  }
}

export async function checkSmartTagStatus() {
  try {
    const res = await api.get('/api/smart-tags/status');
    state.smartTagsConnectionError = false;
    state.smartTagsAvailable = res.ready === true;
    state.smartTagsBuilt = res.built === true;
    state.smartTagsTaxonomyReady = res.taxonomyReady === true;
    state.smartTagsTaxonomyError = res.taxonomyError || '';
    state.smartTagIndexedPhotos = res.indexedPhotos || 0;
    state.smartTagLabeledPhotos = res.labeledPhotos || 0;
    state.smartTagLabelCount = res.labelCount || 0;
    state.smartTagAssignments = res.tagAssignments || 0;
    updateSettingsSmartTagsStatus();
    if (typeof statusChangeCallback === 'function') {
      try { statusChangeCallback(); } catch (_) {}
    }
    return res;
  } catch (_e) {
    state.smartTagsConnectionError = true;
    state.smartTagsAvailable = false;
    updateSettingsSmartTagsStatus();
    if (typeof statusChangeCallback === 'function') {
      try { statusChangeCallback(); } catch (_) {}
    }
    return null;
  }
}
