/**
 * IMAGINE Photo Organizer - Map View & Geotagging
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

import {
  api,
  escapeHtml,
  numeric,
  hasValidGps,
  formatDateTime,
  getOriginalMediaUrl
} from './api.js';
import { state, getGpsMediaItems, invalidateGpsCache } from './state.js';
import { dom, showToast } from './dom.js';
import {
  handleCardSelection,
  updateItemRating,
  updateItemFlag,
  loadMoreMedia,
  renderGrid
} from './media-grid.js';
import { updateInspector } from './inspector.js';
import { openLoupeForMedia } from './loupe.js';

let mapSearchAbortController = null;
const geocodeCache = new Map();
let currentMapSearchId = 0;
let lastNominatimRequestTime = 0;

export function switchViewMode(mode) {
  state.viewMode = mode;
  if (mode === 'map') {
    if (dom.viewGridBtn) dom.viewGridBtn.classList.remove('active');
    if (dom.viewMapBtn) dom.viewMapBtn.classList.add('active');
    if (dom.gridScrollContainer) dom.gridScrollContainer.style.display = 'none';
    if (dom.mapViewContainer) dom.mapViewContainer.style.display = 'flex';
    initMap();
    if (state.mapInstance) {
      state.mapInstance.invalidateSize();
      setTimeout(() => {
        if (state.mapInstance) state.mapInstance.invalidateSize();
      }, 50);
      renderMapMarkers();
      renderUnmappedTray();
      fitMapToBounds();
    }
  } else {
    if (dom.viewGridBtn) dom.viewGridBtn.classList.add('active');
    if (dom.viewMapBtn) dom.viewMapBtn.classList.remove('active');
    if (dom.gridScrollContainer) dom.gridScrollContainer.style.display = 'block';
    if (dom.mapViewContainer) dom.mapViewContainer.style.display = 'none';
    exitPlacementMode();
    renderGrid();
  }
}

export function initMap() {
  if (!window.L || state.mapInstance || !dom.leafletMap) return;
  try {
    state.mapInstance = L.map(dom.leafletMap, {
      center: [30, 0],
      zoom: 2,
      zoomControl: true,
      attributionControl: false,
      keyboard: false
    });
    L.control.attribution({ prefix: false }).addTo(state.mapInstance);

    L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png', {
      maxZoom: 19,
      attribution: '&copy; OpenStreetMap contributors'
    }).addTo(state.mapInstance);

    state.markerLayerGroup = L.layerGroup().addTo(state.mapInstance);

    let mapZoomDebounceTimer = null;
    state.mapInstance.on('zoomend', () => {
      clearTimeout(mapZoomDebounceTimer);
      mapZoomDebounceTimer = setTimeout(() => {
        renderMapMarkers();
      }, 60);
    });

    state.mapInstance.on('click', async (e) => {
      if (state.placementMediaIds && state.placementMediaIds.size > 0) {
        const { lat, lng } = e.latlng;
        const ids = Array.from(state.placementMediaIds);
        await applyGeotagBatch(ids, lat, lng);
      } else if (state.placementMediaId) {
        const { lat, lng } = e.latlng;
        await applyGeotag(state.placementMediaId, lat, lng);
      }
    });
  } catch (err) {
    console.error('Failed to initialize Leaflet map:', err);
  }
}

export function fitMapToBounds() {
  if (!state.mapInstance) return;
  const gpsItems = state.mediaItems.filter(hasValidGps);
  if (gpsItems.length === 0) return;
  try {
    const latLngs = gpsItems.map(it => [it.exif.latitude, it.exif.longitude]);
    state.mapInstance.fitBounds(L.latLngBounds(latLngs).pad(0.25), { maxZoom: 14 });
  } catch (e) {}
}

export function clusterGpsItems(gpsItems, map, radius = 50) {
  if (!map || gpsItems.length === 0) return [];
  const zoom = map.getZoom();

  // Sort items: selected items first, then newest date / id for stability
  const sortedItems = [...gpsItems].sort((a, b) => {
    const aSel = state.selectedIds.has(a.id) ? 1 : 0;
    const bSel = state.selectedIds.has(b.id) ? 1 : 0;
    if (aSel !== bSel) return bSel - aSel;
    if (a.date_taken && b.date_taken) return b.date_taken - a.date_taken;
    return b.id - a.id;
  });

  const clusters = [];
  const grid = new Map();

  for (const item of sortedItems) {
    const lat = item.exif.latitude;
    const lng = item.exif.longitude;
    const pt = map.project([lat, lng], zoom);

    const cx = Math.floor(pt.x / radius);
    const cy = Math.floor(pt.y / radius);

    let closestCluster = null;
    let minDist = radius;

    // Search 3x3 neighboring grid cells
    for (let dx = -1; dx <= 1; dx++) {
      for (let dy = -1; dy <= 1; dy++) {
        const bucket = grid.get(`${cx + dx},${cy + dy}`);
        if (!bucket) continue;
        for (const cl of bucket) {
          const dist = pt.distanceTo(cl.centerPt);
          if (dist < minDist) {
            minDist = dist;
            closestCluster = cl;
          }
        }
      }
    }

    if (closestCluster) {
      closestCluster.items.push(item);
      const n = closestCluster.items.length;
      closestCluster.lat = (closestCluster.lat * (n - 1) + lat) / n;
      closestCluster.lng = (closestCluster.lng * (n - 1) + lng) / n;
      closestCluster.centerPt = map.project([closestCluster.lat, closestCluster.lng], zoom);

      const newCx = Math.floor(closestCluster.centerPt.x / radius);
      const newCy = Math.floor(closestCluster.centerPt.y / radius);
      const newKey = `${newCx},${newCy}`;
      if (newKey !== closestCluster.gridKey) {
        const oldBucket = grid.get(closestCluster.gridKey);
        if (oldBucket) {
          const idx = oldBucket.indexOf(closestCluster);
          if (idx !== -1) oldBucket.splice(idx, 1);
        }
        closestCluster.gridKey = newKey;
        if (!grid.has(newKey)) grid.set(newKey, []);
        grid.get(newKey).push(closestCluster);
      }
    } else {
      const newCluster = {
        items: [item],
        repItem: item,
        centerPt: pt,
        lat: lat,
        lng: lng,
        gridKey: `${cx},${cy}`
      };
      clusters.push(newCluster);
      const key = `${cx},${cy}`;
      if (!grid.has(key)) grid.set(key, []);
      grid.get(key).push(newCluster);
    }
  }

  return clusters;
}

export function renderMapMarkers(force = false) {
  if (!state.mapInstance) return;

  // Preserve open popup media ID across re-clustering on zoom changes
  let openMediaId = null;
  if (state.activeMapMarker && state.activeMapMarker.getPopup && state.activeMapMarker.getPopup() && state.activeMapMarker.getPopup().isOpen()) {
    openMediaId = state.activeMapMarker.activeMediaId ||
      (state.activeMapMarker.clusterItems && state.activeMapMarker.clusterItems[0] ? state.activeMapMarker.clusterItems[0].id : null);
  }
  if (!openMediaId && state.mapMarkers && state.mapMarkers.length > 0) {
    for (const m of state.mapMarkers) {
      if (m.getPopup && m.getPopup() && m.getPopup().isOpen() && m.clusterItems) {
        openMediaId = m.activeMediaId || (m.clusterItems[0] ? m.clusterItems[0].id : null);
        break;
      }
    }
  }

  const gpsItems = getGpsMediaItems();

  if (dom.mapPhotoCount) {
    dom.mapPhotoCount.textContent = gpsItems.length;
  }

  if (dom.mapLoadMoreBtn) {
    if (state.mediaItems.length < state.totalCount) {
      dom.mapLoadMoreBtn.style.display = 'inline-block';
      dom.mapLoadMoreBtn.textContent = `Load More (${state.mediaItems.length}/${state.totalCount})`;
    } else {
      dom.mapLoadMoreBtn.style.display = 'none';
    }
  }
  if (dom.mapLoadAllBtn) {
    if (state.mediaItems.length < state.totalCount) {
      dom.mapLoadAllBtn.style.display = 'inline-block';
    } else {
      dom.mapLoadAllBtn.style.display = 'none';
    }
  }

  const currentZoom = state.mapInstance.getZoom();
  const itemsSig = gpsItems.map(i => `${i.id}:${i.exif ? (i.exif.latitude + ',' + i.exif.longitude) : ''}`).join(';');

  // Avoid expensive map marker rebuild if zoom and items haven't changed
  if (!force && state._lastMapZoom === currentZoom && state._lastMapItemsSig === itemsSig) {
    updateMapMarkerSelections();
    if (openMediaId) {
      const targetMarker = state.mapMarkers.find(m => m.clusterItems && m.clusterItems.some(i => i.id === openMediaId));
      if (targetMarker && (!state.activeMapMarker || state.activeMapMarker !== targetMarker || !targetMarker.getPopup()?.isOpen())) {
        targetMarker.activeMediaId = openMediaId;
        state.activeMapMarker = targetMarker;
        targetMarker.openPopup();
      }
    }
    return;
  }

  state._lastMapZoom = currentZoom;
  state._lastMapItemsSig = itemsSig;

  if (state.markerLayerGroup) {
    state.markerLayerGroup.clearLayers();
  } else {
    state.mapMarkers.forEach(m => m.remove());
  }
  state.mapMarkers = [];

  const clusters = clusterGpsItems(gpsItems, state.mapInstance, 50);

  clusters.forEach((cluster) => {
    const items = cluster.items;
    const repItem = (openMediaId && items.find(i => i.id === openMediaId)) ||
                    items.find(i => state.selectedIds.has(i.id)) ||
                    cluster.repItem || items[0];
    const lat = cluster.lat !== undefined ? cluster.lat : repItem.exif.latitude;
    const lng = cluster.lng !== undefined ? cluster.lng : repItem.exif.longitude;
    const count = items.length;
    const safeFileName = escapeHtml(repItem.file_name);
    const thumbUrl = repItem.content_hash
      ? `/api/thumbnails/${encodeURIComponent(repItem.content_hash)}/256`
      : getOriginalMediaUrl(repItem);

    const isSelected = items.some(i => state.selectedIds.has(i.id));
    const countBadge = count > 1 ? `<span class="photo-pin-count">${count}</span>` : '';

    const pinIcon = L.divIcon({
      className: 'custom-photo-pin',
      html: `
        <div class="photo-pin-inner ${isSelected ? 'selected' : ''}" data-id="${repItem.id}">
          <img src="${thumbUrl}" alt="${safeFileName}" class="photo-pin-thumb" />
          ${countBadge}
        </div>
        <div class="photo-pin-pointer"></div>
      `,
      iconSize: [44, 50],
      iconAnchor: [22, 50],
      popupAnchor: [0, -48]
    });

    const marker = L.marker([lat, lng], { icon: pinIcon });
    if (state.markerLayerGroup) {
      marker.addTo(state.markerLayerGroup);
    } else {
      marker.addTo(state.mapInstance);
    }
    marker.clusterItems = items;
    marker.activeMediaId = repItem.id;
    marker.bindPopup(() => createMapPopupElement(items, marker.activeMediaId), { maxWidth: 280, autoPan: true, autoPanPadding: [80, 80] });
    marker.on('popupopen', () => {
      state.activeMapMarker = marker;
    });
    marker.on('popupclose', () => {
      if (state.activeMapMarker === marker) {
        state.activeMapMarker = null;
      }
    });
    marker.on('click', () => {
      const id = marker.activeMediaId || repItem.id;
      handleCardSelection(id, { shiftKey: false, ctrlKey: false, metaKey: false });
    });
    state.mapMarkers.push(marker);
  });

  if (openMediaId) {
    const targetMarker = state.mapMarkers.find(m => m.clusterItems && m.clusterItems.some(i => i.id === openMediaId));
    if (targetMarker) {
      targetMarker.activeMediaId = openMediaId;
      state.activeMapMarker = targetMarker;
      targetMarker.openPopup();
    } else {
      state.activeMapMarker = null;
    }
  } else {
    state.activeMapMarker = null;
  }
}

export function createMapPopupElement(items, initialItemId) {
  let activeIndex = 0;
  if (initialItemId) {
    const idx = items.findIndex(i => i.id === initialItemId);
    if (idx !== -1) activeIndex = idx;
  } else {
    const selIdx = items.findIndex(i => state.selectedIds.has(i.id));
    if (selIdx !== -1) activeIndex = selIdx;
  }

  const container = document.createElement('div');
  container.className = 'map-popup-card';

  function renderActive() {
    container.innerHTML = '';
    if (activeIndex < 0 || activeIndex >= items.length) {
      activeIndex = 0;
    }
    const rawItem = items[activeIndex];
    if (!rawItem) return;

    const liveItem = state.mediaItems.find(m => m.id === rawItem.id);
    if (!liveItem) {
      const nextValidIdx = items.findIndex(i => state.mediaItems.some(m => m.id === i.id));
      if (nextValidIdx !== -1) {
        activeIndex = nextValidIdx;
        renderActive();
      } else {
        const marker = state.mapMarkers.find(m => m.clusterItems === items) || state.activeMapMarker;
        if (marker && marker.closePopup) marker.closePopup();
      }
      return;
    }
    const item = liveItem;

    container.dataset.mediaId = item.id;
    container.renderActive = renderActive;
    const exif = item.exif || {};
    const safeFileName = escapeHtml(item.file_name);
    const thumbUrl = item.content_hash
      ? `/api/thumbnails/${encodeURIComponent(item.content_hash)}/256`
      : getOriginalMediaUrl(item);

    // Update marker activeMediaId and pin thumbnail if applicable
    const marker = state.mapMarkers.find(m => m.clusterItems === items) || state.activeMapMarker;
    if (marker) {
      marker.activeMediaId = item.id;
      const el = marker.getElement();
      if (el) {
        const img = el.querySelector('.photo-pin-thumb');
        if (img) {
          img.src = thumbUrl;
          img.alt = item.file_name || '';
        }
      }
    }

    if (items.length > 1) {
      const nav = document.createElement('div');
      nav.className = 'map-popup-nav';
      nav.innerHTML = `
        <button class="btn-icon-sm" id="popPrevBtn" title="Previous photo">&lt;</button>
        <span>${activeIndex + 1} of ${items.length}</span>
        <button class="btn-icon-sm" id="popNextBtn" title="Next photo">&gt;</button>
      `;
      nav.querySelector('#popPrevBtn').onclick = (e) => {
        e.stopPropagation();
        activeIndex = (activeIndex - 1 + items.length) % items.length;
        const curItem = items[activeIndex];
        if (marker && curItem) {
          marker.activeMediaId = curItem.id;
        }
        renderActive();
        if (curItem) {
          handleCardSelection(curItem.id, { shiftKey: false, ctrlKey: false, metaKey: false });
        }
      };
      nav.querySelector('#popNextBtn').onclick = (e) => {
        e.stopPropagation();
        activeIndex = (activeIndex + 1) % items.length;
        const curItem = items[activeIndex];
        if (marker && curItem) {
          marker.activeMediaId = curItem.id;
        }
        renderActive();
        if (curItem) {
          handleCardSelection(curItem.id, { shiftKey: false, ctrlKey: false, metaKey: false });
        }
      };
      container.appendChild(nav);
    }

    const thumbWrap = document.createElement('div');
    thumbWrap.className = 'map-popup-thumb-wrap';
    thumbWrap.innerHTML = `
      <img src="${thumbUrl}" alt="${safeFileName}" class="map-popup-thumb">
      <div class="loupe-hint">🔍 View Loupe</div>
    `;
    thumbWrap.onclick = () => {
      openLoupeForMedia(item.id);
    };
    container.appendChild(thumbWrap);

    const body = document.createElement('div');
    body.className = 'map-popup-body';
    const latNum = numeric(exif.latitude);
    const lonNum = numeric(exif.longitude);
    const lat = latNum !== null ? latNum.toFixed(5) : '-';
    const lon = lonNum !== null ? lonNum.toFixed(5) : '-';

    let starsHtml = '';
    for (let s = 1; s <= 5; s++) {
      const activeClass = s <= (item.rating || 0) ? 'active' : '';
      starsHtml += `<span class="${activeClass}" data-star="${s}">★</span>`;
    }

    body.innerHTML = `
      <div class="map-popup-title" title="${safeFileName}">${safeFileName}</div>
      <div class="map-popup-meta">
        <span>${formatDateTime(item.date_taken)}</span>
        <span class="map-popup-coords">📍 ${lat}, ${lon}</span>
      </div>
      <div class="map-popup-actions">
        <div class="map-popup-rating" data-id="${item.id}">
          ${starsHtml}
        </div>
        <div class="map-popup-btns">
          <button class="btn btn-xs ${item.flag === 1 ? 'btn-primary' : 'btn-secondary'}" id="popPickBtn" title="Pick">✔</button>
          <button class="btn btn-xs ${item.flag === -1 ? 'btn-danger' : 'btn-secondary'}" id="popRejectBtn" title="Reject">✖</button>
        </div>
      </div>
    `;

    body.querySelectorAll('.map-popup-rating span').forEach(starEl => {
      starEl.onclick = (e) => {
        e.stopPropagation();
        const curItem = state.mediaItems.find(m => m.id === item.id);
        if (!curItem) return;
        const star = parseInt(starEl.dataset.star, 10);
        const newRating = curItem.rating === star ? 0 : star;
        updateItemRating(curItem.id, newRating);
        curItem.rating = newRating;
        renderActive();
      };
    });

    const pickBtn = body.querySelector('#popPickBtn');
    if (pickBtn) {
      pickBtn.onclick = (e) => {
        e.stopPropagation();
        const curItem = state.mediaItems.find(m => m.id === item.id);
        if (!curItem) return;
        const newFlag = curItem.flag === 1 ? 0 : 1;
        updateItemFlag(curItem.id, newFlag);
        curItem.flag = newFlag;
        renderActive();
      };
    }

    const rejectBtn = body.querySelector('#popRejectBtn');
    if (rejectBtn) {
      rejectBtn.onclick = (e) => {
        e.stopPropagation();
        const curItem = state.mediaItems.find(m => m.id === item.id);
        if (!curItem) return;
        const newFlag = curItem.flag === -1 ? 0 : -1;
        updateItemFlag(curItem.id, newFlag);
        curItem.flag = newFlag;
        renderActive();
      };
    }

    container.appendChild(body);
  }

  renderActive();
  return container;
}

export function patchOpenMapPopup(id) {
  let popupCard = null;
  if (state.activeMapMarker && state.activeMapMarker.getPopup && state.activeMapMarker.getPopup() && state.activeMapMarker.getPopup().isOpen()) {
    const popupEl = state.activeMapMarker.getPopup().getElement();
    if (popupEl) {
      popupCard = popupEl.querySelector('.map-popup-card:not(.search-result-popup)');
    }
  }
  if (!popupCard) {
    popupCard = document.querySelector('.map-popup-card:not(.search-result-popup)');
  }
  if (popupCard && popupCard.dataset.mediaId == id && typeof popupCard.renderActive === 'function') {
    popupCard.renderActive();
  }
}

export function updateMapMarkerSelections() {
  if (!state.mapMarkers) return;
  state.mapMarkers.forEach(m => {
    const isSelected = m.clusterItems && m.clusterItems.some(i => state.selectedIds.has(i.id));
    const el = m.getElement();
    if (el) {
      const pinInner = el.querySelector('.photo-pin-inner');
      if (pinInner) pinInner.classList.toggle('selected', !!isSelected);
    }
  });
}

export function updatePlacementModeState() {
  const count = state.placementMediaIds.size;
  state.placementMediaId = count > 0 ? Array.from(state.placementMediaIds)[0] : null;

  if (count > 0) {
    if (dom.mapViewContainer) dom.mapViewContainer.classList.add('placement-mode');
    if (dom.unmappedSelectedCount) {
      dom.unmappedSelectedCount.textContent = `${count} selected`;
      dom.unmappedSelectedCount.style.display = 'inline';
    }
    if (dom.unmappedDeselectAllBtn) dom.unmappedDeselectAllBtn.style.display = 'inline-block';
    if (dom.unmappedTrayTitle) {
      dom.unmappedTrayTitle.textContent = `${count} photo${count > 1 ? 's' : ''} selected — Click anywhere on the map to place`;
    }
  } else {
    if (dom.mapViewContainer) dom.mapViewContainer.classList.remove('placement-mode');
    if (dom.unmappedSelectedCount) dom.unmappedSelectedCount.style.display = 'none';
    if (dom.unmappedDeselectAllBtn) dom.unmappedDeselectAllBtn.style.display = 'none';
    if (dom.unmappedTrayTitle) {
      dom.unmappedTrayTitle.textContent = 'Unmapped Photos — Select photos, then click anywhere on the map to place them';
    }
  }
}

export function handleUnmappedChipClick(id, event, unmappedList) {
  const isMultiKey = event.ctrlKey || event.metaKey;
  const isShiftKey = event.shiftKey;

  if (isShiftKey && state.lastUnmappedClickedId !== null) {
    const ids = unmappedList.map(i => i.id);
    const startIdx = ids.indexOf(state.lastUnmappedClickedId);
    const endIdx = ids.indexOf(id);
    if (startIdx !== -1 && endIdx !== -1) {
      const [low, high] = startIdx < endIdx ? [startIdx, endIdx] : [endIdx, startIdx];
      for (let i = low; i <= high; i++) {
        state.placementMediaIds.add(ids[i]);
      }
    } else {
      state.placementMediaIds.add(id);
      state.lastUnmappedClickedId = id;
    }
  } else if (isMultiKey) {
    if (state.placementMediaIds.has(id)) {
      state.placementMediaIds.delete(id);
    } else {
      state.placementMediaIds.add(id);
    }
    state.lastUnmappedClickedId = id;
  } else {
    if (state.placementMediaIds.has(id) && state.placementMediaIds.size === 1) {
      state.placementMediaIds.clear();
      state.lastUnmappedClickedId = null;
    } else {
      state.placementMediaIds.clear();
      state.placementMediaIds.add(id);
      state.lastUnmappedClickedId = id;
    }
  }

  updatePlacementModeState();
  renderUnmappedTray();
}

export function renderUnmappedTray() {
  if (!dom.unmappedPhotosList) return;
  dom.unmappedPhotosList.innerHTML = '';
  const unmapped = state.mediaItems.filter(item => !hasValidGps(item));

  if (dom.unmappedBtnLabel) {
    if (state.mediaItems.length < state.totalCount) {
      dom.unmappedBtnLabel.textContent = `Unmapped in loaded photos (${unmapped.length})`;
    } else {
      dom.unmappedBtnLabel.textContent = `Unmapped (${unmapped.length})`;
    }
  }

  if (dom.unmappedLoadMoreBtn) {
    if (state.mediaItems.length < state.totalCount) {
      dom.unmappedLoadMoreBtn.style.display = 'inline-block';
      dom.unmappedLoadMoreBtn.textContent = `Load More (${state.mediaItems.length}/${state.totalCount})`;
    } else {
      dom.unmappedLoadMoreBtn.style.display = 'none';
    }
  }

  // Prune IDs no longer unmapped
  const unmappedIdSet = new Set(unmapped.map(i => i.id));
  Array.from(state.placementMediaIds).forEach(id => {
    if (!unmappedIdSet.has(id)) state.placementMediaIds.delete(id);
  });
  updatePlacementModeState();

  unmapped.forEach(item => {
    const isSelected = state.placementMediaIds.has(item.id);
    const chip = document.createElement('div');
    chip.className = 'unmapped-chip' + (isSelected ? ' active' : '');
    chip.dataset.id = item.id;
    const safeFileName = escapeHtml(item.file_name);
    const thumbUrl = item.content_hash
      ? `/api/thumbnails/${encodeURIComponent(item.content_hash)}/256`
      : getOriginalMediaUrl(item);

    const checkBadge = isSelected ? '<div class="unmapped-chip-check">✓</div>' : '';
    chip.innerHTML = `
      <img src="${thumbUrl}" alt="${safeFileName}" loading="lazy">
      ${checkBadge}
      <div class="chip-name">${safeFileName}</div>
    `;

    chip.onclick = (e) => {
      handleUnmappedChipClick(item.id, e, unmapped);
    };

    dom.unmappedPhotosList.appendChild(chip);
  });

  if (state.mediaItems.length < state.totalCount) {
    const moreChip = document.createElement('div');
    moreChip.className = 'unmapped-chip unmapped-load-more';
    moreChip.id = 'unmappedLoadMoreChip';
    moreChip.title = 'Load more photos into catalog';
    moreChip.innerHTML = `<div class="chip-name">+ Load More (${state.mediaItems.length}/${state.totalCount})</div>`;
    moreChip.onclick = (e) => {
      e.stopPropagation();
      loadMoreMedia();
    };
    dom.unmappedPhotosList.appendChild(moreChip);
  }
}

export function enterPlacementMode(mediaId) {
  state.placementMediaIds.add(mediaId);
  state.lastUnmappedClickedId = mediaId;
  updatePlacementModeState();
  renderUnmappedTray();
}

export function exitPlacementMode() {
  state.placementMediaIds.clear();
  state.placementMediaId = null;
  state.lastUnmappedClickedId = null;
  updatePlacementModeState();
  if (dom.unmappedPhotosList) {
    dom.unmappedPhotosList.querySelectorAll('.unmapped-chip').forEach(c => c.classList.remove('active'));
  }
}

export async function applyGeotagBatch(ids, lat, lon, alt = 0.0) {
  if (!ids || ids.length === 0) return;
  try {
    await api.post('/api/media/batch-gps', {
      ids,
      has_gps: true,
      latitude: lat,
      longitude: lon,
      altitude: alt
    });

    ids.forEach(mediaId => {
      const item = state.mediaItems.find(i => i.id === mediaId);
      if (item) {
        item.exif = item.exif || {};
        item.exif.has_gps = true;
        item.exif.latitude = lat;
        item.exif.longitude = lon;
        item.exif.altitude = alt;
      }
    });

    exitPlacementMode();
    invalidateGpsCache();
    renderMapMarkers(true);
    renderUnmappedTray();
    updateInspector();
  } catch (err) {
    console.error('Failed to batch geotag media:', err);
    showToast('Failed to geotag photos: ' + (err.message || 'Server error'), 'error');
  }
}

export async function applyGeotag(mediaId, lat, lon, alt = 0.0) {
  await applyGeotagBatch([mediaId], lat, lon, alt);
}

export async function clearGeotag(mediaId) {
  try {
    await api.post(`/api/media/${mediaId}/gps`, { has_gps: false });
    const item = state.mediaItems.find(i => i.id === mediaId);
    if (item) {
      item.exif = item.exif || {};
      item.exif.has_gps = false;
      item.exif.latitude = 0;
      item.exif.longitude = 0;
      item.exif.altitude = 0;
    }
    invalidateGpsCache();
    renderMapMarkers(true);
    renderUnmappedTray();
    updateInspector();
  } catch (err) {
    console.error('Failed to clear geotag:', err);
    showToast('Failed to clear geotag: ' + (err.message || 'Server error'), 'error');
  }
}

export function clearMapSearch() {
  if (mapSearchAbortController) {
    mapSearchAbortController.abort();
    mapSearchAbortController = null;
  }
  if (dom.mapSearchInput) dom.mapSearchInput.value = '';
  if (dom.clearMapSearchBtn) dom.clearMapSearchBtn.style.display = 'none';
  if (dom.mapSearchSpinner) dom.mapSearchSpinner.style.display = 'none';
  if (dom.mapSearchIcon) dom.mapSearchIcon.style.display = '';
  if (dom.mapSearchBox) dom.mapSearchBox.classList.remove('has-input');
  if (dom.mapSearchResults) {
    dom.mapSearchResults.style.display = 'none';
    dom.mapSearchResults.innerHTML = '';
  }
  state.mapSearchResults = [];
  state.mapSearchActiveIdx = -1;
  if (state.searchMarker) {
    state.searchMarker.remove();
    state.searchMarker = null;
  }
}

export function navigateToMapPlace(title, subtitle, lat, lng, zoom = 13) {
  if (!state.mapInstance) return;

  state.mapInstance.flyTo([lat, lng], zoom, { duration: 0.8 });

  if (state.searchMarker) {
    state.searchMarker.remove();
    state.searchMarker = null;
  }

  const pinIcon = L.divIcon({
    className: 'search-location-pin',
    html: `
      <div class="search-location-inner" title="${escapeHtml(title)}">
        <svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2.5"><circle cx="12" cy="10" r="3"/><path d="M12 2a8 8 0 0 0-8 8c0 5.25 8 12 8 12s8-6.75 8-12a8 8 0 0 0-8-8z"/></svg>
        <div class="search-location-pointer"></div>
      </div>
    `,
    iconSize: [32, 38],
    iconAnchor: [16, 38],
    popupAnchor: [0, -36]
  });

  state.searchMarker = L.marker([lat, lng], { icon: pinIcon }).addTo(state.mapInstance);
  state.searchMarker.bindPopup(() => createSearchPopupElement(title, subtitle, lat, lng), { maxWidth: 280 }).openPopup();
}

export function createSearchPopupElement(title, subtitle, lat, lng) {
  const container = document.createElement('div');
  container.className = 'map-popup-card search-result-popup';

  const latNum = numeric(lat);
  const lngNum = numeric(lng);
  const latStr = latNum !== null ? latNum.toFixed(5) : (lat || '-');
  const lngStr = lngNum !== null ? lngNum.toFixed(5) : (lng || '-');

  let placePhotoBtnHtml = '';
  const selectedCount = state.placementMediaIds.size;
  if (selectedCount > 0) {
    let label = `Place ${selectedCount} Selected Photos Here`;
    if (selectedCount === 1) {
      const firstId = Array.from(state.placementMediaIds)[0];
      const item = state.mediaItems.find(m => m.id === firstId);
      label = item ? `Place "${escapeHtml(item.file_name)}" Here` : 'Place Selected Photo Here';
    }
    placePhotoBtnHtml = `
      <button class="btn btn-xs btn-primary place-here-btn" style="width: 100%; margin-top: 8px;">
        📍 ${label}
      </button>
    `;
  } else if (state.selectedIds.size === 1) {
    const targetId = Array.from(state.selectedIds)[0];
    const targetItem = state.mediaItems.find(m => m.id === targetId);
    if (targetItem) {
      placePhotoBtnHtml = `
        <button class="btn btn-xs btn-primary place-here-btn" style="width: 100%; margin-top: 8px;">
          📍 Place "${escapeHtml(targetItem.file_name)}" Here
        </button>
      `;
    }
  }

  const safeTitle = escapeHtml(title);
  const safeSubtitle = escapeHtml(subtitle);
  container.innerHTML = `
    <div class="map-popup-header" style="margin-bottom: 2px;">
      <span class="map-popup-title">${safeTitle}</span>
    </div>
    ${subtitle ? `<div style="font-size: 11px; color: var(--text-secondary); margin-bottom: 4px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap;">${safeSubtitle}</div>` : ''}
    <div class="map-popup-coords">${latStr}, ${lngStr}</div>
    ${placePhotoBtnHtml}
    <button class="btn btn-xs btn-secondary clear-pin-btn" style="width: 100%; margin-top: 6px;">Remove Pin</button>
  `;

  const placeBtn = container.querySelector('.place-here-btn');
  if (placeBtn) {
    placeBtn.onclick = async () => {
      if (state.placementMediaIds.size > 0) {
        const ids = Array.from(state.placementMediaIds);
        await applyGeotagBatch(ids, parseFloat(lat), parseFloat(lng));
      } else if (state.selectedIds.size === 1) {
        const targetId = Array.from(state.selectedIds)[0];
        await applyGeotag(targetId, parseFloat(lat), parseFloat(lng));
      }
      if (state.searchMarker) {
        state.searchMarker.remove();
        state.searchMarker = null;
      }
    };
  }

  const clearBtn = container.querySelector('.clear-pin-btn');
  if (clearBtn) {
    clearBtn.onclick = () => {
      if (state.searchMarker) {
        state.searchMarker.remove();
        state.searchMarker = null;
      }
    };
  }

  return container;
}

export async function performMapPlaceSearch(rawQuery) {
  const query = rawQuery.trim();
  if (!query) {
    clearMapSearch();
    return;
  }

  if (mapSearchAbortController) {
    mapSearchAbortController.abort();
    mapSearchAbortController = null;
  }
  const abortController = new AbortController();
  mapSearchAbortController = abortController;

  const searchId = ++currentMapSearchId;
  if (dom.clearMapSearchBtn) dom.clearMapSearchBtn.style.display = 'block';
  if (dom.mapSearchSpinner) dom.mapSearchSpinner.style.display = 'block';

  const results = [];

  // 1. Direct coordinates parsing: e.g. "48.8584, 2.2945" or "-33.8688 151.2093"
  const coordMatch = query.match(/^([+-]?\d+(?:\.\d+)?)[,\s]+([+-]?\d+(?:\.\d+)?)$/);
  if (coordMatch) {
    const lat = numeric(coordMatch[1]);
    const lng = numeric(coordMatch[2]);
    if (lat !== null && lng !== null && lat >= -90 && lat <= 90 && lng >= -180 && lng <= 180) {
      results.push({
        title: `Coordinates: ${lat.toFixed(4)}, ${lng.toFixed(4)}`,
        subtitle: 'Custom GPS Coordinates',
        lat: lat,
        lng: lng,
        type: 'coords',
        badge: 'GPS'
      });
    }
    if (dom.mapSearchSpinner) dom.mapSearchSpinner.style.display = 'none';
    state.mapSearchResults = results;
    state.mapSearchActiveIdx = -1;
    renderMapSearchResults(results);
    return;
  }

  // 2. Catalog search: matched photos with GPS (scan only GPS items)
  const lowerQuery = query.toLowerCase();
  getGpsMediaItems().forEach(item => {
    const fileName = String(item.file_name || '');
    if (fileName.toLowerCase().includes(lowerQuery)) {
      const lat = numeric(item.exif.latitude);
      const lng = numeric(item.exif.longitude);
      results.push({
        title: fileName,
        subtitle: `In Catalog · ${lat !== null ? lat.toFixed(4) : '-' }, ${lng !== null ? lng.toFixed(4) : '-' }`,
        lat: lat !== null ? lat : item.exif.latitude,
        lng: lng !== null ? lng : item.exif.longitude,
        type: 'catalog',
        badge: 'Photo'
      });
    }
  });

  // 3. Online Geocoding via OpenStreetMap Nominatim (query length >= 3, cached, cancellable)
  if (query.length >= 3) {
    const cacheKey = lowerQuery;
    let data = null;
    if (geocodeCache.has(cacheKey)) {
      data = geocodeCache.get(cacheKey);
    } else {
      try {
        const now = Date.now();
        const elapsed = now - lastNominatimRequestTime;
        if (elapsed < 1000) {
          await new Promise(r => setTimeout(r, 1000 - elapsed));
        }
        if (searchId !== currentMapSearchId) return;
        lastNominatimRequestTime = Date.now();

        const url = `/api/geocode?q=${encodeURIComponent(query)}&limit=5`;
        const resp = await fetch(url, { signal: abortController.signal, headers: { 'Accept': 'application/json' } });
        if (searchId !== currentMapSearchId) return;
        if (resp.ok) {
          const rawJson = await resp.json();
          if (searchId !== currentMapSearchId) return;
          data = Array.isArray(rawJson) ? rawJson : [];
          geocodeCache.set(cacheKey, data);
        }
      } catch (e) {
        if (e.name === 'AbortError' || searchId !== currentMapSearchId) return;
        console.warn('Nominatim geocoding unavailable or offline:', e);
      }
    }

    if (Array.isArray(data) && searchId === currentMapSearchId) {
      data.forEach(p => {
        if (!p || typeof p !== 'object') return;
        const lat = parseFloat(p.lat);
        const lng = parseFloat(p.lon);
        results.push({
          title: p.name || (p.display_name ? p.display_name.split(',')[0] : query),
          subtitle: p.display_name || '',
          lat: lat,
          lng: lng,
          type: p.type || 'place',
          badge: p.addresstype || p.type || 'Place'
        });
      });
    }
  }

  if (searchId !== currentMapSearchId) return;
  if (dom.mapSearchSpinner) dom.mapSearchSpinner.style.display = 'none';
  state.mapSearchResults = results;
  state.mapSearchActiveIdx = -1;
  renderMapSearchResults(results);
}

export function renderMapSearchResults(results) {
  if (!dom.mapSearchResults) return;
  dom.mapSearchResults.innerHTML = '';

  if (results.length === 0) {
    dom.mapSearchResults.innerHTML = '<div class="map-search-empty">No matching places or coordinates found</div>';
    dom.mapSearchResults.style.display = 'block';
    return;
  }

  dom.mapSearchResults.style.display = 'block';
  results.forEach((r, idx) => {
    const itemEl = document.createElement('div');
    itemEl.className = 'map-search-item';
    itemEl.dataset.idx = idx;

    let iconSvg = '<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 10c0 7-9 13-9 13s-9-6-9-13a9 9 0 0 1 18 0z"/><circle cx="12" cy="10" r="3"/></svg>';
    if (r.type === 'coords') {
      iconSvg = '<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><line x1="12" y1="2" x2="12" y2="6"/><line x1="12" y1="18" x2="12" y2="22"/><line x1="2" y1="12" x2="6" y2="12"/><line x1="18" y1="12" x2="22" y2="12"/></svg>';
    } else if (r.type === 'catalog') {
      iconSvg = '<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="3" width="18" height="18" rx="2" ry="2"/><circle cx="8.5" cy="8.5" r="1.5"/><polyline points="21 15 16 10 5 21"/></svg>';
    }

    const safeTitle = escapeHtml(r.title);
    const safeSubtitle = escapeHtml(r.subtitle);
    const safeBadge = escapeHtml(r.badge);
    itemEl.innerHTML = `
      <div class="item-icon">${iconSvg}</div>
      <div class="item-text">
        <div class="item-title">${safeTitle}</div>
        ${r.subtitle ? `<div class="item-subtitle">${safeSubtitle}</div>` : ''}
      </div>
      ${r.badge ? `<span class="item-badge">${safeBadge}</span>` : ''}
    `;

    itemEl.addEventListener('click', () => {
      selectMapSearchResult(r);
    });

    dom.mapSearchResults.appendChild(itemEl);
  });
}

export function selectMapSearchResult(r) {
  if (!r) return;
  if (dom.mapSearchInput) dom.mapSearchInput.value = r.title;
  if (dom.mapSearchIcon) dom.mapSearchIcon.style.display = 'none';
  if (dom.mapSearchBox) dom.mapSearchBox.classList.add('has-input');
  if (dom.mapSearchResults) dom.mapSearchResults.style.display = 'none';
  if (dom.clearMapSearchBtn) dom.clearMapSearchBtn.style.display = 'block';
  navigateToMapPlace(r.title, r.subtitle, r.lat, r.lng, r.type === 'coords' ? 14 : 12);
}
