/**
 * IMAGINE Photo Organizer - Keyboard Handling & Shortcuts
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 * Functions alongside views.js and app.js as a top-level input router and shortcut
 * dispatcher, orchestrating actions across modals, loupe, quick-edit, map, and grid views.
 */

import { state } from './state.js';
import { dom, showToast } from './dom.js';
import { t } from './i18n.js';
import { clearCardSelections, selectAllMediaCards } from './media-selection.js';
import {
  updateItemRating,
  updateItemFlag,
  toggleItemFlag,
  toggleFlagsForIds,
  batchUpdateRatings,
  batchUpdateFlags,
} from './rating-flag.js';
import { closeSemanticSearch } from './grid.js';
import { saveInspectorCollapsedState } from './persistence.js';
import {
  switchViewMode,
  clearMapSearch,
  exitPlacementMode
} from './map-view.js';
import {
  closeDeleteMediaModal,
  closeDeleteTagModal,
  closeImportModal,
  closeAlbumModal,
  closeTagModal,
  closeAddToAlbumModal,
  closeBatchDateModal,
  closeBatchMoveModal,
  closeSettingsModal,
  openDeleteMediaModal
} from './modals.js';
import {
  closeLoupe,
  openLoupeForMedia,
  loupeNext,
  loupePrev,
  setLoupeZoom,
  loupeZoomIn,
  loupeZoomOut,
  resetLoupeZoom,
  toggleLoupePlayback
} from './loupe.js';
import {
  quickEditState,
  openQuickEdit,
  closeQuickEdit,
  rotateLeft,
  rotateRight,
  activateMode,
  applyMode,
  cancelMode,
  saveEdits,
  undo,
  redo,
  startCompare,
  stopCompare,
  hideDiscardModal,
  performCloseQuickEdit
} from './quick-edit.js';
import { navigateBackToCategory } from './category-view.js';
import { isAutoEventOpen, cancelAutoEventReview } from './auto-events.js';
import { isDeduplicateOpen, closeDeduplicateModal } from './deduplicate.js';
import { closeSemanticIndexModal } from './semantic-search.js';
import { closeSmartTagsModal, closeClearSmartTagsModal } from './smart-tags.js';

export function handleEscapeKey() {
  // 1. Modals (highest priority)
  if (dom.discardChangesModal && dom.discardChangesModal.style.display === 'flex') {
    if (document.activeElement?.blur) document.activeElement.blur();
    hideDiscardModal();
    return true;
  }
  if (isAutoEventOpen()) {
    cancelAutoEventReview();
    return true;
  }
  if (isDeduplicateOpen()) {
    if (document.activeElement?.blur) document.activeElement.blur();
    closeDeduplicateModal();
    return true;
  }
  if (dom.deleteTagModal && dom.deleteTagModal.style.display === 'flex') {
    if (document.activeElement?.blur) document.activeElement.blur();
    closeDeleteTagModal();
    return true;
  }
  if (dom.deleteMediaModal && dom.deleteMediaModal.style.display === 'flex') {
    if (document.activeElement?.blur) document.activeElement.blur();
    closeDeleteMediaModal();
    return true;
  }
  if (dom.clearSmartTagsModal && dom.clearSmartTagsModal.style.display === 'flex') {
    if (document.activeElement?.blur) document.activeElement.blur();
    closeClearSmartTagsModal();
    return true;
  }
  if (dom.smartTagsModal && dom.smartTagsModal.style.display === 'flex') {
    if (document.activeElement?.blur) document.activeElement.blur();
    closeSmartTagsModal();
    return true;
  }
  if (dom.importModal && dom.importModal.style.display === 'flex') {
    if (document.activeElement?.blur) document.activeElement.blur();
    closeImportModal();
    return true;
  }
  if (dom.newAlbumModal && dom.newAlbumModal.style.display === 'flex') {
    if (document.activeElement?.blur) document.activeElement.blur();
    closeAlbumModal();
    return true;
  }
  if (dom.newTagModal && dom.newTagModal.style.display === 'flex') {
    if (document.activeElement?.blur) document.activeElement.blur();
    closeTagModal();
    return true;
  }
  if (dom.addToAlbumModal && dom.addToAlbumModal.style.display === 'flex') {
    if (document.activeElement?.blur) document.activeElement.blur();
    closeAddToAlbumModal();
    return true;
  }
  if (dom.batchDateModal && dom.batchDateModal.style.display === 'flex') {
    if (document.activeElement?.blur) document.activeElement.blur();
    closeBatchDateModal();
    return true;
  }
  if (dom.batchMoveModal && dom.batchMoveModal.style.display === 'flex') {
    if (document.activeElement?.blur) document.activeElement.blur();
    closeBatchMoveModal();
    return true;
  }
  if (dom.settingsModal && dom.settingsModal.style.display === 'flex') {
    if (document.activeElement?.blur) document.activeElement.blur();
    closeSettingsModal();
    return true;
  }
  if (dom.semanticIndexModal && dom.semanticIndexModal.style.display === 'flex') {
    if (document.activeElement?.blur) document.activeElement.blur();
    closeSemanticIndexModal();
    return true;
  }
  if (dom.iosInstallModal && dom.iosInstallModal.style.display === 'flex') {
    if (document.activeElement?.blur) document.activeElement.blur();
    dom.iosInstallModal.style.display = 'none';
    return true;
  }
  const faceForceModal = document.getElementById('faceForceWarningModal');
  if (faceForceModal && faceForceModal.style.display === 'flex') {
    if (document.activeElement?.blur) document.activeElement.blur();
    document.getElementById('faceForceCancelBtn')?.click();
    return true;
  }
  const faceScanModal = document.getElementById('faceScanBusyModal');
  if (faceScanModal && faceScanModal.style.display === 'flex') {
    if (document.activeElement?.blur) document.activeElement.blur();
    document.getElementById('faceScanBusyCloseBtn')?.click();
    return true;
  }
  const faceJobModal = document.getElementById('faceJobModal');
  if (faceJobModal && faceJobModal.style.display === 'flex') {
    if (document.activeElement?.blur) document.activeElement.blur();
    document.getElementById('faceJobCloseBtn')?.click();
    return true;
  }
  const faceGridModal = document.getElementById('faceGridModal');
  if (faceGridModal && faceGridModal.style.display === 'flex') {
    if (document.activeElement?.blur) document.activeElement.blur();
    document.getElementById('faceGridCloseBtn')?.click();
    return true;
  }
  if (dom.leftSidebar && dom.leftSidebar.classList.contains('open')) {
    dom.leftSidebar.classList.remove('open');
    if (dom.sidebarBackdrop) dom.sidebarBackdrop.classList.remove('active');
    return true;
  }

  // 1b. Quick Edit Mode (cancel crop or exit quick edit)
  if (quickEditState.isOpen) {
    if (quickEditState.activeMode) {
      cancelMode();
      return true;
    }
    closeQuickEdit(true);
    return true;
  }

  // 2. Fullscreen Loupe Viewer
  if (state.loupeIndex >= 0 || (dom.loupeModal && dom.loupeModal.style.display === 'flex')) {
    closeLoupe();
    return true;
  }

  // 3. Map search results dropdown list
  if (dom.mapSearchResults && dom.mapSearchResults.style.display !== 'none') {
    dom.mapSearchResults.style.display = 'none';
    return true;
  }

  // 4. Map active popups
  if (state.viewMode === 'map' && state.mapInstance && document.querySelector('.leaflet-popup')) {
    state.mapInstance.closePopup();
    if (state.searchMarker) clearMapSearch();
    return true;
  }

  // 5. Map placement mode
  if ((state.placementMediaIds && state.placementMediaIds.size > 0) || state.placementMediaId) {
    exitPlacementMode();
    return true;
  }

  // 6. Unmapped tray
  if (state.unmappedTrayOpen || (dom.unmappedTray && dom.unmappedTray.style.display !== 'none')) {
    state.unmappedTrayOpen = false;
    if (dom.unmappedTray) dom.unmappedTray.style.display = 'none';
    exitPlacementMode();
    return true;
  }

  // 6b. Category Drilldown (People, Places, Events, Albums: Back button active)
  if ((dom.categoryBackBtn && dom.categoryBackBtn.style.display !== 'none') ||
      (state.activeTab && state.activeTab !== 'media' && (state.activeTagId || state.activeAlbumId))) {
    if (state.selectedIds && state.selectedIds.size > 0) {
      clearCardSelections();
    }
    navigateBackToCategory();
    return true;
  }

  // 6c. Semantic AI Search Grid
  if (state.semanticSearchActive) {
    if (state.selectedIds && state.selectedIds.size > 0) {
      clearCardSelections();
      return true;
    }
    closeSemanticSearch();
    return true;
  }

  // 7. Active card / photo selections
  if (state.selectedIds.size > 0) {
    clearCardSelections();
    return true;
  }

  // 8. Toasts
  const toastContainer = document.getElementById('toastContainer');
  if (toastContainer && toastContainer.children.length > 0) {
    toastContainer.innerHTML = '';
    return true;
  }

  // 9. Inspector collapse state (collapse if open)
  if (dom.rightInspector && !dom.rightInspector.classList.contains('collapsed')) {
    dom.rightInspector.classList.add('collapsed');
    saveInspectorCollapsedState(true);
    return true;
  }

  return false;
}

export function isAnyBlockingModalOpen() {
  if (dom.discardChangesModal && dom.discardChangesModal.style.display === 'flex') {
    return true;
  }
  if (isAutoEventOpen()) {
    return true;
  }
  if (dom.deleteTagModal && dom.deleteTagModal.style.display === 'flex') {
    return true;
  }
  if (dom.deleteMediaModal && (dom.deleteMediaModal.style.display === 'flex' || (dom.deleteMediaModal.style.display !== 'none' && dom.deleteMediaModal.style.display !== ''))) {
    return true;
  }
  if (dom.clearSmartTagsModal && dom.clearSmartTagsModal.style.display === 'flex') {
    return true;
  }
  if (dom.smartTagsModal && dom.smartTagsModal.style.display === 'flex') {
    return true;
  }
  if (dom.importModal && dom.importModal.style.display === 'flex') {
    return true;
  }
  if (dom.newAlbumModal && dom.newAlbumModal.style.display === 'flex') {
    return true;
  }
  if (dom.newTagModal && dom.newTagModal.style.display === 'flex') {
    return true;
  }
  if (dom.addToAlbumModal && dom.addToAlbumModal.style.display === 'flex') {
    return true;
  }
  if (dom.batchDateModal && dom.batchDateModal.style.display === 'flex') {
    return true;
  }
  if (dom.batchMoveModal && dom.batchMoveModal.style.display === 'flex') {
    return true;
  }
  if (dom.settingsModal && dom.settingsModal.style.display === 'flex') {
    return true;
  }
  if (dom.semanticIndexModal && dom.semanticIndexModal.style.display === 'flex') {
    return true;
  }
  if (dom.iosInstallModal && dom.iosInstallModal.style.display === 'flex') {
    return true;
  }
  const faceForceModal = document.getElementById('faceForceWarningModal');
  if (faceForceModal && faceForceModal.style.display === 'flex') {
    return true;
  }
  const faceScanModal = document.getElementById('faceScanBusyModal');
  if (faceScanModal && faceScanModal.style.display === 'flex') {
    return true;
  }
  const faceJobModal = document.getElementById('faceJobModal');
  if (faceJobModal && faceJobModal.style.display === 'flex') {
    return true;
  }
  const faceGridModal = document.getElementById('faceGridModal');
  if (faceGridModal && faceGridModal.style.display === 'flex') {
    return true;
  }
  return false;
}

let shortcutsInitialized = false;

export function setupKeyboardShortcuts() {
  if (shortcutsInitialized) {
    return;
  }
  shortcutsInitialized = true;

  window.addEventListener('keydown', (e) => {
    // Centralized Escape handler across all modes & overlays
    if (e.key === 'Escape') {
      if (handleEscapeKey()) {
        e.preventDefault();
      }
      return;
    }

    const hasModifier = e.ctrlKey || e.metaKey || e.altKey;
    const active = document.activeElement;

    // Delete Media Modal shortcuts (Space toggles "Also delete from disk", Enter performs deletion)
    if (dom.deleteMediaModal && (dom.deleteMediaModal.style.display === 'flex' || (dom.deleteMediaModal.style.display !== 'none' && dom.deleteMediaModal.style.display !== ''))) {
      if (!hasModifier && (e.key === ' ' || e.key === 'Spacebar' || e.code === 'Space')) {
        e.preventDefault();
        if (dom.deleteFromDiskCheckbox) {
          dom.deleteFromDiskCheckbox.checked = !dom.deleteFromDiskCheckbox.checked;
          dom.deleteFromDiskCheckbox.dispatchEvent(new Event('change', { bubbles: true }));
        }
        return;
      }
      if (!hasModifier && e.key === 'Enter') {
        e.preventDefault();
        if (active === dom.cancelDeleteMediaBtn) {
          dom.cancelDeleteMediaBtn.click();
        } else if (active === dom.closeDeleteMediaModalBtn) {
          dom.closeDeleteMediaModalBtn.click();
        } else if (dom.confirmDeleteMediaBtn) {
          dom.confirmDeleteMediaBtn.click();
        }
        return;
      }
      return;
    }

    // Discard Changes Modal shortcuts (Enter confirms or cancels depending on focus)
    if (dom.discardChangesModal && dom.discardChangesModal.style.display === 'flex') {
      if (!hasModifier && e.key === 'Enter') {
        e.preventDefault();
        if (active === dom.cancelDiscardChangesBtn || active === dom.closeDiscardChangesModalBtn) {
          hideDiscardModal();
        } else if (active?.matches('button') && active !== dom.confirmDiscardChangesBtn) {
          active.click();
        } else {
          performCloseQuickEdit();
        }
        return;
      }
      return;
    }

    // Clear Smart Tags Modal shortcuts (Enter confirms or cancels depending on focus)
    if (dom.clearSmartTagsModal && dom.clearSmartTagsModal.style.display === 'flex') {
      if (!hasModifier && e.key === 'Enter') {
        e.preventDefault();
        if (active === dom.cancelClearSmartTagsBtn || active === dom.closeClearSmartTagsModalBtn) {
          closeClearSmartTagsModal();
        } else if (active === dom.confirmClearSmartTagsBtn) {
          dom.confirmClearSmartTagsBtn.click();
        }
        return;
      }
      return;
    }

    // Suppress shortcuts behind any open modal dialog
    if (isAnyBlockingModalOpen()) {
      return;
    }

    // If typing in input, textarea, or contenteditable, ignore shortcuts
    if (active?.matches('input, textarea, [contenteditable="true"]')) {
      return;
    }

    // Quick Edit mode active (takes priority over button guard so Space, Ctrl+Z etc work reliably)
    if (state.loupeIndex >= 0 && quickEditState.isOpen) {
      if ((e.ctrlKey || e.metaKey) && !e.altKey && (e.key === 'z' || e.key === 'Z')) {
        e.preventDefault();
        if (e.shiftKey) {
          redo();
        } else {
          undo();
        }
        return;
      }
      if ((e.ctrlKey || e.metaKey) && !e.altKey && (e.key === 'y' || e.key === 'Y')) {
        e.preventDefault();
        redo();
        return;
      }
      if ((e.ctrlKey || e.metaKey) && !e.altKey && (e.key === 's' || e.key === 'S')) {
        e.preventDefault();
        saveEdits('overwrite');
        return;
      }

      const isButtonOrSelect = active?.matches('button, select');

      if (!hasModifier && e.key === ' ' && !e.repeat) {
        if (isButtonOrSelect) return;
        e.preventDefault();
        startCompare();
        return;
      }
      if (isButtonOrSelect) {
        return;
      }
      if (!hasModifier && (e.key === 'e' || e.key === 'E')) {
        e.preventDefault();
        closeQuickEdit(true);
        return;
      }
      if (!hasModifier && (e.key === 'l' || e.key === 'L')) {
        e.preventDefault();
        rotateLeft();
        return;
      }
      if (!hasModifier && (e.key === 'r' || e.key === 'R')) {
        e.preventDefault();
        rotateRight();
        return;
      }
      if (!hasModifier && (e.key === 'c' || e.key === 'C')) {
        e.preventDefault();
        activateMode('crop');
        return;
      }
      if (!hasModifier && (e.key === 'h' || e.key === 'H')) {
        e.preventDefault();
        activateMode('horizon');
        return;
      }
      if (!hasModifier && (e.key === 's' || e.key === 'S')) {
        e.preventDefault();
        activateMode('smartfix');
        return;
      }
      if (!hasModifier && e.key === 'Enter') {
        if (quickEditState.activeMode) {
          e.preventDefault();
          applyMode();
          return;
        }
      }
      return;
    }

    // Guard buttons and select elements from unintended mutation and activation shortcuts
    const isButtonOrSelect = active?.matches('button, select');
    const isMutationKey = ['0', '1', '2', '3', '4', '5', 'p', 'P', 'x', 'X', 'u', 'U', 'Delete', 'Backspace', 'Enter', ' '].includes(e.key);
    if (isButtonOrSelect && isMutationKey) {
      return;
    }

    // Loupe mode active
    if (state.loupeIndex >= 0) {
      if (!hasModifier && (e.key === 'e' || e.key === 'E')) {
        e.preventDefault();
        const item = state.mediaItems[state.loupeIndex];
        if (item && item.media_type !== 'video' && item.media_type !== 'audio') {
          openQuickEdit();
        }
        return;
      }

      if (!hasModifier && (e.key === 'ArrowRight' || e.key === 'PageDown')) {
        loupeNext();
      } else if (!hasModifier && (e.key === 'ArrowLeft' || e.key === 'PageUp')) {
        loupePrev();
      } else if ((e.ctrlKey || e.metaKey) && !e.altKey && e.key === '0') {
        e.preventDefault();
        setLoupeZoom(1.0);
      } else if ((e.ctrlKey || e.metaKey) && !e.altKey && (e.key === '=' || e.key === '+')) {
        e.preventDefault();
        loupeZoomIn();
      } else if ((e.ctrlKey || e.metaKey) && !e.altKey && (e.key === '-' || e.key === '_')) {
        e.preventDefault();
        loupeZoomOut();
      } else if (!hasModifier && (e.key === '+' || e.key === '=')) {
        loupeZoomIn();
      } else if (!hasModifier && (e.key === '-' || e.key === '_')) {
        loupeZoomOut();
      } else if (!hasModifier && (e.key === 'z' || e.key === 'Z')) {
        resetLoupeZoom();
      } else if (!hasModifier && e.key === ' ') {
        e.preventDefault();
        toggleLoupePlayback();
      } else if (!hasModifier && e.key >= '0' && e.key <= '5') {
        const item = state.mediaItems[state.loupeIndex];
        if (item) {
          const rating = parseInt(e.key, 10);
          updateItemRating(item.id, rating);
          showToast(rating === 0 ? t('toast_rating_cleared') : t('toast_rating_value', { rating }), 'info');
        }
      } else if (!hasModifier && (e.key === 'Delete' || e.key === 'Backspace')) {
        const item = state.mediaItems[state.loupeIndex];
        if (item) openDeleteMediaModal([item.id]);
      } else if (!hasModifier && (e.key === 'x' || e.key === 'X')) {
        const item = state.mediaItems[state.loupeIndex];
        if (item) {
          toggleItemFlag(item.id, -1);
          showToast(item.flag === -1 ? t('toast_flag_reject') : t('toast_flag_cleared'), 'info');
        }
      } else if (!hasModifier && (e.key === 'p' || e.key === 'P')) {
        const item = state.mediaItems[state.loupeIndex];
        if (item) {
          toggleItemFlag(item.id, 1);
          showToast(item.flag === 1 ? t('toast_flag_pick') : t('toast_flag_cleared'), 'info');
        }
      } else if (!hasModifier && (e.key === 'u' || e.key === 'U')) {
        const item = state.mediaItems[state.loupeIndex];
        if (item) {
          updateItemFlag(item.id, 0);
          showToast(t('toast_flag_cleared'), 'info');
        }
      }
    } else {
      // Suppress grid shortcuts when category overview is active
      if (dom.categoryViewContainer && dom.categoryViewContainer.style.display !== 'none') {
        return;
      }

      // Grid mode shortcuts
      if ((e.ctrlKey || e.metaKey) && !e.altKey && (e.key === 'a' || e.key === 'A')) {
        e.preventDefault();
        selectAllMediaCards();
      } else if (!hasModifier && (e.key === ' ' || e.key === 'Enter')) {
        if (state.selectedIds.size > 0) {
          e.preventDefault();
          openLoupeForMedia(Array.from(state.selectedIds)[0]);
        }
      } else if (!hasModifier && e.key >= '0' && e.key <= '5') {
        const rating = parseInt(e.key, 10);
        if (state.selectedIds.size > 0) {
          batchUpdateRatings(Array.from(state.selectedIds), rating);
          showToast(rating === 0 ? t('toast_rating_cleared') : t('toast_rating_value', { rating }), 'info');
        }
      } else if (!hasModifier && (e.key === 'p' || e.key === 'P')) {
        if (state.selectedIds.size > 0) {
          const nextFlag = toggleFlagsForIds(Array.from(state.selectedIds), 1);
          showToast(nextFlag === 1 ? t('toast_flag_pick') : t('toast_flag_cleared'), 'info');
        }
      } else if (!hasModifier && (e.key === 'Delete' || e.key === 'Backspace')) {
        if (state.selectedIds.size > 0) {
          openDeleteMediaModal(Array.from(state.selectedIds));
        }
      } else if (!hasModifier && (e.key === 'x' || e.key === 'X')) {
        if (state.selectedIds.size > 0) {
          const nextFlag = toggleFlagsForIds(Array.from(state.selectedIds), -1);
          showToast(nextFlag === -1 ? t('toast_flag_reject') : t('toast_flag_cleared'), 'info');
        }
      } else if (!hasModifier && (e.key === 'u' || e.key === 'U')) {
        if (state.selectedIds.size > 0) {
          batchUpdateFlags(Array.from(state.selectedIds), 0);
          showToast(t('toast_flag_cleared'), 'info');
        }
      } else if (!hasModifier && (e.key === 'm' || e.key === 'M')) {
        switchViewMode('map');
      } else if (!hasModifier && (e.key === 'g' || e.key === 'G')) {
        switchViewMode('grid');
      } else if (!hasModifier && state.viewMode === 'map' && e.key === '/') {
        e.preventDefault();
        if (dom.mapSearchInput) dom.mapSearchInput.focus();
      }
    }
  });

  window.addEventListener('keyup', (e) => {
    if (e.key === ' ' && quickEditState.isOpen && quickEditState.isComparing) {
      stopCompare();
    }
  });

  window.addEventListener('blur', () => {
    if (quickEditState.isOpen && quickEditState.isComparing) {
      stopCompare();
    }
  });
}
