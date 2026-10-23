/**
 * IMAGINE Photo Organizer - Photo Card Components & DOM Cache
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

import { api, escapeHtml, formatDate, formatDuration, getOriginalMediaUrl } from './api.js';
import { state } from './state.js';
import { dom, showToast } from './dom.js';
import { isAlbumOrderMode } from './media-params.js';
import { t } from './i18n.js';

export const cardMap = new Map();

export function clearCardMap() {
  cardMap.clear();
}
let albumOrderSaveInFlight = false;
let renderGridCallback = () => {};

export function setCardsRenderGridCallback(fn) {
  renderGridCallback = fn;
}

export function getCardElement(id) {
  let card = cardMap.get(id);
  if (card) {
    if (card.isConnected) return card;
    cardMap.delete(id);
  }
  if (!dom.mediaGrid) return null;
  card = dom.mediaGrid.querySelector(`.photo-card[data-id="${CSS.escape(String(id))}"]`);
  if (card) cardMap.set(id, card);
  return card;
}

export function patchCardRating(id, rating) {
  const card = getCardElement(id);
  if (!card) return;
  const starsContainer = card.querySelector('.card-stars');
  if (!starsContainer) return;
  const stars = starsContainer.querySelectorAll('span');
  stars.forEach(s => {
    const sVal = parseInt(s.dataset.star, 10);
    s.classList.toggle('active', sVal <= rating);
  });
}

export function patchCardFlag(id, flag) {
  const card = getCardElement(id);
  if (!card) return;
  const badgesContainer = card.querySelector('.card-badges');
  if (!badgesContainer) return;
  const oldFlagBadge = badgesContainer.querySelector('.flag-badge');
  if (oldFlagBadge) oldFlagBadge.remove();

  if (flag === 1) {
    const badge = document.createElement('span');
    badge.className = 'flag-badge pick';
    badge.title = t('pick');
    badge.textContent = '✔';
    badgesContainer.appendChild(badge);
  } else if (flag === -1) {
    const badge = document.createElement('span');
    badge.className = 'flag-badge reject';
    badge.title = t('reject');
    badge.textContent = '✖';
    badgesContainer.appendChild(badge);
  }
}

function handleAlbumOrderDragStart(event) {
  if (!isAlbumOrderMode() || albumOrderSaveInFlight) {
    event.preventDefault();
    return;
  }
  const card = event.currentTarget;
  card.classList.add('dragging');
  card.setAttribute('aria-grabbed', 'true');
  event.dataTransfer.effectAllowed = 'move';
  event.dataTransfer.setData('text/plain', card.dataset.id);
}

function handleAlbumOrderDragOver(event) {
  if (!isAlbumOrderMode() || albumOrderSaveInFlight) return;
  event.preventDefault();
  event.dataTransfer.dropEffect = 'move';
  event.currentTarget.classList.add('drop-target');
}

function handleAlbumOrderDragLeave(event) {
  event.currentTarget.classList.remove('drop-target');
}

async function handleAlbumOrderDrop(event) {
  event.preventDefault();
  const targetCard = event.currentTarget;
  targetCard.classList.remove('drop-target');
  if (!isAlbumOrderMode() || albumOrderSaveInFlight) return;

  const sourceId = Number(event.dataTransfer.getData('text/plain'));
  const targetId = Number(targetCard.dataset.id);
  if (!Number.isFinite(sourceId) || !Number.isFinite(targetId) || sourceId === targetId) return;

  const previousItems = [...state.mediaItems];
  const sourceIndex = previousItems.findIndex(item => item.id === sourceId);
  const originalTargetIndex = previousItems.findIndex(item => item.id === targetId);
  if (sourceIndex < 0 || originalTargetIndex < 0) return;

  const reordered = [...previousItems];
  const [movedItem] = reordered.splice(sourceIndex, 1);
  const targetIndex = reordered.findIndex(item => item.id === targetId);
  const insertIndex = sourceIndex < originalTargetIndex ? targetIndex + 1 : targetIndex;
  reordered.splice(insertIndex, 0, movedItem);

  const albumId = state.activeAlbumId;
  state.mediaItems = reordered;
  albumOrderSaveInFlight = true;
  renderGridCallback();
  try {
    await api.post(`/api/albums/${albumId}/order`, {
      media_ids: reordered.map(item => item.id),
    });
  } catch (err) {
    if (state.activeAlbumId === albumId && state.sortBy === 'album_order') {
      state.mediaItems = previousItems;
      renderGridCallback();
      showToast(`${t('album_reorder_failed')}: ${err.message || 'Server error'}`, 'error');
    }
  } finally {
    albumOrderSaveInFlight = false;
    if (isAlbumOrderMode() && state.activeAlbumId === albumId) renderGridCallback();
  }
}

function handleAlbumOrderDragEnd(event) {
  event.currentTarget.classList.remove('dragging');
  event.currentTarget.setAttribute('aria-grabbed', 'false');
  dom.mediaGrid?.querySelectorAll('.drop-target').forEach(card => card.classList.remove('drop-target'));
}

export function createPhotoCard(item) {
  const card = document.createElement('div');
  const isSelected = state.selectedIds.has(item.id);
  card.className = 'photo-card' + (isSelected ? ' selected' : '');
  card.dataset.id = item.id;
  card.dataset.mediaType = item.media_type || 'photo';
  card.setAttribute('role', 'article');
  card.setAttribute('aria-selected', isSelected ? 'true' : 'false');
  card.setAttribute('tabindex', '-1');
  const dateStr = item.date_taken ? `, ${formatDate(item.date_taken)}` : '';
  card.setAttribute('aria-label', `${item.file_name || 'Photo'}${dateStr}`);
  cardMap.set(item.id, card);

  if (state.semanticSearchActive && item.rank) {
    card.dataset.rank = String(item.rank);
  }

  if (isAlbumOrderMode()) {
    card.classList.add('album-order-card');
    card.draggable = !albumOrderSaveInFlight;
    card.setAttribute('aria-grabbed', 'false');
    card.addEventListener('dragstart', handleAlbumOrderDragStart);
    card.addEventListener('dragover', handleAlbumOrderDragOver);
    card.addEventListener('dragleave', handleAlbumOrderDragLeave);
    card.addEventListener('drop', handleAlbumOrderDrop);
    card.addEventListener('dragend', handleAlbumOrderDragEnd);
  }

  // Thumbnail URL with fallback
  const originalMediaUrl = getOriginalMediaUrl(item);
  const thumbUrl = item.content_hash
    ? `/api/thumbnails/${encodeURIComponent(item.content_hash)}/256?v=2`
    : originalMediaUrl;
  const rankBadge =
    state.semanticSearchActive && item.rank
      ? `<span class="similarity-rank-badge" title="Rank #${item.rank}${item.similarity_score != null ? ` (${Math.round(item.similarity_score * 100)}%)` : ''}">#${item.rank}</span>`
      : '';

  const flagBadge =
    item.flag === 1
      ? `<span class="flag-badge pick" title="${escapeHtml(t('pick'))}">✔</span>`
      : item.flag === -1
        ? `<span class="flag-badge reject" title="${escapeHtml(t('reject'))}">✖</span>`
        : '';

  let mediaBadge = '';
  if (item.media_type === 'video') {
    const durStr = formatDuration(item.duration);
    mediaBadge = `<span class="media-type-badge video-badge" title="Video (${durStr})"><span class="badge-icon">▶</span> ${durStr}</span>`;
  } else if (item.media_type === 'audio') {
    const durStr = item.duration > 0 ? formatDuration(item.duration) : '';
    mediaBadge = `<span class="media-type-badge audio-badge" title="Audio${durStr ? ` (${durStr})` : ''}"><span class="badge-icon">🎵</span> ${durStr}</span>`;
  }

  // Star rating HTML
  let starsHtml = '';
  for (let s = 1; s <= 5; s++) {
    const activeClass = s <= (item.rating || 0) ? 'active' : '';
    starsHtml += `<span class="${activeClass}" data-star="${s}">★</span>`;
  }

  const safeFileName = escapeHtml(item.file_name);
  card.innerHTML = `
    <div class="photo-thumb-wrap">
      <img src="${escapeHtml(thumbUrl)}" alt="${safeFileName}" loading="lazy" draggable="false">
      <div class="card-badges">
        ${rankBadge}
        ${flagBadge}
        ${mediaBadge}
      </div>
    </div>
    <div class="card-info">
      <div class="card-filename" title="${safeFileName}">${safeFileName}</div>
      <div class="card-footer">
        <span>${formatDate(item.date_taken)}</span>
        <div class="star-rating card-stars" data-id="${item.id}">
          ${starsHtml}
        </div>
      </div>
    </div>
  `;

  const img = card.querySelector('.photo-thumb-wrap img');
  if (img) {
    img.onerror = () => {
      img.onerror = null;
      if (item.content_hash && thumbUrl !== originalMediaUrl) {
        img.src = originalMediaUrl;
      }
    };
  }

  return card;
}
