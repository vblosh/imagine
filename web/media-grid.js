/**
 * IMAGINE Photo Organizer - Media Grid (Compatibility Re-exports)
 * Split into modular leaf components in Phase 1:
 * - media-params.js
 * - media-cards.js
 * - media-selection.js
 * - media-fetch.js
 * - sidebar.js
 * - timeline.js
 * - rating-flag.js
 * - grid-a11y.js
 * - grid.js
 * - views.js
 */

export {
  cardMap,
  clearCardMap,
  getCardElement,
  createPhotoCard,
  patchCardRating,
  patchCardFlag,
} from './media-cards.js';

export {
  previousSelectedIds,
  clearCardSelections,
  syncSelectionDOM,
  handleCardSelection,
  updateBatchBar,
  addSelectionListener,
  removeSelectionListener,
} from './media-selection.js';

export {
  clearTimelineSelection,
  isAlbumOrderMode,
  enterAlbumOrderMode,
  prepareAlbumOrderMode,
  syncAlbumOrderSortOption,
  buildMediaParams,
} from './media-params.js';

export {
  loadMediaData,
  loadMoreMediaData,
  loadMetadataData,
  abortCurrentMediaLoad,
  abortCurrentLoadMore,
  getCurrentLoadMediaId,
  cancelInFlightMediaLoads,
} from './media-fetch.js';

export {
  getSidebarSelectableItems,
  handleSidebarItemClick,
  applySidebarSearch,
  clearSidebarSearch,
  setCollapsed,
  expandTagCategory,
  collapseTagCategory,
  expandAlbums,
  collapseAlbums,
  expandFolders,
  collapseFolders,
  renderSidebarTags,
  renderSidebarAlbums,
  renderSidebarFolders,
  updateSidebarActive,
  setSidebarPersistenceHooks,
  setSidebarActionHooks,
} from './sidebar.js';

export {
  renderTimeline,
  setTimelineActionHooks,
} from './timeline.js';

export {
  renderStarWidget,
  updateItemRating,
  updateItemFlag,
  batchUpdateFlags,
  batchUpdateRatings,
  toggleFlagValue,
  toggleItemFlag,
  toggleFlagsForIds,
  batchUpdateDates,
  addRatingFlagListener,
  removeRatingFlagListener,
} from './rating-flag.js';

export {
  updateRovingTabindex,
  handleGridArrowNavigation,
  initGridAccessibility,
} from './grid-a11y.js';

export {
  loadMedia,
  loadMoreMedia,
  loadMetadata,
  renderLoadMore,
  renderGrid,
  appendMediaToGrid,
  updateFilterLabel,
  syncSemanticSearchUI,
  closeSemanticSearch,
  clearAllFilters,
} from './grid.js';
