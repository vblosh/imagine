/**
 * IMAGINE Photo Organizer - Web UI Application
 * Inspired by Adobe Photoshop Elements Organizer
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

(function () {
  'use strict';

  // --- State ---
  const state = {
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
    importPollInterval: null
  };

  // --- API Helpers ---
  const api = {
    async get(endpoint, params = {}) {
      const url = new URL(endpoint, window.location.origin);
      Object.keys(params).forEach(k => {
        if (params[k] !== undefined && params[k] !== null && params[k] !== '') {
          url.searchParams.append(k, params[k]);
        }
      });
      const res = await fetch(url.toString());
      if (!res.ok) {
        const err = await res.json().catch(() => ({ error: res.statusText }));
        throw new Error(err.error || 'Request failed');
      }
      return res.json();
    },

    async post(endpoint, data = {}) {
      const res = await fetch(endpoint, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(data)
      });
      if (!res.ok) {
        const err = await res.json().catch(() => ({ error: res.statusText }));
        throw new Error(err.error || 'Request failed');
      }
      return res.json();
    },

    async del(endpoint) {
      const res = await fetch(endpoint, { method: 'DELETE' });
      if (!res.ok) {
        const err = await res.json().catch(() => ({ error: res.statusText }));
        throw new Error(err.error || 'Delete failed');
      }
      return res.json();
    }
  };

  // --- DOM Elements ---
  const dom = {
    mediaGrid: document.getElementById('mediaGrid'),
    emptyState: document.getElementById('emptyState'),
    emptyImportBtn: document.getElementById('emptyImportBtn'),
    gridScrollContainer: document.getElementById('gridScrollContainer'),
    zoomSlider: document.getElementById('zoomSlider'),
    searchInput: document.getElementById('searchInput'),
    clearSearchBtn: document.getElementById('clearSearchBtn'),
    refreshBtn: document.getElementById('refreshBtn'),
    importBtn: document.getElementById('importBtn'),
    viewTabs: document.getElementById('viewTabs'),
    sortSelect: document.getElementById('sortSelect'),
    filterIndicator: document.getElementById('filterIndicator'),
    filterLabel: document.getElementById('filterLabel'),
    clearFiltersBtn: document.getElementById('clearFiltersBtn'),
    totalMediaCount: document.getElementById('totalMediaCount'),
    navAllMedia: document.getElementById('navAllMedia'),
    navPicks: document.getElementById('navPicks'),
    navRejects: document.getElementById('navRejects'),
    navUnrated: document.getElementById('navUnrated'),
    albumsList: document.getElementById('albumsList'),
    newAlbumBtn: document.getElementById('newAlbumBtn'),
    newTagBtn: document.getElementById('newTagBtn'),
    foldersTree: document.getElementById('foldersTree'),
    tagCategoryPeople: document.getElementById('tagCategoryPeople'),
    tagCategoryPlaces: document.getElementById('tagCategoryPlaces'),
    tagCategoryEvents: document.getElementById('tagCategoryEvents'),
    tagCategoryKeyword: document.getElementById('tagCategoryKeyword'),
    batchActionBar: document.getElementById('batchActionBar'),
    batchSelectedCount: document.getElementById('batchSelectedCount'),
    batchRating: document.getElementById('batchRating'),
    batchPickBtn: document.getElementById('batchPickBtn'),
    batchRejectBtn: document.getElementById('batchRejectBtn'),
    batchAddTagBtn: document.getElementById('batchAddTagBtn'),
    batchClearBtn: document.getElementById('batchClearBtn'),
    timelineContainer: document.getElementById('timelineContainer'),
    resetTimelineBtn: document.getElementById('resetTimelineBtn'),
    rightInspector: document.getElementById('rightInspector'),
    toggleInspectorBtn: document.getElementById('toggleInspectorBtn'),
    closeInspectorBtn: document.getElementById('closeInspectorBtn'),
    inspectorNoSelection: document.getElementById('inspectorNoSelection'),
    inspectorSelection: document.getElementById('inspectorSelection'),
    inspectorImg: document.getElementById('inspectorImg'),
    openLoupeFromInspector: document.getElementById('openLoupeFromInspector'),
    inspectorRating: document.getElementById('inspectorRating'),
    inspectorFlag: document.getElementById('inspectorFlag'),
    infoFileName: document.getElementById('infoFileName'),
    infoDimensions: document.getElementById('infoDimensions'),
    infoFileSize: document.getElementById('infoFileSize'),
    infoDateTaken: document.getElementById('infoDateTaken'),
    infoFilePath: document.getElementById('infoFilePath'),
    infoCamera: document.getElementById('infoCamera'),
    infoLens: document.getElementById('infoLens'),
    infoExposure: document.getElementById('infoExposure'),
    infoAperture: document.getElementById('infoAperture'),
    infoIso: document.getElementById('infoIso'),
    infoFocal: document.getElementById('infoFocal'),
    infoGps: document.getElementById('infoGps'),
    inspectorTags: document.getElementById('inspectorTags'),
    addTagInput: document.getElementById('addTagInput'),
    addTagCategorySelect: document.getElementById('addTagCategorySelect'),
    tagSuggestions: document.getElementById('tagSuggestions'),
    addTagBtn: document.getElementById('addTagBtn'),
    loupeModal: document.getElementById('loupeModal'),
    loupeBackdrop: document.getElementById('loupeBackdrop'),
    loupeImg: document.getElementById('loupeImg'),
    loupeFileName: document.getElementById('loupeFileName'),
    loupeIndex: document.getElementById('loupeIndex'),
    loupeRating: document.getElementById('loupeRating'),
    loupeFlag: document.getElementById('loupeFlag'),
    loupePrevBtn: document.getElementById('loupePrevBtn'),
    loupeNextBtn: document.getElementById('loupeNextBtn'),
    loupeCloseBtn: document.getElementById('loupeCloseBtn'),
    importModal: document.getElementById('importModal'),
    importBackdrop: document.getElementById('importBackdrop'),
    closeImportModalBtn: document.getElementById('closeImportModalBtn'),
    cancelImportBtn: document.getElementById('cancelImportBtn'),
    startImportBtn: document.getElementById('startImportBtn'),
    importPathInput: document.getElementById('importPathInput'),
    importRecursiveCheck: document.getElementById('importRecursiveCheck'),
    importProgressBox: document.getElementById('importProgressBox'),
    importProgressBar: document.getElementById('importProgressBar'),
    importStatusCounts: document.getElementById('importStatusCounts'),
    importCurrentFile: document.getElementById('importCurrentFile'),
    newAlbumModal: document.getElementById('newAlbumModal'),
    newAlbumBackdrop: document.getElementById('newAlbumBackdrop'),
    closeNewAlbumModalBtn: document.getElementById('closeNewAlbumModalBtn'),
    cancelAlbumBtn: document.getElementById('cancelAlbumBtn'),
    createAlbumSubmitBtn: document.getElementById('createAlbumSubmitBtn'),
    albumNameInput: document.getElementById('albumNameInput'),
    albumDescInput: document.getElementById('albumDescInput'),
    newTagModal: document.getElementById('newTagModal'),
    newTagBackdrop: document.getElementById('newTagBackdrop'),
    closeNewTagModalBtn: document.getElementById('closeNewTagModalBtn'),
    cancelTagBtn: document.getElementById('cancelTagBtn'),
    createTagSubmitBtn: document.getElementById('createTagSubmitBtn'),
    tagNameInput: document.getElementById('tagNameInput'),
    tagCategorySelect: document.getElementById('tagCategorySelect')
  };

  // --- Utility Functions ---
  function formatBytes(bytes) {
    if (!bytes || bytes === 0) return '0 B';
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB', 'TB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return parseFloat((bytes / Math.pow(k, i)).toFixed(1)) + ' ' + sizes[i];
  }

  function formatDate(timestamp) {
    if (!timestamp || timestamp <= 0) return 'Unknown Date';
    const d = new Date(timestamp * 1000);
    return d.toLocaleDateString(undefined, {
      year: 'numeric',
      month: 'short',
      day: 'numeric'
    });
  }

  function formatDateTime(timestamp) {
    if (!timestamp || timestamp <= 0) return 'Unknown Date';
    const d = new Date(timestamp * 1000);
    return d.toLocaleString(undefined, {
      year: 'numeric',
      month: 'short',
      day: 'numeric',
      hour: '2-digit',
      minute: '2-digit'
    });
  }

  function formatExposureTime(sec) {
    if (!sec || sec <= 0) return '';
    if (sec >= 1) return sec.toFixed(1) + 's';
    const fraction = Math.round(1 / sec);
    return `1/${fraction}s`;
  }

  function getMonthName(monthNumber) {
    const months = [
      'January', 'February', 'March', 'April', 'May', 'June',
      'July', 'August', 'September', 'October', 'November', 'December'
    ];
    return months[monthNumber - 1] || '';
  }

  // --- Load Media & Catalog Data ---
  async function loadMedia() {
    try {
      const params = {
        limit: 500,
        offset: 0,
        sort: `${state.sortBy}-${state.sortDesc ? 'desc' : 'asc'}`
      };

      if (state.searchText) params.search = state.searchText;
      if (state.activeTagId) params.tag_id = state.activeTagId;
      if (state.activeAlbumId) params.album_id = state.activeAlbumId;

      // Nav filters (picks, rejects, unrated)
      if (state.activeNavFilter === 'picks') params.flag = 1;
      else if (state.activeNavFilter === 'rejects') params.flag = -1;
      else if (state.activeNavFilter === 'unrated') {
        params.rating = 0;
        params.max_rating = 0;
      }

      // Timeline filter
      if (state.activeTimelinePeriod) {
        const { year, month } = state.activeTimelinePeriod;
        const start = new Date(year, month - 1, 1).getTime() / 1000;
        const end = new Date(year, month, 0, 23, 59, 59).getTime() / 1000;
        params.date_from = Math.floor(start);
        params.date_to = Math.floor(end);
      }

      const res = await api.get('/api/media', params);
      state.mediaItems = res.items || [];
      state.totalCount = res.total || 0;

      // Track all discovered folders across loads so filtering doesn't remove folders from sidebar
      state.mediaItems.forEach(item => {
        if (item.file_path) {
          const lastSlash = item.file_path.lastIndexOf('/');
          if (lastSlash > 0) {
            state.allFolders.add(item.file_path.substring(0, lastSlash));
          }
        }
      });

      renderGrid();
      renderSidebarFolders();
      updateFilterLabel();
      updateBatchBar();
      updateInspector();
    } catch (err) {
      console.error('Failed to load media:', err);
    }
  }

  async function loadMetadata() {
    try {
      const [tags, albums, timeline, stats] = await Promise.all([
        api.get('/api/tags'),
        api.get('/api/albums'),
        api.get('/api/timeline'),
        api.get('/api/stats')
      ]);

      state.tags = tags || [];
      state.albums = albums || [];
      state.timelineData = timeline || [];
      state.stats = stats || {};

      renderSidebarTags();
      renderSidebarAlbums();
      renderSidebarFolders();
      renderTimeline();

      if (dom.totalMediaCount) {
        dom.totalMediaCount.textContent = state.stats.total_media || 0;
      }
    } catch (err) {
      console.error('Failed to load catalog metadata:', err);
    }
  }

  // --- Render Functions ---
  function renderGrid() {
    if (!dom.mediaGrid) return;
    dom.mediaGrid.innerHTML = '';

    if (state.mediaItems.length === 0) {
      dom.emptyState.style.display = 'flex';
      return;
    }
    dom.emptyState.style.display = 'none';

    // Group items by Month & Year
    const groups = {};
    state.mediaItems.forEach(item => {
      let groupKey = 'Undated';
      if (item.date_taken && item.date_taken > 0) {
        const d = new Date(item.date_taken * 1000);
        groupKey = `${getMonthName(d.getMonth() + 1)} ${d.getFullYear()}`;
      }
      if (!groups[groupKey]) groups[groupKey] = [];
      groups[groupKey].push(item);
    });

    Object.keys(groups).forEach(groupTitle => {
      const groupEl = document.createElement('div');
      groupEl.className = 'date-group';

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
        cardsWrap.appendChild(card);
      });

      groupEl.appendChild(cardsWrap);
      dom.mediaGrid.appendChild(groupEl);
    });
  }

  function createPhotoCard(item) {
    const card = document.createElement('div');
    card.className = 'photo-card' + (state.selectedIds.has(item.id) ? ' selected' : '');
    card.dataset.id = item.id;

    // Thumbnail URL with fallback
    const thumbUrl = item.content_hash
      ? `/api/thumbnails/${item.content_hash}/256`
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

    card.innerHTML = `
      <div class="photo-thumb-wrap">
        <img src="${thumbUrl}" alt="${item.file_name}" loading="lazy" onerror="this.onerror=null;this.src='/api/photos/${item.id}/original';">
        <div class="card-badges">
          ${flagBadge}
        </div>
      </div>
      <div class="card-info">
        <div class="card-filename" title="${item.file_name}">${item.file_name}</div>
        <div class="card-footer">
          <span>${formatDate(item.date_taken)}</span>
          <div class="star-rating card-stars" data-id="${item.id}">
            ${starsHtml}
          </div>
        </div>
      </div>
    `;

    // Click handler for selection
    card.addEventListener('click', (e) => {
      // If clicking directly on a star
      const starTarget = e.target.closest('.card-stars span');
      if (starTarget) {
        e.stopPropagation();
        const star = parseInt(starTarget.dataset.star, 10);
        const newRating = item.rating === star ? 0 : star;
        updateItemRating(item.id, newRating);
        return;
      }

      handleCardSelection(item.id, e);
    });

    // Double click to open loupe
    card.addEventListener('dblclick', () => {
      openLoupeForMedia(item.id);
    });

    return card;
  }

  function handleCardSelection(id, event) {
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
      }
    } else {
      // Single select
      state.selectedIds.clear();
      state.selectedIds.add(id);
      state.lastSelectedId = id;
    }

    // Update selection classes in DOM
    document.querySelectorAll('.photo-card').forEach(c => {
      const cardId = parseInt(c.dataset.id, 10);
      if (state.selectedIds.has(cardId)) {
        c.classList.add('selected');
      } else {
        c.classList.remove('selected');
      }
    });

    updateBatchBar();
    updateInspector();
  }

  function updateBatchBar() {
    const count = state.selectedIds.size;
    if (count > 1) {
      dom.batchActionBar.style.display = 'flex';
      dom.batchSelectedCount.textContent = `${count} selected`;
    } else {
      dom.batchActionBar.style.display = 'none';
    }
  }

  // --- Inspector Panel ---
  async function updateInspector() {
    if (state.selectedIds.size === 0) {
      dom.inspectorNoSelection.style.display = 'block';
      dom.inspectorSelection.style.display = 'none';
      return;
    }

    // Pick first selected ID
    const selectedId = Array.from(state.selectedIds)[0];
    let item = state.mediaItems.find(m => m.id === selectedId);

    try {
      // Fetch fresh details from API
      item = await api.get(`/api/media/${selectedId}`);
    } catch (err) {
      console.warn('Could not fetch single media details:', err);
    }

    if (!item) return;

    dom.inspectorNoSelection.style.display = 'none';
    dom.inspectorSelection.style.display = 'block';

    // Inspector Preview
    const previewUrl = item.content_hash
      ? `/api/thumbnails/${item.content_hash}/1024`
      : `/api/photos/${item.id}/original`;
    dom.inspectorImg.src = previewUrl;
    dom.inspectorImg.onerror = () => {
      dom.inspectorImg.src = `/api/photos/${item.id}/original`;
    };

    // Rating
    renderStarWidget(dom.inspectorRating, item.rating || 0, (newRating) => {
      updateItemRating(item.id, newRating);
    });

    // Flags
    const pickBtn = dom.inspectorFlag.querySelector('.flag-pick');
    const rejectBtn = dom.inspectorFlag.querySelector('.flag-reject');
    pickBtn.classList.toggle('active', item.flag === 1);
    rejectBtn.classList.toggle('active', item.flag === -1);

    // File Properties
    dom.infoFileName.textContent = item.file_name || '-';
    dom.infoDimensions.textContent = item.width && item.height ? `${item.width} × ${item.height} px` : '-';
    dom.infoFileSize.textContent = formatBytes(item.file_size);
    dom.infoDateTaken.textContent = formatDateTime(item.date_taken);
    dom.infoFilePath.textContent = item.file_path || '-';

    // EXIF properties
    const exif = item.exif || {};
    const cameraStr = [exif.camera_make, exif.camera_model].filter(Boolean).join(' ') || '-';
    dom.infoCamera.textContent = cameraStr;
    dom.infoLens.textContent = exif.lens || '-';
    dom.infoExposure.textContent = formatExposureTime(exif.exposure_time) || '-';
    dom.infoAperture.textContent = exif.f_number ? `f/${exif.f_number.toFixed(1)}` : '-';
    dom.infoIso.textContent = exif.iso ? `ISO ${exif.iso}` : '-';
    dom.infoFocal.textContent = exif.focal_length ? `${exif.focal_length.toFixed(1)} mm` : '-';

    if (exif.has_gps && (exif.latitude !== 0 || exif.longitude !== 0)) {
      const lat = exif.latitude.toFixed(5);
      const lon = exif.longitude.toFixed(5);
      dom.infoGps.innerHTML = `
        <a href="https://www.openstreetmap.org/?mlat=${lat}&mlon=${lon}#map=16/${lat}/${lon}" target="_blank" rel="noopener" class="btn-link">
          ${lat}, ${lon} ↗
        </a>
      `;
    } else {
      dom.infoGps.textContent = '-';
    }

    // Tags
    renderInspectorTags(item);
  }

  function renderInspectorTags(item) {
    dom.inspectorTags.innerHTML = '';
    const tags = item.tags || [];

    if (tags.length === 0) {
      dom.inspectorTags.innerHTML = '<span style="color:var(--text-dim);font-size:11px;">No tags</span>';
      return;
    }

    tags.forEach(tag => {
      const badge = document.createElement('span');
      badge.className = 'tag-badge';
      const cat = (tag.category || 'keyword').toLowerCase();
      badge.dataset.category = cat;
      badge.title = `Category: ${tag.category || 'keyword'}`;
      badge.innerHTML = `
        <span>${tag.name}</span>
        <button class="remove-tag" title="Remove tag">&times;</button>
      `;
      badge.querySelector('.remove-tag').addEventListener('click', async () => {
        try {
          await api.del(`/api/media/${item.id}/tags/${tag.id}`);
          updateInspector();
          loadMetadata();
        } catch (err) {
          console.error('Failed to remove tag:', err);
        }
      });
      dom.inspectorTags.appendChild(badge);
    });
  }

  function renderStarWidget(container, currentRating, onSetRating) {
    if (!container) return;
    const stars = container.querySelectorAll('span');
    stars.forEach(s => {
      const starVal = parseInt(s.dataset.star, 10);
      s.classList.toggle('active', starVal <= currentRating);

      s.onclick = (e) => {
        e.stopPropagation();
        const newRating = starVal === currentRating ? 0 : starVal;
        onSetRating(newRating);
      };
    });
  }

  // --- Sidebar Renderers ---
  function renderSidebarTags() {
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
      li.innerHTML = `
        <span class="tag-name">${tag.name}</span>
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
        }
        updateSidebarActive();
        loadMedia();
      });

      const delBtn = li.querySelector('.delete-tag-btn');
      if (delBtn) {
        delBtn.addEventListener('click', async (e) => {
          e.stopPropagation();
          try {
            await api.del(`/api/tags/${tag.id}`);
            if (state.activeTagId === tag.id) {
              state.activeTagId = null;
            }
            await loadMetadata();
            loadMedia();
            updateInspector();
          } catch (err) {
            console.error('Failed to delete tag:', err);
          }
        });
      }

      container.appendChild(li);
    });
  }

  function renderSidebarAlbums() {
    if (!dom.albumsList) return;
    dom.albumsList.innerHTML = '';

    if (state.albums.length === 0) {
      dom.albumsList.innerHTML = '<li class="menu-item" style="color:var(--text-dim);font-style:italic;">No albums</li>';
      return;
    }

    state.albums.forEach(album => {
      const li = document.createElement('li');
      li.className = 'menu-item' + (state.activeAlbumId === album.id ? ' active' : '');
      li.innerHTML = `
        <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2"><path d="M4 19.5A2.5 2.5 0 0 1 6.5 17H20"/><path d="M6.5 2H20v20H6.5A2.5 2.5 0 0 1 4 19.5v-15A2.5 2.5 0 0 1 6.5 2z"/></svg>
        <span class="album-name">${album.name}</span>
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
        }
        updateSidebarActive();
        loadMedia();
      });

      const delBtn = li.querySelector('.delete-album-btn');
      if (delBtn) {
        delBtn.addEventListener('click', async (e) => {
          e.stopPropagation();
          try {
            await api.del(`/api/albums/${album.id}`);
            if (state.activeAlbumId === album.id) {
              state.activeAlbumId = null;
            }
            await loadMetadata();
            loadMedia();
          } catch (err) {
            console.error('Failed to delete album:', err);
          }
        });
      }

      dom.albumsList.appendChild(li);
    });
  }

  function renderSidebarFolders() {
    if (!dom.foldersTree) return;
    dom.foldersTree.innerHTML = '';

    // If allFolders is empty, populate from current media items
    if (state.allFolders.size === 0) {
      state.mediaItems.forEach(item => {
        if (item.file_path) {
          const lastSlash = item.file_path.lastIndexOf('/');
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
      const parts = folder.split('/');
      const shortName = parts[parts.length - 1] || folder;

      el.innerHTML = `
        <svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/></svg>
        <span title="${folder}">${shortName}</span>
      `;
      el.addEventListener('click', () => {
        if (state.activeFolder === folder) {
          state.activeFolder = null;
          state.searchText = '';
          dom.searchInput.value = '';
        } else {
          state.activeFolder = folder;
          state.searchText = folder;
          dom.searchInput.value = folder;
          state.activeTagId = null;
          state.activeAlbumId = null;
          state.activeNavFilter = 'all';
        }
        updateSidebarActive();
        loadMedia();
      });
      dom.foldersTree.appendChild(el);
    });
  }

  function renderTimeline() {
    if (!dom.timelineContainer) return;
    dom.timelineContainer.innerHTML = '';

    if (state.timelineData.length === 0) {
      dom.timelineContainer.innerHTML = '<span style="color:var(--text-dim);font-size:10px;align-self:center;">No timeline data</span>';
      return;
    }

    const maxCount = Math.max(...state.timelineData.map(t => t.count), 1);

    state.timelineData.forEach(entry => {
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

  function updateSidebarActive() {
    dom.navAllMedia.classList.toggle('active', state.activeNavFilter === 'all' && !state.activeTagId && !state.activeAlbumId && !state.activeFolder && !state.searchText);
    dom.navPicks.classList.toggle('active', state.activeNavFilter === 'picks');
    dom.navRejects.classList.toggle('active', state.activeNavFilter === 'rejects');
    dom.navUnrated.classList.toggle('active', state.activeNavFilter === 'unrated');
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

  function updateFilterLabel() {
    let label = 'All Photos';
    let isFiltered = false;

    if (state.activeNavFilter === 'picks') {
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
      const folderName = state.activeFolder.split('/').pop() || state.activeFolder;
      label = `Folder: ${folderName}`;
      isFiltered = true;
    } else if (state.searchText) {
      label += ` • Search: "${state.searchText}"`;
      isFiltered = true;
    }

    if (state.activeTimelinePeriod) {
      const { year, month } = state.activeTimelinePeriod;
      label += ` • ${getMonthName(month)} ${year}`;
      isFiltered = true;
    }

    dom.filterLabel.innerHTML = `<strong>${label}</strong> (${state.totalCount} items)`;
    dom.clearFiltersBtn.style.display = isFiltered ? 'inline-block' : 'none';
  }

  function clearAllFilters() {
    state.activeNavFilter = 'all';
    state.activeTagId = null;
    state.activeAlbumId = null;
    state.activeFolder = null;
    state.activeTimelinePeriod = null;
    state.searchText = '';
    dom.searchInput.value = '';
    updateSidebarActive();
    renderTimeline();
    loadMedia();
  }

  // --- Item Modifications ---
  async function updateItemRating(id, rating) {
    try {
      await api.post(`/api/media/${id}/rating`, { rating });
      // Update local item
      const item = state.mediaItems.find(m => m.id === id);
      if (item) item.rating = rating;
      renderGrid();
      updateInspector();
      if (state.loupeIndex >= 0) updateLoupeControls();
    } catch (err) {
      console.error('Failed to update rating:', err);
    }
  }

  async function updateItemFlag(id, flag) {
    try {
      await api.post(`/api/media/${id}/flag`, { flag });
      const item = state.mediaItems.find(m => m.id === id);
      if (item) item.flag = flag;
      renderGrid();
      updateInspector();
      if (state.loupeIndex >= 0) updateLoupeControls();
    } catch (err) {
      console.error('Failed to update flag:', err);
    }
  }

  // --- Fullscreen Loupe Viewer ---
  function openLoupeForMedia(id) {
    const idx = state.mediaItems.findIndex(m => m.id === id);
    if (idx === -1) return;
    state.loupeIndex = idx;
    dom.loupeModal.style.display = 'flex';
    updateLoupeView();
  }

  function closeLoupe() {
    state.loupeIndex = -1;
    dom.loupeModal.style.display = 'none';
  }

  function updateLoupeView() {
    if (state.loupeIndex < 0 || state.loupeIndex >= state.mediaItems.length) return;
    const item = state.mediaItems[state.loupeIndex];

    dom.loupeImg.src = `/api/photos/${item.id}/original`;
    dom.loupeFileName.textContent = item.file_name;
    dom.loupeIndex.textContent = `${state.loupeIndex + 1} / ${state.mediaItems.length}`;

    updateLoupeControls();
  }

  function updateLoupeControls() {
    const item = state.mediaItems[state.loupeIndex];
    if (!item) return;

    renderStarWidget(dom.loupeRating, item.rating || 0, (newRating) => {
      updateItemRating(item.id, newRating);
    });

    const pickBtn = dom.loupeFlag.querySelector('.flag-pick');
    const rejectBtn = dom.loupeFlag.querySelector('.flag-reject');
    pickBtn.classList.toggle('active', item.flag === 1);
    rejectBtn.classList.toggle('active', item.flag === -1);
  }

  function loupeNext() {
    if (state.mediaItems.length === 0) return;
    state.loupeIndex = (state.loupeIndex + 1) % state.mediaItems.length;
    updateLoupeView();
  }

  function loupePrev() {
    if (state.mediaItems.length === 0) return;
    state.loupeIndex = (state.loupeIndex - 1 + state.mediaItems.length) % state.mediaItems.length;
    updateLoupeView();
  }

  // --- Import Workflow ---
  async function triggerImport(path, recursive) {
    try {
      dom.importProgressBox.style.display = 'flex';
      dom.importProgressBar.style.width = '5%';
      dom.importStatusCounts.textContent = 'Import started...';
      dom.startImportBtn.disabled = true;

      await api.post('/api/import', { path, recursive });

      // Poll progress
      if (state.importPollInterval) clearInterval(state.importPollInterval);
      state.importPollInterval = setInterval(async () => {
        try {
          const prog = await api.get('/api/import/progress');
          const total = prog.total_files || 0;
          const processed = prog.processed_files || 0;
          const imported = prog.imported_files || 0;
          const skipped = prog.skipped_files || 0;
          const failed = prog.failed_files || 0;

          if (total > 0) {
            const percent = Math.min(100, Math.round((processed / total) * 100));
            dom.importProgressBar.style.width = `${percent}%`;
          }

          dom.importStatusCounts.textContent =
            `Processed: ${processed} / ${total} (Imported: ${imported}, Skipped: ${skipped}, Failed: ${failed})`;
          dom.importCurrentFile.textContent = prog.current_file || '';

          if (!prog.is_running && processed >= total) {
            clearInterval(state.importPollInterval);
            state.importPollInterval = null;
            dom.importStatusCounts.textContent = `Completed! ${imported} imported, ${skipped} skipped.`;
            dom.startImportBtn.disabled = false;
            setTimeout(() => {
              dom.importModal.style.display = 'none';
              dom.importProgressBox.style.display = 'none';
              loadMetadata();
              loadMedia();
            }, 1200);
          }
        } catch (pollErr) {
          console.warn('Progress poll error:', pollErr);
        }
      }, 500);

    } catch (err) {
      alert(`Import error: ${err.message}`);
      dom.startImportBtn.disabled = false;
      dom.importProgressBox.style.display = 'none';
    }
  }

  // --- Event Listeners Setup ---
  function setupEventListeners() {
    // Deselect on background click
    if (dom.gridScrollContainer) {
      dom.gridScrollContainer.addEventListener('click', (e) => {
        if (!e.target.closest('.photo-card')) {
          state.selectedIds.clear();
          document.querySelectorAll('.photo-card.selected').forEach(c => c.classList.remove('selected'));
          updateBatchBar();
          updateInspector();
        }
      });
    }

    // Zoom slider
    dom.zoomSlider.addEventListener('input', (e) => {
      document.documentElement.style.setProperty('--thumb-size', `${e.target.value}px`);
    });

    // Search with debounce
    let searchTimer = null;
    dom.searchInput.addEventListener('input', (e) => {
      clearTimeout(searchTimer);
      searchTimer = setTimeout(() => {
        state.searchText = e.target.value.trim();
        loadMedia();
      }, 300);
    });

    dom.clearSearchBtn.addEventListener('click', () => {
      dom.searchInput.value = '';
      state.searchText = '';
      state.activeFolder = null;
      updateSidebarActive();
      loadMedia();
    });

    // Refresh button
    dom.refreshBtn.addEventListener('click', () => {
      loadMetadata();
      loadMedia();
    });

    // Sort selector
    dom.sortSelect.addEventListener('change', (e) => {
      const [field, dir] = e.target.value.split('-');
      state.sortBy = field;
      state.sortDesc = (dir === 'desc');
      loadMedia();
    });

    // Clear filters button
    dom.clearFiltersBtn.addEventListener('click', clearAllFilters);
    dom.resetTimelineBtn.addEventListener('click', () => {
      state.activeTimelinePeriod = null;
      renderTimeline();
      loadMedia();
    });

    // Tab Switcher (Media, People, Places, Events)
    dom.viewTabs.querySelectorAll('.tab-btn').forEach(btn => {
      btn.addEventListener('click', () => {
        dom.viewTabs.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
        btn.classList.add('active');
        state.activeTab = btn.dataset.tab;

        // Automatically filter by category when switching tabs
        if (state.activeTab !== 'media') {
          const catTags = state.tags.filter(t => t.category.toLowerCase() === state.activeTab.toLowerCase());
          if (catTags.length > 0) {
            state.activeTagId = catTags[0].id;
          }
        } else {
          state.activeTagId = null;
        }
        updateSidebarActive();
        loadMedia();
      });
    });

    // Nav sidebar filters (all, picks, rejects, unrated)
    dom.navAllMedia.addEventListener('click', () => {
      clearAllFilters();
    });
    dom.navPicks.addEventListener('click', () => {
      state.activeNavFilter = 'picks';
      state.activeTagId = null;
      state.activeAlbumId = null;
      updateSidebarActive();
      loadMedia();
    });
    dom.navRejects.addEventListener('click', () => {
      state.activeNavFilter = 'rejects';
      state.activeTagId = null;
      state.activeAlbumId = null;
      updateSidebarActive();
      loadMedia();
    });
    dom.navUnrated.addEventListener('click', () => {
      state.activeNavFilter = 'unrated';
      state.activeTagId = null;
      state.activeAlbumId = null;
      updateSidebarActive();
      loadMedia();
    });

    // Tag category headers collapsible
    document.querySelectorAll('.category-header').forEach(hdr => {
      hdr.addEventListener('click', () => {
        const group = hdr.closest('.tag-category-group');
        const items = group.querySelector('.tag-items');
        const arrow = hdr.querySelector('.arrow');
        if (items.style.display === 'none') {
          items.style.display = 'block';
          arrow.textContent = '▼';
        } else {
          items.style.display = 'none';
          arrow.textContent = '▶';
        }
      });
    });

    // Inspector toggle
    dom.toggleInspectorBtn.addEventListener('click', () => {
      dom.rightInspector.classList.toggle('collapsed');
    });
    dom.closeInspectorBtn.addEventListener('click', () => {
      dom.rightInspector.classList.add('collapsed');
    });
    dom.openLoupeFromInspector.addEventListener('click', () => {
      if (state.selectedIds.size > 0) {
        openLoupeForMedia(Array.from(state.selectedIds)[0]);
      }
    });

    // Inspector flag pick/reject
    const inspectorPickBtn = dom.inspectorFlag.querySelector('.flag-pick');
    const inspectorRejectBtn = dom.inspectorFlag.querySelector('.flag-reject');
    inspectorPickBtn.addEventListener('click', () => {
      if (state.selectedIds.size === 0) return;
      const id = Array.from(state.selectedIds)[0];
      const item = state.mediaItems.find(m => m.id === id);
      const newFlag = item && item.flag === 1 ? 0 : 1;
      updateItemFlag(id, newFlag);
    });
    inspectorRejectBtn.addEventListener('click', () => {
      if (state.selectedIds.size === 0) return;
      const id = Array.from(state.selectedIds)[0];
      const item = state.mediaItems.find(m => m.id === id);
      const newFlag = item && item.flag === -1 ? 0 : -1;
      updateItemFlag(id, newFlag);
    });

    // Inspector add tag inline
    async function handleAddTagInline() {
      const tagName = dom.addTagInput.value.trim();
      if (!tagName || state.selectedIds.size === 0) return;
      const id = Array.from(state.selectedIds)[0];
      const category = dom.addTagCategorySelect ? dom.addTagCategorySelect.value : 'keyword';

      try {
        await api.post(`/api/media/${id}/tags`, { name: tagName, category });
        dom.addTagInput.value = '';
        updateInspector();
        loadMetadata();
      } catch (err) {
        alert(`Failed to add tag: ${err.message}`);
      }
    }
    dom.addTagBtn.addEventListener('click', handleAddTagInline);
    dom.addTagInput.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') handleAddTagInline();
    });
    if (dom.addTagCategorySelect) {
      dom.addTagInput.addEventListener('input', () => {
        const val = dom.addTagInput.value.trim().toLowerCase();
        const match = state.tags.find(t => t.name.toLowerCase() === val);
        if (match && match.category) {
          dom.addTagCategorySelect.value = match.category.toLowerCase();
        }
      });
    }

    // Batch Action Bar handlers
    dom.batchClearBtn.addEventListener('click', () => {
      state.selectedIds.clear();
      document.querySelectorAll('.photo-card.selected').forEach(c => c.classList.remove('selected'));
      updateBatchBar();
      updateInspector();
    });

    dom.batchPickBtn.addEventListener('click', async () => {
      for (const id of state.selectedIds) {
        await updateItemFlag(id, 1);
      }
    });

    dom.batchRejectBtn.addEventListener('click', async () => {
      for (const id of state.selectedIds) {
        await updateItemFlag(id, -1);
      }
    });

    renderStarWidget(dom.batchRating, 0, async (newRating) => {
      for (const id of state.selectedIds) {
        await updateItemRating(id, newRating);
      }
    });

    dom.batchAddTagBtn.addEventListener('click', async () => {
      const name = prompt('Enter tag name to add to all selected photos:');
      if (!name || !name.trim()) return;
      const trimmed = name.trim();
      const existing = state.tags.find(t => t.name.toLowerCase() === trimmed.toLowerCase());
      const cat = existing && existing.category ? existing.category.toLowerCase() : 'keyword';
      for (const id of state.selectedIds) {
        try {
          await api.post(`/api/media/${id}/tags`, { name: trimmed, category: cat });
        } catch (e) {
          console.warn(e);
        }
      }
      updateInspector();
      loadMetadata();
    });

    // Loupe navigation
    dom.loupePrevBtn.addEventListener('click', loupePrev);
    dom.loupeNextBtn.addEventListener('click', loupeNext);
    dom.loupeCloseBtn.addEventListener('click', closeLoupe);
    dom.loupeBackdrop.addEventListener('click', closeLoupe);

    const loupePickBtn = dom.loupeFlag.querySelector('.flag-pick');
    const loupeRejectBtn = dom.loupeFlag.querySelector('.flag-reject');
    loupePickBtn.addEventListener('click', () => {
      if (state.loupeIndex < 0) return;
      const item = state.mediaItems[state.loupeIndex];
      const newFlag = item.flag === 1 ? 0 : 1;
      updateItemFlag(item.id, newFlag);
    });
    loupeRejectBtn.addEventListener('click', () => {
      if (state.loupeIndex < 0) return;
      const item = state.mediaItems[state.loupeIndex];
      const newFlag = item.flag === -1 ? 0 : -1;
      updateItemFlag(item.id, newFlag);
    });

    // Global Keyboard Shortcuts
    window.addEventListener('keydown', (e) => {
      // If typing in input/textarea, ignore shortcuts
      if (['INPUT', 'TEXTAREA'].includes(document.activeElement.tagName)) return;

      // Loupe mode active
      if (state.loupeIndex >= 0) {
        if (e.key === 'ArrowRight') {
          loupeNext();
        } else if (e.key === 'ArrowLeft') {
          loupePrev();
        } else if (e.key === 'Escape') {
          closeLoupe();
        } else if (e.key >= '0' && e.key <= '5') {
          const item = state.mediaItems[state.loupeIndex];
          if (item) updateItemRating(item.id, parseInt(e.key, 10));
        } else if (e.key === 'Delete' || e.key === 'x' || e.key === 'X') {
          const item = state.mediaItems[state.loupeIndex];
          if (item) updateItemFlag(item.id, -1);
        } else if (e.key === 'p' || e.key === 'P') {
          const item = state.mediaItems[state.loupeIndex];
          if (item) updateItemFlag(item.id, 1);
        } else if (e.key === 'u' || e.key === 'U') {
          const item = state.mediaItems[state.loupeIndex];
          if (item) updateItemFlag(item.id, 0);
        }
      } else {
        // Grid mode shortcuts
        if ((e.ctrlKey || e.metaKey) && (e.key === 'a' || e.key === 'A')) {
          e.preventDefault();
          state.selectedIds.clear();
          state.mediaItems.forEach(item => state.selectedIds.add(item.id));
          document.querySelectorAll('.photo-card').forEach(c => c.classList.add('selected'));
          updateBatchBar();
          updateInspector();
        } else if (e.key === 'Escape') {
          state.selectedIds.clear();
          document.querySelectorAll('.photo-card.selected').forEach(c => c.classList.remove('selected'));
          updateBatchBar();
          updateInspector();
        } else if (e.key === ' ' || e.key === 'Enter') {
          if (state.selectedIds.size > 0) {
            e.preventDefault();
            openLoupeForMedia(Array.from(state.selectedIds)[0]);
          }
        } else if (e.key >= '0' && e.key <= '5') {
          const rating = parseInt(e.key, 10);
          state.selectedIds.forEach(id => updateItemRating(id, rating));
        } else if (e.key === 'p' || e.key === 'P') {
          state.selectedIds.forEach(id => updateItemFlag(id, 1));
        } else if (e.key === 'x' || e.key === 'X' || e.key === 'Delete') {
          state.selectedIds.forEach(id => updateItemFlag(id, -1));
        } else if (e.key === 'u' || e.key === 'U') {
          state.selectedIds.forEach(id => updateItemFlag(id, 0));
        }
      }
    });

    // Import modal
    function openImportModal() {
      dom.importModal.style.display = 'flex';
      dom.importPathInput.focus();
    }
    function closeImportModal() {
      dom.importModal.style.display = 'none';
      dom.importProgressBox.style.display = 'none';
    }
    dom.importBtn.addEventListener('click', openImportModal);
    dom.emptyImportBtn.addEventListener('click', openImportModal);
    dom.closeImportModalBtn.addEventListener('click', closeImportModal);
    dom.cancelImportBtn.addEventListener('click', closeImportModal);
    dom.importBackdrop.addEventListener('click', closeImportModal);

    dom.startImportBtn.addEventListener('click', () => {
      const path = dom.importPathInput.value.trim();
      if (!path) {
        alert('Please provide a directory path');
        return;
      }
      triggerImport(path, dom.importRecursiveCheck.checked);
    });

    // New Album Modal
    dom.newAlbumBtn.addEventListener('click', () => {
      dom.newAlbumModal.style.display = 'flex';
      dom.albumNameInput.value = '';
      dom.albumDescInput.value = '';
      dom.albumNameInput.focus();
    });
    function closeAlbumModal() { dom.newAlbumModal.style.display = 'none'; }
    dom.closeNewAlbumModalBtn.addEventListener('click', closeAlbumModal);
    dom.cancelAlbumBtn.addEventListener('click', closeAlbumModal);
    dom.newAlbumBackdrop.addEventListener('click', closeAlbumModal);

    dom.createAlbumSubmitBtn.addEventListener('click', async () => {
      const name = dom.albumNameInput.value.trim();
      if (!name) {
        alert('Album name is required');
        return;
      }
      try {
        await api.post('/api/albums', {
          name,
          description: dom.albumDescInput.value.trim()
        });
        closeAlbumModal();
        loadMetadata();
      } catch (err) {
        alert(`Failed to create album: ${err.message}`);
      }
    });

    // New Tag Modal
    dom.newTagBtn.addEventListener('click', () => {
      dom.newTagModal.style.display = 'flex';
      dom.tagNameInput.value = '';
      dom.tagNameInput.focus();
    });
    function closeTagModal() { dom.newTagModal.style.display = 'none'; }
    dom.closeNewTagModalBtn.addEventListener('click', closeTagModal);
    dom.cancelTagBtn.addEventListener('click', closeTagModal);
    dom.newTagBackdrop.addEventListener('click', closeTagModal);

    dom.createTagSubmitBtn.addEventListener('click', async () => {
      const name = dom.tagNameInput.value.trim();
      if (!name) {
        alert('Tag name is required');
        return;
      }
      try {
        await api.post('/api/tags', {
          name,
          category: dom.tagCategorySelect.value
        });
        closeTagModal();
        loadMetadata();
      } catch (err) {
        alert(`Failed to create tag: ${err.message}`);
      }
    });
  }

  // --- Initializer ---
  async function init() {
    setupEventListeners();
    await loadMetadata();
    await loadMedia();
  }

  // Start app when DOM ready
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
