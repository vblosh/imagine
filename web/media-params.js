/**
 * IMAGINE Photo Organizer - Media Query Parameters & Album Order State
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

import { state } from './state.js';
import { dom } from './dom.js';
import { getTimelinePeriodBounds } from './timeline-range.js';

let albumOrderHooks = {
  clearFilterPreferences: () => {},
  updateSidebarActive: () => {},
  renderTimeline: () => {},
};

export function setAlbumOrderHooks(hooks) {
  albumOrderHooks = { ...albumOrderHooks, ...hooks };
}

export function clearTimelineSelection() {
  state.activeTimelinePeriod = null;
  state.timelineAnchor = null;
}

export function isAlbumOrderMode() {
  return Boolean(state.activeAlbumId && state.sortBy === 'album_order');
}

export function enterAlbumOrderMode() {
  if (state.sortBy !== 'album_order') {
    state.albumOrderFallback = { sortBy: state.sortBy, sortDesc: state.sortDesc };
    state.sortBy = 'album_order';
    state.sortDesc = false;
  }
  if (dom.sortSelect) dom.sortSelect.value = 'album_order-asc';
}

export function prepareAlbumOrderMode() {
  if (!isAlbumOrderMode()) return;
  const filtersWereActive =
    state.activeMediaType !== 'all' ||
    Boolean(state.activeStatusFilter) ||
    state.activeLastImported ||
    state.activeTagIds.size > 0 ||
    state.activeFolders.size > 0 ||
    Boolean(state.activeTimelinePeriod) ||
    Boolean(state.searchText);

  state.activeMediaType = 'all';
  state.activeStatusFilter = null;
  state.activeLastImported = false;
  state.activeTagIds.clear();
  state.activeFolders.clear();
  clearTimelineSelection();

  if (state.searchDebounceTimer) {
    clearTimeout(state.searchDebounceTimer);
    state.searchDebounceTimer = null;
  }
  state.searchText = '';
  if (dom.searchInput) dom.searchInput.value = '';

  if (filtersWereActive) {
    albumOrderHooks.clearFilterPreferences();
    albumOrderHooks.updateSidebarActive();
    albumOrderHooks.renderTimeline();
  }
}

export function syncAlbumOrderSortOption() {
  if (!dom.sortSelect) return;

  const option = dom.sortSelect.querySelector('option[value="album_order-asc"]');
  const albumIsActive = Boolean(state.activeAlbumId);
  if (option) {
    option.hidden = !albumIsActive;
    option.disabled = !albumIsActive;
  }

  if (!albumIsActive && state.sortBy === 'album_order') {
    const fallback = state.albumOrderFallback || { sortBy: 'date_taken', sortDesc: true };
    state.sortBy = fallback.sortBy;
    state.sortDesc = fallback.sortDesc;
    state.albumOrderFallback = null;
    dom.sortSelect.value = `${state.sortBy}-${state.sortDesc ? 'desc' : 'asc'}`;
  }
}

export function buildMediaParams() {
  const params = {
    sort: `${state.sortBy}-${state.sortDesc ? 'desc' : 'asc'}`,
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
    if (state.activeTab !== 'albums') {
      params.tag_category = state.activeTab.toLowerCase();
    }
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

  if (state.activeLastImported) {
    params.last_imported = true;
  }

  // Timeline filter in UTC
  if (state.activeTimelinePeriod) {
    const bounds = getTimelinePeriodBounds(state.activeTimelinePeriod);
    params.date_from = bounds.from;
    params.date_to = bounds.to;
  }

  return params;
}
