/**
 * IMAGINE Photo Organizer - Media Selection & Batch Action Bar
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

import { state } from './state.js';
import { dom } from './dom.js';
import { getCardElement } from './media-cards.js';
import { t } from './i18n.js';

export let previousSelectedIds = new Set();
const selectionChangeListeners = new Set();

export function addSelectionListener(fn) {
  selectionChangeListeners.add(fn);
}

export function removeSelectionListener(fn) {
  selectionChangeListeners.delete(fn);
}

function notifySelectionChanged() {
  selectionChangeListeners.forEach(fn => {
    try {
      fn();
    } catch (e) {
      console.error('Error in selection listener:', e);
    }
  });
}

export function clearCardSelections() {
  if (state.selectedIds.size === 0) return;
  for (const id of state.selectedIds) {
    const card = getCardElement(id);
    if (card) {
      card.classList.remove('selected');
      card.setAttribute('aria-selected', 'false');
    }
  }
  state.selectedIds.clear();
  previousSelectedIds.clear();
  state.lastSelectedId = null;
  updateBatchBar();
  notifySelectionChanged();
}

export function handleCardSelection(id, event) {
  if (event.ctrlKey || event.metaKey) {
    // Toggle single item
    if (state.selectedIds.has(id)) {
      state.selectedIds.delete(id);
    } else {
      state.selectedIds.add(id);
    }
    state.lastSelectedId = id;
  } else if (event.shiftKey && state.lastSelectedId !== null) {
    // Shift-range selection
    const ids = state.mediaItems.map(m => m.id);
    const startIdx = ids.indexOf(state.lastSelectedId);
    const endIdx = ids.indexOf(id);
    if (startIdx !== -1 && endIdx !== -1) {
      const [low, high] = startIdx < endIdx ? [startIdx, endIdx] : [endIdx, startIdx];
      state.selectedIds.clear();
      for (let i = low; i <= high; i++) {
        state.selectedIds.add(ids[i]);
      }
    } else {
      state.selectedIds.clear();
      state.selectedIds.add(id);
      state.lastSelectedId = id;
    }
  } else {
    // Single select
    state.selectedIds.clear();
    state.selectedIds.add(id);
    state.lastSelectedId = id;
  }

  syncSelectionDOM();
}

export function syncSelectionDOM({ notify = true } = {}) {
  // Update selection classes in DOM only for changed items
  for (const oldId of previousSelectedIds) {
    if (!state.selectedIds.has(oldId)) {
      const c = getCardElement(oldId);
      if (c) {
        c.classList.remove('selected');
        c.setAttribute('aria-selected', 'false');
      }
    }
  }
  for (const newId of state.selectedIds) {
    if (!previousSelectedIds.has(newId)) {
      const c = getCardElement(newId);
      if (c) {
        c.classList.add('selected');
        c.setAttribute('aria-selected', 'true');
      }
    }
  }
  previousSelectedIds = new Set(state.selectedIds);

  updateBatchBar();
  if (notify) {
    notifySelectionChanged();
  }
}

export function updateBatchBar() {
  if (!dom.batchActionBar) return;
  const count = state.selectedIds.size;
  const sortSelector = document.querySelector('.sort-selector');
  if (dom.batchAddAlbumBtn) {
    const labelKey = state.activeAlbumId ? 'remove_from_album' : 'add_to_album';
    dom.batchAddAlbumBtn.dataset.i18n = labelKey;
    dom.batchAddAlbumBtn.dataset.i18nTitle = labelKey;
    dom.batchAddAlbumBtn.textContent = t(labelKey);
    dom.batchAddAlbumBtn.title = t(labelKey);
  }
  if (dom.batchRateFlagSelect) {
    dom.batchRateFlagSelect.value = '';
  }
  if (count > 1) {
    dom.batchActionBar.style.display = 'flex';
    if (dom.batchSelectedCount) dom.batchSelectedCount.textContent = t('n_selected', { count });
    if (sortSelector) sortSelector.style.display = 'none';
  } else {
    dom.batchActionBar.style.display = 'none';
    if (sortSelector) sortSelector.style.display = state.semanticSearchActive ? 'none' : '';
  }
}
