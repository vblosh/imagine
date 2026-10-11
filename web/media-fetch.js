/**
 * IMAGINE Photo Organizer - Media Data Fetching & State Normalization
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 * Zero render calls: pure fetch, abort, normalize, state-write.
 */

import {
  api,
  normalizeMediaItem,
  normalizeTag,
  normalizeAlbum,
  normalizeTimelineEntry,
  normalizeCatalogStats,
} from './api.js';
import { state, invalidateGpsCache } from './state.js';
import { showToast } from './dom.js';
import { buildMediaParams, prepareAlbumOrderMode, syncAlbumOrderSortOption, isAlbumOrderMode } from './media-params.js';
import { performSemanticSearch } from './semantic-search.js';

let currentLoadMediaId = 0;
let loadMediaAbortController = null;
let loadMoreAbortController = null;

export function getCurrentLoadMediaId() {
  return currentLoadMediaId;
}

export function abortCurrentMediaLoad() {
  if (loadMediaAbortController) {
    loadMediaAbortController.abort();
    loadMediaAbortController = null;
  }
}

export function abortCurrentLoadMore() {
  if (loadMoreAbortController) {
    loadMoreAbortController.abort();
    loadMoreAbortController = null;
  }
}

export async function loadMediaData(append = false, { preserveScroll = false } = {}) {
  let isAppend = append;
  let shouldPreserveScroll = preserveScroll;
  if (typeof append === 'object' && append !== null) {
    isAppend = Boolean(append.append);
    shouldPreserveScroll = Boolean(append.preserveScroll);
  } else {
    isAppend = Boolean(append);
  }

  syncAlbumOrderSortOption();
  prepareAlbumOrderMode();

  if (isAppend) {
    return loadMoreMediaData();
  }

  abortCurrentLoadMore();
  abortCurrentMediaLoad();

  const abortController = new AbortController();
  loadMediaAbortController = abortController;

  if (state.activeTab && state.activeTab !== 'media' && !state.activeTagId && !state.activeAlbumId) {
    state.isLoadingMedia = false;
    return { isCategoryView: true, tab: state.activeTab };
  }

  const fetchId = ++currentLoadMediaId;
  state.isLoadingMedia = true;
  state.mediaOffset = 0;

  try {
    const albumOrderMode = isAlbumOrderMode();
    const isSemantic = Boolean(
      state.semanticSearchEnabled && state.searchText && state.semanticSearchAvailable
    );
    const pageLimit = albumOrderMode ? 1000 : isSemantic ? 100 : state.mediaLimit;
    const params = {
      ...buildMediaParams(),
      limit: pageLimit,
      offset: 0,
    };

    let res;
    if (isSemantic) {
      if (!state.semanticSearchActive) {
        state.semanticSearchSnapshot = {
          activeTab: state.activeTab,
          activeAlbumId: state.activeAlbumId,
          activeTagId: state.activeTagId,
          activeTagIds: new Set(state.activeTagIds),
          activeFolders: new Set(state.activeFolders),
          activeMediaType: state.activeMediaType,
          activeStatusFilter: state.activeStatusFilter,
          activeTimelinePeriod: state.activeTimelinePeriod ? { ...state.activeTimelinePeriod } : null,
          sortBy: state.sortBy,
          sortDesc: state.sortDesc,
        };
      }
      state.semanticSearchActive = true;
      state.semanticSearchQuery = state.searchText;
      res = await performSemanticSearch(state.searchText, pageLimit, {
        signal: abortController.signal,
      });
    } else {
      if (state.semanticSearchActive) {
        state.semanticSearchActive = false;
        state.semanticSearchQuery = '';
        if (state.semanticSearchSnapshot) {
          const snap = state.semanticSearchSnapshot;
          state.activeTab = snap.activeTab || 'media';
          state.activeAlbumId = snap.activeAlbumId || null;
          state.activeTagId = snap.activeTagId || null;
          state.activeTagIds = new Set(snap.activeTagIds || []);
          state.activeFolders = new Set(snap.activeFolders || []);
          state.activeMediaType = snap.activeMediaType || 'all';
          state.activeStatusFilter = snap.activeStatusFilter || null;
          state.activeTimelinePeriod = snap.activeTimelinePeriod
            ? { ...snap.activeTimelinePeriod }
            : null;
          state.sortBy = snap.sortBy || 'date_taken';
          state.sortDesc = snap.sortDesc !== undefined ? snap.sortDesc : true;
          state.semanticSearchSnapshot = null;
        }
      }
      res = await api.get('/api/media', params, { signal: abortController.signal });
    }
    if (fetchId !== currentLoadMediaId) return null;

    let rawList = [];
    if (isSemantic) {
      rawList =
        res && Array.isArray(res.media_items)
          ? [...res.media_items]
          : res && Array.isArray(res.items)
            ? [...res.items]
            : [];
    } else {
      rawList = res && Array.isArray(res.items) ? [...res.items] : [];
    }
    const reportedTotal = isSemantic
      ? rawList.length
      : res && Number.isFinite(Number(res.total))
        ? Number(res.total)
        : rawList.length;

    while (!isSemantic && albumOrderMode && rawList.length < reportedTotal) {
      const nextPage = await api.get(
        '/api/media',
        {
          ...buildMediaParams(),
          limit: pageLimit,
          offset: rawList.length,
        },
        { signal: abortController.signal }
      );
      if (fetchId !== currentLoadMediaId) return null;
      const nextItems = nextPage && Array.isArray(nextPage.items) ? nextPage.items : [];
      if (nextItems.length === 0) break;
      rawList.push(...nextItems);
    }

    const scoreMap = new Map();
    if (isSemantic && res && Array.isArray(res.items)) {
      res.items.forEach(it => {
        if (it && it.mediaId != null) {
          scoreMap.set(it.mediaId, it.score);
        }
      });
    }

    // Deduplicate initial items by ID and normalize schema
    const seenIds = new Set();
    const items = [];
    for (const raw of rawList) {
      const item = normalizeMediaItem(raw);
      if (item && !seenIds.has(item.id)) {
        seenIds.add(item.id);
        if (isSemantic) {
          item.rank = items.length + 1;
          const score = scoreMap.has(item.id)
            ? scoreMap.get(item.id)
            : (raw.score ?? raw.similarity_score);
          if (score !== undefined) {
            item.similarity_score = score;
          }
        }
        items.push(item);
      }
    }

    state.totalCount = reportedTotal;
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

    return {
      success: true,
      isSemantic,
      items,
      shouldPreserveScroll,
      fetchId,
    };
  } catch (err) {
    if (err.name === 'AbortError') return null;
    if (fetchId === currentLoadMediaId) {
      console.error('Failed to load media:', err);
      showToast('Failed to load media: ' + (err.message || 'Server error'), 'error');
    }
    return null;
  } finally {
    if (fetchId === currentLoadMediaId) {
      state.isLoadingMedia = false;
      if (loadMediaAbortController === abortController) {
        loadMediaAbortController = null;
      }
    }
  }
}

export async function loadMoreMediaData() {
  if (
    state.isLoadingMore ||
    state.isLoadingMedia ||
    state.mediaItems.length >= state.totalCount ||
    state.semanticSearchActive ||
    (state.semanticSearchEnabled && state.searchText && state.semanticSearchAvailable)
  ) {
    return null;
  }

  state.isLoadingMore = true;
  const offset = state.mediaItems.length;

  const requestMediaId = currentLoadMediaId;
  const requestParams = buildMediaParams();
  const requestParamsKey = JSON.stringify(requestParams);

  const abortController = new AbortController();
  loadMoreAbortController = abortController;

  try {
    const res = await api.get(
      '/api/media',
      {
        ...requestParams,
        limit: state.mediaLimit,
        offset,
      },
      { signal: abortController.signal }
    );

    if (
      requestMediaId !== currentLoadMediaId ||
      JSON.stringify(buildMediaParams()) !== requestParamsKey
    ) {
      return null;
    }

    const existing = new Set(state.mediaItems.map(item => item.id));
    const rawItems = res && Array.isArray(res.items) ? res.items : [];
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
    state.totalCount =
      res && Number.isFinite(Number(res.total)) ? Number(res.total) : state.totalCount;
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

    newItems.forEach(item => {
      if (item.file_path) {
        const lastSlash = Math.max(item.file_path.lastIndexOf('/'), item.file_path.lastIndexOf('\\'));
        if (lastSlash > 0) {
          state.allFolders.add(item.file_path.substring(0, lastSlash));
        }
      }
    });

    invalidateGpsCache();

    return {
      success: true,
      newItems,
    };
  } catch (err) {
    if (err.name === 'AbortError') return null;
    if (requestMediaId === currentLoadMediaId) {
      console.error('Failed to load more media:', err);
      showToast('Failed to load more media: ' + (err.message || 'Server error'), 'error');
    }
    return null;
  } finally {
    state.isLoadingMore = false;
    if (loadMoreAbortController === abortController) {
      loadMoreAbortController = null;
    }
  }
}

export async function loadMetadataData() {
  try {
    const [tags, albums, folders, timeline, stats] = await Promise.all([
      api.get('/api/tags'),
      api.get('/api/albums'),
      api.get('/api/folders').catch(err => {
        console.warn('Failed to load folders from server, falling back to discovered folders:', err);
        return Array.from(state.allFolders);
      }),
      api.get('/api/timeline'),
      api.get('/api/stats'),
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
    state.timelineData = Array.isArray(timeline)
      ? timeline.map(normalizeTimelineEntry).filter(Boolean)
      : [];
    state.stats = normalizeCatalogStats(stats);

    return { success: true };
  } catch (err) {
    console.error('Failed to load catalog metadata:', err);
    showToast('Failed to load catalog metadata: ' + (err.message || 'Server error'), 'error');
    return null;
  }
}
