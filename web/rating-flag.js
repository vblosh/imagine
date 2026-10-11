/**
 * IMAGINE Photo Organizer - Rating & Flag State Mutations
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

import { api } from './api.js';
import { state } from './state.js';
import { showToast } from './dom.js';
import { patchCardRating, patchCardFlag } from './media-cards.js';

const ratingFlagListeners = new Set();
let metadataReloadCallback = () => {};

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
    s.classList.toggle('active', starVal <= currentRating);

    s.onclick = (e) => {
      e.stopPropagation();
      const newRating = starVal === currentRating ? 0 : starVal;
      onSetRating(newRating);
    };
  });
}

export async function updateItemRating(id, rating) {
  const item = state.mediaItems.find(m => m.id === id);
  const prevRating = item ? (item.rating || 0) : 0;
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
    if (item) {
      item.rating = prevRating;
      patchCardRating(id, prevRating);
      notifyRatingFlagChange(item, id);
    }
    showToast('Failed to update rating: ' + (err.message || 'Server error'), 'error');
  }
}

export async function updateItemFlag(id, flag) {
  const item = state.mediaItems.find(m => m.id === id);
  const prevFlag = item ? (item.flag || 0) : 0;
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
    if (item) {
      item.flag = prevFlag;
      patchCardFlag(id, prevFlag);
      notifyRatingFlagChange(item, id);
    }
    showToast('Failed to update flag: ' + (err.message || 'Server error'), 'error');
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
    showToast('Failed to batch update flags: ' + (err.message || 'Server error'), 'error');
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
    showToast('Failed to batch update ratings: ' + (err.message || 'Server error'), 'error');
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
  if (!ids || ids.length === 0) return;
  const items = ids.map(id => state.mediaItems.find(m => m.id === id)).filter(Boolean);
  const allHave = items.length > 0 && items.every(m => m.flag === targetFlag);
  return batchUpdateFlags(ids, allHave ? 0 : targetFlag);
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
    showToast('Failed to batch update dates: ' + (err.message || 'Server error'), 'error');
  }
}
