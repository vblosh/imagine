/**
 * IMAGINE Photo Organizer - Application State
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

import { hasValidGps } from './api.js';

export const state = {
  mediaItems: [],
  totalCount: 0,
  selectedIds: new Set(),
  lastSelectedId: null,
  activeTab: 'media',      // 'media', 'people', 'places', 'events'
  activeNavFilter: 'all',  // 'all', 'picks', 'rejects', 'unrated'
  activeTagId: null,
  activeAlbumId: null,
  activeFolder: null,
  allFolders: new Set(),
  activeTimelinePeriod: null, // { year, month }
  searchText: '',
  sortBy: 'date_taken',
  sortDesc: true,
  tags: [],
  albums: [],
  timelineData: [],
  stats: {},
  loupeIndex: -1,
  loupeZoom: 1.0,
  loupePanX: 0,
  loupePanY: 0,
  importPollInterval: null,
  viewMode: 'grid',
  mapInstance: null,
  markerLayerGroup: null,
  mapMarkers: [],
  searchMarker: null,
  activeMapMarker: null,
  _cachedGpsItems: null,
  _lastMapZoom: null,
  _lastMapItemsSig: null,
  inspectorMiniMapInstance: null,
  inspectorMiniMarker: null,
  placementMediaId: null,
  placementMediaIds: new Set(),
  lastUnmappedClickedId: null,
  unmappedTrayOpen: false,
  mapSearchResults: [],
  mapSearchActiveIdx: -1,
  mapSearchDebounceTimer: null,
  mediaLimit: 500,
  mediaOffset: 0,
  isLoadingMedia: false,
  isLoadingMore: false,
  isImporting: false
};

export function getGpsMediaItems() {
  if (!state._cachedGpsItems) {
    state._cachedGpsItems = state.mediaItems.filter(hasValidGps);
  }
  return state._cachedGpsItems;
}

export function invalidateGpsCache() {
  state._cachedGpsItems = null;
  state._lastMapItemsSig = null;
}
