/**
 * IMAGINE Photo Organizer - Media Grid, Sidebar & Mutations
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

import {
  api,
  escapeHtml,
  formatDate,
  numeric,
  normalizeMediaItem,
  normalizeTag,
  normalizeAlbum,
  normalizeTimelineEntry,
  normalizeCatalogStats,
  getMonthName
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
import { updateModalTagSuggestions, renderModalSearchHelp } from './modals.js';

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
  if (state.activeFolder) params.folder = state.activeFolder;
  if (state.activeTagId) {
    params.tag_id = state.activeTagId;
  } else if (state.activeTab && state.activeTab !== 'media') {
    params.tag_category = state.activeTab.toLowerCase();
  }
  if (state.activeAlbumId) params.album_id = state.activeAlbumId;

  // Nav filters (picks, rejects, unrated)
  if (state.activeNavFilter === 'picks') params.flag = 1;
  else if (state.activeNavFilter === 'rejects') params.flag = -1;
  else if (state.activeNavFilter === 'unrated') {
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

  const fetchId = ++currentLoadMediaId;
  state.isLoadingMedia = true;
  state.mediaOffset = 0;

  try {
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
    const [tags, albums, timeline, stats] = await Promise.all([
      api.get('/api/tags'),
      api.get('/api/albums'),
      api.get('/api/timeline'),
      api.get('/api/stats')
    ]);

    state.tags = Array.isArray(tags) ? tags.map(normalizeTag).filter(Boolean) : [];
    state.albums = Array.isArray(albums) ? albums.map(normalizeAlbum).filter(Boolean) : [];
    state.timelineData = Array.isArray(timeline) ? timeline.map(normalizeTimelineEntry).filter(Boolean) : [];
    state.stats = normalizeCatalogStats(stats);

    renderSidebarTags();
    renderSidebarAlbums();
    renderSidebarFolders();
    renderTimeline();
    if (typeof updateModalTagSuggestions === 'function') {
      updateModalTagSuggestions();
    }
    if (typeof renderModalSearchHelp === 'function' && dom.newTagModal && dom.newTagModal.style.display === 'flex') {
      renderModalSearchHelp();
    }

    if (dom.totalMediaCount) {
      dom.totalMediaCount.textContent = state.stats.total_media || 0;
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
    let groupKey = 'Undated';
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
    headerEl.innerHTML = `
      <span>${groupTitle}</span>
      <span class="group-count">${groups[groupTitle].length} ${groups[groupTitle].length === 1 ? 'item' : 'items'}</span>
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
        <span class="group-count">0 items</span>
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
        countEl.textContent = `${count} ${count === 1 ? 'item' : 'items'}`;
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

  // Thumbnail URL with fallback
  const thumbUrl = item.content_hash
    ? `/api/thumbnails/${encodeURIComponent(item.content_hash)}/256`
    : `/api/photos/${item.id}/original`;

  const flagBadge = item.flag === 1
    ? '<span class="flag-badge pick" title="Pick">✔</span>'
    : item.flag === -1
      ? '<span class="flag-badge reject" title="Reject">✖</span>'
      : '';

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
  if (count > 1) {
    dom.batchActionBar.style.display = 'flex';
    if (dom.batchSelectedCount) dom.batchSelectedCount.textContent = `${count} selected`;
  } else {
    dom.batchActionBar.style.display = 'none';
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
    li.className = 'tag-item' + (state.activeTagId === tag.id ? ' active' : '');
    const safeTagName = escapeHtml(tag.name);
    li.innerHTML = `
      <span class="tag-name">${safeTagName}</span>
      <span class="count-badge">${tag.media_count || 0}</span>
      <button class="delete-tag-btn" title="Delete tag">&times;</button>
    `;
    li.addEventListener('click', () => {
      if (state.activeTagId === tag.id) {
        state.activeTagId = null;
      } else {
        state.activeTagId = tag.id;
        state.activeAlbumId = null;
        state.activeNavFilter = 'all';
        state.activeFolder = null;
        state.activeTimelinePeriod = null;
      }
      updateSidebarActive();
      renderTimeline();
      loadMedia();
    });

    const delBtn = li.querySelector('.delete-tag-btn');
    if (delBtn) {
      delBtn.addEventListener('click', async (e) => {
        e.stopPropagation();
        if (!window.confirm(`Are you sure you want to delete tag "${tag.name}"?`)) {
          return;
        }
        try {
          await api.del(`/api/tags/${tag.id}`);
          if (state.activeTagId === tag.id) {
            state.activeTagId = null;
          }
          await loadMetadata();
          loadMedia();
          updateInspector();
          showToast(`Tag "${tag.name}" deleted.`, 'info');
        } catch (err) {
          console.error('Failed to delete tag:', err);
          showToast('Failed to delete tag: ' + (err.message || 'Server error'), 'error');
        }
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
        state.activeTagId = null;
        state.activeNavFilter = 'all';
        state.activeFolder = null;
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

  // If allFolders is empty, populate from current media items
  if (state.allFolders.size === 0) {
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
    el.className = 'folder-item' + (state.activeFolder === folder ? ' active' : '');
    const parts = folder.split(/[\/\\]/);
    const shortName = parts[parts.length - 1] || folder;

    el.innerHTML = `
      <svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/></svg>
      <span title="${escapeHtml(folder)}">${escapeHtml(shortName)}</span>
    `;
    el.addEventListener('click', () => {
      if (state.activeFolder === folder) {
        state.activeFolder = null;
      } else {
        state.activeFolder = folder;
        state.activeTagId = null;
        state.activeAlbumId = null;
        state.activeNavFilter = 'all';
        state.activeTimelinePeriod = null;
        state.searchText = '';
        if (dom.searchInput) dom.searchInput.value = '';
      }
      updateSidebarActive();
      renderTimeline();
      loadMedia();
    });
    dom.foldersTree.appendChild(el);
  });
}

export function renderTimeline() {
  if (!dom.timelineContainer) return;
  dom.timelineContainer.innerHTML = '';

  if (state.timelineData.length === 0) {
    dom.timelineContainer.innerHTML = '<span style="color:var(--text-dim);font-size:10px;align-self:center;">No timeline data</span>';
    return;
  }

  const entries = [...state.timelineData].sort((a, b) => b.year - a.year || b.month - a.month);
  const maxCount = Math.max(...entries.map(t => t.count), 1);

  entries.forEach(entry => {
    const wrap = document.createElement('div');
    wrap.className = 'timeline-bar-wrap';
    const isAct = state.activeTimelinePeriod &&
      state.activeTimelinePeriod.year === entry.year &&
      state.activeTimelinePeriod.month === entry.month;
    if (isAct) wrap.classList.add('active');

    const heightPercent = Math.max(10, Math.round((entry.count / maxCount) * 100));
    const monthAbbr = getMonthName(entry.month).substring(0, 3);
    const title = `${monthAbbr} ${entry.year}: ${entry.count} photos`;

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
}

export function updateSidebarActive() {
  if (dom.navAllMedia) dom.navAllMedia.classList.toggle('active', state.activeNavFilter === 'all' && !state.activeTagId && !state.activeAlbumId && !state.activeFolder && !state.searchText && (!state.activeTab || state.activeTab === 'media'));
  if (dom.navPicks) dom.navPicks.classList.toggle('active', state.activeNavFilter === 'picks');
  if (dom.navRejects) dom.navRejects.classList.toggle('active', state.activeNavFilter === 'rejects');
  if (dom.navUnrated) dom.navUnrated.classList.toggle('active', state.activeNavFilter === 'unrated');
  renderSidebarTags();
  renderSidebarAlbums();
  if (dom.foldersTree) {
    dom.foldersTree.querySelectorAll('.folder-item').forEach(el => {
      const titleSpan = el.querySelector('span');
      const folderPath = titleSpan ? titleSpan.getAttribute('title') : '';
      el.classList.toggle('active', Boolean(state.activeFolder && state.activeFolder === folderPath));
    });
  }
}

export function updateFilterLabel() {
  let label = 'All Photos';
  let isFiltered = false;

  if (state.activeTab && state.activeTab !== 'media' && !state.activeTagId) {
    const tabName = state.activeTab.charAt(0).toUpperCase() + state.activeTab.slice(1);
    label = `Category: ${tabName}`;
    isFiltered = true;
  } else if (state.activeNavFilter === 'picks') {
    label = 'Picks';
    isFiltered = true;
  } else if (state.activeNavFilter === 'rejects') {
    label = 'Rejects';
    isFiltered = true;
  } else if (state.activeNavFilter === 'unrated') {
    label = 'Unrated Photos';
    isFiltered = true;
  } else if (state.activeTagId) {
    const tag = state.tags.find(t => t.id === state.activeTagId);
    label = `Tag: ${tag ? tag.name : state.activeTagId}`;
    isFiltered = true;
  } else if (state.activeAlbumId) {
    const album = state.albums.find(a => a.id === state.activeAlbumId);
    label = `Album: ${album ? album.name : state.activeAlbumId}`;
    isFiltered = true;
  }

  if (state.activeFolder) {
    const parts = state.activeFolder.split(/[\/\\]/);
    const folderName = parts[parts.length - 1] || state.activeFolder;
    label = isFiltered && label !== 'All Photos' ? `${label} • Folder: ${folderName}` : `Folder: ${folderName}`;
    isFiltered = true;
  }
  if (state.searchText) {
    label = isFiltered && label !== 'All Photos' ? `${label} • Search: "${state.searchText}"` : `Search: "${state.searchText}"`;
    isFiltered = true;
  }

  if (state.activeTimelinePeriod) {
    const { year, month } = state.activeTimelinePeriod;
    label += ` • ${getMonthName(month)} ${year}`;
    isFiltered = true;
  }

  if (dom.filterLabel) {
    dom.filterLabel.innerHTML = `<strong>${escapeHtml(label)}</strong> (${state.totalCount} items)`;
  }
  if (dom.clearFiltersBtn) {
    dom.clearFiltersBtn.style.display = isFiltered ? 'inline-block' : 'none';
  }
}

export function clearAllFilters() {
  state.activeNavFilter = 'all';
  state.activeTagId = null;
  state.activeAlbumId = null;
  state.activeFolder = null;
  state.activeTimelinePeriod = null;
  state.activeTab = 'media';
  state.searchText = '';
  if (dom.searchInput) dom.searchInput.value = '';
  if (dom.viewTabs) {
    dom.viewTabs.querySelectorAll('.tab-btn').forEach(b => b.classList.toggle('active', b.dataset.tab === 'media'));
  }
  updateSidebarActive();
  renderTimeline();
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
