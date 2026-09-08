/**
 * IMAGINE Photo Organizer - UI State Persistence
 * Handles storing and restoring layout preferences (Option A)
 * and navigation/filter state (Option B) across browser sessions.
 */

import { state } from './state.js';
import { dom } from './dom.js';
import { expandTagCategory, collapseTagCategory, expandFolders, collapseFolders } from './media-grid.js';

export const STORAGE_KEYS = {
  FOLDERS_COLLAPSED: 'imagine_folders_collapsed',
  TAG_CATEGORY_COLLAPSED_PREFIX: 'imagine_tag_category_collapsed_',
  THUMB_ZOOM: 'imagine_thumb_zoom',
  VIEW_MODE: 'imagine_view_mode',
  MAIN_SORT: 'imagine_main_sort',
  INSPECTOR_COLLAPSED: 'imagine_inspector_collapsed',
  ACTIVE_TAB: 'imagine_active_tab',
  ACTIVE_NAV_FILTER: 'imagine_active_nav_filter',
  ACTIVE_MEDIA_TYPE: 'imagine_active_media_type',
  ACTIVE_STATUS_FILTER: 'imagine_active_status_filter',
  ACTIVE_FOLDERS: 'imagine_active_folders',
  ACTIVE_TAG_IDS: 'imagine_active_tag_ids',
  SEARCH_TEXT: 'imagine_search_text'
};

function safeGetItem(key) {
  try {
    return localStorage.getItem(key);
  } catch (_) {
    return null;
  }
}

function safeSetItem(key, value) {
  try {
    localStorage.setItem(key, value);
  } catch (_) {}
}

function safeRemoveItem(key) {
  try {
    localStorage.removeItem(key);
  } catch (_) {}
}

// --- Option A: Layout & View Preferences ---

export function saveFoldersCollapsedState(isCollapsed) {
  safeSetItem(STORAGE_KEYS.FOLDERS_COLLAPSED, isCollapsed ? 'true' : 'false');
}

export function saveTagCategoryCollapsedState(category, isCollapsed) {
  if (!category) return;
  const key = `${STORAGE_KEYS.TAG_CATEGORY_COLLAPSED_PREFIX}${category.toLowerCase()}`;
  safeSetItem(key, isCollapsed ? 'true' : 'false');
}

export function saveZoomPreference(zoom) {
  if (zoom !== undefined && zoom !== null) {
    safeSetItem(STORAGE_KEYS.THUMB_ZOOM, String(zoom));
  }
}

export function saveViewModePreference(mode) {
  if (mode) {
    safeSetItem(STORAGE_KEYS.VIEW_MODE, mode);
  }
}

export function saveSortPreference(sortKey) {
  if (sortKey) {
    safeSetItem(STORAGE_KEYS.MAIN_SORT, sortKey);
  }
}

export function saveInspectorCollapsedState(isCollapsed) {
  safeSetItem(STORAGE_KEYS.INSPECTOR_COLLAPSED, isCollapsed ? 'true' : 'false');
}

// --- Option B: Navigation & Filter State ---

export function saveActiveTabPreference(tab) {
  if (tab) {
    safeSetItem(STORAGE_KEYS.ACTIVE_TAB, tab);
  }
}

export function saveNavFilterPreference(filter) {
  if (filter && filter !== 'all') {
    safeSetItem(STORAGE_KEYS.ACTIVE_NAV_FILTER, filter);
  } else {
    safeRemoveItem(STORAGE_KEYS.ACTIVE_NAV_FILTER);
  }

  if (state.activeMediaType && state.activeMediaType !== 'all') {
    safeSetItem(STORAGE_KEYS.ACTIVE_MEDIA_TYPE, state.activeMediaType);
  } else {
    safeRemoveItem(STORAGE_KEYS.ACTIVE_MEDIA_TYPE);
  }

  if (state.activeStatusFilter) {
    safeSetItem(STORAGE_KEYS.ACTIVE_STATUS_FILTER, state.activeStatusFilter);
  } else {
    safeRemoveItem(STORAGE_KEYS.ACTIVE_STATUS_FILTER);
  }
}

export function saveActiveFoldersPreference(foldersSet) {
  if (foldersSet && foldersSet.size > 0) {
    safeSetItem(STORAGE_KEYS.ACTIVE_FOLDERS, JSON.stringify(Array.from(foldersSet)));
  } else {
    safeRemoveItem(STORAGE_KEYS.ACTIVE_FOLDERS);
  }
}

export function saveActiveTagIdsPreference(tagIdsSet) {
  if (tagIdsSet && tagIdsSet.size > 0) {
    safeSetItem(STORAGE_KEYS.ACTIVE_TAG_IDS, JSON.stringify(Array.from(tagIdsSet)));
  } else {
    safeRemoveItem(STORAGE_KEYS.ACTIVE_TAG_IDS);
  }
}

export function saveSearchPreference(text) {
  if (text) {
    safeSetItem(STORAGE_KEYS.SEARCH_TEXT, text);
  } else {
    safeRemoveItem(STORAGE_KEYS.SEARCH_TEXT);
  }
}

export function clearFilterPreferences() {
  safeRemoveItem(STORAGE_KEYS.ACTIVE_NAV_FILTER);
  safeRemoveItem(STORAGE_KEYS.ACTIVE_MEDIA_TYPE);
  safeRemoveItem(STORAGE_KEYS.ACTIVE_STATUS_FILTER);
  safeRemoveItem(STORAGE_KEYS.ACTIVE_FOLDERS);
  safeRemoveItem(STORAGE_KEYS.ACTIVE_TAG_IDS);
  safeRemoveItem(STORAGE_KEYS.SEARCH_TEXT);
}

// --- Restoration ---

export function restoreLayoutPreferences() {
  // 1. Folders collapse state (default is collapsed)
  const foldersCollapsed = safeGetItem(STORAGE_KEYS.FOLDERS_COLLAPSED);
  if (foldersCollapsed === 'false') {
    expandFolders(false);
  } else if (foldersCollapsed === 'true') {
    collapseFolders(false);
  }

  // 2. Tag categories collapse states (default is collapsed)
  ['people', 'places', 'events', 'keyword'].forEach(cat => {
    const catCollapsed = safeGetItem(`${STORAGE_KEYS.TAG_CATEGORY_COLLAPSED_PREFIX}${cat}`);
    if (catCollapsed === 'false') {
      expandTagCategory(cat, false);
    } else if (catCollapsed === 'true') {
      collapseTagCategory(cat, false);
    }
  });

  // 3. Thumbnail zoom
  const savedZoom = safeGetItem(STORAGE_KEYS.THUMB_ZOOM);
  if (savedZoom && dom.zoomSlider) {
    dom.zoomSlider.value = savedZoom;
    document.documentElement.style.setProperty('--thumb-size', `${savedZoom}px`);
  }

  // 4. Main grid sort
  const savedSort = safeGetItem(STORAGE_KEYS.MAIN_SORT);
  if (savedSort && dom.sortSelect) {
    dom.sortSelect.value = savedSort;
    const [field, dir] = savedSort.split('-');
    state.sortBy = field || 'date_taken';
    state.sortDesc = (dir === 'desc');
  }

  // 5. Inspector collapsed state
  const inspectorCollapsed = safeGetItem(STORAGE_KEYS.INSPECTOR_COLLAPSED);
  if (inspectorCollapsed !== null && dom.rightInspector) {
    if (inspectorCollapsed === 'true') {
      dom.rightInspector.classList.add('collapsed');
    } else {
      dom.rightInspector.classList.remove('collapsed');
    }
  }
}

export function restoreNavigationPreferences() {
  // 1. Search text
  const savedSearch = safeGetItem(STORAGE_KEYS.SEARCH_TEXT);
  if (savedSearch) {
    state.searchText = savedSearch;
    if (dom.searchInput) dom.searchInput.value = savedSearch;
  }

  // 2. Nav filter (picks, rejects, photos, etc.)
  const savedNavFilter = safeGetItem(STORAGE_KEYS.ACTIVE_NAV_FILTER);
  if (savedNavFilter && savedNavFilter !== 'all') {
    state.activeNavFilter = savedNavFilter;
  }
  const savedMediaType = safeGetItem(STORAGE_KEYS.ACTIVE_MEDIA_TYPE);
  if (savedMediaType && ['photos', 'videos', 'audio'].includes(savedMediaType)) {
    state.activeMediaType = savedMediaType;
  }
  const savedStatusFilter = safeGetItem(STORAGE_KEYS.ACTIVE_STATUS_FILTER);
  if (savedStatusFilter && ['picks', 'rejects', 'not_rejects', 'unrated'].includes(savedStatusFilter)) {
    state.activeStatusFilter = savedStatusFilter;
  }

  // 3. Active folders
  const savedFoldersRaw = safeGetItem(STORAGE_KEYS.ACTIVE_FOLDERS);
  if (savedFoldersRaw) {
    try {
      const arr = JSON.parse(savedFoldersRaw);
      if (Array.isArray(arr) && arr.length > 0) {
        state.activeFolders = new Set(arr);
      }
    } catch (_) {}
  }

  // 4. Active tags
  const savedTagIdsRaw = safeGetItem(STORAGE_KEYS.ACTIVE_TAG_IDS);
  if (savedTagIdsRaw) {
    try {
      const arr = JSON.parse(savedTagIdsRaw);
      if (Array.isArray(arr) && arr.length > 0) {
        state.activeTagIds = new Set(arr.map(Number));
      }
    } catch (_) {}
  }

  // 5. Active Tab
  let queryTab = null;
  try {
    const urlParams = new URLSearchParams(window.location.search);
    queryTab = urlParams.get('tab');
  } catch (_) {}

  const savedTab = queryTab || safeGetItem(STORAGE_KEYS.ACTIVE_TAB);
  if (savedTab && ['media', 'people', 'places', 'events'].includes(savedTab)) {
    state.activeTab = savedTab;
    if (dom.viewTabs) {
      dom.viewTabs.querySelectorAll('.tab-btn').forEach(btn => {
        btn.classList.toggle('active', btn.dataset.tab === savedTab);
      });
    }
  }
}

export function getSavedViewMode() {
  return safeGetItem(STORAGE_KEYS.VIEW_MODE) || 'grid';
}
