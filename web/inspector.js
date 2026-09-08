/**
 * IMAGINE Photo Organizer - Inspector Panel & Resizing
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

import {
  api,
  escapeHtml,
  formatBytes,
  formatDateTime,
  formatDate,
  formatDuration,
  formatExposureTime,
  numeric,
  hasValidGps,
  normalizeMediaItem,
  getOriginalMediaUrl
} from './api.js';
import { state } from './state.js';
import { dom, showToast } from './dom.js';
import { updateItemRating, loadMetadata, renderGrid, loadMedia } from './media-grid.js';
import { t } from './i18n.js';
import { saveInspectorCollapsedState } from './persistence.js';

let currentInspectorFetchId = 0;
let inspectorAbortController = null;
let activeResizeObservers = [];
let currentInspectorItem = null;
let inlineEditingInitialized = false;

export function openInspector() {
  if (dom.rightInspector && dom.rightInspector.classList.contains('collapsed')) {
    dom.rightInspector.classList.remove('collapsed');
    saveInspectorCollapsedState(false);
  }
}

export function patchInspectorRatingAndFlag(item) {
  if (state.selectedIds.size === 0) return;
  const selectedId = (state.lastSelectedId && state.selectedIds.has(state.lastSelectedId))
    ? state.lastSelectedId
    : Array.from(state.selectedIds)[0];
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
  currentInspectorItem = item;
  closeAllInlineEditors();
  if (dom.inspectorNoSelection) dom.inspectorNoSelection.style.display = 'none';
  if (dom.inspectorSelection) dom.inspectorSelection.style.display = 'block';

  // Inspector Preview
  const previewUrl = item.content_hash
    ? `/api/thumbnails/${encodeURIComponent(item.content_hash)}/1024`
    : getOriginalMediaUrl(item);
  if (dom.inspectorImg) {
    dom.inspectorImg.src = previewUrl;
    dom.inspectorImg.onerror = () => {
      dom.inspectorImg.src = getOriginalMediaUrl(item);
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
  const typeLabels = { photo: t('type_photo'), video: t('type_video'), audio: t('type_audio') };
  if (dom.infoMediaType) dom.infoMediaType.textContent = typeLabels[item.media_type] || item.media_type || t('type_photo');
  if (dom.infoDimensions) dom.infoDimensions.textContent = item.width && item.height ? `${item.width} × ${item.height} px` : '-';
  if (dom.infoFileSize) dom.infoFileSize.textContent = formatBytes(item.file_size);
  if (dom.infoDateTaken) dom.infoDateTaken.textContent = formatDateTime(item.date_taken);
  if (dom.infoCaption) dom.infoCaption.textContent = item.caption || '-';
  if (dom.infoFilePath) dom.infoFilePath.textContent = item.file_path || '-';

  // Audio / Video Media Properties
  if (dom.avSection) {
    if (item.media_type === 'video' || item.media_type === 'audio') {
      dom.avSection.style.display = 'block';
      if (dom.infoDuration) dom.infoDuration.textContent = item.duration > 0 ? formatDuration(item.duration) : '-';
      if (dom.infoArtist) dom.infoArtist.textContent = item.audio_artist || '-';
      if (dom.infoTitle) dom.infoTitle.textContent = item.audio_title || '-';
      if (dom.infoAlbum) dom.infoAlbum.textContent = item.audio_album || '-';
      if (dom.infoGenre) dom.infoGenre.textContent = item.audio_genre || '-';
      if (dom.infoCodec) dom.infoCodec.textContent = item.codec || '-';
      if (dom.infoBitrate) dom.infoBitrate.textContent = item.bitrate > 0 ? `${Math.round(item.bitrate / 1000)} kbps` : '-';
      if (dom.infoAudioDetails) {
        const details = [];
        if (item.channels > 0) details.push(item.channels === 1 ? 'Mono' : item.channels === 2 ? 'Stereo' : `${item.channels} ch`);
        if (item.sample_rate > 0) details.push(`${(item.sample_rate / 1000).toFixed(1)} kHz`);
        dom.infoAudioDetails.textContent = details.length > 0 ? details.join(', ') : '-';
      }
    } else {
      dom.avSection.style.display = 'none';
    }
  }

  // EXIF properties
  if (dom.exifSection) {
    dom.exifSection.style.display = (item.media_type === 'audio') ? 'none' : 'block';
  }
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
    currentInspectorItem = null;
    closeAllInlineEditors();
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

  // Pick first selected ID (prefer lastSelectedId if valid)
  const selectedId = (state.lastSelectedId && state.selectedIds.has(state.lastSelectedId))
    ? state.lastSelectedId
    : Array.from(state.selectedIds)[0];
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
              const currentH = Math.round(
                (entry.borderBoxSize && entry.borderBoxSize[0])
                  ? entry.borderBoxSize[0].blockSize
                  : target.getBoundingClientRect().height
              );
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

  // 3. Horizontal resizing for Left Sidebar Panel
  const sidebarResizer = dom.sidebarResizerRight || document.getElementById('sidebarResizerRight');
  const leftSidebar = dom.leftSidebar || document.getElementById('leftSidebar');
  if (sidebarResizer && leftSidebar) {
    // Restore saved width
    try {
      const savedWidth = localStorage.getItem('imagine_sidebar_width');
      if (savedWidth) {
        const w = parseInt(savedWidth, 10);
        if (w >= 160 && w <= 600) {
          leftSidebar.style.width = `${w}px`;
        }
      }
    } catch (_) {}

    sidebarResizer.addEventListener('pointerdown', (e) => {
      if (e.button !== 0) return;
      e.preventDefault();
      sidebarResizer.setPointerCapture(e.pointerId);
      sidebarResizer.classList.add('dragging');
      document.body.classList.add('resizing-horizontal');

      const startX = e.clientX;
      const startWidth = leftSidebar.getBoundingClientRect().width;

      function onPointerMove(moveEvent) {
        const deltaX = moveEvent.clientX - startX; // dragging right widens left sidebar
        const minWidth = 160;
        const maxWidth = Math.min(600, window.innerWidth - 200);
        const newWidth = Math.max(minWidth, Math.min(maxWidth, Math.round(startWidth + deltaX)));
        leftSidebar.style.width = `${newWidth}px`;
      }

      function onPointerUp(upEvent) {
        try {
          sidebarResizer.releasePointerCapture(upEvent.pointerId);
        } catch (_) {}
        sidebarResizer.classList.remove('dragging');
        document.body.classList.remove('resizing-horizontal');
        sidebarResizer.removeEventListener('pointermove', onPointerMove);
        sidebarResizer.removeEventListener('pointerup', onPointerUp);
        sidebarResizer.removeEventListener('pointercancel', onPointerUp);

        const finalWidth = parseInt(leftSidebar.style.width, 10);
        if (!isNaN(finalWidth)) {
          try {
            localStorage.setItem('imagine_sidebar_width', finalWidth);
          } catch (_) {}
        }
      }

      sidebarResizer.addEventListener('pointermove', onPointerMove);
      sidebarResizer.addEventListener('pointerup', onPointerUp);
      sidebarResizer.addEventListener('pointercancel', onPointerUp);
    });

    // Double-click resets sidebar width
    sidebarResizer.addEventListener('dblclick', () => {
      leftSidebar.style.width = '';
      try {
        localStorage.removeItem('imagine_sidebar_width');
      } catch (_) {}
    });

    // Keyboard support
    sidebarResizer.addEventListener('keydown', (e) => {
      const step = e.shiftKey ? 25 : 10;
      const currentWidth = leftSidebar.getBoundingClientRect().width;
      if (e.key === 'ArrowRight') {
        e.preventDefault();
        const newW = Math.min(600, Math.round(currentWidth + step));
        leftSidebar.style.width = `${newW}px`;
        try { localStorage.setItem('imagine_sidebar_width', newW); } catch (_) {}
      } else if (e.key === 'ArrowLeft') {
        e.preventDefault();
        const newW = Math.max(160, Math.round(currentWidth - step));
        leftSidebar.style.width = `${newW}px`;
        try { localStorage.setItem('imagine_sidebar_width', newW); } catch (_) {}
      } else if (e.key === 'Enter' || e.key === 'Escape') {
        e.preventDefault();
        leftSidebar.style.width = '';
        try { localStorage.removeItem('imagine_sidebar_width'); } catch (_) {}
      }
    });
  }
}

export function closeAllInlineEditors() {
  closeRenameForm();
  closeDateTakenForm();
  closeCaptionForm();
}

export function closeRenameForm() {
  if (dom.renameFileForm) dom.renameFileForm.style.display = 'none';
  if (dom.infoFileName) dom.infoFileName.style.display = '';
  if (dom.renameFileBtn) dom.renameFileBtn.style.display = '';
}

export function closeDateTakenForm() {
  if (dom.editDateTakenForm) dom.editDateTakenForm.style.display = 'none';
  if (dom.infoDateTaken) dom.infoDateTaken.style.display = '';
  if (dom.editDateTakenBtn) dom.editDateTakenBtn.style.display = '';
}

export function closeCaptionForm() {
  if (dom.editCaptionForm) dom.editCaptionForm.style.display = 'none';
  if (dom.infoCaption) dom.infoCaption.style.display = '';
  if (dom.editCaptionBtn) dom.editCaptionBtn.style.display = '';
}

export function openRenameForm() {
  if (!currentInspectorItem) return;
  closeDateTakenForm();
  closeCaptionForm();

  if (dom.infoFileName) dom.infoFileName.style.display = 'none';
  if (dom.renameFileBtn) dom.renameFileBtn.style.display = 'none';
  if (dom.renameFileForm) {
    dom.renameFileForm.style.display = 'flex';
    if (dom.renameFileInput) {
      dom.renameFileInput.value = currentInspectorItem.file_name || '';
      dom.renameFileInput.focus();
      const dotIdx = dom.renameFileInput.value.lastIndexOf('.');
      if (dotIdx > 0) {
        dom.renameFileInput.setSelectionRange(0, dotIdx);
      } else {
        dom.renameFileInput.select();
      }
    }
  }
}

export async function handleSaveRename() {
  if (!currentInspectorItem || !dom.renameFileInput) return;
  const rawInput = dom.renameFileInput.value.trim();
  if (!rawInput) {
    showToast(t('toast_file_name_empty'), 'error');
    return;
  }
  if (rawInput === currentInspectorItem.file_name) {
    closeRenameForm();
    return;
  }
  if (/[\\/:*?"<>|]/.test(rawInput)) {
    showToast(t('toast_file_name_invalid'), 'error');
    return;
  }

  let finalName = rawInput;
  if (!finalName.includes('.') && currentInspectorItem.file_name && currentInspectorItem.file_name.includes('.')) {
    const ext = currentInspectorItem.file_name.substring(currentInspectorItem.file_name.lastIndexOf('.'));
    finalName += ext;
  }

  try {
    const res = await api.post(`/api/media/${currentInspectorItem.id}/rename`, { name: finalName });
    if (res && res.file_name) {
      currentInspectorItem.file_name = res.file_name;
      if (res.file_path) currentInspectorItem.file_path = res.file_path;

      const idx = state.mediaItems.findIndex(m => m.id === currentInspectorItem.id);
      if (idx !== -1) {
        state.mediaItems[idx].file_name = res.file_name;
        if (res.file_path) state.mediaItems[idx].file_path = res.file_path;
      }

      if (dom.infoFileName) dom.infoFileName.textContent = res.file_name;
      if (dom.infoFilePath) dom.infoFilePath.textContent = res.file_path || currentInspectorItem.file_path || '-';

      const grid = dom.mediaGrid || document.getElementById('mediaGrid');
      if (grid) {
        const card = grid.querySelector(`.photo-card[data-id="${CSS.escape(String(currentInspectorItem.id))}"]`);
        if (card) {
          const fnEl = card.querySelector('.card-filename');
          if (fnEl) {
            fnEl.textContent = res.file_name;
            fnEl.title = res.file_name;
          }
        }
      }

      if (state.sortBy === 'file_name') {
        state.mediaItems.sort((a, b) => {
          const fa = (a.file_name || '').toLowerCase();
          const fb = (b.file_name || '').toLowerCase();
          if (fa !== fb) {
            return state.sortDesc ? fb.localeCompare(fa) : fa.localeCompare(fb);
          }
          const idA = a.id || 0;
          const idB = b.id || 0;
          return state.sortDesc ? (idB - idA) : (idA - idB);
        });
        const prevScroll = dom.gridScrollContainer ? dom.gridScrollContainer.scrollTop : 0;
        renderGrid();
        if (dom.gridScrollContainer) dom.gridScrollContainer.scrollTop = prevScroll;
      }

      closeRenameForm();
      showToast(`File renamed to ${res.file_name}`, 'success');
    }
  } catch (err) {
    showToast('Failed to rename file: ' + (err.message || 'Error'), 'error');
  }
}

export function openDateTakenForm() {
  if (!currentInspectorItem) return;
  closeRenameForm();
  closeCaptionForm();

  if (dom.infoDateTaken) dom.infoDateTaken.style.display = 'none';
  if (dom.editDateTakenBtn) dom.editDateTakenBtn.style.display = 'none';
  if (dom.editDateTakenForm) {
    dom.editDateTakenForm.style.display = 'flex';
    if (dom.editDateTakenInput) {
      if (currentInspectorItem.date_taken && currentInspectorItem.date_taken > 0) {
        const d = new Date(currentInspectorItem.date_taken * 1000);
        const yyyy = d.getUTCFullYear();
        const mm = String(d.getUTCMonth() + 1).padStart(2, '0');
        const dd = String(d.getUTCDate()).padStart(2, '0');
        const hh = String(d.getUTCHours()).padStart(2, '0');
        const min = String(d.getUTCMinutes()).padStart(2, '0');
        dom.editDateTakenInput.value = `${yyyy}-${mm}-${dd}T${hh}:${min}`;
      } else {
        dom.editDateTakenInput.value = '';
      }
      dom.editDateTakenInput.focus();
    }
  }
}

export async function handleSaveDateTaken() {
  if (!currentInspectorItem || !dom.editDateTakenInput) return;
  const val = dom.editDateTakenInput.value;
  if (!val) {
    showToast('Please enter a date and time', 'error');
    return;
  }

  const parts = val.split('T');
  if (parts.length < 1 || !parts[0]) {
    showToast('Invalid date format', 'error');
    return;
  }
  const dateParts = parts[0].split('-').map(Number);
  const timeParts = (parts[1] || '00:00:00').split(':').map(Number);
  const y = dateParts[0];
  const m = dateParts[1];
  const d = dateParts[2];
  const hh = timeParts[0] || 0;
  const min = timeParts[1] || 0;
  const sec = timeParts[2] || 0;
  const ts = Math.floor(Date.UTC(y, m - 1, d, hh, min, sec) / 1000);

  if (isNaN(ts) || ts <= 0) {
    showToast('Invalid date and time', 'error');
    return;
  }

  try {
    const res = await api.post(`/api/media/${currentInspectorItem.id}/date`, { date_taken: ts });
    if (res && res.date_taken !== undefined) {
      currentInspectorItem.date_taken = res.date_taken;
      if (currentInspectorItem.exif) {
        currentInspectorItem.exif.date_taken = res.date_taken;
        if (res.date_taken_str) currentInspectorItem.exif.date_taken_str = res.date_taken_str;
      }

      const idx = state.mediaItems.findIndex(m => m.id === currentInspectorItem.id);
      if (idx !== -1) {
        state.mediaItems[idx].date_taken = res.date_taken;
        if (state.mediaItems[idx].exif) {
          state.mediaItems[idx].exif.date_taken = res.date_taken;
          if (res.date_taken_str) state.mediaItems[idx].exif.date_taken_str = res.date_taken_str;
        }
      }

      if (dom.infoDateTaken) dom.infoDateTaken.textContent = formatDateTime(res.date_taken);

      // If active timeline filter is set and item moved outside the period, reload media
      if (state.activeTimelinePeriod) {
        const itemDate = new Date(res.date_taken * 1000);
        const itemYear = itemDate.getUTCFullYear();
        const itemMonth = itemDate.getUTCMonth() + 1;
        if (itemYear !== state.activeTimelinePeriod.year || itemMonth !== state.activeTimelinePeriod.month) {
          closeDateTakenForm();
          showToast('Date updated', 'success');
          await loadMedia();
          await loadMetadata();
          return;
        }
      }

      // Re-sort media items if sorted by date_taken (default)
      if (state.sortBy === 'date_taken') {
        state.mediaItems.sort((a, b) => {
          const da = (a.date_taken !== undefined && a.date_taken !== null) ? a.date_taken : 0;
          const db = (b.date_taken !== undefined && b.date_taken !== null) ? b.date_taken : 0;
          if (da !== db) {
            return state.sortDesc ? (db - da) : (da - db);
          }
          const idA = a.id || 0;
          const idB = b.id || 0;
          return state.sortDesc ? (idB - idA) : (idA - idB);
        });
      }

      if (state.loupeIndex >= 0 && currentInspectorItem) {
        state.loupeIndex = state.mediaItems.findIndex(m => m.id === currentInspectorItem.id);
      }

      const prevScroll = dom.gridScrollContainer ? dom.gridScrollContainer.scrollTop : 0;
      renderGrid();
      if (dom.gridScrollContainer) dom.gridScrollContainer.scrollTop = prevScroll;

      loadMetadata();

      closeDateTakenForm();
      showToast('Date updated', 'success');
    }
  } catch (err) {
    showToast('Failed to update date: ' + (err.message || 'Error'), 'error');
  }
}

export function openCaptionForm() {
  if (!currentInspectorItem) return;
  closeRenameForm();
  closeDateTakenForm();

  if (dom.infoCaption) dom.infoCaption.style.display = 'none';
  if (dom.editCaptionBtn) dom.editCaptionBtn.style.display = 'none';
  if (dom.editCaptionForm) {
    dom.editCaptionForm.style.display = 'flex';
    if (dom.editCaptionInput) {
      dom.editCaptionInput.value = currentInspectorItem.caption || '';
      dom.editCaptionInput.focus();
      dom.editCaptionInput.select();
    }
  }
}

export async function handleSaveCaption() {
  if (!currentInspectorItem || !dom.editCaptionInput) return;
  const newCaption = dom.editCaptionInput.value.trim();

  try {
    const res = await api.post(`/api/media/${currentInspectorItem.id}/caption`, { caption: newCaption });
    if (res) {
      currentInspectorItem.caption = newCaption;

      const idx = state.mediaItems.findIndex(m => m.id === currentInspectorItem.id);
      if (idx !== -1) {
        state.mediaItems[idx].caption = newCaption;
      }

      if (dom.infoCaption) dom.infoCaption.textContent = newCaption || '-';

      closeCaptionForm();
      showToast(t('toast_caption_saved'), 'success');
    }
  } catch (err) {
    showToast('Failed to update caption: ' + (err.message || 'Error'), 'error');
  }
}

export function setupInspectorInlineEditing() {
  if (inlineEditingInitialized) return;
  inlineEditingInitialized = true;

  // Rename File
  if (dom.renameFileBtn) {
    dom.renameFileBtn.addEventListener('click', (e) => {
      e.stopPropagation();
      openRenameForm();
    });
  }
  if (dom.infoFileName) {
    dom.infoFileName.addEventListener('click', (e) => {
      e.stopPropagation();
      openRenameForm();
    });
  }
  if (dom.saveRenameBtn) {
    dom.saveRenameBtn.addEventListener('click', (e) => {
      e.stopPropagation();
      handleSaveRename();
    });
  }
  if (dom.cancelRenameBtn) {
    dom.cancelRenameBtn.addEventListener('click', (e) => {
      e.stopPropagation();
      closeRenameForm();
    });
  }
  if (dom.renameFileInput) {
    dom.renameFileInput.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') {
        e.preventDefault();
        handleSaveRename();
      } else if (e.key === 'Escape') {
        e.preventDefault();
        e.stopPropagation();
        closeRenameForm();
      }
    });
  }

  // Date Taken
  if (dom.editDateTakenBtn) {
    dom.editDateTakenBtn.addEventListener('click', (e) => {
      e.stopPropagation();
      openDateTakenForm();
    });
  }
  if (dom.infoDateTaken) {
    dom.infoDateTaken.addEventListener('click', (e) => {
      e.stopPropagation();
      openDateTakenForm();
    });
  }
  if (dom.saveDateTakenBtn) {
    dom.saveDateTakenBtn.addEventListener('click', (e) => {
      e.stopPropagation();
      handleSaveDateTaken();
    });
  }
  if (dom.cancelDateTakenBtn) {
    dom.cancelDateTakenBtn.addEventListener('click', (e) => {
      e.stopPropagation();
      closeDateTakenForm();
    });
  }
  if (dom.editDateTakenInput) {
    dom.editDateTakenInput.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') {
        e.preventDefault();
        handleSaveDateTaken();
      } else if (e.key === 'Escape') {
        e.preventDefault();
        e.stopPropagation();
        closeDateTakenForm();
      }
    });
  }

  // Caption
  if (dom.editCaptionBtn) {
    dom.editCaptionBtn.addEventListener('click', (e) => {
      e.stopPropagation();
      openCaptionForm();
    });
  }
  if (dom.infoCaption) {
    dom.infoCaption.addEventListener('click', (e) => {
      e.stopPropagation();
      openCaptionForm();
    });
  }
  if (dom.saveCaptionBtn) {
    dom.saveCaptionBtn.addEventListener('click', (e) => {
      e.stopPropagation();
      handleSaveCaption();
    });
  }
  if (dom.cancelCaptionBtn) {
    dom.cancelCaptionBtn.addEventListener('click', (e) => {
      e.stopPropagation();
      closeCaptionForm();
    });
  }
  if (dom.editCaptionInput) {
    dom.editCaptionInput.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') {
        e.preventDefault();
        handleSaveCaption();
      } else if (e.key === 'Escape') {
        e.preventDefault();
        e.stopPropagation();
        closeCaptionForm();
      }
    });
  }
}

