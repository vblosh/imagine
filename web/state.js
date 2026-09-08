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
  activeMediaType: 'all',  // 'all', 'photos', 'videos', 'audio'
  activeStatusFilter: null,// null, 'picks', 'rejects', 'not_rejects', 'unrated'
  get activeNavFilter() {
    if (this.activeStatusFilter) return this.activeStatusFilter;
    if (this.activeMediaType && this.activeMediaType !== 'all') return this.activeMediaType;
    return 'all';
  },
  set activeNavFilter(val) {
    if (['photos', 'videos', 'audio'].includes(val)) {
      this.activeMediaType = val;
    } else if (['picks', 'rejects', 'not_rejects', 'unrated'].includes(val)) {
      this.activeStatusFilter = val;
    } else {
      this.activeMediaType = 'all';
      this.activeStatusFilter = null;
    }
  },
  activeTagIds: new Set(),
  activeFolders: new Set(),
  lastSidebarClickedItem: null,
  get activeTagId() {
    return this.activeTagIds.size > 0 ? this.activeTagIds.values().next().value : null;
  },
  set activeTagId(val) {
    this.activeTagIds.clear();
    if (val !== null && val !== undefined) {
      this.activeTagIds.add(Number(val));
    }
  },
  activeAlbumId: null,
  get activeFolder() {
    return this.activeFolders.size > 0 ? this.activeFolders.values().next().value : null;
  },
  set activeFolder(val) {
    this.activeFolders.clear();
    if (val !== null && val !== undefined) {
      this.activeFolders.add(String(val));
    }
  },
  allFolders: new Set(),
  activeTimelinePeriod: null, // { year, month }
  searchText: '',
  sortBy: 'date_taken',
  sortDesc: true,
  categorySort: {
    people: 'count-desc',
    places: 'name-asc',
    events: 'last_date-desc'
  },
  tags: [],
  albums: [],
  timelineData: [],
  stats: {},
  loupeIndex: -1,
  loupeZoom: 1.0,
  loupePanX: 0,
  loupePanY: 0,
  loupeShowOriginal: false,
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
