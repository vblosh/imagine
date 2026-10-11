/**
 * IMAGINE Photo Organizer - Sidebar Navigation & Accordion Components
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

import { state } from './state.js';
import { dom, showToast } from './dom.js';
import { api, escapeHtml } from './api.js';
import { t } from './i18n.js';
import { clearTimelineSelection, enterAlbumOrderMode, syncAlbumOrderSortOption } from './media-params.js';

let persistenceHooks = {
  saveAlbumsCollapsedState: () => {},
  saveFoldersCollapsedState: () => {},
  saveTagCategoryCollapsedState: () => {},
  saveActiveFoldersPreference: () => {},
  saveActiveTagIdsPreference: () => {},
  saveActiveTabPreference: () => {},
  saveNavFilterPreference: () => {},
};

export function setSidebarPersistenceHooks(hooks) {
  persistenceHooks = { ...persistenceHooks, ...hooks };
}

let actionHooks = {
  loadMedia: () => {},
  loadMetadata: () => {},
  renderTimeline: () => {},
  openDeleteTagModal: () => {},
  clearAllFilters: () => {},
};

export function setSidebarActionHooks(hooks) {
  actionHooks = { ...actionHooks, ...hooks };
}

export function getSidebarSelectableItems() {
  const items = [];
  const catContainers = [
    dom.tagCategoryPeople,
    dom.tagCategoryPlaces,
    dom.tagCategoryEvents,
    dom.tagCategoryKeyword,
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
    persistenceHooks.saveActiveTabPreference('media');
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
  if (dom.gridScrollContainer) {
    dom.gridScrollContainer.scrollTop = 0;
    dom.gridScrollContainer.scrollLeft = 0;
  }

  clearTimelineSelection();

  if (isShift) {
    const allItems = getSidebarSelectableItems();
    const currIdx = allItems.findIndex(it =>
      it.type === itemType && (itemType === 'tag' ? it.id === itemValue : it.path === itemValue)
    );
    const anchor = state.lastSidebarClickedItem;
    let anchorIdx = -1;
    if (anchor) {
      anchorIdx = allItems.findIndex(it =>
        it.type === anchor.type && (anchor.type === 'tag' ? it.id === anchor.id : it.path === anchor.path)
      );
    }

    if (currIdx !== -1) {
      const start = anchorIdx !== -1 ? Math.min(anchorIdx, currIdx) : currIdx;
      const end = anchorIdx !== -1 ? Math.max(anchorIdx, currIdx) : currIdx;

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
          path: itemType === 'folder' ? itemValue : undefined,
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
    const isOnlyTag =
      itemType === 'tag' &&
      state.activeTagIds.size === 1 &&
      state.activeTagIds.has(itemValue) &&
      state.activeFolders.size === 0;
    const isOnlyFolder =
      itemType === 'folder' &&
      state.activeFolders.size === 1 &&
      state.activeFolders.has(itemValue) &&
      state.activeTagIds.size === 0;

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

  persistenceHooks.saveActiveFoldersPreference(state.activeFolders);
  persistenceHooks.saveActiveTagIdsPreference(state.activeTagIds);
  updateSidebarActive();
  actionHooks.renderTimeline();
  actionHooks.loadMedia();
}

const SIDEBAR_TAG_CATEGORIES = ['people', 'places', 'events', 'keyword'];
let sidebarExpansionSnapshot = null;

function normalizedSidebarSearch() {
  return String(state.sidebarSearchText || '').trim().toLocaleLowerCase();
}

function matchesSidebarSearch(value) {
  const query = normalizedSidebarSearch();
  return !query || String(value || '').toLocaleLowerCase().includes(query);
}

function captureSidebarExpansionState() {
  sidebarExpansionSnapshot = {
    albums: dom.albumsList?.style.display !== 'none',
    folders: dom.foldersTree?.style.display !== 'none',
    tags: Object.fromEntries(
      SIDEBAR_TAG_CATEGORIES.map(category => {
        const group = document.querySelector(`.tag-category-group[data-category="${category}"]`);
        return [category, group?.querySelector('.tag-items')?.style.display !== 'none'];
      })
    ),
  };
}

function restoreSidebarExpansionState() {
  if (!sidebarExpansionSnapshot) return;
  if (sidebarExpansionSnapshot.albums) expandAlbums(false);
  else collapseAlbums(false);
  if (sidebarExpansionSnapshot.folders) expandFolders(false);
  else collapseFolders(false);
  SIDEBAR_TAG_CATEGORIES.forEach(category => {
    if (sidebarExpansionSnapshot.tags[category]) expandTagCategory(category, false);
    else collapseTagCategory(category, false);
  });
  sidebarExpansionSnapshot = null;
}

function updateSidebarSearchStatus() {
  if (!dom.sidebarSearchStatus) return;
  const query = normalizedSidebarSearch();
  const matchCount = query
    ? state.albums.filter(album => matchesSidebarSearch(album.name)).length +
      state.tags.filter(tag => matchesSidebarSearch(tag.name)).length +
      Array.from(state.allFolders).filter(folder => matchesSidebarSearch(folder)).length
    : 0;
  dom.sidebarSearchStatus.hidden = !query || matchCount > 0;
}

export function applySidebarSearch(value) {
  const nextSearch = String(value || '').trim();
  if (!state.sidebarSearchText && nextSearch) {
    captureSidebarExpansionState();
  }
  state.sidebarSearchText = nextSearch;
  renderSidebarTags();
  renderSidebarAlbums();
  renderSidebarFolders();
  if (!nextSearch) {
    restoreSidebarExpansionState();
  }
  updateSidebarSearchStatus();
}

export function clearSidebarSearch() {
  applySidebarSearch('');
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
    if (save) persistenceHooks.saveTagCategoryCollapsedState(cat, false);
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
    if (save) persistenceHooks.saveTagCategoryCollapsedState(cat, true);
  }
}

export function expandAlbums(save = true) {
  if (dom.albumsList) {
    dom.albumsList.style.display = 'block';
    const arrow = dom.albumsHeader?.querySelector('.arrow') || dom.albumsArrow;
    if (arrow) arrow.textContent = '▼';
    if (dom.albumsHeader) dom.albumsHeader.setAttribute('aria-expanded', 'true');
    if (save) persistenceHooks.saveAlbumsCollapsedState(false);
  }
}

export function collapseAlbums(save = true) {
  if (dom.albumsList) {
    dom.albumsList.style.display = 'none';
    const arrow = dom.albumsHeader?.querySelector('.arrow') || dom.albumsArrow;
    if (arrow) arrow.textContent = '▶';
    if (dom.albumsHeader) dom.albumsHeader.setAttribute('aria-expanded', 'false');
    if (save) persistenceHooks.saveAlbumsCollapsedState(true);
  }
}

export function expandFolders(save = true) {
  if (dom.foldersTree) {
    dom.foldersTree.style.display = 'block';
    const arrow = dom.foldersHeader?.querySelector('.arrow') || dom.foldersArrow;
    if (arrow) arrow.textContent = '▼';
    if (dom.foldersHeader) dom.foldersHeader.setAttribute('aria-expanded', 'true');
    if (save) persistenceHooks.saveFoldersCollapsedState(false);
  }
}

export function collapseFolders(save = true) {
  if (dom.foldersTree) {
    dom.foldersTree.style.display = 'none';
    const arrow = dom.foldersHeader?.querySelector('.arrow') || dom.foldersArrow;
    if (arrow) arrow.textContent = '▶';
    if (dom.foldersHeader) dom.foldersHeader.setAttribute('aria-expanded', 'false');
    if (save) persistenceHooks.saveFoldersCollapsedState(true);
  }
}

export function renderSidebarTags() {
  const categories = {
    people: dom.tagCategoryPeople,
    places: dom.tagCategoryPlaces,
    events: dom.tagCategoryEvents,
    keyword: dom.tagCategoryKeyword,
  };

  Object.values(categories).forEach(el => {
    if (el) {
      el.innerHTML = '';
      const group = el.closest('.tag-category-group');
      if (group) group.hidden = false;
    }
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

  state.tags
    .filter(tag => matchesSidebarSearch(tag.name))
    .forEach(tag => {
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
          actionHooks.openDeleteTagModal(tag);
        });
      }

      container.appendChild(li);
    });

  if (normalizedSidebarSearch()) {
    Object.entries(categories).forEach(([category, container]) => {
      if (!container) return;
      const hasMatches = container.children.length > 0;
      const group = container.closest('.tag-category-group');
      if (group) group.hidden = !hasMatches;
      if (hasMatches) expandTagCategory(category, false);
    });
  }
  updateSidebarSearchStatus();
}

export function renderSidebarAlbums() {
  if (!dom.albumsList) return;
  dom.albumsList.innerHTML = '';

  if (state.albums.length === 0) {
    dom.albumsList.innerHTML = `<li class="menu-item" style="color:var(--text-dim);font-style:italic;">${escapeHtml(t('no_albums'))}</li>`;
    updateSidebarSearchStatus();
    return;
  }

  const matchingAlbums = state.albums.filter(album => matchesSidebarSearch(album.name));
  matchingAlbums.forEach(album => {
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
        enterAlbumOrderMode();
        state.activeFolders.clear();
        clearTimelineSelection();
      }
      updateSidebarActive();
      actionHooks.renderTimeline();
      actionHooks.loadMedia();
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
          await actionHooks.loadMetadata();
          actionHooks.loadMedia();
          showToast(`Album "${album.name}" deleted.`, 'info');
        } catch (err) {
          console.error('Failed to delete album:', err);
          showToast('Failed to delete album: ' + (err.message || 'Server error'), 'error');
        }
      });
    }

    dom.albumsList.appendChild(li);
  });
  if (normalizedSidebarSearch() && matchingAlbums.length > 0) {
    expandAlbums(false);
  }
  updateSidebarSearchStatus();
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
    dom.foldersTree.innerHTML = `<div style="color:var(--text-dim);font-style:italic;">${escapeHtml(t('no_folders'))}</div>`;
    updateSidebarSearchStatus();
    return;
  }

  const sortedFolders = Array.from(state.allFolders).filter(matchesSidebarSearch).sort();
  sortedFolders.forEach(folder => {
    const el = document.createElement('div');
    el.className = 'folder-item' + (state.activeFolders.has(folder) ? ' active' : '');
    el.dataset.folderPath = folder;
    const parts = folder.split(/[/\\]/);
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
  if (normalizedSidebarSearch() && sortedFolders.length > 0) {
    expandFolders(false);
  }
  updateSidebarSearchStatus();
}

export function updateSidebarActive() {
  syncAlbumOrderSortOption();

  const hasTag = state.activeTagIds && state.activeTagIds.size > 0;
  const hasFolder = state.activeFolders && state.activeFolders.size > 0;
  const isAllMedia =
    (!state.activeMediaType || state.activeMediaType === 'all') &&
    !state.activeStatusFilter &&
    !state.activeLastImported &&
    !hasTag &&
    !state.activeAlbumId &&
    !hasFolder &&
    !state.searchText &&
    (!state.activeTab || state.activeTab === 'media') &&
    !state.activeTimelinePeriod;

  if (dom.navAllMedia) dom.navAllMedia.classList.toggle('active', isAllMedia);
  if (dom.navLastImported) dom.navLastImported.classList.toggle('active', state.activeLastImported);
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
  if (state.activeAlbumId) {
    expandAlbums();
  }
}

export function bindSidebarHandlers() {
  const updateSidebarSearchClearVisibility = () => {
    if (!dom.clearSidebarSearchBtn) return;
    const hasText = Boolean(dom.sidebarSearchInput?.value || state.sidebarSearchText);
    dom.clearSidebarSearchBtn.classList.toggle('visible', hasText);
  };
  if (dom.sidebarSearchInput) {
    dom.sidebarSearchInput.addEventListener('input', updateSidebarSearchClearVisibility);
    dom.sidebarSearchInput.addEventListener('keydown', (e) => {
      if (e.key !== 'Enter') return;
      e.preventDefault();
      applySidebarSearch(dom.sidebarSearchInput.value);
      updateSidebarSearchClearVisibility();
    });
  }
  if (dom.clearSidebarSearchBtn) {
    dom.clearSidebarSearchBtn.addEventListener('click', () => {
      if (dom.sidebarSearchInput) {
        dom.sidebarSearchInput.value = '';
        dom.sidebarSearchInput.focus();
      }
      clearSidebarSearch();
      updateSidebarSearchClearVisibility();
    });
  }
  updateSidebarSearchClearVisibility();

  // Accordion Toggles
  if (dom.albumsHeader) {
    dom.albumsHeader.addEventListener('click', () => {
      const isCurrentlyCollapsed = dom.albumsList && dom.albumsList.style.display === 'none';
      if (isCurrentlyCollapsed) {
        expandAlbums(true);
      } else {
        collapseAlbums(true);
      }
    });
    dom.albumsHeader.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') {
        e.preventDefault();
        dom.albumsHeader.click();
      }
    });
  }

  // Tag category headers collapsible
  document.querySelectorAll('.category-header').forEach(hdr => {
    hdr.addEventListener('click', () => {
      const group = hdr.closest('.tag-category-group');
      const cat = group ? group.dataset.category : null;
      const items = group ? group.querySelector('.tag-items') : null;
      const isCurrentlyCollapsed = items && items.style.display === 'none';
      if (isCurrentlyCollapsed) {
        expandTagCategory(cat, true);
      } else {
        collapseTagCategory(cat, true);
      }
    });
    hdr.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') {
        e.preventDefault();
        hdr.click();
      }
    });
  });

  // Folders header collapsible
  if (dom.foldersHeader) {
    dom.foldersHeader.addEventListener('click', () => {
      const isCurrentlyCollapsed = dom.foldersTree && dom.foldersTree.style.display === 'none';
      if (isCurrentlyCollapsed) {
        expandFolders(true);
      } else {
        collapseFolders(true);
      }
    });
    dom.foldersHeader.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') {
        e.preventDefault();
        dom.foldersHeader.click();
      }
    });
  }

  // Nav sidebar filters (all, media types, flags/status)
  if (dom.navAllMedia) {
    dom.navAllMedia.addEventListener('click', () => {
      actionHooks.clearAllFilters();
    });
  }
  if (dom.navLastImported) {
    dom.navLastImported.addEventListener('click', () => {
      state.activeLastImported = !state.activeLastImported;
      state.activeFolder = null;
      clearTimelineSelection();
      persistenceHooks.saveNavFilterPreference(state.activeNavFilter);
      persistenceHooks.saveActiveFoldersPreference(state.activeFolders);
      updateSidebarActive();
      actionHooks.renderTimeline();
      actionHooks.loadMedia();
    });
  }
  if (dom.navPhotos) {
    dom.navPhotos.addEventListener('click', () => {
      state.activeMediaType = state.activeMediaType === 'photos' ? 'all' : 'photos';
      state.activeFolder = null;
      clearTimelineSelection();
      persistenceHooks.saveNavFilterPreference(state.activeNavFilter);
      persistenceHooks.saveActiveFoldersPreference(state.activeFolders);
      updateSidebarActive();
      actionHooks.renderTimeline();
      actionHooks.loadMedia();
    });
  }
  if (dom.navVideos) {
    dom.navVideos.addEventListener('click', () => {
      state.activeMediaType = state.activeMediaType === 'videos' ? 'all' : 'videos';
      state.activeFolder = null;
      clearTimelineSelection();
      persistenceHooks.saveNavFilterPreference(state.activeNavFilter);
      persistenceHooks.saveActiveFoldersPreference(state.activeFolders);
      updateSidebarActive();
      actionHooks.renderTimeline();
      actionHooks.loadMedia();
    });
  }
  if (dom.navAudio) {
    dom.navAudio.addEventListener('click', () => {
      state.activeMediaType = state.activeMediaType === 'audio' ? 'all' : 'audio';
      state.activeFolder = null;
      clearTimelineSelection();
      persistenceHooks.saveNavFilterPreference(state.activeNavFilter);
      persistenceHooks.saveActiveFoldersPreference(state.activeFolders);
      updateSidebarActive();
      actionHooks.renderTimeline();
      actionHooks.loadMedia();
    });
  }
  if (dom.navPicks) {
    dom.navPicks.addEventListener('click', () => {
      state.activeStatusFilter = state.activeStatusFilter === 'picks' ? null : 'picks';
      state.activeFolder = null;
      clearTimelineSelection();
      persistenceHooks.saveNavFilterPreference(state.activeNavFilter);
      persistenceHooks.saveActiveFoldersPreference(state.activeFolders);
      updateSidebarActive();
      actionHooks.renderTimeline();
      actionHooks.loadMedia();
    });
  }
  if (dom.navRejects) {
    dom.navRejects.addEventListener('click', () => {
      state.activeStatusFilter = state.activeStatusFilter === 'rejects' ? null : 'rejects';
      state.activeFolder = null;
      clearTimelineSelection();
      persistenceHooks.saveNavFilterPreference(state.activeNavFilter);
      persistenceHooks.saveActiveFoldersPreference(state.activeFolders);
      updateSidebarActive();
      actionHooks.renderTimeline();
      actionHooks.loadMedia();
    });
  }
  if (dom.navNotRejects) {
    dom.navNotRejects.addEventListener('click', () => {
      state.activeStatusFilter = state.activeStatusFilter === 'not_rejects' ? null : 'not_rejects';
      state.activeFolder = null;
      clearTimelineSelection();
      persistenceHooks.saveNavFilterPreference(state.activeNavFilter);
      persistenceHooks.saveActiveFoldersPreference(state.activeFolders);
      updateSidebarActive();
      actionHooks.renderTimeline();
      actionHooks.loadMedia();
    });
  }
  if (dom.navUnrated) {
    dom.navUnrated.addEventListener('click', () => {
      state.activeStatusFilter = state.activeStatusFilter === 'unrated' ? null : 'unrated';
      state.activeFolder = null;
      clearTimelineSelection();
      persistenceHooks.saveNavFilterPreference(state.activeNavFilter);
      persistenceHooks.saveActiveFoldersPreference(state.activeFolders);
      updateSidebarActive();
      actionHooks.renderTimeline();
      actionHooks.loadMedia();
    });
  }
}
