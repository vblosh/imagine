/**
 * IMAGINE Photo Organizer - Rating & Flag State Mutations
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

import { api } from './api.js';
import { state } from './state.js';
import { showToast } from './dom.js';
import { t } from './i18n.js';
import { patchCardRating, patchCardFlag } from './media-cards.js';

const ratingFlagListeners = new Set();
let metadataReloadCallback = () => {};
const ratingMutationId = new Map();
const flagMutationId = new Map();

export function addRatingFlagListener(fn) {
  ratingFlagListeners.add(fn);
}

export function removeRatingFlagListener(fn) {
  ratingFlagListeners.delete(fn);
}

export function setRatingFlagMetadataReload(fn) {
  metadataReloadCallback = fn;
}

function notifyRatingFlagChange(item, id) {
  ratingFlagListeners.forEach(fn => {
    try {
      fn(item, id);
    } catch (e) {
      console.error('Error in rating/flag listener:', e);
    }
  });
}

export function renderStarWidget(container, currentRating, onSetRating) {
  if (!container) return;
  const stars = container.querySelectorAll('span');
  stars.forEach(s => {
    const starVal = parseInt(s.dataset.star, 10);
    const isActive = starVal <= currentRating;
    s.classList.toggle('active', isActive);
    s.setAttribute('role', 'radio');
    s.setAttribute('aria-checked', isActive ? 'true' : 'false');
    s.setAttribute('aria-label', `${starVal} star${starVal !== 1 ? 's' : ''}`);
    if (!s.hasAttribute('tabindex')) s.setAttribute('tabindex', '0');

    s.onclick = (e) => {
      e.stopPropagation();
      const newRating = starVal === currentRating ? 0 : starVal;
      onSetRating(newRating);
    };
    s.onkeydown = (e) => {
      if (e.key === 'Enter' || e.key === ' ') {
        e.preventDefault();
        e.stopPropagation();
        const newRating = starVal === currentRating ? 0 : starVal;
        onSetRating(newRating);
      }
    };
  });
}

export async function updateItemRating(id, rating) {
  const item = state.mediaItems.find(m => m.id === id);
  const prevRating = item ? (item.rating || 0) : 0;
  const mutId = (ratingMutationId.get(id) || 0) + 1;
  ratingMutationId.set(id, mutId);
  if (item) {
    item.rating = rating;
    patchCardRating(id, rating);
    notifyRatingFlagChange(item, id);
  }
  try {
    await api.post(`/api/media/${id}/rating`, { rating });
    metadataReloadCallback();
  } catch (err) {
    console.error('Failed to update rating:', err);
    if (item && ratingMutationId.get(id) === mutId) {
      item.rating = prevRating;
      patchCardRating(id, prevRating);
      notifyRatingFlagChange(item, id);
    }
    showToast(t('toast_failed_update_rating', { error: err.message || 'Server error' }), 'error');
  }
}

export async function updateItemFlag(id, flag) {
  const item = state.mediaItems.find(m => m.id === id);
  const prevFlag = item ? (item.flag || 0) : 0;
  const mutId = (flagMutationId.get(id) || 0) + 1;
  flagMutationId.set(id, mutId);
  if (item) {
    item.flag = flag;
    patchCardFlag(id, flag);
    notifyRatingFlagChange(item, id);
  }
  try {
    await api.post(`/api/media/${id}/flag`, { flag });
    metadataReloadCallback();
  } catch (err) {
    console.error('Failed to update flag:', err);
    if (item && flagMutationId.get(id) === mutId) {
      item.flag = prevFlag;
      patchCardFlag(id, prevFlag);
      notifyRatingFlagChange(item, id);
    }
    showToast(t('toast_failed_update_flag', { error: err.message || 'Server error' }), 'error');
  }
}

export async function batchUpdateFlags(ids, flag) {
  if (!ids || ids.length === 0) return;
  const prevFlags = new Map();
  ids.forEach(id => {
    const item = state.mediaItems.find(m => m.id === id);
    if (item) {
      prevFlags.set(id, item.flag || 0);
      item.flag = flag;
      patchCardFlag(id, flag);
      notifyRatingFlagChange(item, id);
    }
  });
  try {
    await api.post('/api/media/batch-flag', { ids, flag });
    metadataReloadCallback();
  } catch (err) {
    console.error('Failed to batch update flags:', err);
    prevFlags.forEach((oldFlag, id) => {
      const item = state.mediaItems.find(m => m.id === id);
      if (item) {
        item.flag = oldFlag;
        patchCardFlag(id, oldFlag);
        notifyRatingFlagChange(item, id);
      }
    });
    showToast(t('toast_failed_batch_flags', { error: err.message || 'Server error' }), 'error');
  }
}

export async function batchUpdateRatings(ids, rating) {
  if (!ids || ids.length === 0) return;
  const prevRatings = new Map();
  ids.forEach(id => {
    const item = state.mediaItems.find(m => m.id === id);
    if (item) {
      prevRatings.set(id, item.rating || 0);
      item.rating = rating;
      patchCardRating(id, rating);
      notifyRatingFlagChange(item, id);
    }
  });
  try {
    await api.post('/api/media/batch-rating', { ids, rating });
    metadataReloadCallback();
  } catch (err) {
    console.error('Failed to batch update ratings:', err);
    prevRatings.forEach((oldRating, id) => {
      const item = state.mediaItems.find(m => m.id === id);
      if (item) {
        item.rating = oldRating;
        patchCardRating(id, oldRating);
        notifyRatingFlagChange(item, id);
      }
    });
    showToast(t('toast_failed_batch_ratings', { error: err.message || 'Server error' }), 'error');
  }
}

export function toggleFlagValue(currentFlag, targetFlag) {
  return currentFlag === targetFlag ? 0 : targetFlag;
}

export function toggleItemFlag(id, targetFlag) {
  const item = state.mediaItems.find(m => m.id === id);
  const current = item ? item.flag : 0;
  return updateItemFlag(id, toggleFlagValue(current, targetFlag));
}

export function toggleFlagsForIds(ids, targetFlag) {
  if (!ids || ids.length === 0) return 0;
  const items = ids.map(id => state.mediaItems.find(m => m.id === id)).filter(Boolean);
  const allHave = items.length > 0 && items.every(m => m.flag === targetFlag);
  const nextFlag = allHave ? 0 : targetFlag;
  batchUpdateFlags(ids, nextFlag);
  return nextFlag;
}

export async function batchUpdateDates(updates) {
  if (!updates || updates.length === 0) return;
  const prevDates = new Map();
  updates.forEach(({ id, date_taken, date_taken_str }) => {
    const item = state.mediaItems.find(m => m.id === id);
    if (item) {
      prevDates.set(id, {
        date_taken: item.date_taken,
        exif_date_taken: item.exif?.date_taken,
        date_taken_str: item.exif?.date_taken_str,
      });
      item.date_taken = date_taken;
      if (item.exif) {
        item.exif.date_taken = date_taken;
        item.exif.date_taken_str = date_taken_str;
      }
    }
  });
  try {
    await api.post('/api/media/batch-date', { updates });
    metadataReloadCallback();
  } catch (err) {
    console.error('Failed to batch update dates:', err);
    prevDates.forEach((oldDates, id) => {
      const item = state.mediaItems.find(m => m.id === id);
      if (item) {
        item.date_taken = oldDates.date_taken;
        if (item.exif) {
          item.exif.date_taken = oldDates.exif_date_taken;
          item.exif.date_taken_str = oldDates.date_taken_str;
        }
      }
    });
    showToast(t('toast_failed_batch_dates', { error: err.message || 'Server error' }), 'error');
    throw err;
  }
}
