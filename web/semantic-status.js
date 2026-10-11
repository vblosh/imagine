/**
 * IMAGINE Photo Organizer - Semantic Search Status
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

import { state } from './state.js';
import { dom } from './dom.js';
import { api } from './api.js';
import { t } from './i18n.js';

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
  } catch (_e) {
    state.semanticSearchAvailable = false;
    state.semanticBuilt = false;
    updateSemanticToggleVisibility();
    updateSettingsSemanticStatus();
    return null;
  }
}

export function updateSemanticToggleVisibility() {
  if (dom.semanticToggle) {
    dom.semanticToggle.style.display = 'inline-flex';
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
