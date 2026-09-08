/**
 * IMAGINE Photo Organizer - Category View (People, Places, Events)
 * Photoshop Elements Organizer style grid views and tag selection drilldown.
 */

import { state } from './state.js';
import { dom } from './dom.js';
import { escapeHtml } from './api.js';
import { t } from './i18n.js';
import {
  loadMedia,
  updateSidebarActive,
  updateFilterLabel
} from './media-grid.js';
import { openTagModal } from './modals.js';

export function getCategoryConfig(cat) {
  const c = (cat || 'people').toLowerCase();
  if (c === 'places') {
    return {
      category: 'places',
      title: t('tab_places'),
      singular: t('place_singular'),
      plural: t('place_plural'),
      addLabel: t('add_place'),
      emptyTitle: t('no_places_found'),
      emptyText: t('tag_places_prompt'),
      iconSvg: '<svg viewBox="0 0 24 24" width="48" height="48" fill="none" stroke="currentColor" stroke-width="1.5"><path d="M21 10c0 7-9 13-9 13s-9-6-9-13a9 9 0 0 1 18 0z"/><circle cx="12" cy="10" r="3"/></svg>',
      thumbPlaceholderSvg: '<svg viewBox="0 0 24 24" width="36" height="36" fill="none" stroke="currentColor" stroke-width="1.5"><path d="M21 10c0 7-9 13-9 13s-9-6-9-13a9 9 0 0 1 18 0z"/><circle cx="12" cy="10" r="3"/></svg>'
    };
  }
  if (c === 'events') {
    return {
      category: 'events',
      title: t('tab_events'),
      singular: t('event_singular'),
      plural: t('event_plural'),
      addLabel: t('add_event'),
      emptyTitle: t('no_events_found'),
      emptyText: t('tag_events_prompt'),
      iconSvg: '<svg viewBox="0 0 24 24" width="48" height="48" fill="none" stroke="currentColor" stroke-width="1.5"><rect x="3" y="4" width="18" height="18" rx="2" ry="2"/><line x1="16" y1="2" x2="16" y2="6"/><line x1="8" y1="2" x2="8" y2="6"/><line x1="3" y1="10" x2="21" y2="10"/></svg>',
      thumbPlaceholderSvg: '<svg viewBox="0 0 24 24" width="36" height="36" fill="none" stroke="currentColor" stroke-width="1.5"><rect x="3" y="4" width="18" height="18" rx="2" ry="2"/><line x1="16" y1="2" x2="16" y2="6"/><line x1="8" y1="2" x2="8" y2="6"/><line x1="3" y1="10" x2="21" y2="10"/></svg>'
    };
  }
  // Default: People
  return {
    category: 'people',
    title: t('tab_people'),
    singular: t('person_singular'),
    plural: t('person_plural'),
    addLabel: t('add_person'),
    emptyTitle: t('no_people_found'),
    emptyText: t('tag_people_prompt'),
    iconSvg: '<svg viewBox="0 0 24 24" width="48" height="48" fill="none" stroke="currentColor" stroke-width="1.5"><path d="M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2"/><circle cx="12" cy="7" r="4"/></svg>',
    thumbPlaceholderSvg: '<svg viewBox="0 0 24 24" width="36" height="36" fill="none" stroke="currentColor" stroke-width="1.5"><path d="M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2"/><circle cx="12" cy="7" r="4"/></svg>'
  };
}

export function getTagCoverUrl(tag) {
  if (tag.cover_hash) {
    return `/api/thumbnails/${encodeURIComponent(tag.cover_hash)}/256`;
  }
  if (tag.cover_media_id) {
    return `/api/photos/${tag.cover_media_id}/original`;
  }
  // Fallback: search in state.mediaItems (prefer photos over videos/audio)
  const matchingPhoto = (state.mediaItems || []).find(m =>
    (!m.media_type || m.media_type === 'photo') &&
    m.tags && m.tags.some(t => t.id === tag.id || (t.name && tag.name && t.name.toLowerCase() === tag.name.toLowerCase()))
  );
  const matching = matchingPhoto || (state.mediaItems || []).find(m =>
    m.tags && m.tags.some(t => t.id === tag.id || (t.name && tag.name && t.name.toLowerCase() === tag.name.toLowerCase()))
  );
  if (matching) {
    if (matching.content_hash) {
      return `/api/thumbnails/${encodeURIComponent(matching.content_hash)}/256`;
    }
    return `/api/photos/${matching.id}/original`;
  }
  return null;
}

export const DEFAULT_CATEGORY_SORTS = {
  people: 'count-desc',
  places: 'name-asc',
  events: 'last_date-desc'
};

export function getCategorySort(category) {
  const cat = (category || state.activeTab || 'people').toLowerCase();
  if (state.categorySort && state.categorySort[cat]) {
    return state.categorySort[cat];
  }
  try {
    const saved = localStorage.getItem(`imagine_category_sort_${cat}`);
    if (saved) return saved;
  } catch (_) {}
  return DEFAULT_CATEGORY_SORTS[cat] || 'name-asc';
}

export function setCategorySort(category, sortKey) {
  const cat = (category || state.activeTab || 'people').toLowerCase();
  if (!state.categorySort) {
    state.categorySort = { ...DEFAULT_CATEGORY_SORTS };
  }
  state.categorySort[cat] = sortKey;
  try {
    localStorage.setItem(`imagine_category_sort_${cat}`, sortKey);
  } catch (_) {}
}

export function getTagDates(tag) {
  let first = (tag.first_date != null && Number.isFinite(Number(tag.first_date)) && Number(tag.first_date) > 0)
    ? Number(tag.first_date)
    : null;
  let last = (tag.last_date != null && Number.isFinite(Number(tag.last_date)) && Number(tag.last_date) > 0)
    ? Number(tag.last_date)
    : null;

  // Fallback to inspecting state.mediaItems if first or last is missing
  if ((first === null || last === null) && Array.isArray(state.mediaItems) && state.mediaItems.length > 0) {
    const matching = state.mediaItems.filter(m =>
      m.tags && m.tags.some(t => t.id === tag.id || (t.name && tag.name && t.name.toLowerCase() === tag.name.toLowerCase()))
    );
    for (const m of matching) {
      const dt = m.date_taken || m.created_at || m.file_modified_time;
      if (dt && Number.isFinite(dt) && dt > 0) {
        if (first === null || dt < first) first = dt;
        if (last === null || dt > last) last = dt;
      }
    }
  }

  return { firstDate: first, lastDate: last };
}

export function sortCategoryTags(tags, sortKey) {
  const items = tags.map(tag => {
    const dates = getTagDates(tag);
    return {
      tag,
      firstDate: dates.firstDate,
      lastDate: dates.lastDate,
      count: Number(tag.media_count) || 0,
      name: String(tag.name || '')
    };
  });

  items.sort((a, b) => {
    switch (sortKey) {
      case 'date':
      case 'date-desc':
      case 'last_date-desc':
      case 'last_date_desc':
      case 'date_last_desc': {
        if (a.lastDate !== null && b.lastDate !== null) {
          if (b.lastDate !== a.lastDate) return b.lastDate - a.lastDate;
        } else if (a.lastDate !== null) {
          return -1;
        } else if (b.lastDate !== null) {
          return 1;
        }
        return a.name.localeCompare(b.name);
      }
      case 'last_date-asc':
      case 'last_date_asc':
      case 'date_last_asc': {
        if (a.lastDate !== null && b.lastDate !== null) {
          if (a.lastDate !== b.lastDate) return a.lastDate - b.lastDate;
        } else if (a.lastDate !== null) {
          return -1;
        } else if (b.lastDate !== null) {
          return 1;
        }
        return a.name.localeCompare(b.name);
      }
      case 'first_date-desc':
      case 'first_date_desc':
      case 'date_first_desc': {
        if (a.firstDate !== null && b.firstDate !== null) {
          if (b.firstDate !== a.firstDate) return b.firstDate - a.firstDate;
        } else if (a.firstDate !== null) {
          return -1;
        } else if (b.firstDate !== null) {
          return 1;
        }
        return a.name.localeCompare(b.name);
      }
      case 'date-asc':
      case 'first_date-asc':
      case 'first_date_asc':
      case 'date_first_asc': {
        if (a.firstDate !== null && b.firstDate !== null) {
          if (a.firstDate !== b.firstDate) return a.firstDate - b.firstDate;
        } else if (a.firstDate !== null) {
          return -1;
        } else if (b.firstDate !== null) {
          return 1;
        }
        return a.name.localeCompare(b.name);
      }
      case 'count-desc':
      case 'count_desc':
      case 'images-desc': {
        if (b.count !== a.count) return b.count - a.count;
        return a.name.localeCompare(b.name);
      }
      case 'count-asc':
      case 'count_asc':
      case 'images-asc': {
        if (a.count !== b.count) return a.count - b.count;
        return a.name.localeCompare(b.name);
      }
      case 'name-desc':
      case 'name_desc': {
        return b.name.localeCompare(a.name);
      }
      case 'name-asc':
      case 'name_asc':
      default: {
        return a.name.localeCompare(b.name);
      }
    }
  });

  for (let i = 0; i < items.length; i++) {
    tags[i] = items[i].tag;
  }
  return tags;
}

export function renderCategoryView(category) {
  if (!dom.categoryViewContainer || !dom.categoryCardsGrid) return;

  const cat = (category || state.activeTab || 'people').toLowerCase();
  const config = getCategoryConfig(cat);

  // Switch display containers and toolbars
  dom.categoryViewContainer.style.display = 'flex';
  if (dom.contentToolbar) dom.contentToolbar.style.display = 'none';
  if (dom.categoryToolbar) dom.categoryToolbar.style.display = 'flex';
  if (dom.gridScrollContainer) dom.gridScrollContainer.style.display = 'none';
  if (dom.mapViewContainer) dom.mapViewContainer.style.display = 'none';
  if (dom.emptyState) dom.emptyState.style.display = 'none';
  if (dom.categoryBackBtn) dom.categoryBackBtn.style.display = 'none';
  if (dom.mediaGrid) dom.mediaGrid.innerHTML = '';

  // Filter tags by category
  let tags = (state.tags || []).filter(t => (t.category || 'keyword').toLowerCase() === cat);

  // Search query filtering
  if (state.searchText) {
    const q = state.searchText.toLowerCase().trim();
    tags = tags.filter(t => (t.name || '').toLowerCase().includes(q));
  }

  // Update combobox selection to reflect active sort for this category
  const currentSort = getCategorySort(cat);
  if (dom.categorySortSelect && dom.categorySortSelect.value !== currentSort) {
    dom.categorySortSelect.value = currentSort;
  }

  // Sort tags using active sort
  sortCategoryTags(tags, currentSort);

  // Update header labels
  if (dom.categoryViewTitle) {
    dom.categoryViewTitle.textContent = config.title;
  }
  if (dom.categoryViewSubtitle) {
    dom.categoryViewSubtitle.textContent = `${tags.length} ${tags.length === 1 ? config.singular : config.plural}`;
  }
  if (dom.categoryAddBtnLabel) {
    dom.categoryAddBtnLabel.textContent = config.addLabel;
  }

  // If no tags found
  if (tags.length === 0) {
    if (dom.categoryCardsGrid) {
      dom.categoryCardsGrid.innerHTML = '';
      dom.categoryCardsGrid.style.display = 'none';
    }
    if (dom.categoryEmptyState) {
      dom.categoryEmptyState.style.display = 'flex';
      if (dom.categoryEmptyIcon) dom.categoryEmptyIcon.innerHTML = config.iconSvg;
      if (dom.categoryEmptyTitle) dom.categoryEmptyTitle.textContent = config.emptyTitle;
      if (dom.categoryEmptyText) dom.categoryEmptyText.textContent = config.emptyText;
      if (dom.categoryEmptyActionBtn) dom.categoryEmptyActionBtn.textContent = config.addLabel;
    }
    return;
  }

  // Tags exist: hide empty state, populate grid
  if (dom.categoryEmptyState) {
    dom.categoryEmptyState.style.display = 'none';
  }
  dom.categoryCardsGrid.style.display = 'grid';
  dom.categoryCardsGrid.innerHTML = '';

  tags.forEach(tag => {
    const card = document.createElement('div');
    card.className = 'category-card-item';
    card.dataset.id = String(tag.id);
    card.dataset.category = cat;
    card.tabIndex = 0;
    card.setAttribute('role', 'button');
    card.setAttribute('aria-label', `${tag.name} (${tag.media_count || 0} photos)`);

    const safeName = escapeHtml(tag.name || 'Unnamed');
    const coverUrl = getTagCoverUrl(tag);
    const count = tag.media_count || 0;
    const countText = `${count} ${count === 1 ? 'photo' : 'photos'}`;

    card.innerHTML = `
      <div class="category-card-thumb-wrapper">
        ${coverUrl ? `<img src="${coverUrl}" alt="${safeName}" class="category-card-thumb" loading="lazy">` : ''}
        <div class="category-card-placeholder" style="${coverUrl ? 'display:none;' : ''}">
          ${config.thumbPlaceholderSvg}
        </div>
        <span class="category-card-badge">${countText}</span>
      </div>
      <div class="category-card-footer">
        <span class="category-card-name" title="${safeName}">${safeName}</span>
      </div>
    `;

    // Handle image fallback on loading error
    const img = card.querySelector('.category-card-thumb');
    if (img) {
      img.addEventListener('error', () => {
        img.style.display = 'none';
        const placeholder = card.querySelector('.category-card-placeholder');
        if (placeholder) placeholder.style.display = 'flex';
      });
    }

    // Click handler: select category item and show its photos
    card.addEventListener('click', () => {
      selectCategoryItem(tag);
    });

    // Keyboard accessibility
    card.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') {
        e.preventDefault();
        selectCategoryItem(tag);
      }
    });

    dom.categoryCardsGrid.appendChild(card);
  });
}

export function selectCategoryItem(tag) {
  if (!tag) return;

  state.activeTagId = tag.id;
  state.activeFolder = null;
  state.activeTimelinePeriod = null;
  state.activeAlbumId = null;
  state.activeMediaType = 'all';
  state.activeStatusFilter = null;

  // Clear previous grid contents immediately so previous person's photos don't flash
  if (dom.mediaGrid) dom.mediaGrid.innerHTML = '';
  if (dom.emptyState) dom.emptyState.style.display = 'none';

  // Transition from category view to photo grid
  if (dom.categoryViewContainer) dom.categoryViewContainer.style.display = 'none';
  if (dom.categoryToolbar) dom.categoryToolbar.style.display = 'none';
  if (dom.contentToolbar) dom.contentToolbar.style.display = 'flex';
  if (dom.gridScrollContainer) dom.gridScrollContainer.style.display = 'block';
  if (dom.mapViewContainer) dom.mapViewContainer.style.display = 'none';

  updateSidebarActive();
  updateFilterLabel();
  loadMedia();
}

export function navigateBackToCategory() {
  state.activeTagId = null;
  if (dom.mediaGrid) dom.mediaGrid.innerHTML = '';
  if (dom.emptyState) dom.emptyState.style.display = 'none';
  updateSidebarActive();
  updateFilterLabel();
  renderCategoryView();
}

export function initCategoryView() {
  if (dom.categoryBackBtn) {
    dom.categoryBackBtn.addEventListener('click', () => {
      navigateBackToCategory();
    });
  }

  if (dom.categoryAddBtn) {
    dom.categoryAddBtn.addEventListener('click', () => {
      openTagModal();
    });
  }

  if (dom.categoryEmptyActionBtn) {
    dom.categoryEmptyActionBtn.addEventListener('click', () => {
      openTagModal();
    });
  }

  if (dom.categorySortSelect) {
    dom.categorySortSelect.addEventListener('change', (e) => {
      const val = e.target.value;
      const cat = (state.activeTab || 'people').toLowerCase();
      setCategorySort(cat, val);
      renderCategoryView(cat);
    });
  }
}
