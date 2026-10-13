/**
 * IMAGINE Photo Organizer - Top-Level Views Orchestrator
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 * The ONLY module allowed to import inspector, map-view, loupe, category-view, modals.
 */

import { state } from './state.js';
import { dom } from './dom.js';
import {
  openInspector,
  updateInspector,
  patchInspectorRatingAndFlag,
} from './inspector.js';
import {
  renderMapMarkers,
  renderUnmappedTray,
  patchOpenMapPopup,
  updateMapMarkerSelections,
  setMapViewActionHooks,
} from './map-view.js';
import { openLoupeForMedia, updateLoupeControls, closeLoupe } from './loupe.js';
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
  setModalCloseLoupeHandler,
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
import { loadMediaData, loadMoreMediaData, loadMetadataData } from './media-fetch.js';
import { clearFilterPreferences } from './persistence.js';

const LAST_IMPORTED_TAG_NAME = 'Last Imported';
const LAST_IMPORTED_TAG_CATEGORY = 'keyword';

export function renderCurrentView() {
  if (state.activeTab && state.activeTab !== 'media' && !state.activeTagId && !state.activeAlbumId) {
    renderCategoryView(state.activeTab);
  } else if (state.viewMode === 'map') {
    renderMapMarkers();
    renderUnmappedTray();
  } else {
    renderGrid();
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
  if (dom.gridScrollContainer && state.viewMode !== 'map') {
    dom.gridScrollContainer.style.display = 'block';
  }

  if (state.viewMode !== 'map') {
    renderGrid();
    if (!shouldPreserveScroll && dom.gridScrollContainer) {
      dom.gridScrollContainer.scrollTop = 0;
      dom.gridScrollContainer.scrollLeft = 0;
    }
  }
  if (state.viewMode === 'map' || state.mapInstance) {
    renderMapMarkers();
    renderUnmappedTray();
  }
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
    refreshAfterMediaLoad({ shouldPreserveScroll: result.shouldPreserveScroll });
  }
}

export async function loadMoreMedia() {
  const result = await loadMoreMediaData();
  if (!result || !result.newItems) return;

  if (state.viewMode !== 'map') {
    appendMediaToGrid(result.newItems);
  }
  if (state.viewMode === 'map' || state.mapInstance) {
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

export function initViews() {
  setViewsOrchestrator({
    loadMedia,
    loadMoreMedia,
    loadMetadata,
    updateItemRating,
  });

  setGridOpenLoupeHandler(openLoupeForMedia);
  setModalCloseLoupeHandler(closeLoupe);

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
