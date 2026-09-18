/**
 * IMAGINE Photo Organizer - Keyboard Handling & Shortcuts
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

import { state } from './state.js';
import { dom, showToast } from './dom.js';
import {
  clearCardSelections,
  updateItemRating,
  updateItemFlag,
  toggleItemFlag,
  toggleFlagsForIds,
  batchUpdateRatings,
  batchUpdateFlags,
  updateBatchBar
} from './media-grid.js';
import { updateInspector } from './inspector.js';
import { saveInspectorCollapsedState } from './persistence.js';
import {
  updateMapMarkerSelections,
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
  toggleCrop,
  applyCrop,
  cancelCrop,
  toggleSmartFix,
  saveEdits
} from './quick-edit.js';
import { navigateBackToCategory } from './category-view.js';
import { isAutoEventOpen, cancelAutoEventReview } from './auto-events.js';

export function handleEscapeKey() {
  // 1. Modals (highest priority)
  if (isAutoEventOpen()) {
    cancelAutoEventReview();
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
  if (dom.iosInstallModal && dom.iosInstallModal.style.display === 'flex') {
    if (document.activeElement?.blur) document.activeElement.blur();
    dom.iosInstallModal.style.display = 'none';
    return true;
  }
  if (dom.leftSidebar && dom.leftSidebar.classList.contains('open')) {
    dom.leftSidebar.classList.remove('open');
    if (dom.sidebarBackdrop) dom.sidebarBackdrop.classList.remove('active');
    return true;
  }

  // 1b. Quick Edit Mode (cancel crop or exit quick edit)
  if (quickEditState.isOpen) {
    if (quickEditState.cropActive) {
      cancelCrop();
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

export function setupKeyboardShortcuts() {
  window.addEventListener('keydown', (e) => {
    // Centralized Escape handler across all modes & overlays
    if (e.key === 'Escape') {
      if (handleEscapeKey()) {
        e.preventDefault();
      }
      return;
    }

    if (isAutoEventOpen() && dom.autoEventModal && dom.autoEventModal.style.display === 'flex' && e.key === 'Tab') {
      const focusable = Array.from(dom.autoEventModal.querySelectorAll(
        'button:not([disabled]), input:not([disabled]), [tabindex]:not([tabindex="-1"])'
      )).filter(el => el.offsetParent !== null || el.offsetWidth > 0 || el.offsetHeight > 0);
      if (focusable.length) {
        const first = focusable[0];
        const last = focusable[focusable.length - 1];
        if ((!e.shiftKey && document.activeElement === last) || (e.shiftKey && document.activeElement === first) || !dom.autoEventModal.contains(document.activeElement)) {
          e.preventDefault();
          (e.shiftKey ? last : first).focus();
        }
      }
      return;
    }

    const active = document.activeElement;

    // Delete Media Modal shortcuts (Space toggles "Also delete from disk", Enter performs deletion)
    if (dom.deleteMediaModal && (dom.deleteMediaModal.style.display === 'flex' || (dom.deleteMediaModal.style.display !== 'none' && dom.deleteMediaModal.style.display !== ''))) {
      if (e.key === ' ' || e.key === 'Spacebar' || e.code === 'Space') {
        e.preventDefault();
        if (dom.deleteFromDiskCheckbox) {
          dom.deleteFromDiskCheckbox.checked = !dom.deleteFromDiskCheckbox.checked;
          dom.deleteFromDiskCheckbox.dispatchEvent(new Event('change', { bubbles: true }));
        }
        return;
      }
      if (e.key === 'Enter') {
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
      if (e.key === 'Tab') {
        const focusable = Array.from(
          dom.deleteMediaModal.querySelectorAll(
            'button:not([disabled]), input:not([disabled]), [tabindex]:not([tabindex="-1"])'
          )
        ).filter(el => el.offsetParent !== null || el.offsetWidth > 0 || el.offsetHeight > 0);

        if (focusable.length > 0) {
          const firstEl = focusable[0];
          const lastEl = focusable[focusable.length - 1];
          if (e.shiftKey) {
            if (document.activeElement === firstEl || !dom.deleteMediaModal.contains(document.activeElement)) {
              e.preventDefault();
              lastEl.focus();
            }
          } else {
            if (document.activeElement === lastEl || !dom.deleteMediaModal.contains(document.activeElement)) {
              e.preventDefault();
              firstEl.focus();
            }
          }
        } else {
          e.preventDefault();
        }
        return;
      }
      return;
    }

    // If typing in input, textarea, or contenteditable, ignore shortcuts
    if (active?.matches('input, textarea, [contenteditable="true"]')) {
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
      if (quickEditState.isOpen) {
        if ((e.ctrlKey || e.metaKey) && (e.key === 's' || e.key === 'S')) {
          e.preventDefault();
          saveEdits('overwrite');
          return;
        }
        if (e.key === 'e' || e.key === 'E') {
          e.preventDefault();
          closeQuickEdit(true);
          return;
        }
        if (e.key === 'l' || e.key === 'L') {
          e.preventDefault();
          rotateLeft();
          return;
        }
        if (e.key === 'r' || e.key === 'R') {
          e.preventDefault();
          rotateRight();
          return;
        }
        if (e.key === 'c' || e.key === 'C') {
          e.preventDefault();
          toggleCrop();
          return;
        }
        if (e.key === 's' || e.key === 'S') {
          e.preventDefault();
          toggleSmartFix();
          return;
        }
        if (e.key === 'Enter') {
          if (quickEditState.cropActive) {
            e.preventDefault();
            applyCrop();
            return;
          }
        }
        return;
      }

      if (e.key === 'e' || e.key === 'E') {
        e.preventDefault();
        const item = state.mediaItems[state.loupeIndex];
        if (item && item.media_type !== 'video' && item.media_type !== 'audio') {
          openQuickEdit();
        }
        return;
      }

      if (e.key === 'ArrowRight' || e.key === 'PageDown') {
        loupeNext();
      } else if (e.key === 'ArrowLeft' || e.key === 'PageUp') {
        loupePrev();
      } else if ((e.ctrlKey || e.metaKey) && e.key === '0') {
        e.preventDefault();
        setLoupeZoom(1.0);
      } else if ((e.ctrlKey || e.metaKey) && (e.key === '=' || e.key === '+')) {
        e.preventDefault();
        loupeZoomIn();
      } else if ((e.ctrlKey || e.metaKey) && (e.key === '-' || e.key === '_')) {
        e.preventDefault();
        loupeZoomOut();
      } else if (e.key === '+' || e.key === '=') {
        loupeZoomIn();
      } else if (e.key === '-' || e.key === '_') {
        loupeZoomOut();
      } else if (e.key === 'z' || e.key === 'Z') {
        resetLoupeZoom();
      } else if (e.key === ' ') {
        e.preventDefault();
        toggleLoupePlayback();
      } else if (e.key >= '0' && e.key <= '5') {
        const item = state.mediaItems[state.loupeIndex];
        if (item) {
          const rating = parseInt(e.key, 10);
          updateItemRating(item.id, rating);
          showToast(rating === 0 ? 'Rating cleared' : `Rating: ${rating}★`, 'info');
        }
      } else if (e.key === 'Delete' || e.key === 'Backspace') {
        const item = state.mediaItems[state.loupeIndex];
        if (item) openDeleteMediaModal([item.id]);
      } else if (e.key === 'x' || e.key === 'X') {
        const item = state.mediaItems[state.loupeIndex];
        if (item) {
          toggleItemFlag(item.id, -1);
          showToast(item.flag === -1 ? 'Flag: Reject' : 'Flag cleared', 'info');
        }
      } else if (e.key === 'p' || e.key === 'P') {
        const item = state.mediaItems[state.loupeIndex];
        if (item) {
          toggleItemFlag(item.id, 1);
          showToast(item.flag === 1 ? 'Flag: Pick' : 'Flag cleared', 'info');
        }
      } else if (e.key === 'u' || e.key === 'U') {
        const item = state.mediaItems[state.loupeIndex];
        if (item) {
          updateItemFlag(item.id, 0);
          showToast('Flag cleared', 'info');
        }
      }
    } else {
      // Grid mode shortcuts
      if ((e.ctrlKey || e.metaKey) && (e.key === 'a' || e.key === 'A')) {
        e.preventDefault();
        state.selectedIds.clear();
        state.mediaItems.forEach(item => state.selectedIds.add(item.id));
        if (dom.mediaGrid) {
          dom.mediaGrid.querySelectorAll('.photo-card').forEach(c => c.classList.add('selected'));
        }
        updateBatchBar();
        updateInspector();
        updateMapMarkerSelections();
      } else if (e.key === ' ' || e.key === 'Enter') {
        if (state.selectedIds.size > 0) {
          e.preventDefault();
          openLoupeForMedia(Array.from(state.selectedIds)[0]);
        }
      } else if (e.key >= '0' && e.key <= '5') {
        const rating = parseInt(e.key, 10);
        if (state.selectedIds.size > 0) {
          batchUpdateRatings(Array.from(state.selectedIds), rating);
          showToast(rating === 0 ? 'Rating cleared' : `Rating: ${rating}★`, 'info');
        }
      } else if (e.key === 'p' || e.key === 'P') {
        if (state.selectedIds.size > 0) {
          toggleFlagsForIds(Array.from(state.selectedIds), 1);
          showToast('Flag: Pick', 'info');
        }
      } else if (e.key === 'Delete' || e.key === 'Backspace') {
        if (state.selectedIds.size > 0) {
          openDeleteMediaModal(Array.from(state.selectedIds));
        }
      } else if (e.key === 'x' || e.key === 'X') {
        if (state.selectedIds.size > 0) {
          toggleFlagsForIds(Array.from(state.selectedIds), -1);
          showToast('Flag: Reject', 'info');
        }
      } else if (e.key === 'u' || e.key === 'U') {
        if (state.selectedIds.size > 0) {
          batchUpdateFlags(Array.from(state.selectedIds), 0);
          showToast('Flag cleared', 'info');
        }
      } else if (e.key === 'm' || e.key === 'M') {
        switchViewMode('map');
      } else if (e.key === 'g' || e.key === 'G') {
        switchViewMode('grid');
      } else if (state.viewMode === 'map' && e.key === '/') {
        e.preventDefault();
        if (dom.mapSearchInput) dom.mapSearchInput.focus();
      }
    }
  });

  document.addEventListener('focusin', (e) => {
    if (dom.deleteMediaModal && (dom.deleteMediaModal.style.display === 'flex' || (dom.deleteMediaModal.style.display !== 'none' && dom.deleteMediaModal.style.display !== ''))) {
      if (!dom.deleteMediaModal.contains(e.target)) {
        if (dom.deleteFromDiskCheckbox) {
          dom.deleteFromDiskCheckbox.focus();
        } else if (dom.confirmDeleteMediaBtn) {
          dom.confirmDeleteMediaBtn.focus();
        }
      }
    }
  });
}
