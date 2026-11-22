/**
 * IMAGINE Photo Organizer - Top-Level Views Orchestrator
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 * Along with keyboard.js (top-level input router) and app.js (application entry point),
 * this module orchestrates inspector, map-view, loupe, category-view, and modals.
 */

import { state, invalidateGpsCache, pruneMediaSelection } from './state.js';
import { dom } from './dom.js';
import {
  openInspector,
  updateInspector,
  patchInspectorRatingAndFlag,
} from './inspector.js';
import {
  initMap,
  fitMapToBounds,
  renderMapMarkers,
  renderUnmappedTray,
  patchOpenMapPopup,
  updateMapMarkerSelections,
  setMapViewActionHooks,
} from './map-view.js';
import { openLoupeForMedia, updateLoupeControls, closeLoupe, updateLoupeView, updateLoupePosition, reconcileLoupeAfterMediaLoad } from './loupe.js';
import {
  renderCategoryView,
  setCategoryViewActionHooks,
} from './category-view.js';
import {
  updateModalTagSuggestions,
  renderModalSearchHelp,
  openDeleteTagModal,
  openTagModal,
  openAlbumModal,
  setModalLoupeDeleteHandler,
} from './modals.js';
import {
  renderGrid,
  appendMediaToGrid,
  updateFilterLabel,
  syncSemanticSearchUI,
  clearAllFilters,
  setViewsOrchestrator,
  setGridOpenLoupeHandler,
} from './grid.js';
import {
  renderSidebarTags,
  renderSidebarAlbums,
  renderSidebarFolders,
  updateSidebarActive,
  setSidebarActionHooks,
} from './sidebar.js';
import { renderTimeline, setTimelineActionHooks } from './timeline.js';
import {
  updateBatchBar,
  addSelectionListener,
} from './media-selection.js';
import { setAlbumOrderHooks } from './media-params.js';
import {
  addRatingFlagListener,
  setRatingFlagMetadataReload,
  updateItemRating,
} from './rating-flag.js';
import { updateRovingTabindex } from './grid-a11y.js';
import { cancelInFlightMediaLoads, loadMediaData, loadMoreMediaData, loadMetadataData } from './media-fetch.js';
import { clearFilterPreferences } from './persistence.js';
import { setDeduplicateActionHooks, openDeduplicateModal } from './deduplicate.js';

const LAST_IMPORTED_TAG_NAME = 'Last Imported';
const LAST_IMPORTED_TAG_CATEGORY = 'keyword';

export function syncViewModeDisplay({ shouldPreserveScroll = false } = {}) {
  if (state.viewMode === 'map') {
    if (dom.gridScrollContainer) dom.gridScrollContainer.style.display = 'none';
    if (dom.mapViewContainer) dom.mapViewContainer.style.display = 'flex';
    if (dom.viewGridBtn) dom.viewGridBtn.classList.remove('active');
    if (dom.viewMapBtn) dom.viewMapBtn.classList.add('active');
    if (!state.mapInstance) {
      initMap().then(() => {
        if (state.viewMode === 'map' && state.mapInstance) {
          state.mapInstance.invalidateSize();
          renderMapMarkers();
          renderUnmappedTray();
          fitMapToBounds();
        }
      });
    } else {
      state.mapInstance.invalidateSize();
      setTimeout(() => {
        if (state.viewMode === 'map' && state.mapInstance) {
          state.mapInstance.invalidateSize();
        }
      }, 50);
      renderMapMarkers();
      renderUnmappedTray();
      fitMapToBounds();
    }
  } else {
    if (dom.gridScrollContainer) dom.gridScrollContainer.style.display = 'block';
    if (dom.mapViewContainer) dom.mapViewContainer.style.display = 'none';
    if (dom.viewGridBtn) dom.viewGridBtn.classList.add('active');
    if (dom.viewMapBtn) dom.viewMapBtn.classList.remove('active');
    renderGrid();
    if (!shouldPreserveScroll && dom.gridScrollContainer) {
      dom.gridScrollContainer.scrollTop = 0;
      dom.gridScrollContainer.scrollLeft = 0;
    }
    if (state.mapInstance) {
      renderMapMarkers();
      renderUnmappedTray();
    }
  }
}

export function renderCurrentView() {
  if (state.activeTab && state.activeTab !== 'media' && !state.activeTagId && !state.activeAlbumId) {
    renderCategoryView(state.activeTab);
  } else {
    if (dom.categoryViewContainer) dom.categoryViewContainer.style.display = 'none';
    if (dom.categoryToolbar) dom.categoryToolbar.style.display = 'none';
    if (dom.contentToolbar) dom.contentToolbar.style.display = 'flex';
    syncViewModeDisplay();
  }
  updateFilterLabel();
}

export function refreshAfterMediaLoad({ shouldPreserveScroll = false } = {}) {
  if (state.activeTab && state.activeTab !== 'media' && !state.activeTagId && !state.activeAlbumId) {
    renderCategoryView(state.activeTab);
    updateFilterLabel();
    return;
  }

  if (dom.categoryViewContainer) dom.categoryViewContainer.style.display = 'none';
  if (dom.categoryToolbar) dom.categoryToolbar.style.display = 'none';
  if (dom.contentToolbar) dom.contentToolbar.style.display = 'flex';

  syncViewModeDisplay({ shouldPreserveScroll });
  renderSidebarFolders();
  updateFilterLabel();
  updateBatchBar();
  updateInspector();
  syncSemanticSearchUI();
}

export function refreshAfterMetadataLoad() {
  renderSidebarTags();
  renderSidebarAlbums();
  renderSidebarFolders();
  renderTimeline();
  updateSidebarActive();
  if (state.activeTab && state.activeTab !== 'media' && !state.activeTagId && !state.activeAlbumId) {
    renderCategoryView(state.activeTab);
  }
  updateModalTagSuggestions();
  if (dom.newTagModal && dom.newTagModal.style.display === 'flex') {
    renderModalSearchHelp();
  }

  if (dom.totalMediaCount) {
    dom.totalMediaCount.textContent = state.stats.total_media || 0;
  }
  if (dom.totalLastImportedCount) {
    const lastImportedTag = state.tags.find(
      tag =>
        tag.name === LAST_IMPORTED_TAG_NAME &&
        (tag.category || 'keyword').toLowerCase() === LAST_IMPORTED_TAG_CATEGORY
    );
    dom.totalLastImportedCount.textContent = lastImportedTag?.media_count || 0;
  }
  if (dom.totalPhotosCount) {
    dom.totalPhotosCount.textContent = state.stats.total_photos || 0;
  }
  if (dom.totalVideosCount) {
    dom.totalVideosCount.textContent = state.stats.total_videos || 0;
  }
  if (dom.totalAudioCount) {
    dom.totalAudioCount.textContent = state.stats.total_audio || 0;
  }
  if (dom.totalPicksCount) {
    dom.totalPicksCount.textContent = state.stats.total_picks || 0;
  }
  if (dom.totalRejectsCount) {
    dom.totalRejectsCount.textContent = state.stats.total_rejects || 0;
  }
  if (dom.totalNotRejectsCount) {
    dom.totalNotRejectsCount.textContent = state.stats.total_not_rejects || 0;
  }
  if (dom.totalUnratedCount) {
    dom.totalUnratedCount.textContent = state.stats.total_unrated || 0;
  }
  if (dom.totalDuplicatesCount) {
    dom.totalDuplicatesCount.textContent = state.stats.total_duplicates || 0;
  }
}

export async function loadMedia(append = false, options = {}) {
  const result = await loadMediaData(append, options);
  if (!result) return;
  if (result.isCategoryView) {
    renderCategoryView(result.tab);
    updateFilterLabel();
    return;
  }
  if (result.success) {
    reconcileLoupeAfterMediaLoad();
    refreshAfterMediaLoad({ shouldPreserveScroll: result.shouldPreserveScroll });
  }
}

export async function loadMoreMedia() {
  const result = await loadMoreMediaData();
  if (!result || !result.newItems) return;

  if (state.viewMode !== 'map') {
    appendMediaToGrid(result.newItems);
  }
  if (state.viewMode === 'map') {
    renderMapMarkers();
    renderUnmappedTray();
  }
  renderSidebarFolders();
  updateFilterLabel();
  updateBatchBar();
  updateInspector();

  return result.newItems;
}

export async function loadMetadata() {
  const result = await loadMetadataData();
  if (result?.success) {
    refreshAfterMetadataLoad();
  }
}

function applyLoupeDeletion(deletedIds) {
  const currentIndex = state.loupeIndex;
  if (currentIndex < 0 || state.loupeDisabledActions) return false;

  const items = state.mediaItems;
  const current = items[currentIndex];
  if (!current) return false;

  const remaining = items.filter(item => !deletedIds.has(item.id));
  const currentDeleted = deletedIds.has(current.id);
  const preceding = items.slice(0, currentIndex).filter(item => !deletedIds.has(item.id)).length;
  cancelInFlightMediaLoads();
  state.mediaItems = remaining;
  state.mediaOffset = remaining.length;
  state.totalCount = Math.max(remaining.length, state.totalCount - (items.length - remaining.length));
  invalidateGpsCache();
  pruneMediaSelection();

  if (state.semanticSearchActive) {
    remaining.forEach((item, index) => { item.rank = index + 1; });
  }
  if (remaining.length === 0) {
    closeLoupe();
  } else {
    state.loupeIndex = currentDeleted ? Math.max(0, preceding - 1) : preceding;
    if (currentDeleted) {
      state.lastSelectedId = remaining[state.loupeIndex].id;
      state.selectedIds.clear();
      state.selectedIds.add(state.lastSelectedId);
      updateLoupeView();
    } else {
      updateLoupePosition();
    }
  }

  setTimeout(() => {
    if (state.mediaItems !== remaining) return;
    refreshAfterMediaLoad({ shouldPreserveScroll: true });
    if (state.semanticSearchActive) {
      void loadMedia(false, { preserveScroll: true });
    } else if (remaining.length === 0 && state.totalCount > 0) {
      void loadMoreMedia();
    }
  }, 0);
  void loadMetadata();
  return true;
}

export function initViews() {
  setViewsOrchestrator({
    loadMedia,
    loadMoreMedia,
    loadMetadata,
    updateItemRating,
  });

  setGridOpenLoupeHandler(openLoupeForMedia);
  setModalLoupeDeleteHandler(applyLoupeDeletion);

  setMapViewActionHooks({
    loadMoreMedia,
    renderGrid,
    updateInspector,
    openInspector,
    openLoupeForMedia,
  });

  setCategoryViewActionHooks({
    updateSidebarActive,
    loadMedia,
    updateFilterLabel,
    openTagModal,
    openAlbumModal,
  });

  setAlbumOrderHooks({
    clearFilterPreferences,
    updateSidebarActive,
    renderTimeline,
  });

  setSidebarActionHooks({
    loadMedia,
    loadMetadata,
    renderTimeline,
    openDeleteTagModal,
    clearAllFilters,
    openDeduplicateModal,
  });

  setDeduplicateActionHooks({
    loadMedia,
    loadMetadata,
    updateInspector,
    openLoupeForMedia,
  });

  setTimelineActionHooks({
    loadMedia,
    updateSidebarActive,
  });

  setRatingFlagMetadataReload(() => {
    loadMetadata();
  });

  addRatingFlagListener((item, id) => {
    if (item) patchInspectorRatingAndFlag(item);
    patchOpenMapPopup(id);
    if (state.loupeIndex >= 0) updateLoupeControls();
  });

  addSelectionListener(() => {
    updateInspector();
    updateMapMarkerSelections();
    updateRovingTabindex();
  });
}
