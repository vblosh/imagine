/**
 * IMAGINE Photo Organizer - Deduplicate UI Module
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

import { api, escapeHtml, formatBytes, formatDate } from './api.js';
import { state } from './state.js';
import { dom, showToast } from './dom.js';
import { t, onLanguageChange } from './i18n.js';

let actionHooks = {
  loadMedia: () => Promise.resolve(),
  loadMetadata: () => Promise.resolve(),
  updateInspector: () => {},
  openLoupeForMedia: () => {},
};

export function setDeduplicateActionHooks(hooks) {
  actionHooks = { ...actionHooks, ...hooks };
}

let review = null;
let opener = null;
let requestSerial = 0;

export function isDeduplicateOpen() {
  return Boolean(dom.deduplicateModal && dom.deduplicateModal.style.display === 'flex');
}

export function closeDeduplicateModal() {
  if (review && review.resolving) return;
  review = null;
  requestSerial += 1;
  if (dom.deduplicateModal) {
    dom.deduplicateModal.style.display = 'none';
  }
  if (opener && typeof opener.focus === 'function') {
    try {
      opener.focus();
    } catch (_) {}
  }
  opener = null;
}

function setDialogState(stateName, message = '') {
  if (!dom.dedupLoading || !dom.dedupEmpty || !dom.dedupError || !dom.dedupReviewContainer) return;
  dom.dedupLoading.style.display = stateName === 'loading' ? 'flex' : 'none';
  dom.dedupEmpty.style.display = stateName === 'empty' ? 'flex' : 'none';
  dom.dedupError.style.display = stateName === 'error' ? 'flex' : 'none';
  if (stateName === 'error') {
    dom.dedupError.innerHTML = `<span>${escapeHtml(message)}</span>
      <button type="button" class="btn btn-sm btn-secondary dedup-retry-btn" style="margin-top: 8px;">${escapeHtml(t('retry') || 'Retry')}</button>`;
    const retryBtn = dom.dedupError.querySelector('.dedup-retry-btn');
    retryBtn?.addEventListener('click', () => {
      scanDuplicates();
    });
  } else {
    dom.dedupError.textContent = message;
  }
  dom.dedupReviewContainer.style.display = stateName === 'ready' ? 'block' : 'none';
}

function updateSummaryBanner() {
  if (!review) return;
  const groups = review.groups || [];
  const enabledGroups = groups.filter(g => g.enabled);
  let totalReclaimable = 0;
  let totalDuplicatePhotos = 0;

  for (const group of enabledGroups) {
    totalReclaimable += computeGroupReclaimable(group);
    totalDuplicatePhotos += Math.max(0, group.photos.length - 1);
  }

  if (dom.dedupSummaryCount) {
    dom.dedupSummaryCount.textContent = t('dedup_summary_banner', {
      groups: enabledGroups.length,
      photos: totalDuplicatePhotos,
      reclaimable: formatBytes(totalReclaimable),
    });
  }

  if (dom.dedupSummaryReclaimable) {
    dom.dedupSummaryReclaimable.textContent = t('dedup_reclaimable', {
      size: formatBytes(totalReclaimable),
    });
  }
}

function updateBadgeCount(count) {
  if (dom.totalDuplicatesCount) {
    dom.totalDuplicatesCount.textContent = String(count || 0);
  }
}

function getPhotoThumb(photo) {
  if (photo.content_hash) {
    return `/api/thumbnails/${encodeURIComponent(photo.content_hash)}/256`;
  }
  return photo.thumb_small || photo.thumb_large || '';
}

function computeGroupReclaimable(group) {
  const keeperId = Number(group.keeper);
  let bytes = 0;
  for (const photo of group.photos) {
    if (Number(photo.id) !== keeperId) {
      bytes += Number(photo.file_size) || 0;
    }
  }
  return bytes;
}

export async function openDeduplicateModal(options = {}) {
  opener = options.opener || document.activeElement;
  const scopeIds = Array.isArray(options.mediaIds) && options.mediaIds.length > 0
    ? [...new Set(options.mediaIds.map(Number).filter(Number.isFinite))]
    : [];

  review = {
    scopeIds,
    groups: [],
    resolving: false,
    scanning: false,
  };

  if (dom.dedupScopeLabel) {
    dom.dedupScopeLabel.textContent = scopeIds.length > 0
      ? t('dedup_scope_selected', { count: scopeIds.length })
      : t('dedup_scope_all');
  }

  // Exact toggle defaults ON
  if (dom.dedupExactToggle) {
    dom.dedupExactToggle.checked = true;
  }

  // Visual toggle with disabled tooltip if CLIP is not available
  const clipAvailable = Boolean(state.semanticSearchAvailable);
  if (dom.dedupVisualToggle) {
    if (!clipAvailable) {
      dom.dedupVisualToggle.checked = false;
      dom.dedupVisualToggle.disabled = true;
      if (dom.dedupVisualToggleLabel) {
        dom.dedupVisualToggleLabel.classList.add('disabled');
        dom.dedupVisualToggleLabel.title = t('dedup_visual_disabled_tooltip');
      }
    } else {
      dom.dedupVisualToggle.disabled = false;
      if (dom.dedupVisualToggleLabel) {
        dom.dedupVisualToggleLabel.classList.remove('disabled');
        dom.dedupVisualToggleLabel.title = t('dedup_visual_match');
      }
    }
  }

  // Slider defaults to 97.5%
  if (dom.dedupVisualThresholdSlider) {
    dom.dedupVisualThresholdSlider.value = '97.5';
  }
  if (dom.dedupVisualThresholdValue) {
    dom.dedupVisualThresholdValue.textContent = '97.5%';
  }

  if (dom.deduplicateModal) {
    dom.deduplicateModal.style.display = 'flex';
  }

  setDialogState('idle');

  // If autoScan requested or directly opened, scan
  if (options.autoScan) {
    await scanDuplicates();
  } else {
    // Focus scan button
    dom.dedupScanBtn?.focus();
  }
}

export async function scanDuplicates() {
  if (!review) {
    review = { scopeIds: [], groups: [], resolving: false, scanning: false };
  }
  const serial = ++requestSerial;
  review.scanning = true;
  setDialogState('loading');
  if (dom.dedupScanBtn) dom.dedupScanBtn.disabled = true;

  try {
    const payload = {};
    if (review.scopeIds && review.scopeIds.length > 0) {
      payload.media_ids = review.scopeIds;
    }
    const exactEnabled = dom.dedupExactToggle ? dom.dedupExactToggle.checked : true;
    const visualEnabled = dom.dedupVisualToggle ? (dom.dedupVisualToggle.checked && !dom.dedupVisualToggle.disabled) : false;

    if (visualEnabled && dom.dedupVisualThresholdSlider) {
      payload.visual_threshold = Number(dom.dedupVisualThresholdSlider.value) / 100.0;
    }

    const result = await api.post('/api/media/duplicates/scan', payload);
    if (serial !== requestSerial || !review) return;

    let rawGroups = result.groups || [];
    if (!exactEnabled) {
      rawGroups = rawGroups.filter(g => g.type !== 'exact');
    }

    // Hydrate photo records
    review.groups = rawGroups.map(group => {
      const memberIds = (group.members || []).map(Number);
      const keeperId = group.keeper != null ? Number(group.keeper) : memberIds[0];
      const photos = (group.photos && group.photos.length)
        ? group.photos
        : memberIds.map(id => {
            const cached = state.mediaItems.find(m => m.id === id);
            return cached || { id, file_name: String(id), file_size: 0, width: 0, height: 0, date_taken: 0 };
          });

      return {
        type: group.type || 'exact',
        keeper: keeperId,
        members: memberIds,
        min_similarity: typeof group.min_similarity === 'number' ? group.min_similarity : 1.0,
        enabled: true,
        photos,
      };
    });

    const exactCount = review.groups.filter(g => g.type === 'exact').length;
    updateBadgeCount(exactCount);

    if (review.groups.length === 0) {
      setDialogState('empty');
    } else {
      setDialogState('ready');
      renderReview();
    }
  } catch (error) {
    if (serial !== requestSerial || !review) return;
    setDialogState('error', error.message || String(error));
  } finally {
    if (review) review.scanning = false;
    if (dom.dedupScanBtn) dom.dedupScanBtn.disabled = false;
  }
}

function renderReview() {
  if (!review || !dom.dedupGroupsList) return;
  const groups = review.groups;
  if (groups.length === 0) {
    setDialogState('empty');
    updateResolveButton();
    return;
  }

  updateSummaryBanner();

  dom.dedupGroupsList.innerHTML = groups.map((group, groupIndex) => {
    const isExact = group.type === 'exact';
    const matchBadge = isExact
      ? `<span class="dedup-match-badge exact">${escapeHtml(t('dedup_match_exact'))}</span>`
      : `<span class="dedup-match-badge visual">${escapeHtml(t('dedup_match_visual', { similarity: (group.min_similarity * 100).toFixed(1) }))}</span>`;

    const groupReclaimable = computeGroupReclaimable(group);
    const reclaimBadge = `<span class="dedup-reclaimable-badge">${escapeHtml(t('dedup_reclaimable', { size: formatBytes(groupReclaimable) }))}</span>`;

    const membersHtml = group.photos.map(photo => {
      const isKeeper = Number(photo.id) === Number(group.keeper);
      const thumb = getPhotoThumb(photo);
      const safeFileName = escapeHtml(photo.file_name || String(photo.id));
      const safePath = escapeHtml(photo.file_path || photo.file_name || '');

      const pills = [];
      if (photo.width && photo.height) {
        pills.push(`<span class="dedup-pill">${photo.width} × ${photo.height}</span>`);
      }
      if (photo.file_size) {
        pills.push(`<span class="dedup-pill">${formatBytes(photo.file_size)}</span>`);
      }
      // date_taken hidden only when == 0; negative pre-1970 dates display normally
      if (photo.date_taken != null && Number(photo.date_taken) !== 0) {
        pills.push(`<span class="dedup-pill">${escapeHtml(formatDate(photo.date_taken))}</span>`);
      }
      if (photo.flag === 1 || photo.flag === 'pick') {
        pills.push(`<span class="dedup-pill pill-flag">★</span>`);
      }
      if (photo.tags && photo.tags.length > 0) {
        pills.push(`<span class="dedup-pill">${escapeHtml(t('dedup_tags_count', { count: photo.tags.length }))}</span>`);
      }

      return `<div class="dedup-member-card${isKeeper ? ' is-keeper' : ''}" data-media-id="${photo.id}">
        <div class="dedup-thumb-wrap" data-media-id="${photo.id}">
          ${thumb ? `<img src="${escapeHtml(thumb)}" alt="${safeFileName}" loading="lazy">` : ''}
          <button type="button" class="dedup-loupe-btn" data-media-id="${photo.id}" title="${escapeHtml(t('dedup_loupe_tooltip'))}">
            <svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="2"><use href="#icon-expand-arrows"></use></svg>
          </button>
        </div>
        <div class="dedup-keeper-controls">
          <label class="dedup-keeper-label">
            <input type="radio" name="dedup-keeper-${groupIndex}" class="dedup-keeper-radio" data-group-index="${groupIndex}" data-media-id="${photo.id}" ${isKeeper ? 'checked' : ''}>
            ${isKeeper ? `<span class="dedup-keeper-badge">${escapeHtml(t('dedup_keeper'))}</span>` : ''}
          </label>
          ${!isKeeper ? `<button type="button" class="btn btn-xs btn-secondary dedup-swap-btn" data-group-index="${groupIndex}" data-media-id="${photo.id}">${escapeHtml(t('dedup_keep_this_instead'))}</button>` : ''}
        </div>
        <div class="dedup-meta-pills">
          <div class="dedup-filename" title="${safePath}">${safeFileName}</div>
          <div class="dedup-pills-row">${pills.join('')}</div>
        </div>
      </div>`;
    }).join('');

    return `<section class="dedup-group-card${group.enabled ? '' : ' excluded'}" data-group-index="${groupIndex}">
      <div class="dedup-group-header">
        <div class="dedup-group-title-row">
          <input type="checkbox" class="dedup-group-checkbox" data-group-index="${groupIndex}" ${group.enabled ? 'checked' : ''} aria-label="${escapeHtml(t('dedup_group', { index: groupIndex + 1 }))}">
          <span class="dedup-group-title">${escapeHtml(t('dedup_group', { index: groupIndex + 1 }))}</span>
          ${matchBadge}
          ${reclaimBadge}
        </div>
        <div class="dedup-group-actions">
          <button type="button" class="btn btn-xs btn-secondary dedup-dismiss-group-btn" data-group-index="${groupIndex}" title="${escapeHtml(t('dedup_dismiss_group'))}">${escapeHtml(t('dedup_dismiss_group'))}</button>
        </div>
      </div>
      <div class="dedup-members-grid">${membersHtml}</div>
    </section>`;
  }).join('');

  bindReviewEvents();
  updateResolveButton();
}

function updateResolveButton() {
  if (!dom.dedupResolveBtn) return;
  const enabledCount = review ? review.groups.filter(g => g.enabled).length : 0;
  dom.dedupResolveBtn.disabled = enabledCount === 0;
  dom.dedupResolveBtn.textContent = enabledCount === 1
    ? t('dedup_resolve_single')
    : t('dedup_resolve_groups', { count: enabledCount });
}

function bindReviewEvents() {
  if (!dom.dedupGroupsList) return;

  // Group checkboxes
  dom.dedupGroupsList.querySelectorAll('.dedup-group-checkbox').forEach(input => {
    input.addEventListener('change', () => {
      const idx = Number(input.dataset.groupIndex);
      if (review && review.groups[idx]) {
        review.groups[idx].enabled = input.checked;
        const card = input.closest('.dedup-group-card');
        if (card) card.classList.toggle('excluded', !input.checked);
        updateResolveButton();
        updateSummaryBanner();
      }
    });
  });

  // Keeper radios and Swap buttons
  const setKeeper = (groupIndex, mediaId) => {
    if (!review || !review.groups[groupIndex]) return;
    review.groups[groupIndex].keeper = Number(mediaId);
    renderReview();
  };

  dom.dedupGroupsList.querySelectorAll('.dedup-keeper-radio').forEach(radio => {
    radio.addEventListener('change', () => {
      setKeeper(radio.dataset.groupIndex, radio.dataset.mediaId);
    });
  });

  dom.dedupGroupsList.querySelectorAll('.dedup-swap-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      setKeeper(btn.dataset.groupIndex, btn.dataset.mediaId);
    });
  });

  // Thumbnail / Loupe click
  dom.dedupGroupsList.querySelectorAll('.dedup-thumb-wrap').forEach(wrap => {
    wrap.addEventListener('click', (e) => {
      const mediaId = Number(wrap.dataset.mediaId);
      if (Number.isFinite(mediaId)) {
        // Ensure photo is accessible in state.mediaItems for loupe viewer
        const existing = state.mediaItems.find(m => m.id === mediaId);
        if (!existing) {
          for (const g of review.groups) {
            const p = g.photos.find(x => Number(x.id) === mediaId);
            if (p) {
              state.mediaItems.push(p);
              break;
            }
          }
        }
        actionHooks.openLoupeForMedia(mediaId);
      }
    });
  });

  // Group Dismiss button
  dom.dedupGroupsList.querySelectorAll('.dedup-dismiss-group-btn').forEach(btn => {
    btn.addEventListener('click', async () => {
      const idx = Number(btn.dataset.groupIndex);
      await dismissGroup(idx);
    });
  });
}

async function dismissGroup(groupIndex) {
  if (!review || !review.groups[groupIndex]) return;
  const group = review.groups[groupIndex];
  const members = (group.members || []).map(Number);
  const pairs = [];
  for (let i = 0; i < members.length; i++) {
    for (let j = i + 1; j < members.length; j++) {
      pairs.push([Math.min(members[i], members[j]), Math.max(members[i], members[j])]);
    }
  }

  try {
    if (pairs.length > 0) {
      await api.post('/api/media/duplicates/dismiss', { pairs });
    }
    review.groups.splice(groupIndex, 1);
    showToast(t('dedup_toast_dismissed'), 'info');
    const exactCount = review.groups.filter(g => g.type === 'exact').length;
    updateBadgeCount(exactCount);
    renderReview();
  } catch (error) {
    showToast(error.message || String(error), 'error');
  }
}

async function resolveGroups() {
  if (!review || review.resolving) return;
  const enabledGroups = review.groups.filter(g => g.enabled);
  if (enabledGroups.length === 0) return;

  review.resolving = true;
  if (dom.dedupResolveBtn) dom.dedupResolveBtn.disabled = true;
  if (dom.cancelDeduplicateBtn) dom.cancelDeduplicateBtn.disabled = true;

  const mergeMetadata = dom.dedupMergeMetadataCheckbox ? dom.dedupMergeMetadataCheckbox.checked : true;
  const deleteFromDisk = dom.dedupDeleteFromDiskCheckbox ? dom.dedupDeleteFromDiskCheckbox.checked : false;

  const resolveGroupsPayload = enabledGroups.map(g => ({
    keeper: Number(g.keeper),
    remove: g.members.filter(id => Number(id) !== Number(g.keeper)).map(Number),
    merge_metadata: mergeMetadata,
    delete_from_disk: deleteFromDisk,
  }));

  try {
    await api.post('/api/media/duplicates/resolve', {
      groups: resolveGroupsPayload,
      merge_metadata: mergeMetadata,
      delete_from_disk: deleteFromDisk,
    });

    const resolvedKeepers = new Set(enabledGroups.map(g => Number(g.keeper)));
    review.groups = review.groups.filter(g => !resolvedKeepers.has(Number(g.keeper)));

    await Promise.all([
      actionHooks.loadMetadata(),
      actionHooks.loadMedia(),
    ]);
    actionHooks.updateInspector();

    showToast(t('dedup_toast_resolved'), 'success');
    const exactCount = review.groups.filter(g => g.type === 'exact').length;
    updateBadgeCount(exactCount);

    if (review.groups.length === 0) {
      closeDeduplicateModal();
    } else {
      renderReview();
    }
  } catch (error) {
    showToast(error.message || String(error), 'error');
  } finally {
    if (review) {
      review.resolving = false;
      if (dom.cancelDeduplicateBtn) dom.cancelDeduplicateBtn.disabled = false;
      updateResolveButton();
    }
  }
}

export function setupDeduplicateListeners() {

  // Batch action bar Find Duplicates entry
  dom.batchDeduplicateBtn?.addEventListener('click', () => {
    const selected = [...state.selectedIds];
    openDeduplicateModal({ mediaIds: selected, autoScan: true });
  });

  // Modal controls
  dom.closeDeduplicateModalBtn?.addEventListener('click', closeDeduplicateModal);
  dom.cancelDeduplicateBtn?.addEventListener('click', closeDeduplicateModal);
  dom.deduplicateBackdrop?.addEventListener('click', closeDeduplicateModal);

  // Scan button
  dom.dedupScanBtn?.addEventListener('click', () => {
    scanDuplicates();
  });

  // Threshold slider
  dom.dedupVisualThresholdSlider?.addEventListener('input', (e) => {
    if (dom.dedupVisualThresholdValue) {
      dom.dedupVisualThresholdValue.textContent = e.target.value + '%';
    }
  });

  // Summary bulk actions
  dom.dedupSelectAllBtn?.addEventListener('click', () => {
    if (!review) return;
    review.groups.forEach(g => { g.enabled = true; });
    renderReview();
  });

  dom.dedupDeselectAllBtn?.addEventListener('click', () => {
    if (!review) return;
    review.groups.forEach(g => { g.enabled = false; });
    renderReview();
  });

  dom.dedupDismissAllBtn?.addEventListener('click', async () => {
    if (!review || review.groups.length === 0) return;
    const allPairs = [];
    for (const g of review.groups) {
      const members = (g.members || []).map(Number);
      for (let i = 0; i < members.length; i++) {
        for (let j = i + 1; j < members.length; j++) {
          allPairs.push([Math.min(members[i], members[j]), Math.max(members[i], members[j])]);
        }
      }
    }
    try {
      if (allPairs.length > 0) {
        await api.post('/api/media/duplicates/dismiss', { pairs: allPairs });
      }
      review.groups = [];
      updateBadgeCount(0);
      showToast(t('dedup_toast_dismissed'), 'info');
      setDialogState('empty');
      updateResolveButton();
    } catch (error) {
      showToast(error.message || String(error), 'error');
    }
  });

  // Resolve button
  dom.dedupResolveBtn?.addEventListener('click', () => {
    resolveGroups();
  });

  // Modal Escape key trap
  dom.deduplicateModal?.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') {
      e.preventDefault();
      e.stopPropagation();
      closeDeduplicateModal();
    }
  });

  onLanguageChange(() => {
    if (isDeduplicateOpen() && review) {
      renderReview();
    }
  });
}
