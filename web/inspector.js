/**
 * IMAGINE Photo Organizer - Inspector Panel & Resizing
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

import {
  api,
  escapeHtml,
  formatBytes,
  formatDateTime,
  formatExposureTime,
  numeric,
  hasValidGps,
  normalizeMediaItem
} from './api.js';
import { state } from './state.js';
import { dom } from './dom.js';
import { updateItemRating, loadMetadata } from './media-grid.js';

let currentInspectorFetchId = 0;
let inspectorAbortController = null;
let activeResizeObservers = [];

export function patchInspectorRatingAndFlag(item) {
  if (state.selectedIds.size === 0) return;
  const selectedId = Array.from(state.selectedIds)[0];
  if (selectedId !== item.id) return;
  if (dom.inspectorRating) {
    dom.inspectorRating.querySelectorAll('span').forEach(span => {
      const star = parseInt(span.dataset.star, 10);
      span.classList.toggle('active', star <= (item.rating || 0));
    });
  }
  if (dom.inspectorFlag) {
    const pickBtn = dom.inspectorFlag.querySelector('.flag-pick');
    const rejectBtn = dom.inspectorFlag.querySelector('.flag-reject');
    if (pickBtn) pickBtn.classList.toggle('active', item.flag === 1);
    if (rejectBtn) rejectBtn.classList.toggle('active', item.flag === -1);
  }
}

export function renderInspectorContent(item) {
  if (!item) return;
  if (dom.inspectorNoSelection) dom.inspectorNoSelection.style.display = 'none';
  if (dom.inspectorSelection) dom.inspectorSelection.style.display = 'block';

  // Inspector Preview
  const previewUrl = item.content_hash
    ? `/api/thumbnails/${encodeURIComponent(item.content_hash)}/1024`
    : `/api/photos/${item.id}/original`;
  if (dom.inspectorImg) {
    dom.inspectorImg.src = previewUrl;
    dom.inspectorImg.onerror = () => {
      dom.inspectorImg.src = `/api/photos/${item.id}/original`;
    };
  }

  // Rating
  if (dom.inspectorRating) {
    renderStarWidget(dom.inspectorRating, item.rating || 0, (newRating) => {
      updateItemRating(item.id, newRating);
    });
  }

  // Flags
  if (dom.inspectorFlag) {
    const pickBtn = dom.inspectorFlag.querySelector('.flag-pick');
    const rejectBtn = dom.inspectorFlag.querySelector('.flag-reject');
    if (pickBtn) pickBtn.classList.toggle('active', item.flag === 1);
    if (rejectBtn) rejectBtn.classList.toggle('active', item.flag === -1);
  }

  // File Properties
  if (dom.infoFileName) dom.infoFileName.textContent = item.file_name || '-';
  if (dom.infoDimensions) dom.infoDimensions.textContent = item.width && item.height ? `${item.width} × ${item.height} px` : '-';
  if (dom.infoFileSize) dom.infoFileSize.textContent = formatBytes(item.file_size);
  if (dom.infoDateTaken) dom.infoDateTaken.textContent = formatDateTime(item.date_taken);
  if (dom.infoFilePath) dom.infoFilePath.textContent = item.file_path || '-';

  // EXIF properties
  const exif = item.exif || {};
  const cameraStr = [exif.camera_make, exif.camera_model].filter(Boolean).join(' ') || '-';
  if (dom.infoCamera) dom.infoCamera.textContent = cameraStr;
  if (dom.infoLens) dom.infoLens.textContent = exif.lens || '-';
  if (dom.infoExposure) dom.infoExposure.textContent = formatExposureTime(exif.exposure_time) || '-';
  const aperture = numeric(exif.f_number);
  if (dom.infoAperture) dom.infoAperture.textContent = aperture !== null ? `f/${aperture.toFixed(1)}` : '-';
  if (dom.infoIso) dom.infoIso.textContent = (exif.iso !== undefined && exif.iso !== null && exif.iso !== '') ? `ISO ${exif.iso}` : '-';
  const focal = numeric(exif.focal_length);
  if (dom.infoFocal) dom.infoFocal.textContent = focal !== null ? `${focal.toFixed(1)} mm` : '-';

  if (dom.infoGps) {
    if (hasValidGps(item)) {
      const latNum = numeric(exif.latitude);
      const lonNum = numeric(exif.longitude);
      if (latNum !== null && lonNum !== null) {
        const lat = latNum.toFixed(5);
        const lon = lonNum.toFixed(5);
        dom.infoGps.innerHTML = `
          <a href="https://www.openstreetmap.org/?mlat=${lat}&mlon=${lon}#map=16/${lat}/${lon}" target="_blank" rel="noopener" class="btn-link">
            ${lat}, ${lon} ↗
          </a>
        `;
      } else {
        dom.infoGps.textContent = '-';
      }
    } else {
      dom.infoGps.textContent = '-';
    }
  }

  // Tags
  renderInspectorTags(item);

  // Mini-map
  updateInspectorMiniMap(item);
}

export async function updateInspector() {
  if (state.selectedIds.size === 0) {
    if (inspectorAbortController) {
      inspectorAbortController.abort();
      inspectorAbortController = null;
    }
    if (dom.inspectorNoSelection) dom.inspectorNoSelection.style.display = 'block';
    if (dom.inspectorSelection) dom.inspectorSelection.style.display = 'none';
    updateInspectorMiniMap(null);
    return;
  }

  if (inspectorAbortController) {
    inspectorAbortController.abort();
  }
  inspectorAbortController = new AbortController();
  const signal = inspectorAbortController.signal;

  // Pick first selected ID
  const selectedId = Array.from(state.selectedIds)[0];
  let item = state.mediaItems.find(m => m.id === selectedId);

  // Render immediately from memory to eliminate lag
  if (item) {
    renderInspectorContent(item);
  }

  const fetchId = ++currentInspectorFetchId;
  try {
    // Fetch fresh details from API
    const freshRaw = await api.get(`/api/media/${selectedId}`, {}, { signal });
    if (fetchId !== currentInspectorFetchId) return;
    if (!state.selectedIds.has(selectedId)) return;
    const freshItem = normalizeMediaItem(freshRaw);
    if (!freshItem) return;
    const idx = state.mediaItems.findIndex(m => m.id === selectedId);
    if (idx !== -1) {
      const currentItem = state.mediaItems[idx];
      const merged = Object.assign({}, freshItem, {
        rating: (currentItem.rating !== undefined && currentItem.rating !== null) ? currentItem.rating : freshItem.rating,
        flag: (currentItem.flag !== undefined && currentItem.flag !== null) ? currentItem.flag : freshItem.flag
      });
      state.mediaItems[idx] = merged;
      renderInspectorContent(merged);
    } else {
      renderInspectorContent(freshItem);
    }
  } catch (err) {
    if (err.name === 'AbortError') return;
    if (fetchId !== currentInspectorFetchId) return;
    console.warn('Could not fetch single media details:', err);
  }
}

export function renderInspectorTags(item) {
  if (!dom.inspectorTags) return;
  dom.inspectorTags.innerHTML = '';
  const tags = Array.isArray(item && item.tags) ? item.tags : [];

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
      <span>${escapeHtml(tag.name)}</span>
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

export function renderStarWidget(container, currentRating, onSetRating) {
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

export function updateInspectorMiniMap(item) {
  if (!dom.inspectorMiniMap) return;
  if (!item || !window.L) {
    if (dom.inspectorGpsActions) dom.inspectorGpsActions.style.display = 'none';
    if (dom.inspectorPlaceOnMapBtn) dom.inspectorPlaceOnMapBtn.style.display = 'none';
    dom.inspectorMiniMap.style.display = 'none';
    return;
  }

  const exif = item.exif || {};
  if (hasValidGps(item)) {
    if (dom.inspectorGpsActions) dom.inspectorGpsActions.style.display = 'flex';
    if (dom.inspectorPlaceOnMapBtn) dom.inspectorPlaceOnMapBtn.style.display = 'none';
    dom.inspectorMiniMap.style.display = 'block';

    const lat = exif.latitude;
    const lon = exif.longitude;

    if (!state.inspectorMiniMapInstance) {
      state.inspectorMiniMapInstance = L.map(dom.inspectorMiniMap, {
        center: [lat, lon],
        zoom: 12,
        zoomControl: false,
        attributionControl: false,
        dragging: false,
        scrollWheelZoom: false,
        doubleClickZoom: false
      });
      L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png', {
        maxZoom: 19
      }).addTo(state.inspectorMiniMapInstance);
      state.inspectorMiniMarker = L.marker([lat, lon]).addTo(state.inspectorMiniMapInstance);
      setTimeout(() => {
        if (state.inspectorMiniMapInstance) state.inspectorMiniMapInstance.invalidateSize();
      }, 50);
    } else {
      state.inspectorMiniMapInstance.setView([lat, lon], 12);
      state.inspectorMiniMarker.setLatLng([lat, lon]);
      setTimeout(() => {
        if (state.inspectorMiniMapInstance) state.inspectorMiniMapInstance.invalidateSize();
      }, 50);
    }
  } else {
    if (dom.inspectorGpsActions) dom.inspectorGpsActions.style.display = 'none';
    if (dom.inspectorPlaceOnMapBtn) dom.inspectorPlaceOnMapBtn.style.display = 'block';
    dom.inspectorMiniMap.style.display = 'none';
  }
}

export function setupResizablePanels() {
  activeResizeObservers.forEach(ro => ro.disconnect());
  activeResizeObservers = [];

  // 1. Vertical resizing for File Information and Camera & Exposure (EXIF) panels
  const panelResizers = document.querySelectorAll('.panel-resizer');
  panelResizers.forEach(resizer => {
    const targetId = resizer.dataset.target;
    const target = document.getElementById(targetId);
    if (!target) return;

    // Restore saved height from localStorage
    try {
      const savedHeight = localStorage.getItem(`imagine_${targetId}_height`);
      if (savedHeight) {
        const h = parseInt(savedHeight, 10);
        if (h >= 50 && h <= 600) {
          target.style.height = `${h}px`;
        }
      }
    } catch (_) {}

    // Pointer drag handler
    resizer.addEventListener('pointerdown', (e) => {
      if (e.button !== 0) return; // primary button only
      e.preventDefault();
      resizer.setPointerCapture(e.pointerId);
      resizer.classList.add('dragging');
      document.body.classList.add('resizing-vertical');

      const startY = e.clientY;
      const startHeight = target.getBoundingClientRect().height;

      function onPointerMove(moveEvent) {
        const deltaY = moveEvent.clientY - startY;
        const minHeight = 50;
        const maxHeight = 600;
        const newHeight = Math.max(minHeight, Math.min(maxHeight, Math.round(startHeight + deltaY)));
        target.style.height = `${newHeight}px`;
      }

      function onPointerUp(upEvent) {
        try {
          resizer.releasePointerCapture(upEvent.pointerId);
        } catch (_) {}
        resizer.classList.remove('dragging');
        document.body.classList.remove('resizing-vertical');
        resizer.removeEventListener('pointermove', onPointerMove);
        resizer.removeEventListener('pointerup', onPointerUp);
        resizer.removeEventListener('pointercancel', onPointerUp);

        const finalHeight = parseInt(target.style.height, 10);
        if (!isNaN(finalHeight)) {
          try {
            localStorage.setItem(`imagine_${targetId}_height`, finalHeight);
          } catch (_) {}
        }
      }

      resizer.addEventListener('pointermove', onPointerMove);
      resizer.addEventListener('pointerup', onPointerUp);
      resizer.addEventListener('pointercancel', onPointerUp);
    });

    // Double-click to reset height to auto
    resizer.addEventListener('dblclick', () => {
      target.style.height = '';
      try {
        localStorage.removeItem(`imagine_${targetId}_height`);
      } catch (_) {}
    });

    // Keyboard support
    resizer.addEventListener('keydown', (e) => {
      const step = e.shiftKey ? 25 : 10;
      const currentHeight = target.getBoundingClientRect().height;
      if (e.key === 'ArrowDown') {
        e.preventDefault();
        const newH = Math.min(600, Math.round(currentHeight + step));
        target.style.height = `${newH}px`;
        try { localStorage.setItem(`imagine_${targetId}_height`, newH); } catch (_) {}
      } else if (e.key === 'ArrowUp') {
        e.preventDefault();
        const newH = Math.max(50, Math.round(currentHeight - step));
        target.style.height = `${newH}px`;
        try { localStorage.setItem(`imagine_${targetId}_height`, newH); } catch (_) {}
      } else if (e.key === 'Enter' || e.key === 'Escape') {
        e.preventDefault();
        target.style.height = '';
        try { localStorage.removeItem(`imagine_${targetId}_height`); } catch (_) {}
      }
    });

    // ResizeObserver to detect native CSS resize corner or programmatic resize
    if (window.ResizeObserver) {
      let resizeTimer = null;
      const ro = new ResizeObserver((entries) => {
        for (const entry of entries) {
          if (target.style.height) {
            clearTimeout(resizeTimer);
            resizeTimer = setTimeout(() => {
              const currentH = Math.round(entry.contentRect.height);
              if (currentH >= 50 && currentH <= 600) {
                try { localStorage.setItem(`imagine_${targetId}_height`, currentH); } catch (_) {}
              }
            }, 200);
          }
        }
      });
      ro.observe(target);
      activeResizeObservers.push(ro);
    }
  });

  // 2. Horizontal resizing for Right Inspector Panel
  const inspectorResizer = dom.inspectorResizerLeft;
  if (inspectorResizer && dom.rightInspector) {
    // Restore saved width
    try {
      const savedWidth = localStorage.getItem('imagine_inspector_width');
      if (savedWidth) {
        const w = parseInt(savedWidth, 10);
        if (w >= 240 && w <= 800) {
          dom.rightInspector.style.width = `${w}px`;
        }
      }
    } catch (_) {}

    inspectorResizer.addEventListener('pointerdown', (e) => {
      if (e.button !== 0) return;
      e.preventDefault();
      inspectorResizer.setPointerCapture(e.pointerId);
      inspectorResizer.classList.add('dragging');
      document.body.classList.add('resizing-horizontal');

      const startX = e.clientX;
      const startWidth = dom.rightInspector.getBoundingClientRect().width;

      function onPointerMove(moveEvent) {
        const deltaX = startX - moveEvent.clientX; // dragging left widens right inspector
        const minWidth = 240;
        const maxWidth = Math.min(800, window.innerWidth - 100);
        const newWidth = Math.max(minWidth, Math.min(maxWidth, Math.round(startWidth + deltaX)));
        dom.rightInspector.style.width = `${newWidth}px`;
      }

      function onPointerUp(upEvent) {
        try {
          inspectorResizer.releasePointerCapture(upEvent.pointerId);
        } catch (_) {}
        inspectorResizer.classList.remove('dragging');
        document.body.classList.remove('resizing-horizontal');
        inspectorResizer.removeEventListener('pointermove', onPointerMove);
        inspectorResizer.removeEventListener('pointerup', onPointerUp);
        inspectorResizer.removeEventListener('pointercancel', onPointerUp);

        const finalWidth = parseInt(dom.rightInspector.style.width, 10);
        if (!isNaN(finalWidth)) {
          try {
            localStorage.setItem('imagine_inspector_width', finalWidth);
          } catch (_) {}
        }
      }

      inspectorResizer.addEventListener('pointermove', onPointerMove);
      inspectorResizer.addEventListener('pointerup', onPointerUp);
      inspectorResizer.addEventListener('pointercancel', onPointerUp);
    });

    // Double-click resets inspector width
    inspectorResizer.addEventListener('dblclick', () => {
      dom.rightInspector.style.width = '';
      try {
        localStorage.removeItem('imagine_inspector_width');
      } catch (_) {}
    });

    // Keyboard support
    inspectorResizer.addEventListener('keydown', (e) => {
      const step = e.shiftKey ? 25 : 10;
      const currentWidth = dom.rightInspector.getBoundingClientRect().width;
      if (e.key === 'ArrowLeft') {
        e.preventDefault();
        const newW = Math.min(800, Math.round(currentWidth + step));
        dom.rightInspector.style.width = `${newW}px`;
        try { localStorage.setItem('imagine_inspector_width', newW); } catch (_) {}
      } else if (e.key === 'ArrowRight') {
        e.preventDefault();
        const newW = Math.max(240, Math.round(currentWidth - step));
        dom.rightInspector.style.width = `${newW}px`;
        try { localStorage.setItem('imagine_inspector_width', newW); } catch (_) {}
      } else if (e.key === 'Enter' || e.key === 'Escape') {
        e.preventDefault();
        dom.rightInspector.style.width = '';
        try { localStorage.removeItem('imagine_inspector_width'); } catch (_) {}
      }
    });
  }
}
