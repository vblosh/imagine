/**
 * IMAGINE Photo Organizer - Media Grid, Sidebar & Mutations
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

import {
  api,
  escapeHtml,
  formatDate,
  formatDuration,
  numeric,
  normalizeMediaItem,
  normalizeTag,
  normalizeAlbum,
  normalizeTimelineEntry,
  normalizeCatalogStats,
  getMonthName,
  getOriginalMediaUrl
} from './api.js';
import { state, invalidateGpsCache } from './state.js';
import { dom, showToast } from './dom.js';
import {
  renderMapMarkers,
  renderUnmappedTray,
  patchOpenMapPopup,
  updateMapMarkerSelections
} from './map-view.js';
import { updateInspector, patchInspectorRatingAndFlag } from './inspector.js';
import { openLoupeForMedia, updateLoupeControls } from './loupe.js';
import { updateModalTagSuggestions, renderModalSearchHelp, openDeleteTagModal } from './modals.js';
import { renderCategoryView } from './category-view.js';
import { t, getShortMonthName } from './i18n.js';
import {
  saveActiveFoldersPreference,
  saveActiveTagIdsPreference,
  clearFilterPreferences,
  saveActiveTabPreference,
  saveFoldersCollapsedState,
  saveTagCategoryCollapsedState
} from './persistence.js';

// DOM Caching & Query Scoping
export const cardMap = new Map();
export let previousSelectedIds = new Set();

export function getCardElement(id) {
  let card = cardMap.get(id);
  if (card && card.isConnected) return card;
  if (!dom.mediaGrid) return null;
  card = dom.mediaGrid.querySelector(`.photo-card[data-id="${CSS.escape(String(id))}"]`);
  if (card) cardMap.set(id, card);
  return card;
}

export function clearCardSelections() {
  if (state.selectedIds.size === 0) return;
  for (const id of state.selectedIds) {
    const card = getCardElement(id);
    if (card) card.classList.remove('selected');
  }
  state.selectedIds.clear();
  previousSelectedIds.clear();
  state.lastSelectedId = null;
  updateBatchBar();
  updateInspector();
  updateMapMarkerSelections();
}

let currentLoadMediaId = 0;
let loadMediaAbortController = null;
let loadMoreAbortController = null;

export function buildMediaParams() {
  const params = {
    sort: `${state.sortBy}-${state.sortDesc ? 'desc' : 'asc'}`
  };

  if (state.searchText) params.search = state.searchText;

  const hasTags = state.activeTagIds && state.activeTagIds.size > 0;
  const hasFolders = state.activeFolders && state.activeFolders.size > 0;

  if (hasTags || hasFolders) {
    if (hasTags) {
      params.tag_id = Array.from(state.activeTagIds);
      params.tag_ids = Array.from(state.activeTagIds).join(',');
    }
    if (hasFolders) {
      params.folder = Array.from(state.activeFolders);
      params.folders = JSON.stringify(Array.from(state.activeFolders));
    }
    params.tag_folder_mode = 'or';
  } else if (state.activeTab && state.activeTab !== 'media') {
    params.tag_category = state.activeTab.toLowerCase();
  }
  if (state.activeAlbumId) params.album_id = state.activeAlbumId;

  // Media type filter (Photos, Videos, Audio)
  if (state.activeMediaType === 'photos') {
    params.media_type = 'photo';
  } else if (state.activeMediaType === 'videos') {
    params.media_type = 'video';
  } else if (state.activeMediaType === 'audio') {
    params.media_type = 'audio';
  } else if (['photo', 'video', 'audio'].includes(state.activeMediaType)) {
    params.media_type = state.activeMediaType;
  }

  // Nav status filters (picks, rejects, not_rejects, unrated)
  if (state.activeStatusFilter === 'picks') {
    params.flag = 1;
  } else if (state.activeStatusFilter === 'rejects') {
    params.flag = -1;
  } else if (state.activeStatusFilter === 'not_rejects') {
    params.not_flag = -1;
  } else if (state.activeStatusFilter === 'unrated') {
    params.rating = 0;
    params.max_rating = 0;
  }

  // Timeline filter in UTC
  if (state.activeTimelinePeriod) {
    const { year, month } = state.activeTimelinePeriod;
    const start = Date.UTC(year, month - 1, 1, 0, 0, 0) / 1000;
    const end = Date.UTC(year, month, 1, 0, 0, 0) / 1000 - 1;
    params.date_from = Math.floor(start);
    params.date_to = Math.floor(end);
  }

  return params;
}

export async function loadMedia(append = false) {
  if (append) {
    return loadMoreMedia();
  }

  // Avoid concurrent non-append and append requests:
  // Abort pending append requests as well as any prior non-append load
  if (loadMoreAbortController) {
    loadMoreAbortController.abort();
    loadMoreAbortController = null;
  }
  if (loadMediaAbortController) {
    loadMediaAbortController.abort();
    loadMediaAbortController = null;
  }

  const abortController = new AbortController();
  loadMediaAbortController = abortController;

  if (state.activeTab && state.activeTab !== 'media' && !state.activeTagId) {
    state.isLoadingMedia = false;
    renderCategoryView(state.activeTab);
    updateFilterLabel();
    return;
  }

  const fetchId = ++currentLoadMediaId;
  state.isLoadingMedia = true;
  state.mediaOffset = 0;

  try {
    if (dom.categoryViewContainer) dom.categoryViewContainer.style.display = 'none';
    if (dom.categoryToolbar) dom.categoryToolbar.style.display = 'none';
    if (dom.contentToolbar) dom.contentToolbar.style.display = 'flex';
    if (dom.gridScrollContainer && state.viewMode !== 'map') dom.gridScrollContainer.style.display = 'block';

    const params = {
      ...buildMediaParams(),
      limit: state.mediaLimit,
      offset: 0
    };

    const res = await api.get('/api/media', params, { signal: abortController.signal });
    if (fetchId !== currentLoadMediaId) return;

    // Deduplicate initial items by ID and normalize schema
    const seenIds = new Set();
    const items = [];
    const rawList = (res && Array.isArray(res.items)) ? res.items : [];
    for (const raw of rawList) {
      const item = normalizeMediaItem(raw);
      if (item && !seenIds.has(item.id)) {
        seenIds.add(item.id);
        items.push(item);
      }
    }

    state.totalCount = (res && Number.isFinite(Number(res.total))) ? Number(res.total) : items.length;
    state.mediaItems = items;
    state.mediaOffset = items.length;

    // Reconcile selection: prune IDs no longer visible
    const visibleIds = new Set(state.mediaItems.map(item => item.id));
    state.selectedIds.forEach(id => {
      if (!visibleIds.has(id)) {
        state.selectedIds.delete(id);
      }
    });
    if (state.lastSelectedId !== null && !visibleIds.has(state.lastSelectedId)) {
      state.lastSelectedId = null;
    }

    // Track all discovered folders across loads so filtering doesn't remove folders from sidebar
    state.mediaItems.forEach(item => {
      if (item.file_path) {
        const lastSlash = Math.max(item.file_path.lastIndexOf('/'), item.file_path.lastIndexOf('\\'));
        if (lastSlash > 0) {
          state.allFolders.add(item.file_path.substring(0, lastSlash));
        }
      }
    });

    invalidateGpsCache();
    previousSelectedIds = new Set(state.selectedIds);

    if (state.viewMode !== 'map') {
      renderGrid();
    }
    if (state.viewMode === 'map' || state.mapInstance) {
      renderMapMarkers();
      renderUnmappedTray();
    }
    renderSidebarFolders();
    updateFilterLabel();
    updateBatchBar();
    updateInspector();
  } catch (err) {
    if (err.name === 'AbortError') return;
    if (fetchId === currentLoadMediaId) {
      console.error('Failed to load media:', err);
      showToast('Failed to load media: ' + (err.message || 'Server error'), 'error');
    }
  } finally {
    if (fetchId === currentLoadMediaId) {
      state.isLoadingMedia = false;
      if (loadMediaAbortController === abortController) {
        loadMediaAbortController = null;
      }
    }
  }
}

export async function loadMoreMedia() {
  // Avoid concurrent non-append and append requests, or multiple append requests
  if (state.isLoadingMore || state.isLoadingMedia || state.mediaItems.length >= state.totalCount) {
    return;
  }

  state.isLoadingMore = true;
  const offset = state.mediaItems.length;

  // Capture filter/sort request identity for pagination
  const requestMediaId = currentLoadMediaId;
  const requestParams = buildMediaParams();
  const requestParamsKey = JSON.stringify(requestParams);

  const abortController = new AbortController();
  loadMoreAbortController = abortController;

  try {
    const res = await api.get('/api/media', {
      ...requestParams,
      limit: state.mediaLimit,
      offset
    }, { signal: abortController.signal });

    // If a non-append loadMedia started or filter/sort identity changed while pending, discard
    if (requestMediaId !== currentLoadMediaId || JSON.stringify(buildMediaParams()) !== requestParamsKey) {
      return;
    }

    // Deduplicate by ID against existing items and within the new page (with schema normalization)
    const existing = new Set(state.mediaItems.map(item => item.id));
    const rawItems = (res && Array.isArray(res.items)) ? res.items : [];
    const newItems = [];
    for (const raw of rawItems) {
      const item = normalizeMediaItem(raw);
      if (item && !existing.has(item.id)) {
        existing.add(item.id);
        newItems.push(item);
      }
    }

    state.mediaItems.push(...newItems);
    state.mediaOffset = state.mediaItems.length;
    state.totalCount = (res && Number.isFinite(Number(res.total))) ? Number(res.total) : state.totalCount;
    if (state.mediaItems.length > state.totalCount) {
      state.totalCount = state.mediaItems.length;
    }

    // Reconcile selection: prune IDs no longer visible
    const visibleIds = new Set(state.mediaItems.map(item => item.id));
    state.selectedIds.forEach(id => {
      if (!visibleIds.has(id)) {
        state.selectedIds.delete(id);
      }
    });
    if (state.lastSelectedId !== null && !visibleIds.has(state.lastSelectedId)) {
      state.lastSelectedId = null;
    }

    // Track all discovered folders across loads so filtering doesn't remove folders from sidebar
    newItems.forEach(item => {
      if (item.file_path) {
        const lastSlash = Math.max(item.file_path.lastIndexOf('/'), item.file_path.lastIndexOf('\\'));
        if (lastSlash > 0) {
          state.allFolders.add(item.file_path.substring(0, lastSlash));
        }
      }
    });

    invalidateGpsCache();
    previousSelectedIds = new Set(state.selectedIds);

    if (state.viewMode !== 'map') {
      appendMediaToGrid(newItems);
    }
    if (state.viewMode === 'map' || state.mapInstance) {
      renderMapMarkers();
      renderUnmappedTray();
    }
    renderSidebarFolders();
    updateFilterLabel();
    updateBatchBar();
    updateInspector();

    return newItems;
  } catch (err) {
    if (err.name === 'AbortError') return;
    if (requestMediaId === currentLoadMediaId) {
      console.error('Failed to load more media:', err);
      showToast('Failed to load more media: ' + (err.message || 'Server error'), 'error');
    }
  } finally {
    state.isLoadingMore = false;
    if (loadMoreAbortController === abortController) {
      loadMoreAbortController = null;
    }
  }
}

export async function loadMetadata() {
  try {
    const [tags, albums, folders, timeline, stats] = await Promise.all([
      api.get('/api/tags'),
      api.get('/api/albums'),
      api.get('/api/folders').catch(err => {
        console.warn('Failed to load folders from server, falling back to discovered folders:', err);
        return Array.from(state.allFolders);
      }),
      api.get('/api/timeline'),
      api.get('/api/stats')
    ]);

    state.tags = Array.isArray(tags) ? tags.map(normalizeTag).filter(Boolean) : [];
    state.albums = Array.isArray(albums) ? albums.map(normalizeAlbum).filter(Boolean) : [];
    state.allFolders = new Set(Array.isArray(folders) ? folders.filter(Boolean) : []);
    if (state.activeFolders) {
      for (const f of Array.from(state.activeFolders)) {
        if (!state.allFolders.has(f)) {
          state.activeFolders.delete(f);
        }
      }
    }
    if (state.activeTagIds) {
      const tagIdSet = new Set(state.tags.map(t => t.id));
      for (const tid of Array.from(state.activeTagIds)) {
        if (!tagIdSet.has(tid)) {
          state.activeTagIds.delete(tid);
        }
      }
    }
    state.timelineData = Array.isArray(timeline) ? timeline.map(normalizeTimelineEntry).filter(Boolean) : [];
    state.stats = normalizeCatalogStats(stats);

    renderSidebarTags();
    renderSidebarAlbums();
    renderSidebarFolders();
    renderTimeline();
    updateSidebarActive();
    if (state.activeTab && state.activeTab !== 'media' && !state.activeTagId) {
      renderCategoryView(state.activeTab);
    }
    if (typeof updateModalTagSuggestions === 'function') {
      updateModalTagSuggestions();
    }
    if (typeof renderModalSearchHelp === 'function' && dom.newTagModal && dom.newTagModal.style.display === 'flex') {
      renderModalSearchHelp();
    }

    if (dom.totalMediaCount) {
      dom.totalMediaCount.textContent = state.stats.total_media || 0;
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
  } catch (err) {
    console.error('Failed to load catalog metadata:', err);
    showToast('Failed to load catalog metadata: ' + (err.message || 'Server error'), 'error');
  }
}

export function renderGrid() {
  if (!dom.mediaGrid) return;

  cardMap.clear();

  if (state.mediaItems.length === 0) {
    dom.mediaGrid.innerHTML = '';
    if (dom.emptyState) dom.emptyState.style.display = 'flex';
    previousSelectedIds.clear();
    return;
  }
  if (dom.emptyState) dom.emptyState.style.display = 'none';

  // Group items by Month & Year in UTC
  const groups = {};
  state.mediaItems.forEach(item => {
    let groupKey = t('undated');
    if (item.date_taken && item.date_taken > 0) {
      const d = new Date(item.date_taken * 1000);
      groupKey = `${getMonthName(d.getUTCMonth() + 1)} ${d.getUTCFullYear()}`;
    }
    if (!groups[groupKey]) groups[groupKey] = [];
    groups[groupKey].push(item);
  });

  const fragment = document.createDocumentFragment();

  Object.keys(groups).forEach(groupTitle => {
    const groupEl = document.createElement('div');
    groupEl.className = 'date-group';
    groupEl.dataset.groupKey = groupTitle;

    const headerEl = document.createElement('div');
    headerEl.className = 'date-header';
    const itemNoun = groups[groupTitle].length === 1 ? t('item_singular') : t('item_plural');
    headerEl.innerHTML = `
      <span>${groupTitle}</span>
      <span class="group-count">${groups[groupTitle].length} ${itemNoun}</span>
    `;
    groupEl.appendChild(headerEl);

    const cardsWrap = document.createElement('div');
    cardsWrap.className = 'group-cards';

    groups[groupTitle].forEach(item => {
      const card = createPhotoCard(item);
      cardMap.set(item.id, card);
      cardsWrap.appendChild(card);
    });

    groupEl.appendChild(cardsWrap);
    fragment.appendChild(groupEl);
  });

  if (state.mediaItems.length < state.totalCount) {
    const moreWrap = document.createElement('div');
    moreWrap.className = 'grid-load-more-wrap';
    moreWrap.style.cssText = 'padding:20px;text-align:center;width:100%;grid-column:1/-1;';
    moreWrap.innerHTML = `
      <div style="color:var(--text-dim);font-size:12px;margin-bottom:8px;">Showing ${state.mediaItems.length} of ${state.totalCount} photos</div>
      <button class="btn btn-secondary btn-sm" id="gridLoadMoreBtn">Load More</button>
    `;
    const btn = moreWrap.querySelector('#gridLoadMoreBtn');
    if (btn) btn.onclick = () => loadMoreMedia();
    fragment.appendChild(moreWrap);
  }

  dom.mediaGrid.innerHTML = '';
  dom.mediaGrid.appendChild(fragment);
  previousSelectedIds = new Set(state.selectedIds);
}

export function appendMediaToGrid(newItems) {
  if (!dom.mediaGrid) return;

  // Remove existing load more wrap
  const existingLoadMore = dom.mediaGrid.querySelector('.grid-load-more-wrap');
  if (existingLoadMore) existingLoadMore.remove();

  if (!newItems || newItems.length === 0) {
    if (state.mediaItems.length < state.totalCount) {
      const moreWrap = document.createElement('div');
      moreWrap.className = 'grid-load-more-wrap';
      moreWrap.style.cssText = 'padding:20px;text-align:center;width:100%;grid-column:1/-1;';
      moreWrap.innerHTML = `
        <div style="color:var(--text-dim);font-size:12px;margin-bottom:8px;">Showing ${state.mediaItems.length} of ${state.totalCount} photos</div>
        <button class="btn btn-secondary btn-sm" id="gridLoadMoreBtn">Load More</button>
      `;
      const btn = moreWrap.querySelector('#gridLoadMoreBtn');
      if (btn) btn.onclick = () => loadMoreMedia();
      dom.mediaGrid.appendChild(moreWrap);
    }
    return;
  }

  if (state.mediaItems.length > 0 && dom.emptyState) {
    dom.emptyState.style.display = 'none';
  }

  (newItems || []).forEach(rawItem => {
    const item = normalizeMediaItem(rawItem) || rawItem;
    if (!item || item.id == null) return;

    // Guard against duplicate card already present in DOM
    if (getCardElement(item.id)) {
      return;
    }

    let groupKey = 'Undated';
    if (item.date_taken && item.date_taken > 0) {
      const d = new Date(item.date_taken * 1000);
      groupKey = `${getMonthName(d.getUTCMonth() + 1)} ${d.getUTCFullYear()}`;
    }

    const itemIdx = state.mediaItems.findIndex(m => m.id === item.id);
    let groupEl = dom.mediaGrid.querySelector(`.date-group[data-group-key="${groupKey}"]`);
    if (!groupEl) {
      groupEl = document.createElement('div');
      groupEl.className = 'date-group';
      groupEl.dataset.groupKey = groupKey;

      const headerEl = document.createElement('div');
      headerEl.className = 'date-header';
      headerEl.innerHTML = `
        <span>${groupKey}</span>
        <span class="group-count">0 ${t('item_plural')}</span>
      `;
      groupEl.appendChild(headerEl);

      const cardsWrap = document.createElement('div');
      cardsWrap.className = 'group-cards';
      groupEl.appendChild(cardsWrap);

      // Reconcile group ordering among existing date-group elements
      const existingGroups = Array.from(dom.mediaGrid.querySelectorAll('.date-group'));
      let nextGroupEl = null;
      for (const grp of existingGroups) {
        const firstCard = grp.querySelector('.photo-card');
        if (firstCard) {
          const firstId = parseInt(firstCard.dataset.id, 10);
          const grpIdx = state.mediaItems.findIndex(m => m.id === firstId);
          if (grpIdx !== -1 && itemIdx !== -1 && grpIdx > itemIdx) {
            nextGroupEl = grp;
            break;
          }
        }
      }

      if (nextGroupEl) {
        dom.mediaGrid.insertBefore(groupEl, nextGroupEl);
      } else {
        dom.mediaGrid.appendChild(groupEl);
      }
    }

    const cardsWrap = groupEl.querySelector('.group-cards');
    if (cardsWrap) {
      const card = createPhotoCard(item);
      cardMap.set(item.id, card);

      // Reconcile card ordering within date group
      let nextCardEl = null;
      for (const existingCard of cardsWrap.children) {
        const existingId = parseInt(existingCard.dataset.id, 10);
        const existingIdx = state.mediaItems.findIndex(m => m.id === existingId);
        if (existingIdx !== -1 && itemIdx !== -1 && existingIdx > itemIdx) {
          nextCardEl = existingCard;
          break;
        }
      }

      if (nextCardEl) {
        cardsWrap.insertBefore(card, nextCardEl);
      } else {
        cardsWrap.appendChild(card);
      }

      const count = cardsWrap.children.length;
      const countEl = groupEl.querySelector('.group-count');
      if (countEl) {
        countEl.textContent = `${count} ${count === 1 ? t('item_singular') : t('item_plural')}`;
      }
    }
  });

  if (state.mediaItems.length < state.totalCount) {
    const moreWrap = document.createElement('div');
    moreWrap.className = 'grid-load-more-wrap';
    moreWrap.style.cssText = 'padding:20px;text-align:center;width:100%;grid-column:1/-1;';
    moreWrap.innerHTML = `
      <div style="color:var(--text-dim);font-size:12px;margin-bottom:8px;">Showing ${state.mediaItems.length} of ${state.totalCount} photos</div>
      <button class="btn btn-secondary btn-sm" id="gridLoadMoreBtn">Load More</button>
    `;
    const btn = moreWrap.querySelector('#gridLoadMoreBtn');
    if (btn) btn.onclick = () => loadMoreMedia();
    dom.mediaGrid.appendChild(moreWrap);
  }
  previousSelectedIds = new Set(state.selectedIds);
}

export function createPhotoCard(item) {
  const card = document.createElement('div');
  card.className = 'photo-card' + (state.selectedIds.has(item.id) ? ' selected' : '');
  card.dataset.id = item.id;
  card.dataset.mediaType = item.media_type || 'photo';

  // Thumbnail URL with fallback
  const thumbUrl = item.content_hash
    ? `/api/thumbnails/${encodeURIComponent(item.content_hash)}/256`
    : getOriginalMediaUrl(item);

  const flagBadge = item.flag === 1
    ? '<span class="flag-badge pick" title="Pick">✔</span>'
    : item.flag === -1
      ? '<span class="flag-badge reject" title="Reject">✖</span>'
      : '';

  let mediaBadge = '';
  if (item.media_type === 'video') {
    const durStr = formatDuration(item.duration);
    mediaBadge = `<span class="media-type-badge video-badge" title="Video (${durStr})"><span class="badge-icon">▶</span> ${durStr}</span>`;
  } else if (item.media_type === 'audio') {
    const durStr = item.duration > 0 ? formatDuration(item.duration) : '';
    mediaBadge = `<span class="media-type-badge audio-badge" title="Audio${durStr ? ` (${durStr})` : ''}"><span class="badge-icon">🎵</span> ${durStr}</span>`;
  }

  // Star rating HTML
  let starsHtml = '';
  for (let s = 1; s <= 5; s++) {
    const activeClass = s <= (item.rating || 0) ? 'active' : '';
    starsHtml += `<span class="${activeClass}" data-star="${s}">★</span>`;
  }

  const safeFileName = escapeHtml(item.file_name);
  card.innerHTML = `
    <div class="photo-thumb-wrap">
      <img src="${thumbUrl}" alt="${safeFileName}" loading="lazy" onerror="this.onerror=null;this.src='/api/photos/${item.id}/original';">
      <div class="card-badges">
        ${flagBadge}
        ${mediaBadge}
      </div>
    </div>
    <div class="card-info">
      <div class="card-filename" title="${safeFileName}">${safeFileName}</div>
      <div class="card-footer">
        <span>${formatDate(item.date_taken)}</span>
        <div class="star-rating card-stars" data-id="${item.id}">
          ${starsHtml}
        </div>
      </div>
    </div>
  `;

  return card;
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

  // Update selection classes in DOM only for changed items
  for (const oldId of previousSelectedIds) {
    if (!state.selectedIds.has(oldId)) {
      const c = getCardElement(oldId);
      if (c) c.classList.remove('selected');
    }
  }
  for (const newId of state.selectedIds) {
    if (!previousSelectedIds.has(newId)) {
      const c = getCardElement(newId);
      if (c) c.classList.add('selected');
    }
  }
  previousSelectedIds = new Set(state.selectedIds);

  updateBatchBar();
  updateInspector();
  updateMapMarkerSelections();
}

export function updateBatchBar() {
  if (!dom.batchActionBar) return;
  const count = state.selectedIds.size;
  const sortSelector = document.querySelector('.sort-selector');
  if (count > 1) {
    dom.batchActionBar.style.display = 'flex';
    if (dom.batchSelectedCount) dom.batchSelectedCount.textContent = t('n_selected', { count });
    if (sortSelector) sortSelector.style.display = 'none';
  } else {
    dom.batchActionBar.style.display = 'none';
    if (sortSelector) sortSelector.style.display = '';
  }
}

export function getSidebarSelectableItems() {
  const items = [];
  const catContainers = [
    dom.tagCategoryPeople,
    dom.tagCategoryPlaces,
    dom.tagCategoryEvents,
    dom.tagCategoryKeyword
  ];
  catContainers.forEach(container => {
    if (!container) return;
    const lis = container.querySelectorAll('.tag-item');
    lis.forEach(li => {
      const tid = Number(li.dataset.tagId);
      if (Number.isFinite(tid)) {
        items.push({ type: 'tag', id: tid, element: li });
      }
    });
  });

  if (dom.foldersTree) {
    const divs = dom.foldersTree.querySelectorAll('.folder-item');
    divs.forEach(div => {
      const f = div.dataset.folderPath;
      if (f) {
        items.push({ type: 'folder', path: f, element: div });
      }
    });
  }

  return items;
}

export function handleSidebarItemClick(itemType, itemValue, e) {
  const isCtrl = Boolean(e && (e.ctrlKey || e.metaKey));
  const isShift = Boolean(e && e.shiftKey);

  // If on category drilldown view, return to media view
  if (state.activeTab && state.activeTab !== 'media') {
    state.activeTab = 'media';
    saveActiveTabPreference('media');
    if (dom.viewTabs) {
      dom.viewTabs.querySelectorAll('.tab-btn').forEach(btn => {
        btn.classList.toggle('active', btn.dataset.tab === 'media');
      });
    }
  }
  if (dom.categoryViewContainer) dom.categoryViewContainer.style.display = 'none';
  if (dom.categoryToolbar) dom.categoryToolbar.style.display = 'none';
  if (dom.contentToolbar) dom.contentToolbar.style.display = 'flex';
  if (dom.categoryBackBtn) dom.categoryBackBtn.style.display = 'none';
  if (dom.gridScrollContainer && state.viewMode !== 'map') dom.gridScrollContainer.style.display = 'block';

  state.activeTimelinePeriod = null;

  if (isShift) {
    const allItems = getSidebarSelectableItems();
    const currIdx = allItems.findIndex(it => (
      it.type === itemType && (itemType === 'tag' ? it.id === itemValue : it.path === itemValue)
    ));
    const anchor = state.lastSidebarClickedItem;
    let anchorIdx = -1;
    if (anchor) {
      anchorIdx = allItems.findIndex(it => (
        it.type === anchor.type && (anchor.type === 'tag' ? it.id === anchor.id : it.path === anchor.path)
      ));
    }

    if (currIdx !== -1) {
      const start = (anchorIdx !== -1) ? Math.min(anchorIdx, currIdx) : currIdx;
      const end = (anchorIdx !== -1) ? Math.max(anchorIdx, currIdx) : currIdx;

      if (!isCtrl) {
        state.activeTagIds.clear();
        state.activeFolders.clear();
      }

      for (let i = start; i <= end; i++) {
        const it = allItems[i];
        if (it.type === 'tag') state.activeTagIds.add(it.id);
        else if (it.type === 'folder') state.activeFolders.add(it.path);
      }

      if (!anchor || anchorIdx === -1) {
        state.lastSidebarClickedItem = {
          type: itemType,
          id: itemType === 'tag' ? itemValue : undefined,
          path: itemType === 'folder' ? itemValue : undefined
        };
      }
    }
  } else if (isCtrl) {
    if (itemType === 'tag') {
      if (state.activeTagIds.has(itemValue)) {
        state.activeTagIds.delete(itemValue);
      } else {
        state.activeTagIds.add(itemValue);
      }
      state.lastSidebarClickedItem = { type: 'tag', id: itemValue };
    } else if (itemType === 'folder') {
      if (state.activeFolders.has(itemValue)) {
        state.activeFolders.delete(itemValue);
      } else {
        state.activeFolders.add(itemValue);
      }
      state.lastSidebarClickedItem = { type: 'folder', path: itemValue };
    }
  } else {
    // Normal single click
    const isOnlyTag = (itemType === 'tag' && state.activeTagIds.size === 1 && state.activeTagIds.has(itemValue) && state.activeFolders.size === 0);
    const isOnlyFolder = (itemType === 'folder' && state.activeFolders.size === 1 && state.activeFolders.has(itemValue) && state.activeTagIds.size === 0);

    if (isOnlyTag || isOnlyFolder) {
      // Toggle off
      state.activeTagIds.clear();
      state.activeFolders.clear();
      state.lastSidebarClickedItem = null;
    } else {
      state.activeTagIds.clear();
      state.activeFolders.clear();
      if (itemType === 'tag') {
        state.activeTagIds.add(itemValue);
        state.lastSidebarClickedItem = { type: 'tag', id: itemValue };
      } else {
        state.activeFolders.add(itemValue);
        state.lastSidebarClickedItem = { type: 'folder', path: itemValue };
      }
    }
  }

  saveActiveFoldersPreference(state.activeFolders);
  saveActiveTagIdsPreference(state.activeTagIds);
  updateSidebarActive();
  renderTimeline();
  loadMedia();
}

export function expandTagCategory(category, save = true) {
  const cat = (category || 'keyword').toLowerCase();
  const group = document.querySelector(`.tag-category-group[data-category="${cat}"]`);
  if (!group) return;
  const items = group.querySelector('.tag-items');
  const arrow = group.querySelector('.arrow');
  if (items) {
    items.style.display = 'block';
    if (arrow) arrow.textContent = '▼';
    const hdr = group.querySelector('.category-header');
    if (hdr) hdr.setAttribute('aria-expanded', 'true');
    if (save) saveTagCategoryCollapsedState(cat, false);
  }
}

export function collapseTagCategory(category, save = true) {
  const cat = (category || 'keyword').toLowerCase();
  const group = document.querySelector(`.tag-category-group[data-category="${cat}"]`);
  if (!group) return;
  const items = group.querySelector('.tag-items');
  const arrow = group.querySelector('.arrow');
  if (items) {
    items.style.display = 'none';
    if (arrow) arrow.textContent = '▶';
    const hdr = group.querySelector('.category-header');
    if (hdr) hdr.setAttribute('aria-expanded', 'false');
    if (save) saveTagCategoryCollapsedState(cat, true);
  }
}

export function expandFolders(save = true) {
  if (dom.foldersTree) {
    dom.foldersTree.style.display = 'block';
    const arrow = dom.foldersHeader?.querySelector('.arrow') || dom.foldersArrow;
    if (arrow) arrow.textContent = '▼';
    if (dom.foldersHeader) dom.foldersHeader.setAttribute('aria-expanded', 'true');
    if (save) saveFoldersCollapsedState(false);
  }
}

export function collapseFolders(save = true) {
  if (dom.foldersTree) {
    dom.foldersTree.style.display = 'none';
    const arrow = dom.foldersHeader?.querySelector('.arrow') || dom.foldersArrow;
    if (arrow) arrow.textContent = '▶';
    if (dom.foldersHeader) dom.foldersHeader.setAttribute('aria-expanded', 'false');
    if (save) saveFoldersCollapsedState(true);
  }
}

export function renderSidebarTags() {
  const categories = {
    people: dom.tagCategoryPeople,
    places: dom.tagCategoryPlaces,
    events: dom.tagCategoryEvents,
    keyword: dom.tagCategoryKeyword
  };

  Object.values(categories).forEach(el => {
    if (el) el.innerHTML = '';
  });

  if (dom.tagSuggestions) {
    dom.tagSuggestions.innerHTML = '';
    state.tags.forEach(tag => {
      const opt = document.createElement('option');
      opt.value = tag.name;
      opt.label = `${tag.name} (${tag.category || 'keyword'})`;
      dom.tagSuggestions.appendChild(opt);
    });
  }

  state.tags.forEach(tag => {
    const cat = (tag.category || 'keyword').toLowerCase();
    const container = categories[cat] || categories.keyword;
    if (!container) return;

    const li = document.createElement('li');
    li.className = 'tag-item' + (state.activeTagIds.has(tag.id) ? ' active' : '');
    li.dataset.tagId = String(tag.id);
    const safeTagName = escapeHtml(tag.name);
    li.innerHTML = `
      <span class="tag-name">${safeTagName}</span>
      <span class="count-badge">${tag.media_count || 0}</span>
      <button class="delete-tag-btn" title="Delete tag">&times;</button>
    `;
    li.addEventListener('click', (e) => {
      handleSidebarItemClick('tag', tag.id, e);
    });

    const delBtn = li.querySelector('.delete-tag-btn');
    if (delBtn) {
      delBtn.addEventListener('click', (e) => {
        e.stopPropagation();
        openDeleteTagModal(tag);
      });
    }

    container.appendChild(li);
  });
}

export function renderSidebarAlbums() {
  if (!dom.albumsList) return;
  dom.albumsList.innerHTML = '';

  if (state.albums.length === 0) {
    dom.albumsList.innerHTML = '<li class="menu-item" style="color:var(--text-dim);font-style:italic;">No albums</li>';
    return;
  }

  state.albums.forEach(album => {
    const li = document.createElement('li');
    li.className = 'menu-item' + (state.activeAlbumId === album.id ? ' active' : '');
    const safeAlbumName = escapeHtml(album.name);
    li.innerHTML = `
      <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2"><path d="M4 19.5A2.5 2.5 0 0 1 6.5 17H20"/><path d="M6.5 2H20v20H6.5A2.5 2.5 0 0 1 4 19.5v-15A2.5 2.5 0 0 1 6.5 2z"/></svg>
      <span class="album-name">${safeAlbumName}</span>
      <span class="count-badge">${album.item_count || 0}</span>
      <button class="delete-album-btn" title="Delete album">&times;</button>
    `;
    li.addEventListener('click', () => {
      if (state.activeAlbumId === album.id) {
        state.activeAlbumId = null;
      } else {
        state.activeAlbumId = album.id;
        state.activeFolders.clear();
        state.activeTimelinePeriod = null;
      }
      updateSidebarActive();
      renderTimeline();
      loadMedia();
    });

    const delBtn = li.querySelector('.delete-album-btn');
    if (delBtn) {
      delBtn.addEventListener('click', async (e) => {
        e.stopPropagation();
        if (!window.confirm(`Are you sure you want to delete album "${album.name}"?`)) {
          return;
        }
        try {
          await api.del(`/api/albums/${album.id}`);
          if (state.activeAlbumId === album.id) {
            state.activeAlbumId = null;
          }
          await loadMetadata();
          loadMedia();
          showToast(`Album "${album.name}" deleted.`, 'info');
        } catch (err) {
          console.error('Failed to delete album:', err);
          showToast('Failed to delete album: ' + (err.message || 'Server error'), 'error');
        }
      });
    }

    dom.albumsList.appendChild(li);
  });
}

export function renderSidebarFolders() {
  if (!dom.foldersTree) return;
  dom.foldersTree.innerHTML = '';

  // If allFolders is empty, populate from current media items (fallback)
  if (state.allFolders.size === 0 && state.mediaItems && state.mediaItems.length > 0) {
    state.mediaItems.forEach(item => {
      if (item.file_path) {
        const lastSlash = Math.max(item.file_path.lastIndexOf('/'), item.file_path.lastIndexOf('\\'));
        if (lastSlash > 0) {
          state.allFolders.add(item.file_path.substring(0, lastSlash));
        }
      }
    });
  }

  if (state.allFolders.size === 0) {
    dom.foldersTree.innerHTML = '<div style="color:var(--text-dim);font-style:italic;">No folders</div>';
    return;
  }

  const sortedFolders = Array.from(state.allFolders).sort();
  sortedFolders.forEach(folder => {
    const el = document.createElement('div');
    el.className = 'folder-item' + (state.activeFolders.has(folder) ? ' active' : '');
    el.dataset.folderPath = folder;
    const parts = folder.split(/[\/\\]/);
    const shortName = parts[parts.length - 1] || folder;

    el.innerHTML = `
      <svg class="folder-icon" viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/></svg>
      <span class="folder-name" title="${escapeHtml(folder)}">${escapeHtml(shortName)}</span>
    `;
    el.addEventListener('click', (e) => {
      handleSidebarItemClick('folder', folder, e);
    });
    dom.foldersTree.appendChild(el);
  });
}

export function renderTimeline() {
  if (!dom.timelineContainer) return;
  dom.timelineContainer.innerHTML = '';

  if (state.timelineData.length === 0) {
    dom.timelineContainer.innerHTML = `<span style="color:var(--text-dim);font-size:10px;align-self:center;">${t('no_timeline_data')}</span>`;
    return;
  }

  const entries = [...state.timelineData].sort((a, b) => b.year - a.year || b.month - a.month);
  const maxCount = Math.max(...entries.map(t => t.count), 1);

  let activeWrap = null;

  entries.forEach(entry => {
    const wrap = document.createElement('div');
    wrap.className = 'timeline-bar-wrap';
    const isAct = state.activeTimelinePeriod &&
      state.activeTimelinePeriod.year === entry.year &&
      state.activeTimelinePeriod.month === entry.month;
    if (isAct) {
      wrap.classList.add('active');
      activeWrap = wrap;
    }

    const heightPercent = Math.max(10, Math.round((entry.count / maxCount) * 100));
    const monthAbbr = getShortMonthName(entry.month);
    const itemNoun = entry.count === 1 ? t('photo_singular') : t('photo_plural');
    const title = `${monthAbbr} ${entry.year}: ${entry.count} ${itemNoun}`;

    wrap.title = title;
    wrap.innerHTML = `
      <div class="timeline-bar" style="height: ${heightPercent}%;"></div>
      <span class="timeline-tick-label">${monthAbbr} '${String(entry.year).slice(-2)}</span>
    `;

    wrap.addEventListener('click', () => {
      if (isAct) {
        state.activeTimelinePeriod = null;
      } else {
        state.activeTimelinePeriod = { year: entry.year, month: entry.month };
      }
      renderTimeline();
      loadMedia();
    });

    dom.timelineContainer.appendChild(wrap);
  });

  if (activeWrap) {
    activeWrap.scrollIntoView({ block: 'nearest', inline: 'nearest' });
  }
}

export function updateSidebarActive() {
  const hasTag = state.activeTagIds && state.activeTagIds.size > 0;
  const hasFolder = state.activeFolders && state.activeFolders.size > 0;
  const isAllMedia = (!state.activeMediaType || state.activeMediaType === 'all')
    && !state.activeStatusFilter
    && !hasTag
    && !state.activeAlbumId
    && !hasFolder
    && !state.searchText
    && (!state.activeTab || state.activeTab === 'media')
    && !state.activeTimelinePeriod;

  if (dom.navAllMedia) dom.navAllMedia.classList.toggle('active', isAllMedia);
  if (dom.navPhotos) dom.navPhotos.classList.toggle('active', state.activeMediaType === 'photos');
  if (dom.navVideos) dom.navVideos.classList.toggle('active', state.activeMediaType === 'videos');
  if (dom.navAudio) dom.navAudio.classList.toggle('active', state.activeMediaType === 'audio');
  if (dom.navPicks) dom.navPicks.classList.toggle('active', state.activeStatusFilter === 'picks');
  if (dom.navRejects) dom.navRejects.classList.toggle('active', state.activeStatusFilter === 'rejects');
  if (dom.navNotRejects) dom.navNotRejects.classList.toggle('active', state.activeStatusFilter === 'not_rejects');
  if (dom.navUnrated) dom.navUnrated.classList.toggle('active', state.activeStatusFilter === 'unrated');
  renderSidebarTags();
  renderSidebarAlbums();
  if (dom.foldersTree) {
    dom.foldersTree.querySelectorAll('.folder-item').forEach(el => {
      const folderPath = el.dataset.folderPath || (el.querySelector('span')?.getAttribute('title') || '');
      el.classList.toggle('active', Boolean(folderPath && state.activeFolders && state.activeFolders.has(folderPath)));
    });
  }
  if (hasTag && state.tags) {
    state.tags.forEach(t => {
      if (state.activeTagIds.has(t.id)) {
        expandTagCategory(t.category);
      }
    });
  }
  if (hasFolder) {
    expandFolders();
  }
}

export function updateFilterLabel() {
  if (state.activeTab && state.activeTab !== 'media') {
    if (state.activeTagId) {
      if (dom.categoryBackBtn) {
        dom.categoryBackBtn.style.display = 'inline-flex';
        const tabKey = `tab_${state.activeTab}`;
        const tabName = t(tabKey);
        if (dom.categoryBackBtnLabel) {
          dom.categoryBackBtnLabel.textContent = t('back_to_tab', { tab: tabName });
        }
      }
    } else {
      if (dom.categoryBackBtn) {
        dom.categoryBackBtn.style.display = 'none';
      }
      const tabKey = `tab_${state.activeTab}`;
      const tabName = t(tabKey);
      const catTags = (state.tags || []).filter(t => (t.category || 'keyword').toLowerCase() === state.activeTab.toLowerCase());
      const singularKey = state.activeTab === 'people' ? 'person_singular' : state.activeTab === 'places' ? 'place_singular' : 'event_singular';
      const pluralKey = state.activeTab === 'people' ? 'person_plural' : state.activeTab === 'places' ? 'place_plural' : 'event_plural';
      const noun = catTags.length === 1 ? t(singularKey) : t(pluralKey);
      if (dom.filterLabel) {
        dom.filterLabel.innerHTML = `<strong>${escapeHtml(tabName)}</strong> (${catTags.length} ${noun})`;
      }
      if (dom.clearFiltersBtn) {
        dom.clearFiltersBtn.style.display = state.searchText ? 'inline-block' : 'none';
      }
      return;
    }
  } else {
    if (dom.categoryBackBtn) {
      dom.categoryBackBtn.style.display = 'none';
    }
  }

  const parts = [];

  if (state.activeMediaType === 'photos') {
    parts.push(t('nav_photos'));
  } else if (state.activeMediaType === 'videos') {
    parts.push(t('nav_videos'));
  } else if (state.activeMediaType === 'audio') {
    parts.push(t('nav_audio'));
  }

  if (state.activeStatusFilter === 'picks') {
    parts.push(t('nav_picks'));
  } else if (state.activeStatusFilter === 'rejects') {
    parts.push(t('nav_rejects'));
  } else if (state.activeStatusFilter === 'not_rejects') {
    parts.push(t('nav_not_rejects'));
  } else if (state.activeStatusFilter === 'unrated') {
    parts.push(t('unrated_photos'));
  }

  if (state.activeAlbumId) {
    const album = state.albums.find(a => a.id === state.activeAlbumId);
    parts.push(`${t('album')}: ${album ? album.name : state.activeAlbumId}`);
  }

  const tagCount = state.activeTagIds ? state.activeTagIds.size : 0;
  const folderCount = state.activeFolders ? state.activeFolders.size : 0;

  if (tagCount === 1 && folderCount === 0) {
    const tagId = state.activeTagId;
    const tag = state.tags.find(t => t.id === tagId);
    if (state.activeTab && state.activeTab !== 'media') {
      const tabKey = `tab_${state.activeTab}`;
      const tabName = t(tabKey);
      parts.push(`${tabName}: ${tag ? tag.name : tagId}`);
    } else {
      parts.push(`${t('filter_tag')}: ${tag ? tag.name : tagId}`);
    }
  } else if (folderCount === 1 && tagCount === 0) {
    const folder = state.activeFolder;
    const partsFolder = folder.split(/[\/\\]/);
    const folderName = partsFolder[partsFolder.length - 1] || folder;
    parts.push(`${t('filter_folder')}: ${folderName}`);
  } else if (tagCount + folderCount > 1) {
    const orLabels = [];
    state.activeTagIds.forEach(id => {
      const tag = state.tags.find(t => t.id === id);
      orLabels.push(tag ? tag.name : String(id));
    });
    state.activeFolders.forEach(folder => {
      const folderParts = folder.split(/[\/\\]/);
      const shortName = folderParts[folderParts.length - 1] || folder;
      orLabels.push(shortName);
    });
    parts.push(`Filter (OR): ${orLabels.join(', ')}`);
  }

  if (state.searchText) {
    parts.push(`Search: "${state.searchText}"`);
  }

  if (state.activeTimelinePeriod) {
    const { year, month } = state.activeTimelinePeriod;
    parts.push(`${getMonthName(month)} ${year}`);
  }

  const isFiltered = parts.length > 0;
  const label = isFiltered ? parts.join(' • ') : t('all_photos');
  const itemsNoun = state.totalCount === 1 ? t('item_singular') : t('item_plural');

  if (dom.filterLabel) {
    dom.filterLabel.innerHTML = `<strong>${escapeHtml(label)}</strong> (${state.totalCount} ${itemsNoun})`;
  }
  if (dom.clearFiltersBtn) {
    dom.clearFiltersBtn.style.display = isFiltered ? 'inline-block' : 'none';
  }
}

export function clearAllFilters() {
  state.activeMediaType = 'all';
  state.activeStatusFilter = null;
  state.activeTagIds.clear();
  state.activeAlbumId = null;
  state.activeFolders.clear();
  state.lastSidebarClickedItem = null;
  state.activeTimelinePeriod = null;
  state.activeTab = 'media';
  state.searchText = '';
  if (dom.searchInput) dom.searchInput.value = '';
  if (dom.categoryViewContainer) dom.categoryViewContainer.style.display = 'none';
  if (dom.categoryToolbar) dom.categoryToolbar.style.display = 'none';
  if (dom.contentToolbar) dom.contentToolbar.style.display = 'flex';
  if (dom.categoryBackBtn) dom.categoryBackBtn.style.display = 'none';
  if (dom.gridScrollContainer) dom.gridScrollContainer.style.display = 'block';
  if (dom.viewTabs) {
    dom.viewTabs.querySelectorAll('.tab-btn').forEach(b => b.classList.toggle('active', b.dataset.tab === 'media'));
  }
  updateSidebarActive();
  renderTimeline();
  clearFilterPreferences();
  saveActiveTabPreference('media');
  loadMedia();
}

export function patchCardRating(id, rating) {
  const card = getCardElement(id);
  if (card) {
    card.querySelectorAll('.card-stars span').forEach(span => {
      const star = parseInt(span.dataset.star, 10);
      span.classList.toggle('active', star <= (rating || 0));
    });
  }
}

export function patchCardFlag(id, flag) {
  const card = getCardElement(id);
  if (card) {
    const badgeWrap = card.querySelector('.card-badges');
    if (badgeWrap) {
      badgeWrap.innerHTML = flag === 1
        ? '<span class="flag-badge pick" title="Pick">✔</span>'
        : flag === -1
          ? '<span class="flag-badge reject" title="Reject">✖</span>'
          : '';
    }
  }
}

export async function updateItemRating(id, rating) {
  const item = state.mediaItems.find(m => m.id === id);
  const prevRating = item ? (item.rating || 0) : 0;
  if (item) {
    item.rating = rating;
    patchCardRating(id, rating);
    patchInspectorRatingAndFlag(item);
    patchOpenMapPopup(id);
  }
  if (state.loupeIndex >= 0) updateLoupeControls();
  try {
    await api.post(`/api/media/${id}/rating`, { rating });
    loadMetadata();
  } catch (err) {
    console.error('Failed to update rating:', err);
    if (item) {
      item.rating = prevRating;
      patchCardRating(id, prevRating);
      patchInspectorRatingAndFlag(item);
      patchOpenMapPopup(id);
    }
    if (state.loupeIndex >= 0) updateLoupeControls();
    showToast('Failed to update rating: ' + (err.message || 'Server error'), 'error');
  }
}

export async function updateItemFlag(id, flag) {
  const item = state.mediaItems.find(m => m.id === id);
  const prevFlag = item ? (item.flag || 0) : 0;
  if (item) {
    item.flag = flag;
    patchCardFlag(id, flag);
    patchInspectorRatingAndFlag(item);
    patchOpenMapPopup(id);
  }
  if (state.loupeIndex >= 0) updateLoupeControls();
  try {
    await api.post(`/api/media/${id}/flag`, { flag });
    loadMetadata();
  } catch (err) {
    console.error('Failed to update flag:', err);
    if (item) {
      item.flag = prevFlag;
      patchCardFlag(id, prevFlag);
      patchInspectorRatingAndFlag(item);
      patchOpenMapPopup(id);
    }
    if (state.loupeIndex >= 0) updateLoupeControls();
    showToast('Failed to update flag: ' + (err.message || 'Server error'), 'error');
  }
}

export async function batchUpdateFlags(ids, flag) {
  if (!ids || ids.length === 0) return;
  const prevFlags = new Map();
  ids.forEach(id => {
    const item = state.mediaItems.find(m => m.id === id);
    if (item) {
      prevFlags.set(id, item.flag || 0);
      item.flag = flag;
      patchCardFlag(id, flag);
      patchInspectorRatingAndFlag(item);
      patchOpenMapPopup(id);
    }
  });
  if (state.loupeIndex >= 0) updateLoupeControls();
  try {
    await api.post('/api/media/batch-flag', { ids, flag });
    loadMetadata();
  } catch (err) {
    console.error('Failed to batch update flags:', err);
    prevFlags.forEach((oldFlag, id) => {
      const item = state.mediaItems.find(m => m.id === id);
      if (item) {
        item.flag = oldFlag;
        patchCardFlag(id, oldFlag);
        patchInspectorRatingAndFlag(item);
        patchOpenMapPopup(id);
      }
    });
    if (state.loupeIndex >= 0) updateLoupeControls();
    showToast('Failed to batch update flags: ' + (err.message || 'Server error'), 'error');
  }
}

export async function batchUpdateRatings(ids, rating) {
  if (!ids || ids.length === 0) return;
  const prevRatings = new Map();
  ids.forEach(id => {
    const item = state.mediaItems.find(m => m.id === id);
    if (item) {
      prevRatings.set(id, item.rating || 0);
      item.rating = rating;
      patchCardRating(id, rating);
      patchInspectorRatingAndFlag(item);
      patchOpenMapPopup(id);
    }
  });
  if (state.loupeIndex >= 0) updateLoupeControls();
  try {
    await api.post('/api/media/batch-rating', { ids, rating });
    loadMetadata();
  } catch (err) {
    console.error('Failed to batch update ratings:', err);
    prevRatings.forEach((oldRating, id) => {
      const item = state.mediaItems.find(m => m.id === id);
      if (item) {
        item.rating = oldRating;
        patchCardRating(id, oldRating);
        patchInspectorRatingAndFlag(item);
        patchOpenMapPopup(id);
      }
    });
    if (state.loupeIndex >= 0) updateLoupeControls();
    showToast('Failed to batch update ratings: ' + (err.message || 'Server error'), 'error');
  }
}

export function toggleFlagValue(currentFlag, targetFlag) {
  return currentFlag === targetFlag ? 0 : targetFlag;
}

export function toggleItemFlag(id, targetFlag) {
  const item = state.mediaItems.find(m => m.id === id);
  const current = item ? item.flag : 0;
  return updateItemFlag(id, toggleFlagValue(current, targetFlag));
}

export function toggleFlagsForIds(ids, targetFlag) {
  if (!ids || ids.length === 0) return;
  const items = ids.map(id => state.mediaItems.find(m => m.id === id)).filter(Boolean);
  const allHave = items.length > 0 && items.every(m => m.flag === targetFlag);
  return batchUpdateFlags(ids, allHave ? 0 : targetFlag);
}

export async function batchUpdateDates(updates) {
  if (!updates || updates.length === 0) return;
  const prevDates = new Map();
  updates.forEach(({ id, date_taken, date_taken_str }) => {
    const item = state.mediaItems.find(m => m.id === id);
    if (item) {
      prevDates.set(id, {
        date_taken: item.date_taken,
        exif_date_taken: item.exif?.date_taken,
        date_taken_str: item.exif?.date_taken_str
      });
      item.date_taken = date_taken;
      if (item.exif) {
        item.exif.date_taken = date_taken;
        if (date_taken_str) item.exif.date_taken_str = date_taken_str;
      }
    }
  });

  try {
    try {
      await api.post('/api/media/batch-date', { items: updates });
    } catch (batchErr) {
      console.warn('Batch date API call failed, falling back to individual date calls:', batchErr);
      await Promise.all(updates.map(u => api.post(`/api/media/${u.id}/date`, { date_taken: u.date_taken })));
    }
  } catch (err) {
    console.error('Failed to batch update dates:', err);
    prevDates.forEach((old, id) => {
      const item = state.mediaItems.find(m => m.id === id);
      if (item) {
        item.date_taken = old.date_taken;
        if (item.exif) {
          item.exif.date_taken = old.exif_date_taken;
          item.exif.date_taken_str = old.date_taken_str;
        }
      }
    });
    throw err;
  }
}
