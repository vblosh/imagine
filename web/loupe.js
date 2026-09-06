/**
 * IMAGINE Photo Organizer - Fullscreen Loupe Viewer
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

import { state } from './state.js';
import { dom } from './dom.js';
import { updateItemRating, loadMoreMedia } from './media-grid.js';
import { renderStarWidget } from './inspector.js';
import { closeQuickEdit, quickEditState } from './quick-edit.js';

export const LOUPE_MIN_ZOOM = 1.0;
export const LOUPE_MAX_ZOOM = 5.0;
export const LOUPE_ZOOM_STEPS = [1.0, 1.25, 1.5, 2.0, 3.0, 4.0, 5.0];

export function openLoupeForMedia(id) {
  const idx = state.mediaItems.findIndex(m => m.id === id);
  if (idx === -1) return;
  state.loupeIndex = idx;
  if (dom.loupeModal) dom.loupeModal.style.display = 'flex';
  if (document.activeElement && typeof document.activeElement.blur === 'function') {
    document.activeElement.blur();
  }
  updateLoupeView();
}

export function closeLoupe() {
  if (quickEditState.isOpen) {
    closeQuickEdit(false);
  }
  state.loupeIndex = -1;
  resetLoupeZoomToFit();
  if (dom.loupeVideo) {
    dom.loupeVideo.pause();
    dom.loupeVideo.removeAttribute('src');
    dom.loupeVideo.load();
  }
  if (dom.loupeAudio) {
    dom.loupeAudio.pause();
    dom.loupeAudio.removeAttribute('src');
    dom.loupeAudio.load();
  }
  if (dom.loupeModal) dom.loupeModal.style.display = 'none';
}

export function updateLoupeView() {
  if (quickEditState.isOpen) {
    closeQuickEdit(false);
  }
  if (state.loupeIndex < 0 || state.loupeIndex >= state.mediaItems.length) return;
  const item = state.mediaItems[state.loupeIndex];

  // Stop previous video/audio
  if (dom.loupeVideo) {
    dom.loupeVideo.pause();
    dom.loupeVideo.removeAttribute('src');
    dom.loupeVideo.load();
    dom.loupeVideo.style.display = 'none';
  }
  if (dom.loupeAudio) {
    dom.loupeAudio.pause();
    dom.loupeAudio.removeAttribute('src');
    dom.loupeAudio.load();
  }
  if (dom.loupeAudioContainer) {
    dom.loupeAudioContainer.style.display = 'none';
  }
  if (dom.loupeImg) {
    dom.loupeImg.style.display = 'none';
  }

  const fileUrl = `/api/photos/${item.id}/original`;

  if (item.media_type === 'video') {
    if (dom.loupeVideo) {
      dom.loupeVideo.src = fileUrl;
      dom.loupeVideo.style.display = 'block';
      dom.loupeVideo.load();
    }
    if (dom.loupeZoomControls) {
      dom.loupeZoomControls.style.display = 'none';
    }
  } else if (item.media_type === 'audio') {
    if (dom.loupeAudioContainer) {
      dom.loupeAudioContainer.style.display = 'flex';
    }
    if (dom.loupeAudioCover) {
      const coverUrl = item.content_hash
        ? `/api/thumbnails/${encodeURIComponent(item.content_hash)}/256`
        : '';
      dom.loupeAudioCover.src = coverUrl;
    }
    if (dom.loupeAudioTitle) {
      dom.loupeAudioTitle.textContent = item.audio_title || item.file_name;
    }
    if (dom.loupeAudioArtist) {
      const artist = item.audio_artist || 'Unknown Artist';
      const album = item.audio_album ? ` - ${item.audio_album}` : '';
      dom.loupeAudioArtist.textContent = `${artist}${album}`;
    }
    if (dom.loupeAudio) {
      dom.loupeAudio.src = fileUrl;
      dom.loupeAudio.load();
    }
    if (dom.loupeZoomControls) {
      dom.loupeZoomControls.style.display = 'none';
    }
  } else {
    // Photo
    if (dom.loupeImg) {
      dom.loupeImg.src = fileUrl;
      dom.loupeImg.style.display = 'block';
    }
    if (dom.loupeZoomControls) {
      dom.loupeZoomControls.style.display = 'flex';
    }
  }

  if (dom.loupeFileName) dom.loupeFileName.textContent = item.file_name;
  if (dom.loupeIndex) {
    if (state.totalCount > state.mediaItems.length) {
      dom.loupeIndex.textContent = `${state.loupeIndex + 1} / ${state.mediaItems.length} (${state.totalCount} total)`;
    } else {
      dom.loupeIndex.textContent = `${state.loupeIndex + 1} / ${state.mediaItems.length}`;
    }
  }

  resetLoupeZoomToFit();
  updateLoupeControls();
}

export function toggleLoupePlayback() {
  if (state.loupeIndex < 0 || state.loupeIndex >= state.mediaItems.length) return false;
  const item = state.mediaItems[state.loupeIndex];
  if (!item) return false;

  if (item.media_type === 'video' && dom.loupeVideo) {
    if (dom.loupeVideo.paused) {
      dom.loupeVideo.play().catch(() => {});
    } else {
      dom.loupeVideo.pause();
    }
    return true;
  }
  if (item.media_type === 'audio' && dom.loupeAudio) {
    if (dom.loupeAudio.paused) {
      dom.loupeAudio.play().catch(() => {});
    } else {
      dom.loupeAudio.pause();
    }
    return true;
  }
  return false;
}

export function setLoupeZoom(targetZoom, focusClientX = null, focusClientY = null) {
  const prevZoom = state.loupeZoom || 1.0;
  const clampedZoom = Math.min(LOUPE_MAX_ZOOM, Math.max(LOUPE_MIN_ZOOM, Math.round(targetZoom * 100) / 100));

  if (clampedZoom === 1.0) {
    state.loupeZoom = 1.0;
    state.loupePanX = 0;
    state.loupePanY = 0;
  } else {
    if (focusClientX !== null && focusClientY !== null && dom.loupeImageViewport) {
      const rect = dom.loupeImageViewport.getBoundingClientRect();
      const cx = focusClientX - (rect.left + rect.width / 2);
      const cy = focusClientY - (rect.top + rect.height / 2);
      const zoomRatio = clampedZoom / prevZoom;
      state.loupePanX = state.loupePanX - (cx - state.loupePanX) * (zoomRatio - 1);
      state.loupePanY = state.loupePanY - (cy - state.loupePanY) * (zoomRatio - 1);
    } else {
      const zoomRatio = clampedZoom / prevZoom;
      state.loupePanX = state.loupePanX * zoomRatio;
      state.loupePanY = state.loupePanY * zoomRatio;
    }
    state.loupeZoom = clampedZoom;
    clampLoupePan();
  }

  applyLoupeTransform();
  updateLoupeZoomUI();
}

export function clampLoupePan() {
  if (!dom.loupeImg || !dom.loupeImageViewport) return;
  if (state.loupeZoom <= 1.0) {
    state.loupePanX = 0;
    state.loupePanY = 0;
    return;
  }

  const vpRect = dom.loupeImageViewport.getBoundingClientRect();
  const imgW = dom.loupeImg.offsetWidth || 800;
  const imgH = dom.loupeImg.offsetHeight || 600;
  const scaledW = imgW * state.loupeZoom;
  const scaledH = imgH * state.loupeZoom;

  const maxPanX = Math.max(0, (scaledW - vpRect.width) / 2 + 50);
  const maxPanY = Math.max(0, (scaledH - vpRect.height) / 2 + 50);

  state.loupePanX = Math.min(maxPanX, Math.max(-maxPanX, state.loupePanX));
  state.loupePanY = Math.min(maxPanY, Math.max(-maxPanY, state.loupePanY));
}

export function applyLoupeTransform() {
  if (!dom.loupeImg) return;
  if (state.loupeZoom <= 1.0) {
    dom.loupeImg.style.transform = '';
    if (dom.loupeImageViewport) {
      dom.loupeImageViewport.classList.remove('is-zoomed');
    }
  } else {
    dom.loupeImg.style.transform = `translate(${state.loupePanX}px, ${state.loupePanY}px) scale(${state.loupeZoom})`;
    if (dom.loupeImageViewport) {
      dom.loupeImageViewport.classList.add('is-zoomed');
    }
  }
}

export function updateLoupeZoomUI() {
  const percent = Math.round(state.loupeZoom * 100);
  if (dom.loupeZoomResetBtn) {
    dom.loupeZoomResetBtn.textContent = `${percent}%`;
    dom.loupeZoomResetBtn.title = state.loupeZoom > 1.0 ? 'Reset Zoom (Z or Ctrl+0)' : 'Zoom to 200% (Z)';
  }
  if (dom.loupeZoomSlider) {
    dom.loupeZoomSlider.value = percent;
  }
  if (dom.loupeZoomOutBtn) {
    dom.loupeZoomOutBtn.disabled = state.loupeZoom <= 1.0;
  }
  if (dom.loupeZoomInBtn) {
    dom.loupeZoomInBtn.disabled = state.loupeZoom >= LOUPE_MAX_ZOOM;
  }
}

export function resetLoupeZoomToFit() {
  state.loupeZoom = 1.0;
  state.loupePanX = 0;
  state.loupePanY = 0;
  applyLoupeTransform();
  updateLoupeZoomUI();
}

export function loupeZoomIn() {
  const curr = state.loupeZoom;
  const nextStep = LOUPE_ZOOM_STEPS.find(s => s > curr + 0.05);
  const target = nextStep !== undefined ? nextStep : Math.min(LOUPE_MAX_ZOOM, curr + 0.5);
  setLoupeZoom(target);
}

export function loupeZoomOut() {
  const curr = state.loupeZoom;
  const prevSteps = LOUPE_ZOOM_STEPS.filter(s => s < curr - 0.05);
  const target = prevSteps.length > 0 ? prevSteps[prevSteps.length - 1] : LOUPE_MIN_ZOOM;
  setLoupeZoom(target);
}

export function resetLoupeZoom() {
  if (state.loupeZoom > 1.05) {
    setLoupeZoom(1.0);
  } else {
    setLoupeZoom(2.0);
  }
}

export function updateLoupeControls() {
  const item = state.mediaItems[state.loupeIndex];
  if (!item) return;

  if (dom.loupeRating) {
    renderStarWidget(dom.loupeRating, item.rating || 0, (newRating) => {
      updateItemRating(item.id, newRating);
    });
  }

  if (dom.loupeFlag) {
    const pickBtn = dom.loupeFlag.querySelector('.flag-pick');
    const rejectBtn = dom.loupeFlag.querySelector('.flag-reject');
    if (pickBtn) pickBtn.classList.toggle('active', item.flag === 1);
    if (rejectBtn) rejectBtn.classList.toggle('active', item.flag === -1);
  }
}

export async function loupeNext() {
  if (state.mediaItems.length === 0) return;
  if (state.loupeIndex === state.mediaItems.length - 1 && state.mediaItems.length < state.totalCount && !state.isLoadingMore) {
    const prevLen = state.mediaItems.length;
    await (window._imagineApp?.loadMoreMedia ? window._imagineApp.loadMoreMedia() : loadMoreMedia());
    if (state.mediaItems.length > prevLen) {
      state.loupeIndex = prevLen;
      updateLoupeView();
      return;
    }
  }
  state.loupeIndex = (state.loupeIndex + 1) % state.mediaItems.length;
  updateLoupeView();
}

export function loupePrev() {
  if (state.mediaItems.length === 0) return;
  state.loupeIndex = (state.loupeIndex - 1 + state.mediaItems.length) % state.mediaItems.length;
  updateLoupeView();
}
