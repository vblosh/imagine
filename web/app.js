/**
 * IMAGINE Photo Organizer - Web UI Application Entry Point
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

import {
  api,
  numeric,
  formatBytes,
  normalizeMediaItem,
  normalizeExif,
  normalizeTag,
  normalizeAlbum,
  normalizeCatalogStats,
  normalizeTimelineEntry,
  normalizeImportProgress,
  getOriginalMediaUrl
} from './api.js';
import { state } from './state.js';
import { dom, validateRequiredDom, showToast } from './dom.js';
import { initI18n, setLanguage, onLanguageChange, t, SUPPORTED_LANGUAGES } from './i18n.js';
import {
  cardMap,
  getCardElement,
  clearCardSelections,
  buildMediaParams,
  loadMedia,
  loadMoreMedia,
  loadMetadata,
  renderGrid,
  appendMediaToGrid,
  handleCardSelection,
  updateBatchBar,
  renderSidebarTags,
  renderSidebarAlbums,
  renderSidebarFolders,
  renderTimeline,
  updateSidebarActive,
  updateFilterLabel,
  clearAllFilters,
  updateItemRating,
  updateItemFlag,
  batchUpdateFlags,
  batchUpdateRatings,
  toggleItemFlag,
  toggleFlagsForIds,
  expandTagCategory
} from './media-grid.js';
import {
  updateInspector,
  openInspector,
  renderStarWidget,
  setupResizablePanels,
  setupInspectorInlineEditing
} from './inspector.js';
import {
  switchViewMode,
  initMap,
  fitMapToBounds,
  clusterGpsItems,
  renderMapMarkers,
  renderUnmappedTray,
  enterPlacementMode,
  exitPlacementMode,
  showUnmappedTooltip,
  hideUnmappedTooltip,
  clearGeotag,
  clearMapSearch,
  performMapPlaceSearch,
  selectMapSearchResult,
  patchOpenMapPopup
} from './map-view.js';
import {
  openLoupeForMedia,
  closeLoupe,
  updateLoupeView,
  setLoupeZoom,
  clampLoupePan,
  applyLoupeTransform,
  loupeZoomIn,
  loupeZoomOut,
  resetLoupeZoom,
  loupeNext,
  loupePrev,
  loadLoupeOriginal
} from './loupe.js';
import {
  openQuickEdit,
  closeQuickEdit,
  rotateLeft,
  rotateRight,
  toggleCrop,
  setCropAspectRatio,
  applyCrop,
  cancelCrop,
  toggleSmartFix,
  setSmartFixIntensity,
  startCompare,
  stopCompare,
  resetAllEdits,
  saveEdits,
  setupCropMouseListeners,
  quickEditState
} from './quick-edit.js';
import {
  openImportModal,
  closeImportModal,
  cancelImport,
  pollImportProgress,
  triggerImport,
  openAlbumModal,
  closeAlbumModal,
  submitCreateAlbum,
  openAddToAlbumModal,
  closeAddToAlbumModal,
  submitAddToAlbum,
  openTagModal,
  closeTagModal,
  setModalCategory,
  renderModalSearchHelp,
  submitCreateTag,
  openDeleteMediaModal,
  closeDeleteMediaModal,
  submitDeleteMedia,
  closeDeleteTagModal,
  submitDeleteTag,
  openBatchDateModal,
  closeBatchDateModal,
  submitBatchDateChange,
  openBatchMoveModal,
  closeBatchMoveModal,
  submitBatchMove,
  onBatchDateInputChange,
  onBatchDateShiftHoursChange,
  adjustBatchDateShift,
  onBatchDateTimezoneSelectChange,
  onBatchDateTzCalcChange,
  updateBatchDatePreview
} from './modals.js';
import { handleEscapeKey, setupKeyboardShortcuts } from './keyboard.js';
import { renderCategoryView, initCategoryView, navigateBackToCategory } from './category-view.js';

export function setupEventListeners() {
  initCategoryView();

  // Deselect on actual background click
  if (dom.gridScrollContainer) {
    dom.gridScrollContainer.addEventListener('click', (e) => {
      if (e.target === dom.gridScrollContainer || e.target === dom.mediaGrid) {
        clearCardSelections();
      }
    });
  }

  // Delegated click and dblclick handlers on mediaGrid
  if (dom.mediaGrid) {
    dom.mediaGrid.addEventListener('click', (e) => {
      const starTarget = e.target.closest('.card-stars span');
      if (starTarget) {
        e.stopPropagation();
        const star = parseInt(starTarget.dataset.star, 10);
        const card = e.target.closest('.photo-card');
        if (card) {
          const cardId = parseInt(card.dataset.id, 10);
          const item = state.mediaItems.find(m => m.id === cardId);
          const currentRating = item ? (item.rating || 0) : 0;
          const newRating = currentRating === star ? 0 : star;
          updateItemRating(cardId, newRating);
        }
        return;
      }

      const card = e.target.closest('.photo-card');
      if (!card) return;
      const cardId = parseInt(card.dataset.id, 10);
      if (!isNaN(cardId)) {
        handleCardSelection(cardId, e);
      }
    });

    dom.mediaGrid.addEventListener('dblclick', (e) => {
      if (e.target.closest('.card-stars')) return;
      const card = e.target.closest('.photo-card');
      if (!card) return;
      const cardId = parseInt(card.dataset.id, 10);
      if (!isNaN(cardId)) {
        openLoupeForMedia(cardId);
      }
    });
  }

  // Zoom slider
  if (dom.zoomSlider) {
    dom.zoomSlider.addEventListener('input', (e) => {
      document.documentElement.style.setProperty('--thumb-size', `${e.target.value}px`);
    });
  }

  // Search with debounce
  let searchTimer = null;
  if (dom.searchInput) {
    dom.searchInput.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') {
        if (dom.searchInput.value) {
          dom.searchInput.value = '';
          state.searchText = '';
          loadMedia();
        }
        dom.searchInput.blur();
        e.stopPropagation();
      }
    });
    dom.searchInput.addEventListener('input', (e) => {
      clearTimeout(searchTimer);
      const val = e.target.value.trim();
      if (state.activeFolder) {
        state.activeFolder = null;
        updateSidebarActive();
      }
      const debounceDelay = window.__TEST_MODE__ ? 50 : 300;
      searchTimer = setTimeout(() => {
        state.searchText = val;
        loadMedia();
      }, debounceDelay);
    });
  }

  if (dom.clearSearchBtn) {
    dom.clearSearchBtn.addEventListener('click', () => {
      if (dom.searchInput) dom.searchInput.value = '';
      state.searchText = '';
      loadMedia();
    });
  }

  // Grid scroll pagination
  if (dom.gridScrollContainer) {
    dom.gridScrollContainer.addEventListener('scroll', () => {
      const { scrollTop, scrollHeight, clientHeight } = dom.gridScrollContainer;
      if (scrollTop + clientHeight >= scrollHeight - 250 && !state.isLoadingMore && !state.isLoadingMedia && state.mediaItems.length < state.totalCount) {
        loadMoreMedia();
      }
    });
  }

  // Refresh button
  if (dom.refreshBtn) {
    dom.refreshBtn.addEventListener('click', () => {
      loadMetadata();
      loadMedia();
    });
  }

  // Sort selector
  if (dom.sortSelect) {
    dom.sortSelect.addEventListener('change', (e) => {
      const [field, dir] = e.target.value.split('-');
      state.sortBy = field;
      state.sortDesc = (dir === 'desc');
      loadMedia();
    });
  }

  // Clear filters button
  if (dom.clearFiltersBtn) {
    dom.clearFiltersBtn.addEventListener('click', clearAllFilters);
  }
  if (dom.resetTimelineBtn) {
    dom.resetTimelineBtn.addEventListener('click', () => {
      state.activeTimelinePeriod = null;
      renderTimeline();
      loadMedia();
    });
  }
  if (dom.timelineContainer) {
    dom.timelineContainer.addEventListener('wheel', (e) => {
      if (e.deltaY !== 0 && e.deltaX === 0) {
        e.preventDefault();
        dom.timelineContainer.scrollLeft += e.deltaY;
      }
    }, { passive: false });
  }

  // View Mode Toggle (Grid vs Map)
  if (dom.viewGridBtn) {
    dom.viewGridBtn.addEventListener('click', () => switchViewMode('grid'));
  }
  if (dom.viewMapBtn) {
    dom.viewMapBtn.addEventListener('click', () => switchViewMode('map'));
  }
  if (dom.mapFitBoundsBtn) {
    dom.mapFitBoundsBtn.addEventListener('click', () => fitMapToBounds());
  }
  if (dom.mapToggleUnmappedBtn) {
    dom.mapToggleUnmappedBtn.addEventListener('click', () => {
      state.unmappedTrayOpen = !state.unmappedTrayOpen;
      if (dom.unmappedTray) dom.unmappedTray.style.display = state.unmappedTrayOpen ? 'flex' : 'none';
      if (state.unmappedTrayOpen) renderUnmappedTray();
      else exitPlacementMode();
    });
  }
  if (dom.closeUnmappedTrayBtn) {
    dom.closeUnmappedTrayBtn.addEventListener('click', () => {
      state.unmappedTrayOpen = false;
      if (dom.unmappedTray) dom.unmappedTray.style.display = 'none';
      exitPlacementMode();
    });
  }
  if (dom.unmappedSelectAllBtn) {
    dom.unmappedSelectAllBtn.addEventListener('click', () => {
      const unmapped = state.mediaItems.filter(item => !(item && item.exif && item.exif.has_gps && numeric(item.exif.latitude) !== null && numeric(item.exif.longitude) !== null));
      unmapped.forEach(item => {
        state.placementMediaIds.add(item.id);
        state.selectedIds.add(item.id);
      });
      state.lastUnmappedClickedId = unmapped.length > 0 ? unmapped[unmapped.length - 1].id : null;
      if (unmapped.length > 0 && (!state.lastSelectedId || !state.selectedIds.has(state.lastSelectedId))) {
        state.lastSelectedId = unmapped[0].id;
      }
      if (dom.mapViewContainer) dom.mapViewContainer.classList.add('placement-mode');
      if (dom.unmappedSelectedCount) {
        dom.unmappedSelectedCount.textContent = `${state.placementMediaIds.size} selected`;
        dom.unmappedSelectedCount.style.display = 'inline';
      }
      if (dom.unmappedDeselectAllBtn) dom.unmappedDeselectAllBtn.style.display = 'inline-block';
      if (unmapped.length > 0) openInspector();
      renderUnmappedTray();
      updateInspector();
    });
  }
  if (dom.mapLoadMoreBtn) {
    dom.mapLoadMoreBtn.addEventListener('click', () => {
      loadMoreMedia();
    });
  }
  if (dom.mapLoadAllBtn) {
    dom.mapLoadAllBtn.addEventListener('click', async () => {
      dom.mapLoadAllBtn.disabled = true;
      try {
        while (state.mediaItems.length < state.totalCount) {
          const added = await loadMoreMedia();
          if (!added || added.length === 0) break;
        }
      } finally {
        dom.mapLoadAllBtn.disabled = false;
      }
    });
  }
  if (dom.unmappedLoadMoreBtn) {
    dom.unmappedLoadMoreBtn.addEventListener('click', () => {
      loadMoreMedia();
    });
  }
  if (dom.unmappedPhotosList) {
    dom.unmappedPhotosList.addEventListener('scroll', () => {
      hideUnmappedTooltip();
      const { scrollLeft, scrollWidth, clientWidth } = dom.unmappedPhotosList;
      if (scrollWidth - (scrollLeft + clientWidth) < 100) {
        if (!state.isLoadingMore && !state.isLoadingMedia && state.mediaItems.length < state.totalCount) {
          loadMoreMedia();
        }
      }
    });
  }
  if (dom.unmappedDeselectAllBtn) {
    dom.unmappedDeselectAllBtn.addEventListener('click', () => {
      exitPlacementMode();
      state.selectedIds.clear();
      state.lastSelectedId = null;
      renderUnmappedTray();
      updateInspector();
    });
  }

  // Map Place Search Listeners
  if (dom.mapSearchInput) {
    dom.mapSearchInput.addEventListener('input', (e) => {
      clearTimeout(state.mapSearchDebounceTimer);
      const q = e.target.value;
      const hasText = q.length > 0;
      if (dom.mapSearchIcon) dom.mapSearchIcon.style.display = hasText ? 'none' : '';
      if (dom.mapSearchBox) dom.mapSearchBox.classList.toggle('has-input', hasText);
      if (!q.trim()) {
        if (dom.clearMapSearchBtn) dom.clearMapSearchBtn.style.display = 'none';
        if (dom.mapSearchResults) {
          dom.mapSearchResults.style.display = 'none';
          dom.mapSearchResults.innerHTML = '';
        }
        state.mapSearchResults = [];
        state.mapSearchActiveIdx = -1;
        return;
      }
      if (dom.clearMapSearchBtn) dom.clearMapSearchBtn.style.display = 'block';
      const debounceDelay = window.__TEST_MODE__ ? 50 : 800;
      state.mapSearchDebounceTimer = setTimeout(() => {
        performMapPlaceSearch(q);
      }, debounceDelay);
    });

    dom.mapSearchInput.addEventListener('keydown', (e) => {
      const items = dom.mapSearchResults ? dom.mapSearchResults.querySelectorAll('.map-search-item') : [];
      if (e.key === 'ArrowDown') {
        e.preventDefault();
        if (items.length > 0) {
          state.mapSearchActiveIdx = Math.min(state.mapSearchActiveIdx + 1, items.length - 1);
          items.forEach((it, i) => it.classList.toggle('highlighted', i === state.mapSearchActiveIdx));
          items[state.mapSearchActiveIdx].scrollIntoView({ block: 'nearest' });
        }
      } else if (e.key === 'ArrowUp') {
        e.preventDefault();
        if (items.length > 0) {
          state.mapSearchActiveIdx = Math.max(state.mapSearchActiveIdx - 1, 0);
          items.forEach((it, i) => it.classList.toggle('highlighted', i === state.mapSearchActiveIdx));
          items[state.mapSearchActiveIdx].scrollIntoView({ block: 'nearest' });
        }
      } else if (e.key === 'Enter') {
        e.preventDefault();
        if (state.mapSearchActiveIdx >= 0 && state.mapSearchResults[state.mapSearchActiveIdx]) {
          selectMapSearchResult(state.mapSearchResults[state.mapSearchActiveIdx]);
        } else if (state.mapSearchResults.length > 0) {
          selectMapSearchResult(state.mapSearchResults[0]);
        } else if (dom.mapSearchInput.value.trim()) {
          performMapPlaceSearch(dom.mapSearchInput.value.trim()).then(() => {
            if (state.mapSearchResults.length > 0) {
              selectMapSearchResult(state.mapSearchResults[0]);
            }
          });
        }
      } else if (e.key === 'Escape') {
        if (dom.mapSearchResults) dom.mapSearchResults.style.display = 'none';
      }
    });
  }

  if (dom.clearMapSearchBtn) {
    dom.clearMapSearchBtn.addEventListener('click', () => {
      clearMapSearch();
      if (dom.mapSearchInput) dom.mapSearchInput.focus();
    });
  }

  document.addEventListener('click', (e) => {
    if (dom.mapSearchOverlay && !dom.mapSearchOverlay.contains(e.target)) {
      if (dom.mapSearchResults) dom.mapSearchResults.style.display = 'none';
    }
  });

  // Inspector Map Actions
  if (dom.inspectorShowOnMapBtn) {
    dom.inspectorShowOnMapBtn.addEventListener('click', () => {
      if (state.selectedIds.size === 0) return;
      const id = Array.from(state.selectedIds)[0];
      const item = state.mediaItems.find(i => i.id === id);
      if (!item || !item.exif || !item.exif.has_gps) return;
      switchViewMode('map');
      if (state.mapInstance) {
        const targetLat = numeric(item.exif.latitude);
        const targetLng = numeric(item.exif.longitude);
        if (targetLat === null || targetLng === null) return;
        state.mapInstance.flyTo([targetLat, targetLng], 14, { duration: 0.5 });
        setTimeout(() => {
          const marker = state.mapMarkers.find(marker =>
            marker.clusterItems?.some(candidate => candidate.id === item.id)
          );
          if (marker) {
            marker.activeMediaId = item.id;
            marker.openPopup();
          }
        }, 300);
      }
    });
  }
  if (dom.inspectorClearGpsBtn) {
    dom.inspectorClearGpsBtn.addEventListener('click', () => {
      if (state.selectedIds.size === 0) return;
      const id = Array.from(state.selectedIds)[0];
      clearGeotag(id);
    });
  }
  if (dom.inspectorPlaceOnMapBtn) {
    dom.inspectorPlaceOnMapBtn.addEventListener('click', () => {
      if (state.selectedIds.size === 0) return;
      const id = Array.from(state.selectedIds)[0];
      switchViewMode('map');
      state.unmappedTrayOpen = true;
      if (dom.unmappedTray) dom.unmappedTray.style.display = 'flex';
      renderUnmappedTray();
      enterPlacementMode(id);
    });
  }

  // Tab Switcher (Media, People, Places, Events)
  if (dom.viewTabs) {
    dom.viewTabs.querySelectorAll('.tab-btn').forEach(btn => {
      btn.addEventListener('click', () => {
        dom.viewTabs.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
        btn.classList.add('active');
        state.activeTab = btn.dataset.tab;
        state.activeTagId = null;
        state.activeAlbumId = null;
        state.activeFolder = null;
        state.activeTimelinePeriod = null;
        state.activeMediaType = 'all';
        state.activeStatusFilter = null;
        updateSidebarActive();
        renderTimeline();
        if (state.activeTab === 'media') {
          if (dom.categoryViewContainer) dom.categoryViewContainer.style.display = 'none';
          if (dom.categoryToolbar) dom.categoryToolbar.style.display = 'none';
          if (dom.contentToolbar) dom.contentToolbar.style.display = 'flex';
          if (dom.categoryBackBtn) dom.categoryBackBtn.style.display = 'none';
          if (dom.gridScrollContainer && state.viewMode !== 'map') dom.gridScrollContainer.style.display = 'block';
          loadMedia();
        } else {
          if (dom.mediaGrid) dom.mediaGrid.innerHTML = '';
          if (dom.emptyState) dom.emptyState.style.display = 'none';
          renderCategoryView(state.activeTab);
          updateFilterLabel();
        }
      });
    });
  }

  // Nav sidebar filters (all, media types, flags/status)
  if (dom.navAllMedia) {
    dom.navAllMedia.addEventListener('click', () => {
      clearAllFilters();
    });
  }
  if (dom.navPhotos) {
    dom.navPhotos.addEventListener('click', () => {
      state.activeMediaType = (state.activeMediaType === 'photos') ? 'all' : 'photos';
      state.activeFolder = null;
      state.activeTimelinePeriod = null;
      updateSidebarActive();
      renderTimeline();
      loadMedia();
    });
  }
  if (dom.navVideos) {
    dom.navVideos.addEventListener('click', () => {
      state.activeMediaType = (state.activeMediaType === 'videos') ? 'all' : 'videos';
      state.activeFolder = null;
      state.activeTimelinePeriod = null;
      updateSidebarActive();
      renderTimeline();
      loadMedia();
    });
  }
  if (dom.navAudio) {
    dom.navAudio.addEventListener('click', () => {
      state.activeMediaType = (state.activeMediaType === 'audio') ? 'all' : 'audio';
      state.activeFolder = null;
      state.activeTimelinePeriod = null;
      updateSidebarActive();
      renderTimeline();
      loadMedia();
    });
  }
  if (dom.navPicks) {
    dom.navPicks.addEventListener('click', () => {
      state.activeStatusFilter = (state.activeStatusFilter === 'picks') ? null : 'picks';
      state.activeFolder = null;
      state.activeTimelinePeriod = null;
      updateSidebarActive();
      renderTimeline();
      loadMedia();
    });
  }
  if (dom.navRejects) {
    dom.navRejects.addEventListener('click', () => {
      state.activeStatusFilter = (state.activeStatusFilter === 'rejects') ? null : 'rejects';
      state.activeFolder = null;
      state.activeTimelinePeriod = null;
      updateSidebarActive();
      renderTimeline();
      loadMedia();
    });
  }
  if (dom.navNotRejects) {
    dom.navNotRejects.addEventListener('click', () => {
      state.activeStatusFilter = (state.activeStatusFilter === 'not_rejects') ? null : 'not_rejects';
      state.activeFolder = null;
      state.activeTimelinePeriod = null;
      updateSidebarActive();
      renderTimeline();
      loadMedia();
    });
  }
  if (dom.navUnrated) {
    dom.navUnrated.addEventListener('click', () => {
      state.activeStatusFilter = (state.activeStatusFilter === 'unrated') ? null : 'unrated';
      state.activeFolder = null;
      state.activeTimelinePeriod = null;
      updateSidebarActive();
      renderTimeline();
      loadMedia();
    });
  }

  // Tag category headers collapsible
  document.querySelectorAll('.category-header').forEach(hdr => {
    hdr.addEventListener('click', () => {
      const group = hdr.closest('.tag-category-group');
      const items = group ? group.querySelector('.tag-items') : null;
      const arrow = hdr.querySelector('.arrow');
      if (items) {
        if (items.style.display === 'none') {
          items.style.display = 'block';
          if (arrow) arrow.textContent = '▼';
          hdr.setAttribute('aria-expanded', 'true');
        } else {
          items.style.display = 'none';
          if (arrow) arrow.textContent = '▶';
          hdr.setAttribute('aria-expanded', 'false');
        }
      }
    });
    hdr.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') {
        e.preventDefault();
        hdr.click();
      }
    });
  });

  // Folders header collapsible
  if (dom.foldersHeader) {
    dom.foldersHeader.addEventListener('click', () => {
      const tree = dom.foldersTree;
      const arrow = dom.foldersHeader.querySelector('.arrow') || dom.foldersArrow;
      if (tree) {
        if (tree.style.display === 'none') {
          tree.style.display = 'block';
          if (arrow) arrow.textContent = '▼';
          dom.foldersHeader.setAttribute('aria-expanded', 'true');
        } else {
          tree.style.display = 'none';
          if (arrow) arrow.textContent = '▶';
          dom.foldersHeader.setAttribute('aria-expanded', 'false');
        }
      }
    });
    dom.foldersHeader.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') {
        e.preventDefault();
        dom.foldersHeader.click();
      }
    });
  }

  // Inspector toggle
  if (dom.toggleInspectorBtn) {
    dom.toggleInspectorBtn.addEventListener('click', () => {
      if (dom.rightInspector) dom.rightInspector.classList.toggle('collapsed');
    });
  }
  if (dom.closeInspectorBtn) {
    dom.closeInspectorBtn.addEventListener('click', () => {
      if (dom.rightInspector) dom.rightInspector.classList.add('collapsed');
    });
  }
  if (dom.openLoupeFromInspector) {
    dom.openLoupeFromInspector.addEventListener('click', () => {
      if (state.selectedIds.size > 0) {
        openLoupeForMedia(Array.from(state.selectedIds)[0]);
      }
    });
  }

  // Inspector flag pick/reject
  if (dom.inspectorFlag) {
    const inspectorPickBtn = dom.inspectorFlag.querySelector('.flag-pick');
    const inspectorRejectBtn = dom.inspectorFlag.querySelector('.flag-reject');
    if (inspectorPickBtn) {
      inspectorPickBtn.addEventListener('click', () => {
        if (state.selectedIds.size === 0) return;
        toggleItemFlag(Array.from(state.selectedIds)[0], 1);
      });
    }
    if (inspectorRejectBtn) {
      inspectorRejectBtn.addEventListener('click', () => {
        if (state.selectedIds.size === 0) return;
        toggleItemFlag(Array.from(state.selectedIds)[0], -1);
      });
    }
  }

  // Inspector add tag inline
  async function handleAddTagInline() {
    if (!dom.addTagInput) return;
    const tagName = dom.addTagInput.value.trim();
    if (!tagName || state.selectedIds.size === 0) return;
    const id = Array.from(state.selectedIds)[0];
    const category = dom.addTagCategorySelect ? dom.addTagCategorySelect.value : 'keyword';

    try {
      await api.post(`/api/media/${id}/tags`, { name: tagName, category });
      if (dom.addTagInput) dom.addTagInput.value = '';
      updateInspector();
      loadMetadata();
      expandTagCategory(category);
    } catch (err) {
      alert(`Failed to add tag: ${err.message}`);
    }
  }
  if (dom.addTagBtn) {
    dom.addTagBtn.addEventListener('click', handleAddTagInline);
  }
  if (dom.addTagInput) {
    dom.addTagInput.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') handleAddTagInline();
    });
    if (dom.addTagCategorySelect) {
      dom.addTagInput.addEventListener('input', () => {
        const val = dom.addTagInput.value.trim().toLowerCase();
        const match = state.tags.find(t => t.name.toLowerCase() === val);
        if (match && match.category) {
          dom.addTagCategorySelect.value = match.category.toLowerCase();
        }
      });
    }
  }

  if (dom.inspectorAddToAlbumBtn) {
    dom.inspectorAddToAlbumBtn.addEventListener('click', () => {
      const ids = state.selectedIds.size > 0 ? Array.from(state.selectedIds) : [];
      if (ids.length > 0) {
        openAddToAlbumModal(ids);
      }
    });
  }

  // Batch Action Bar handlers
  if (dom.batchClearBtn) {
    dom.batchClearBtn.addEventListener('click', () => {
      clearCardSelections();
    });
  }

  if (dom.batchPickBtn) {
    dom.batchPickBtn.addEventListener('click', async () => {
      await toggleFlagsForIds(Array.from(state.selectedIds), 1);
    });
  }

  if (dom.batchRejectBtn) {
    dom.batchRejectBtn.addEventListener('click', async () => {
      await toggleFlagsForIds(Array.from(state.selectedIds), -1);
    });
  }

  if (dom.batchDeleteBtn) {
    dom.batchDeleteBtn.addEventListener('click', () => {
      openDeleteMediaModal(Array.from(state.selectedIds));
    });
  }

  if (dom.batchRating) {
    renderStarWidget(dom.batchRating, 0, async (newRating) => {
      await batchUpdateRatings(Array.from(state.selectedIds), newRating);
    });
  }

  if (dom.batchAddTagBtn) {
    dom.batchAddTagBtn.addEventListener('click', () => {
      openTagModal(Array.from(state.selectedIds));
    });
  }

  if (dom.batchAddAlbumBtn) {
    dom.batchAddAlbumBtn.addEventListener('click', () => {
      openAddToAlbumModal(Array.from(state.selectedIds));
    });
  }

  if (dom.batchDateBtn) {
    dom.batchDateBtn.addEventListener('click', () => {
      openBatchDateModal(Array.from(state.selectedIds));
    });
  }

  if (dom.batchMoveBtn) {
    dom.batchMoveBtn.addEventListener('click', () => {
      openBatchMoveModal(Array.from(state.selectedIds));
    });
  }

  const handleInspectorMove = () => {
    if (state.selectedIds.size > 0) {
      openBatchMoveModal(Array.from(state.selectedIds));
    } else if (state.lastSelectedId) {
      openBatchMoveModal([state.lastSelectedId]);
    } else if (state.loupeIndex >= 0 && state.mediaItems[state.loupeIndex]) {
      openBatchMoveModal([state.mediaItems[state.loupeIndex].id]);
    }
  };

  if (dom.inspectorMoveBtn) {
    dom.inspectorMoveBtn.addEventListener('click', handleInspectorMove);
  }

  if (dom.inspectorMovePathBtn) {
    dom.inspectorMovePathBtn.addEventListener('click', handleInspectorMove);
  }

  if (dom.inspectorDeleteBtn) {
    dom.inspectorDeleteBtn.addEventListener('click', () => {
      if (state.selectedIds.size > 0) {
        openDeleteMediaModal(Array.from(state.selectedIds));
      } else if (state.loupeIndex >= 0 && state.mediaItems[state.loupeIndex]) {
        openDeleteMediaModal([state.mediaItems[state.loupeIndex].id]);
      }
    });
  }

  if (dom.loupeDeleteBtn) {
    dom.loupeDeleteBtn.addEventListener('click', () => {
      if (state.loupeIndex >= 0 && state.mediaItems[state.loupeIndex]) {
        openDeleteMediaModal([state.mediaItems[state.loupeIndex].id]);
      }
    });
  }

  // Quick Edit controls
  if (dom.loupeQuickEditBtn) {
    dom.loupeQuickEditBtn.addEventListener('click', () => {
      const item = (state.loupeIndex >= 0 && state.loupeIndex < state.mediaItems.length)
        ? state.mediaItems[state.loupeIndex]
        : null;
      if (item && (item.media_type === 'video' || item.media_type === 'audio')) {
        return;
      }
      if (quickEditState.isOpen) {
        closeQuickEdit(true);
      } else {
        openQuickEdit();
      }
    });
  }
  if (dom.quickEditRotateLeftBtn) dom.quickEditRotateLeftBtn.addEventListener('click', rotateLeft);
  if (dom.quickEditRotateRightBtn) dom.quickEditRotateRightBtn.addEventListener('click', rotateRight);
  if (dom.quickEditCropBtn) dom.quickEditCropBtn.addEventListener('click', toggleCrop);
  if (dom.cropAspectRatioSelect) {
    dom.cropAspectRatioSelect.addEventListener('change', (e) => setCropAspectRatio(e.target.value));
  }
  if (dom.quickEditApplyCropBtn) dom.quickEditApplyCropBtn.addEventListener('click', applyCrop);
  if (dom.quickEditCancelCropBtn) dom.quickEditCancelCropBtn.addEventListener('click', cancelCrop);
  if (dom.quickEditSmartFixBtn) dom.quickEditSmartFixBtn.addEventListener('click', toggleSmartFix);
  if (dom.smartFixIntensitySlider) {
    dom.smartFixIntensitySlider.addEventListener('input', (e) => setSmartFixIntensity(parseInt(e.target.value, 10)));
  }
  if (dom.quickEditCompareBtn) {
    dom.quickEditCompareBtn.addEventListener('pointerdown', startCompare);
    dom.quickEditCompareBtn.addEventListener('pointerup', stopCompare);
    dom.quickEditCompareBtn.addEventListener('pointerleave', stopCompare);
  }
  if (dom.quickEditResetBtn) dom.quickEditResetBtn.addEventListener('click', resetAllEdits);
  if (dom.quickEditCancelBtn) dom.quickEditCancelBtn.addEventListener('click', () => closeQuickEdit(true));
  if (dom.quickEditSaveBtn) dom.quickEditSaveBtn.addEventListener('click', () => saveEdits('overwrite'));
  if (dom.quickEditSaveCopyBtn) dom.quickEditSaveCopyBtn.addEventListener('click', () => saveEdits('copy'));
  setupCropMouseListeners();

  // Loupe navigation
  if (dom.loupePrevBtn) dom.loupePrevBtn.addEventListener('click', loupePrev);
  if (dom.loupeNextBtn) dom.loupeNextBtn.addEventListener('click', loupeNext);
  if (dom.loupeCloseBtn) dom.loupeCloseBtn.addEventListener('click', closeLoupe);
  if (dom.loupeBackdrop) dom.loupeBackdrop.addEventListener('click', closeLoupe);

  // Loupe zoom controls
  if (dom.loupeZoomInBtn) {
    dom.loupeZoomInBtn.addEventListener('click', loupeZoomIn);
  }
  if (dom.loupeZoomOutBtn) {
    dom.loupeZoomOutBtn.addEventListener('click', loupeZoomOut);
  }
  if (dom.loupeZoomResetBtn) {
    dom.loupeZoomResetBtn.addEventListener('click', resetLoupeZoom);
  }
  if (dom.loupeZoomSlider) {
    dom.loupeZoomSlider.addEventListener('input', (e) => {
      setLoupeZoom(parseFloat(e.target.value) / 100);
    });
  }

  // Loupe pan and drag handling
  let isLoupeDragging = false;
  let loupeDragStartX = 0;
  let loupeDragStartY = 0;
  let loupeDragMoved = false;

  if (dom.loupeImageViewport) {
    dom.loupeImageViewport.addEventListener('mousedown', (e) => {
      if (e.button !== 0) return;
      if (e.target.closest('.loupe-toolbar')) return;
      loupeDragMoved = false;
      if (state.loupeZoom <= 1.0) return;

      isLoupeDragging = true;
      loupeDragStartX = e.clientX - state.loupePanX;
      loupeDragStartY = e.clientY - state.loupePanY;
      dom.loupeImageViewport.classList.add('is-dragging');
      e.preventDefault();
    });

    dom.loupeImageViewport.addEventListener('click', (e) => {
      if (e.target.closest('.loupe-toolbar')) return;
      if (loupeDragMoved) return;
      loadLoupeOriginal();
    });

    dom.loupeImageViewport.addEventListener('dblclick', (e) => {
      if (e.target.closest('.loupe-toolbar')) return;
      if (state.loupeZoom > 1.05) {
        setLoupeZoom(1.0);
      } else {
        setLoupeZoom(2.0, e.clientX, e.clientY);
      }
    });

    dom.loupeImageViewport.addEventListener('wheel', (e) => {
      e.preventDefault();
      const zoomFactor = e.deltaY < 0 ? 1.15 : 1 / 1.15;
      setLoupeZoom(state.loupeZoom * zoomFactor, e.clientX, e.clientY);
    }, { passive: false });
  }

  window.addEventListener('mousemove', (e) => {
    if (!isLoupeDragging) return;
    loupeDragMoved = true;
    state.loupePanX = e.clientX - loupeDragStartX;
    state.loupePanY = e.clientY - loupeDragStartY;
    clampLoupePan();
    applyLoupeTransform();
  });

  window.addEventListener('mouseup', () => {
    if (!isLoupeDragging) return;
    isLoupeDragging = false;
    if (dom.loupeImageViewport) {
      dom.loupeImageViewport.classList.remove('is-dragging');
    }
  });

  window.addEventListener('resize', () => {
    if (state.loupeIndex >= 0 && state.loupeZoom > 1.0) {
      clampLoupePan();
      applyLoupeTransform();
    }
  });

  if (dom.loupeFlag) {
    const loupePickBtn = dom.loupeFlag.querySelector('.flag-pick');
    const loupeRejectBtn = dom.loupeFlag.querySelector('.flag-reject');
    if (loupePickBtn) {
      loupePickBtn.addEventListener('click', () => {
        if (state.loupeIndex < 0) return;
        const item = state.mediaItems[state.loupeIndex];
        if (item) toggleItemFlag(item.id, 1);
      });
    }
    if (loupeRejectBtn) {
      loupeRejectBtn.addEventListener('click', () => {
        if (state.loupeIndex < 0) return;
        const item = state.mediaItems[state.loupeIndex];
        if (item) toggleItemFlag(item.id, -1);
      });
    }
  }

  // Import modal
  if (dom.importBtn) dom.importBtn.addEventListener('click', openImportModal);
  if (dom.emptyImportBtn) dom.emptyImportBtn.addEventListener('click', openImportModal);
  if (dom.closeImportModalBtn) dom.closeImportModalBtn.addEventListener('click', closeImportModal);
  if (dom.cancelImportBtn) dom.cancelImportBtn.addEventListener('click', cancelImport);
  if (dom.importBackdrop) dom.importBackdrop.addEventListener('click', closeImportModal);

  if (dom.startImportBtn) {
    dom.startImportBtn.addEventListener('click', () => {
      const path = dom.importPathInput ? dom.importPathInput.value.trim() : '';
      if (!path) {
        alert('Please provide a directory path');
        return;
      }
      triggerImport(path, dom.importRecursiveCheck ? dom.importRecursiveCheck.checked : false);
    });
  }

  // New Album Modal
  if (dom.newAlbumBtn) dom.newAlbumBtn.addEventListener('click', openAlbumModal);
  if (dom.closeNewAlbumModalBtn) dom.closeNewAlbumModalBtn.addEventListener('click', closeAlbumModal);
  if (dom.cancelAlbumBtn) dom.cancelAlbumBtn.addEventListener('click', closeAlbumModal);
  if (dom.newAlbumBackdrop) dom.newAlbumBackdrop.addEventListener('click', closeAlbumModal);
  if (dom.createAlbumSubmitBtn) dom.createAlbumSubmitBtn.addEventListener('click', submitCreateAlbum);

  // Add to Album Modal
  if (dom.closeAddToAlbumModalBtn) dom.closeAddToAlbumModalBtn.addEventListener('click', closeAddToAlbumModal);
  if (dom.cancelAddToAlbumBtn) dom.cancelAddToAlbumBtn.addEventListener('click', closeAddToAlbumModal);
  if (dom.addToAlbumBackdrop) dom.addToAlbumBackdrop.addEventListener('click', closeAddToAlbumModal);
  if (dom.confirmAddToAlbumBtn) dom.confirmAddToAlbumBtn.addEventListener('click', submitAddToAlbum);

  // New Tag Modal
  if (dom.newTagBtn) dom.newTagBtn.addEventListener('click', () => openTagModal());
  if (dom.inspectorAddTagModalBtn) {
    dom.inspectorAddTagModalBtn.addEventListener('click', () => {
      openTagModal(state.selectedIds.size > 0 ? Array.from(state.selectedIds) : []);
    });
  }
  if (dom.inspectorOpenTagModalBtn) {
    dom.inspectorOpenTagModalBtn.addEventListener('click', () => {
      openTagModal(state.selectedIds.size > 0 ? Array.from(state.selectedIds) : []);
    });
  }
  if (dom.closeNewTagModalBtn) dom.closeNewTagModalBtn.addEventListener('click', closeTagModal);
  if (dom.cancelTagBtn) dom.cancelTagBtn.addEventListener('click', closeTagModal);
  if (dom.newTagBackdrop) dom.newTagBackdrop.addEventListener('click', closeTagModal);

  if (dom.tagModalClearInputBtn) {
    dom.tagModalClearInputBtn.addEventListener('click', () => {
      if (dom.tagNameInput) dom.tagNameInput.value = '';
      if (dom.tagModalClearInputBtn) dom.tagModalClearInputBtn.style.display = 'none';
      renderModalSearchHelp();
      if (dom.tagNameInput) dom.tagNameInput.focus();
    });
  }

  if (dom.tagCategoryCards) {
    dom.tagCategoryCards.querySelectorAll('.category-card').forEach(card => {
      card.addEventListener('click', () => {
        const cat = card.getAttribute('data-cat');
        if (cat) setModalCategory(cat);
      });
    });
  }

  if (dom.tagCategorySelect) {
    dom.tagCategorySelect.addEventListener('change', () => {
      setModalCategory(dom.tagCategorySelect.value);
    });
  }

  if (dom.tagNameInput) {
    dom.tagNameInput.addEventListener('input', () => {
      const val = dom.tagNameInput.value;
      if (dom.tagModalClearInputBtn) {
        dom.tagModalClearInputBtn.style.display = val.length > 0 ? 'inline-block' : 'none';
      }
      renderModalSearchHelp();
    });
    dom.tagNameInput.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') {
        e.preventDefault();
        if (dom.createTagSubmitBtn) dom.createTagSubmitBtn.click();
      }
    });
  }

  if (dom.createTagSubmitBtn) {
    dom.createTagSubmitBtn.addEventListener('click', submitCreateTag);
  }

  // Delete Media Modal
  if (dom.closeDeleteMediaModalBtn) dom.closeDeleteMediaModalBtn.addEventListener('click', closeDeleteMediaModal);
  if (dom.cancelDeleteMediaBtn) dom.cancelDeleteMediaBtn.addEventListener('click', closeDeleteMediaModal);
  if (dom.deleteMediaBackdrop) dom.deleteMediaBackdrop.addEventListener('click', closeDeleteMediaModal);
  if (dom.confirmDeleteMediaBtn) dom.confirmDeleteMediaBtn.addEventListener('click', submitDeleteMedia);
  if (dom.deleteFromDiskCheckbox) {
    dom.deleteFromDiskCheckbox.addEventListener('change', () => {
      const checked = dom.deleteFromDiskCheckbox.checked;
      if (dom.deleteMediaWarningText) {
        if (checked) {
          dom.deleteMediaWarningText.textContent =
            'Warning: This will permanently delete the original file(s) from disk in addition to removing them from the catalog database. This action cannot be undone.';
          dom.deleteMediaWarningText.style.color = 'var(--reject-color, #e05252)';
        } else {
          dom.deleteMediaWarningText.textContent =
            'This removes photo metadata, ratings, tags, and album associations from the catalog database. The original files on disk will not be deleted.';
          dom.deleteMediaWarningText.style.color = 'var(--text-dim)';
        }
      }
      if (dom.confirmDeleteMediaBtn) {
        dom.confirmDeleteMediaBtn.textContent = checked ? 'Delete from Disk' : 'Delete from Catalog';
      }
    });
  }

  // Delete Tag Modal
  if (dom.closeDeleteTagModalBtn) dom.closeDeleteTagModalBtn.addEventListener('click', closeDeleteTagModal);
  if (dom.cancelDeleteTagBtn) dom.cancelDeleteTagBtn.addEventListener('click', closeDeleteTagModal);
  if (dom.deleteTagBackdrop) dom.deleteTagBackdrop.addEventListener('click', closeDeleteTagModal);
  if (dom.confirmDeleteTagBtn) dom.confirmDeleteTagBtn.addEventListener('click', submitDeleteTag);

  // Batch Change Date Modal
  if (dom.closeBatchDateModalBtn) dom.closeBatchDateModalBtn.addEventListener('click', closeBatchDateModal);
  if (dom.cancelBatchDateBtn) dom.cancelBatchDateBtn.addEventListener('click', closeBatchDateModal);
  if (dom.batchDateBackdrop) dom.batchDateBackdrop.addEventListener('click', closeBatchDateModal);
  if (dom.confirmBatchDateBtn) dom.confirmBatchDateBtn.addEventListener('click', submitBatchDateChange);

  if (dom.batchDateInput) {
    dom.batchDateInput.addEventListener('input', onBatchDateInputChange);
  }
  if (dom.batchDateShiftHours) {
    dom.batchDateShiftHours.addEventListener('input', onBatchDateShiftHoursChange);
  }
  if (dom.shiftMinus24Btn) dom.shiftMinus24Btn.addEventListener('click', () => adjustBatchDateShift(-24));
  if (dom.shiftMinus1Btn) dom.shiftMinus1Btn.addEventListener('click', () => adjustBatchDateShift(-1));
  if (dom.shiftPlus1Btn) dom.shiftPlus1Btn.addEventListener('click', () => adjustBatchDateShift(1));
  if (dom.shiftPlus24Btn) dom.shiftPlus24Btn.addEventListener('click', () => adjustBatchDateShift(24));

  if (dom.batchDateTimezoneSelect) {
    dom.batchDateTimezoneSelect.addEventListener('change', onBatchDateTimezoneSelectChange);
  }
  if (dom.batchDateTzFrom) {
    dom.batchDateTzFrom.addEventListener('change', onBatchDateTzCalcChange);
  }
  if (dom.batchDateTzTo) {
    dom.batchDateTzTo.addEventListener('change', onBatchDateTzCalcChange);
  }
  if (dom.batchDateModeShift) {
    dom.batchDateModeShift.addEventListener('change', updateBatchDatePreview);
  }
  if (dom.batchDateModeExact) {
    dom.batchDateModeExact.addEventListener('change', updateBatchDatePreview);
  }

  if (dom.closeBatchMoveModalBtn) {
    dom.closeBatchMoveModalBtn.addEventListener('click', closeBatchMoveModal);
  }
  if (dom.cancelBatchMoveBtn) {
    dom.cancelBatchMoveBtn.addEventListener('click', closeBatchMoveModal);
  }
  if (dom.batchMoveBackdrop) {
    dom.batchMoveBackdrop.addEventListener('click', closeBatchMoveModal);
  }
  if (dom.confirmBatchMoveBtn) {
    dom.confirmBatchMoveBtn.addEventListener('click', submitBatchMove);
  }
  if (dom.batchMovePathInput) {
    dom.batchMovePathInput.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') {
        e.preventDefault();
        submitBatchMove();
      }
    });
  }

  setupResizablePanels();
  setupInspectorInlineEditing();
  setupKeyboardShortcuts();
}

let initialized = false;

export async function init() {
  if (initialized) return;
  initialized = true;
  initI18n();
  onLanguageChange(() => {
    updateFilterLabel();
    updateBatchBar();
    renderTimeline();
    if (state.activeTab && state.activeTab !== 'media') {
      renderCategoryView(state.activeTab);
    } else {
      renderGrid();
    }
    if (state.selectedIds.size > 0) {
      updateInspector();
    }
    if (state.viewMode === 'map') {
      renderUnmappedTray();
    }
  });
  validateRequiredDom();
  setupEventListeners();
  await loadMetadata();
  await loadMedia();
}

// Start app when DOM ready
if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', init);
} else {
  init();
}

// Expose state and API for UI test assertions and debugging
window._imagineState = state;
window._imagineApp = {
  state,
  dom,
  numeric,
  formatBytes,
  normalizeMediaItem,
  normalizeExif,
  normalizeTag,
  normalizeAlbum,
  normalizeCatalogStats,
  normalizeTimelineEntry,
  normalizeImportProgress,
  clusterGpsItems,
  patchOpenMapPopup,
  buildMediaParams,
  loadMedia,
  loadMoreMedia,
  appendMediaToGrid,
  pollImportProgress,
  validateRequiredDom,
  setupEventListeners,
  handleEscapeKey,
  updateItemRating,
  updateItemFlag,
  batchUpdateRatings,
  batchUpdateFlags,
  openBatchMoveModal,
  closeBatchMoveModal,
  submitBatchMove,
  renderTimeline,
  renderUnmappedTray,
  updateLoupeView,
  loupeNext,
  loupePrev,
  clearAllFilters,
  showToast,
  getOriginalMediaUrl,
  setLanguage,
  t,
  SUPPORTED_LANGUAGES
};
