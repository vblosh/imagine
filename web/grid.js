/**
 * IMAGINE Photo Organizer - Media Grid Container & View Controls
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

import { state } from './state.js';
import { dom } from './dom.js';
import { escapeHtml, getMonthName } from './api.js';
import { t } from './i18n.js';
import { cardMap, createPhotoCard, setCardsRenderGridCallback } from './media-cards.js';
import { clearCardSelections, handleCardSelection } from './media-selection.js';
import { isAlbumOrderMode, clearTimelineSelection } from './media-params.js';
import { initGridAccessibility, updateRovingTabindex } from './grid-a11y.js';
import { updateSidebarActive } from './sidebar.js';
import { renderTimeline } from './timeline.js';
import { isWholeYearPeriod, isSingleMonthPeriod } from './timeline-range.js';
import { clearFilterPreferences, saveActiveTabPreference } from './persistence.js';
import { restoreSemanticSnapshot } from './semantic-status.js';

let viewsOrchestrator = null;

export function setViewsOrchestrator(orch) {
  viewsOrchestrator = orch;
}

setCardsRenderGridCallback(() => {
  renderGrid();
});

export async function loadMedia(append = false, options = {}) {
  if (viewsOrchestrator?.loadMedia) {
    return viewsOrchestrator.loadMedia(append, options);
  }
}

export async function loadMoreMedia() {
  if (viewsOrchestrator?.loadMoreMedia) {
    return viewsOrchestrator.loadMoreMedia();
  }
}

export async function loadMetadata() {
  if (viewsOrchestrator?.loadMetadata) {
    return viewsOrchestrator.loadMetadata();
  }
}

export function renderLoadMore(container, onLoadMore = loadMoreMedia) {
  if (state.mediaItems.length >= state.totalCount) return null;
  const moreWrap = document.createElement('div');
  moreWrap.className = 'grid-load-more-wrap';
  moreWrap.style.cssText = 'padding:20px;text-align:center;width:100%;grid-column:1/-1;';
  const showingText = t('showing_photos_count', {
    current: state.mediaItems.length,
    total: state.totalCount,
  });
  moreWrap.innerHTML = `
    <div style="color:var(--text-dim);font-size:12px;margin-bottom:8px;">${showingText}</div>
    <button class="btn btn-secondary btn-sm" id="gridLoadMoreBtn">${t('load_more')}</button>
  `;
  const btn = moreWrap.querySelector('#gridLoadMoreBtn');
  if (btn) btn.onclick = () => onLoadMore();
  if (container) {
    container.appendChild(moreWrap);
  }
  return moreWrap;
}

export function renderGrid() {
  if (!dom.mediaGrid) return;

  cardMap.clear();

  if (state.mediaItems.length === 0) {
    dom.mediaGrid.innerHTML = '';
    if (dom.emptyState) dom.emptyState.style.display = 'flex';
    clearCardSelections();
    return;
  }
  if (dom.emptyState) dom.emptyState.style.display = 'none';

  if (state.semanticSearchActive) {
    const cardsWrap = document.createElement('div');
    cardsWrap.className = 'group-cards semantic-search-grid';
    state.mediaItems.forEach(item => {
      const card = createPhotoCard(item);
      cardMap.set(item.id, card);
      cardsWrap.appendChild(card);
    });
    dom.mediaGrid.innerHTML = '';
    dom.mediaGrid.appendChild(cardsWrap);
    initGridAccessibility();
    updateRovingTabindex();
    return;
  }

  if (isAlbumOrderMode()) {
    const cardsWrap = document.createElement('div');
    cardsWrap.className = 'group-cards album-order-grid';
    state.mediaItems.forEach(item => {
      const card = createPhotoCard(item);
      cardMap.set(item.id, card);
      cardsWrap.appendChild(card);
    });
    dom.mediaGrid.innerHTML = '';
    dom.mediaGrid.appendChild(cardsWrap);
    return;
  }

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

  renderLoadMore(fragment);

  dom.mediaGrid.innerHTML = '';
  dom.mediaGrid.appendChild(fragment);
  initGridAccessibility();
  updateRovingTabindex();
}

export function appendMediaToGrid(newItems) {
  if (!dom.mediaGrid) return;

  if (state.semanticSearchActive || isAlbumOrderMode()) {
    renderGrid();
    return;
  }

  // Remove existing load more wrap
  const existingLoadMore = dom.mediaGrid.querySelector('.grid-load-more-wrap');
  if (existingLoadMore) existingLoadMore.remove();

  if (!newItems || newItems.length === 0) {
    renderLoadMore(dom.mediaGrid);
    return;
  }

  // Group new items by Month & Year in UTC
  const groups = {};
  newItems.forEach(item => {
    let groupKey = t('undated');
    if (item.date_taken && item.date_taken > 0) {
      const d = new Date(item.date_taken * 1000);
      groupKey = `${getMonthName(d.getUTCMonth() + 1)} ${d.getUTCFullYear()}`;
    }
    if (!groups[groupKey]) groups[groupKey] = [];
    groups[groupKey].push(item);
  });

  Object.keys(groups).forEach(groupTitle => {
    let groupEl = dom.mediaGrid.querySelector(`.date-group[data-group-key="${CSS.escape(groupTitle)}"]`);

    if (groupEl) {
      const cardsWrap = groupEl.querySelector('.group-cards');
      if (cardsWrap) {
        groups[groupTitle].forEach(item => {
          const card = createPhotoCard(item);
          cardMap.set(item.id, card);
          cardsWrap.appendChild(card);
        });
      }
      const countEl = groupEl.querySelector('.group-count');
      if (countEl) {
        const totalInGroup = cardsWrap ? cardsWrap.querySelectorAll('.photo-card').length : groups[groupTitle].length;
        const itemNoun = totalInGroup === 1 ? t('item_singular') : t('item_plural');
        countEl.textContent = `${totalInGroup} ${itemNoun}`;
      }
    } else {
      groupEl = document.createElement('div');
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
      dom.mediaGrid.appendChild(groupEl);
    }
  });

  renderLoadMore(dom.mediaGrid);
}

export function updateFilterLabel() {
  if (state.activeTab && state.activeTab !== 'media') {
    if (state.activeTagId || (state.activeTab === 'albums' && state.activeAlbumId)) {
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
      if (state.activeTab === 'albums') {
        const noun = state.albums.length === 1 ? t('album_singular') : t('album_plural');
        if (dom.filterLabel) {
          dom.filterLabel.innerHTML = `<strong>${escapeHtml(tabName)}</strong> (${state.albums.length} ${noun})`;
        }
      } else {
        const catTags = (state.tags || []).filter(t => (t.category || 'keyword').toLowerCase() === state.activeTab.toLowerCase());
        const singularKey = state.activeTab === 'people' ? 'person_singular' : state.activeTab === 'places' ? 'place_singular' : 'event_singular';
        const pluralKey = state.activeTab === 'people' ? 'person_plural' : state.activeTab === 'places' ? 'place_plural' : 'event_plural';
        const noun = catTags.length === 1 ? t(singularKey) : t(pluralKey);
        if (dom.filterLabel) {
          dom.filterLabel.innerHTML = `<strong>${escapeHtml(tabName)}</strong> (${catTags.length} ${noun})`;
        }
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

  if (state.activeLastImported) {
    parts.push(t('nav_last_imported'));
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
    const partsFolder = folder.split(/[/\\]/);
    const folderName = partsFolder[partsFolder.length - 1] || folder;
    parts.push(`${t('filter_folder')}: ${folderName}`);
  } else if (tagCount + folderCount > 1) {
    const orLabels = [];
    state.activeTagIds.forEach(id => {
      const tag = state.tags.find(t => t.id === id);
      orLabels.push(tag ? tag.name : String(id));
    });
    state.activeFolders.forEach(folder => {
      const folderParts = folder.split(/[/\\]/);
      const shortName = folderParts[folderParts.length - 1] || folder;
      orLabels.push(shortName);
    });
    parts.push(`Filter (OR): ${orLabels.join(', ')}`);
  }

  if (state.searchText) {
    if (state.semanticSearchEnabled && state.semanticSearchAvailable) {
      parts.push(`Semantic: "${state.searchText}"`);
    } else {
      parts.push(`Search: "${state.searchText}"`);
    }
  }

  if (state.activeTimelinePeriod) {
    const period = state.activeTimelinePeriod;
    if (isWholeYearPeriod(period)) {
      parts.push(String(period.startYear));
    } else if (isSingleMonthPeriod(period)) {
      parts.push(`${getMonthName(period.startMonth)} ${period.startYear}`);
    } else {
      const start = `${getMonthName(period.startMonth)} ${period.startYear}`;
      const end = `${getMonthName(period.endMonth)} ${period.endYear}`;
      parts.push(t('timeline_range', { start, end }));
    }
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

export function syncSemanticSearchUI() {
  const isSearchActive = Boolean(state.semanticSearchActive);
  if (dom.contentToolbar) {
    dom.contentToolbar.classList.toggle('semantic-search-active', isSearchActive);
  }
  if (dom.semanticSearchToolbar) {
    dom.semanticSearchToolbar.style.display = isSearchActive ? 'flex' : 'none';
    if (isSearchActive) {
      const query = state.semanticSearchQuery || state.searchText || '';
      if (dom.semanticSearchTitle) {
        dom.semanticSearchTitle.textContent = query
          ? t('semantic_search_results_title', { query })
          : (t('semantic_search_results_title_generic') || 'AI Search');
      }
      if (dom.semanticSearchCount) {
        const count = state.mediaItems.length;
        const noun = count === 1 ? t('item_singular') : t('item_plural');
        dom.semanticSearchCount.textContent = `${count} ${noun}`;
      }
      if (dom.semanticSearchHint) {
        dom.semanticSearchHint.textContent = t('semantic_search_rank_hint') || '(sorted by probability rank)';
      }
    }
  }

  if (dom.filterIndicator) {
    dom.filterIndicator.style.display = isSearchActive ? 'none' : '';
  }
  if (dom.viewModeToggle) {
    dom.viewModeToggle.style.display = isSearchActive ? 'none' : '';
  }
  const sortSelector = dom.sortSelect ? dom.sortSelect.closest('.sort-selector') : document.querySelector('.sort-selector');
  if (sortSelector) {
    sortSelector.style.display = isSearchActive ? 'none' : '';
  }
  if (dom.bottomScrubBar) {
    dom.bottomScrubBar.style.display = isSearchActive ? 'none' : '';
  }
}

export async function closeSemanticSearch() {
  if (!state.semanticSearchActive && !state.searchText) return;

  state.semanticSearchActive = false;
  state.semanticSearchQuery = '';

  if (state.searchDebounceTimer) {
    clearTimeout(state.searchDebounceTimer);
    state.searchDebounceTimer = null;
  }
  state.searchText = '';
  if (dom.searchInput) {
    dom.searchInput.value = '';
  }
  if (dom.clearSearchBtn) {
    dom.clearSearchBtn.style.display = 'none';
  }

  restoreSemanticSnapshot();

  syncSemanticSearchUI();
  updateSidebarActive();
  renderTimeline();
  await loadMedia();
}

export function clearAllFilters() {
  state.semanticSearchActive = false;
  state.semanticSearchQuery = '';
  state.semanticSearchSnapshot = null;
  syncSemanticSearchUI();
  state.activeMediaType = 'all';
  state.activeStatusFilter = null;
  state.activeLastImported = false;
  state.activeTagIds.clear();
  state.activeAlbumId = null;
  state.activeFolders.clear();
  state.lastSidebarClickedItem = null;
  clearTimelineSelection();
  if (state.searchDebounceTimer) {
    clearTimeout(state.searchDebounceTimer);
    state.searchDebounceTimer = null;
  }
  state.activeTab = 'media';
  state.searchText = '';
  if (dom.searchInput) dom.searchInput.value = '';
  if (dom.categoryViewContainer) dom.categoryViewContainer.style.display = 'none';
  if (dom.categoryToolbar) dom.categoryToolbar.style.display = 'none';
  if (dom.contentToolbar) dom.contentToolbar.style.display = 'flex';
  if (dom.categoryBackBtn) dom.categoryBackBtn.style.display = 'none';
  if (dom.gridScrollContainer) {
    dom.gridScrollContainer.style.display = 'block';
    dom.gridScrollContainer.scrollTop = 0;
    dom.gridScrollContainer.scrollLeft = 0;
  }
  if (dom.viewTabs) {
    dom.viewTabs.querySelectorAll('.tab-btn').forEach(b => b.classList.toggle('active', b.dataset.tab === 'media'));
  }
  updateSidebarActive();
  renderTimeline();
  clearFilterPreferences();
  saveActiveTabPreference('media');
  loadMedia();
}

let openLoupeHandler = () => {};
export function setGridOpenLoupeHandler(fn) {
  openLoupeHandler = fn;
}

export function bindGridHandlers() {
  if (dom.gridScrollContainer) {
    dom.gridScrollContainer.addEventListener('click', (e) => {
      if (e.target === dom.gridScrollContainer || e.target === dom.mediaGrid) {
        clearCardSelections();
      }
    });
  }

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
          if (viewsOrchestrator?.updateItemRating) {
            viewsOrchestrator.updateItemRating(cardId, newRating);
          }
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
        openLoupeHandler(cardId);
      }
    });
  }
}
