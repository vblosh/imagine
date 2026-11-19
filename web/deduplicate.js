/**
 * IMAGINE Photo Organizer - Deduplicate UI Module
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

import {
  api,
  escapeHtml,
  formatBytes,
  formatDate,
  getOriginalMediaUrl,
  normalizeMediaItem,
} from './api.js';
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

const RENDER_CHUNK_SIZE = 40;
let review = null;
let opener = null;
let lastDedupLoupeTrigger = null;
let renderedGroupCount = 0;
let sentinelObserver = null;
let reviewDelegationAttached = false;

export function isDeduplicateOpen() {
  return Boolean(dom.deduplicateModal && dom.deduplicateModal.style.display === 'flex');
}

export function isResetWarningOpen() {
  return Boolean(dom.dedupResetWarningModal && dom.dedupResetWarningModal.style.display === 'flex');
}

export function closeResetWarningModal() {
  if (resetWarningResolver) {
    const res = resetWarningResolver;
    resetWarningResolver = null;
    res(false);
  }
  if (dom.dedupResetWarningModal) {
    dom.dedupResetWarningModal.style.display = 'none';
  }
}

export function closeDeduplicateModal() {
  if (review && review.resolving) return;
  closeResetWarningModal();
  removeSentinel();
  review = null;
  updateReviewActions();
  if (dom.deduplicateModal) {
    dom.deduplicateModal.style.display = 'none';
  }
  if (opener && typeof opener.focus === 'function') {
    try {
      opener.focus();
    } catch (_) {}
  }
  opener = null;
  lastDedupLoupeTrigger = null;
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
  const enabledGroups = groups.filter((g) => g.enabled);
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
  const safeCount = Number.isFinite(Number(count)) ? Number(count) : 0;
  if (dom.totalDuplicatesCount) {
    dom.totalDuplicatesCount.textContent = String(safeCount);
  }
  if (state.stats) {
    state.stats.total_duplicates = safeCount;
  }
}

function setScanAndResetButtonsDisabled(disabled) {
  if (dom.dedupScanBtn) dom.dedupScanBtn.disabled = disabled;
  if (dom.dedupResetDismissalsBtn) dom.dedupResetDismissalsBtn.disabled = disabled;
  if (dom.dedupResetDismissalsEmptyBtn) dom.dedupResetDismissalsEmptyBtn.disabled = disabled;
}

function getPhotoThumb(photo) {
  if (!photo) return '';
  if (photo.content_hash) {
    return `/api/thumbnails/${encodeURIComponent(photo.content_hash)}/256`;
  }
  if (photo.thumb_small) return photo.thumb_small;
  if (photo.thumb_large) return photo.thumb_large;
  const isImage = !photo.media_type || photo.media_type === 'photo' || photo.media_type === 'image';
  return isImage ? getOriginalMediaUrl(photo) : '';
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
  if (review?.resolving) return;
  opener = options.opener || document.activeElement;
  const scopeIds =
    Array.isArray(options.mediaIds) && options.mediaIds.length > 0
      ? [...new Set(options.mediaIds.map(Number).filter(Number.isFinite))]
      : [];

  removeSentinel();
  review = {
    scopeIds,
    groups: [],
    resolving: false,
    scanning: false,
    pendingDismissals: new Set(),
  };

  if (dom.dedupScopeLabel) {
    dom.dedupScopeLabel.textContent =
      scopeIds.length > 0
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
  updateReviewActions();

  // If autoScan requested or directly opened, scan
  if (options.autoScan) {
    await scanDuplicates();
  } else {
    // Focus scan button
    dom.dedupScanBtn?.focus();
  }
}

export async function scanDuplicates() {
  const activeReview = review;
  if (!activeReview || isReviewBusy()) return;

  activeReview.scanning = true;
  activeReview.groups = [];
  removeSentinel();
  dom.dedupGroupsList?.replaceChildren();
  setDialogState('loading');
  updateReviewActions();

  try {
    const payload = {};
    if (activeReview.scopeIds.length > 0) {
      payload.media_ids = activeReview.scopeIds;
    }
    const exactEnabled = dom.dedupExactToggle ? dom.dedupExactToggle.checked : true;
    const visualEnabled = dom.dedupVisualToggle
      ? dom.dedupVisualToggle.checked && !dom.dedupVisualToggle.disabled
      : false;

    payload.exact = exactEnabled;
    payload.visual = visualEnabled;

    if (visualEnabled && dom.dedupVisualThresholdSlider) {
      payload.visual_threshold = Number(dom.dedupVisualThresholdSlider.value) / 100.0;
    }

    const result = await api.post('/api/media/duplicates/scan', payload);
    if (review !== activeReview) return;

    let rawGroups = result.groups || [];
    if (!exactEnabled) {
      rawGroups = rawGroups.filter((g) => g.type !== 'exact');
    }
    if (!visualEnabled) {
      rawGroups = rawGroups.filter((g) => g.type !== 'visual');
    }

    const catalogMediaMap = new Map((state.mediaItems || []).map((m) => [Number(m.id), m]));
    const makePlaceholderPhoto = (id) =>
      normalizeMediaItem({
        id,
        file_name: String(id),
        file_size: 0,
        width: 0,
        height: 0,
        date_taken: 0,
        media_type: 'photo',
      }) || {
        id,
        file_name: String(id),
        file_size: 0,
        width: 0,
        height: 0,
        date_taken: 0,
        media_type: 'photo',
      };

    // Hydrate photo records
    activeReview.groups = rawGroups.map((group) => {
      const memberIds = (group.members || [])
        .map(Number)
        .filter((id) => Number.isFinite(id) && id > 0);
      const rawList =
        Array.isArray(group.photos) && group.photos.length > 0
          ? group.photos
          : Array.isArray(group.items) && group.items.length > 0
            ? group.items
            : [];

      const photoMap = new Map();
      for (const p of rawList) {
        if (!p) continue;
        const norm = normalizeMediaItem(p);
        const mid = Number(norm ? norm.id : p.id);
        if (Number.isFinite(mid) && mid > 0) {
          photoMap.set(mid, norm || makePlaceholderPhoto(mid));
        }
      }

      const effectiveIds =
        memberIds.length > 0
          ? memberIds
          : rawList.map((p) => Number(p && p.id)).filter((id) => Number.isFinite(id) && id > 0);
      const keeperId = group.keeper != null ? Number(group.keeper) : effectiveIds[0];

      const photos = effectiveIds.map((id) => {
        if (photoMap.has(id) && photoMap.get(id)) {
          return photoMap.get(id);
        }
        const cached = catalogMediaMap.get(id);
        const normCached = cached ? normalizeMediaItem(cached) : null;
        return normCached || makePlaceholderPhoto(id);
      });

      return {
        type: group.type || 'exact',
        keeper: keeperId,
        members: effectiveIds,
        min_similarity: typeof group.min_similarity === 'number' ? group.min_similarity : 1.0,
        enabled: true,
        photos,
      };
    });

    const totalGroupsCount = activeReview.groups.length;
    updateBadgeCount(totalGroupsCount);

    if (activeReview.groups.length === 0) {
      setDialogState('empty');
    } else {
      setDialogState('ready');
      renderReview();
    }
  } catch (error) {
    if (review !== activeReview) return;
    activeReview.groups = [];
    setDialogState('error', error.message || String(error));
  } finally {
    if (review === activeReview) {
      activeReview.scanning = false;
      updateReviewActions();
    }
  }
}

function renderGroupCardHtml(group, groupIndex) {
  const isExact = group.type === 'exact';
  const matchBadge = isExact
    ? `<span class="dedup-match-badge exact">${escapeHtml(t('dedup_match_exact'))}</span>`
    : `<span class="dedup-match-badge visual">${escapeHtml(t('dedup_match_visual', { similarity: (group.min_similarity * 100).toFixed(1) }))}</span>`;

  const groupReclaimable = computeGroupReclaimable(group);
  const reclaimBadge = `<span class="dedup-reclaimable-badge">${escapeHtml(t('dedup_reclaimable', { size: formatBytes(groupReclaimable) }))}</span>`;

  const membersHtml = group.photos
    .map((photo) => {
      const isKeeper = Number(photo.id) === Number(group.keeper);
      const thumb = getPhotoThumb(photo);
      const safeFileName = escapeHtml(photo.file_name || String(photo.id));
      const safePath = escapeHtml(photo.file_path || photo.file_name || '');
      const rawPath = (photo.file_path || '').replace(/\\/g, '/');
      const lastSlashIdx = rawPath.lastIndexOf('/');
      const folderName = lastSlashIdx > 0 ? rawPath.substring(0, lastSlashIdx) : '';
      const folderHtml = folderName
        ? `<div class="dedup-folder" title="${safePath}">
          <svg viewBox="0 0 24 24" width="11" height="11" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true"><use href="#icon-folder"></use></svg>
          <span class="dedup-folder-name">${escapeHtml(folderName)}</span>
        </div>`
        : '';

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
        pills.push(
          `<span class="dedup-pill">${escapeHtml(t('dedup_tags_count', { count: photo.tags.length }))}</span>`
        );
      }

      return `<div class="dedup-member-card${isKeeper ? ' is-keeper' : ''}" data-media-id="${photo.id}">
      <div class="dedup-thumb-wrap" data-media-id="${photo.id}">
        ${thumb ? `<img src="${escapeHtml(thumb)}" alt="${safeFileName}" loading="lazy" data-media-id="${photo.id}">` : ''}
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
        ${folderHtml}
        <div class="dedup-pills-row">${pills.join('')}</div>
      </div>
    </div>`;
    })
    .join('');

  return `<section class="dedup-group-card${group.enabled ? '' : ' excluded'}" data-group-index="${groupIndex}">
    <div class="dedup-group-header">
      <div class="dedup-group-title-row">
        <input type="checkbox" class="dedup-group-checkbox" data-group-index="${groupIndex}" ${group.enabled ? 'checked' : ''} aria-label="${escapeHtml(t('dedup_group', { index: groupIndex + 1 }))}">
        <span class="dedup-group-title">${escapeHtml(t('dedup_group', { index: groupIndex + 1 }))}</span>
        ${matchBadge}
        ${reclaimBadge}
      </div>
      <div class="dedup-group-actions">
        <button type="button" class="btn btn-xs btn-secondary dedup-dismiss-group-btn" data-group-index="${groupIndex}" title="${escapeHtml(t('dedup_dismiss_group'))}" ${canDismissGroup(group) ? '' : 'disabled'}>${escapeHtml(t('dedup_dismiss_group'))}</button>
      </div>
    </div>
    <div class="dedup-members-grid">${membersHtml}</div>
  </section>`;
}

function removeSentinel() {
  if (sentinelObserver) {
    sentinelObserver.disconnect();
    sentinelObserver = null;
  }
  const s = dom.dedupGroupsList?.querySelector('.dedup-sentinel');
  if (s) s.remove();
}

function ensureSentinel() {
  if (!dom.dedupGroupsList) return;
  let s = dom.dedupGroupsList.querySelector('.dedup-sentinel');
  if (!s) {
    s = document.createElement('div');
    s.className = 'dedup-sentinel';
    s.setAttribute('aria-hidden', 'true');
    dom.dedupGroupsList.appendChild(s);
  }
  if (sentinelObserver) {
    sentinelObserver.observe(s);
  }
}

function appendNextChunk() {
  if (!review || !dom.dedupGroupsList) return;
  const total = review.groups.length;
  if (renderedGroupCount >= total) {
    removeSentinel();
    return;
  }

  const nextEnd = Math.min(renderedGroupCount + RENDER_CHUNK_SIZE, total);
  const chunkHtmlArr = [];
  for (let i = renderedGroupCount; i < nextEnd; i++) {
    chunkHtmlArr.push(renderGroupCardHtml(review.groups[i], i));
  }

  const existingSentinel = dom.dedupGroupsList.querySelector('.dedup-sentinel');
  if (existingSentinel) {
    existingSentinel.remove();
  }

  dom.dedupGroupsList.insertAdjacentHTML('beforeend', chunkHtmlArr.join(''));
  renderedGroupCount = nextEnd;

  if (renderedGroupCount < total) {
    ensureSentinel();
  } else {
    removeSentinel();
  }
}

function setupSentinelObserver() {
  if (sentinelObserver) {
    sentinelObserver.disconnect();
    sentinelObserver = null;
  }

  if (typeof IntersectionObserver !== 'undefined' && dom.deduplicateBody) {
    sentinelObserver = new IntersectionObserver(
      (entries) => {
        for (const entry of entries) {
          if (entry.isIntersecting) {
            appendNextChunk();
          }
        }
      },
      {
        root: dom.deduplicateBody,
        rootMargin: '400px 0px',
        threshold: 0,
      }
    );
  }

  ensureSentinel();
}

function renderReview() {
  if (!review || !dom.dedupGroupsList) return;
  const groups = review.groups;
  if (groups.length === 0) {
    setDialogState('empty');
    updateReviewActions();
    return;
  }

  updateSummaryBanner();
  updateReviewActions();

  ensureReviewDelegation();

  dom.dedupGroupsList.innerHTML = '';
  renderedGroupCount = 0;

  appendNextChunk();
  setupSentinelObserver();
}

function rerenderReviewPreservingScroll() {
  const body = dom.deduplicateBody;
  const scrollTop = body ? body.scrollTop : 0;
  renderReview();
  if (body) {
    body.scrollTop = scrollTop;
  }
}

function isReviewBusy() {
  return Boolean(resettingDismissals || review?.scanning || review?.resolving || review?.pendingDismissals.size);
}

function canDismissGroup(group) {
  return Boolean(group && review && !review.scanning && !review.resolving &&
    !resettingDismissals && !review.pendingDismissals.has(group));
}

function updateReviewActions() {
  const busy = isReviewBusy();
  setScanAndResetButtonsDisabled(busy);
  if (dom.cancelDeduplicateBtn) dom.cancelDeduplicateBtn.disabled = Boolean(review?.resolving);
  if (dom.dedupDismissAllBtn) dom.dedupDismissAllBtn.disabled = busy || !review?.groups.length;
  dom.dedupGroupsList?.querySelectorAll('.dedup-dismiss-group-btn').forEach(button => {
    button.disabled = !canDismissGroup(review?.groups[Number(button.dataset.groupIndex)]);
  });
  if (!dom.dedupResolveBtn) return;
  const enabledCount = review ? review.groups.filter((g) => g.enabled).length : 0;
  dom.dedupResolveBtn.disabled = busy || enabledCount === 0;
  dom.dedupResolveBtn.textContent =
    enabledCount === 1
      ? t('dedup_resolve_single')
      : t('dedup_resolve_groups', { count: enabledCount });
}

function setKeeper(groupIndex, mediaId) {
  if (!review || review.scanning || review.resolving || !review.groups[groupIndex]) return;
  review.groups[groupIndex].keeper = Number(mediaId);

  // Update only the targeted group card in DOM and preserve focus
  const card = dom.dedupGroupsList?.querySelector(
    `.dedup-group-card[data-group-index="${groupIndex}"]`
  );
  if (card) {
    const hadFocus = card.contains(document.activeElement);
    const temp = document.createElement('div');
    temp.innerHTML = renderGroupCardHtml(review.groups[groupIndex], groupIndex);
    const newCard = temp.firstElementChild;
    if (newCard) {
      card.replaceWith(newCard);
      if (hadFocus) {
        newCard.querySelector('.dedup-keeper-radio:checked')?.focus();
      }
    }
  }

  updateSummaryBanner();
}

function handleThumbClick(wrap, mediaId) {
  lastDedupLoupeTrigger = wrap.querySelector('.dedup-loupe-btn') || wrap;

  const groupCard = wrap.closest('.dedup-group-card');
  const groupIndex = groupCard ? Number(groupCard.dataset.groupIndex) : -1;
  const group = review && review.groups && groupIndex >= 0 ? review.groups[groupIndex] : null;

  const groupPhotos =
    group && Array.isArray(group.photos) && group.photos.length > 0 ? group.photos : null;

  if (groupPhotos) {
    for (const p of groupPhotos) {
      if (!state.mediaItems.some((m) => Number(m.id) === Number(p.id))) {
        state.mediaItems.push(p);
      }
    }
  } else {
    const existing = state.mediaItems.find((m) => Number(m.id) === mediaId);
    if (!existing && review && review.groups) {
      for (const g of review.groups) {
        const p = g.photos.find((x) => Number(x.id) === mediaId);
        if (p) {
          state.mediaItems.push(p);
          break;
        }
      }
    }
  }

  actionHooks.openLoupeForMedia(mediaId, { items: groupPhotos, fromDeduplicate: true });
}

function handleThumbError(img) {
  const mediaId = Number(img.dataset.mediaId);
  if (!Number.isFinite(mediaId)) return;
  let photo = null;
  if (review && review.groups) {
    for (const g of review.groups) {
      const found = g.photos && g.photos.find((p) => Number(p && p.id) === mediaId);
      if (found) {
        photo = found;
        break;
      }
    }
  }
  const isImage =
    !photo || !photo.media_type || photo.media_type === 'photo' || photo.media_type === 'image';
  if (!isImage) return;

  const fallbackUrl = photo ? getOriginalMediaUrl(photo) : `/api/photos/${mediaId}/original`;
  const currentSrcAttr = img.getAttribute('src');
  if (
    fallbackUrl &&
    currentSrcAttr !== fallbackUrl &&
    img.src !== new URL(fallbackUrl, window.location.href).href
  ) {
    img.src = fallbackUrl;
  }
}

function ensureReviewDelegation() {
  if (reviewDelegationAttached || !dom.dedupGroupsList) return;
  reviewDelegationAttached = true;

  // Group checkboxes and keeper radio buttons
  dom.dedupGroupsList.addEventListener('change', (e) => {
    const groupCheckbox = e.target.closest('.dedup-group-checkbox');
    if (groupCheckbox) {
      const idx = Number(groupCheckbox.dataset.groupIndex);
      if (review && !review.resolving && !review.scanning && review.groups[idx]) {
        review.groups[idx].enabled = groupCheckbox.checked;
        const card = groupCheckbox.closest('.dedup-group-card');
        if (card) card.classList.toggle('excluded', !groupCheckbox.checked);
        updateReviewActions();
        updateSummaryBanner();
      }
      return;
    }

    const radio = e.target.closest('.dedup-keeper-radio');
    if (radio) {
      const idx = Number(radio.dataset.groupIndex);
      const mediaId = Number(radio.dataset.mediaId);
      setKeeper(idx, mediaId);
    }
  });

  // Swap button, Dismiss button, Thumbnail/Loupe click
  dom.dedupGroupsList.addEventListener('click', (e) => {
    const swapBtn = e.target.closest('.dedup-swap-btn');
    if (swapBtn) {
      const idx = Number(swapBtn.dataset.groupIndex);
      const mediaId = Number(swapBtn.dataset.mediaId);
      setKeeper(idx, mediaId);
      return;
    }

    const dismissBtn = e.target.closest('.dedup-dismiss-group-btn');
    if (dismissBtn) {
      const idx = Number(dismissBtn.dataset.groupIndex);
      const group = review?.groups[idx];
      if (canDismissGroup(group)) dismissGroups([group]);
      return;
    }

    const wrap = e.target.closest('.dedup-thumb-wrap');
    if (wrap) {
      const mediaId = Number(wrap.dataset.mediaId);
      if (Number.isFinite(mediaId)) {
        handleThumbClick(wrap, mediaId);
      }
    }
  });

  // Thumbnail fallback on error (capturing listener handles all present and future imgs)
  dom.dedupGroupsList.addEventListener(
    'error',
    (e) => {
      if (e.target && e.target.tagName === 'IMG') {
        handleThumbError(e.target);
      }
    },
    true
  );

  // Fallback scroll listener on scroll container for fast scrolling
  dom.deduplicateBody?.addEventListener(
    'scroll',
    () => {
      if (!review || renderedGroupCount >= review.groups.length) return;
      const body = dom.deduplicateBody;
      if (body.scrollTop + body.clientHeight >= body.scrollHeight - 600) {
        appendNextChunk();
      }
    },
    { passive: true }
  );
}

async function dismissGroups(groups) {
  const activeReview = review;
  const dismissedGroups = new Set(groups);
  for (const group of dismissedGroups) activeReview.pendingDismissals.add(group);
  updateReviewActions();
  const batchSize = 500;
  let pairs = [];
  try {
    for (const group of dismissedGroups) {
      const members = group.members;
      for (let i = 0; i < members.length; i++) {
        for (let j = i + 1; j < members.length; j++) {
          pairs.push([Math.min(members[i], members[j]), Math.max(members[i], members[j])]);
          if (pairs.length === batchSize) {
            await api.post('/api/media/duplicates/dismiss', { pairs });
            pairs = [];
          }
        }
      }
    }
    if (pairs.length > 0) await api.post('/api/media/duplicates/dismiss', { pairs });
    if (review !== activeReview) return;

    activeReview.groups = activeReview.groups.filter(group => !dismissedGroups.has(group));
    showToast(t('dedup_toast_dismissed'), 'info');
    const totalGroupsCount = activeReview.groups.length;
    updateBadgeCount(totalGroupsCount);
    if (totalGroupsCount === 0) {
      setDialogState('empty');
    } else {
      rerenderReviewPreservingScroll();
    }
  } catch (error) {
    if (review === activeReview) showToast(error.message || String(error), 'error');
  } finally {
    for (const group of dismissedGroups) activeReview.pendingDismissals.delete(group);
    if (review === activeReview) updateReviewActions();
  }
}

async function resolveGroups() {
  if (!review || isReviewBusy()) return;
  const enabledGroups = review.groups.filter((g) => g.enabled);
  if (enabledGroups.length === 0) return;

  review.resolving = true;
  updateReviewActions();

  const mergeMetadata = dom.dedupMergeMetadataCheckbox
    ? dom.dedupMergeMetadataCheckbox.checked
    : true;
  const deleteFromDisk = dom.dedupDeleteFromDiskCheckbox
    ? dom.dedupDeleteFromDiskCheckbox.checked
    : false;

  const resolveGroupsPayload = enabledGroups.map((g) => ({
    keeper: Number(g.keeper),
    remove: g.members.filter((id) => Number(id) !== Number(g.keeper)).map(Number),
    expected_hashes: Object.fromEntries(g.photos.map(photo => [photo.id, photo.content_hash || ''])),
    merge_metadata: mergeMetadata,
    delete_from_disk: deleteFromDisk,
  }));

  const diskFailedIds = new Set();
  try {
    // Server enforces kMaxBatchSize = 1000 for both the groups array and total remove items.
    // Stay well under both limits (400 groups / 800 removals per request).
    const MAX_GROUPS_PER_REQUEST = 400;
    const MAX_REMOVALS_PER_REQUEST = 800;
    const resolvedKeepers = new Set();
    const failedKeepers = new Set();
    let anonymousFailures = 0;
    let currentChunk = [];
    let currentRemoveCount = 0;
    // For groups split across requests: keeper -> { totalParts, okParts }.
    // A split group is only considered resolved once every part succeeded,
    // so a partial failure keeps the group retryable (already-deleted members
    // are skipped server-side with per-item 404s).
    const splitGroupProgress = new Map();

    const applyResolvedKeepers = (keepers) => {
      if (keepers.size === 0) return;
      review.groups = review.groups.filter((g) => !keepers.has(Number(g.keeper)));
      updateBadgeCount(review.groups.length);
      if (review.groups.length === 0) {
        setDialogState('empty');
      } else {
        rerenderReviewPreservingScroll();
      }
    };

    const markPartResolved = (keeper) => {
      const progress = splitGroupProgress.get(keeper);
      if (!progress) {
        resolvedKeepers.add(keeper);
        return;
      }
      progress.ok += 1;
      if (progress.ok >= progress.total) {
        splitGroupProgress.delete(keeper);
        resolvedKeepers.add(keeper);
      }
    };

    const executeChunk = async (chunk) => {
      if (chunk.length === 0) return;
      const res = await api.post('/api/media/duplicates/resolve', {
        groups: chunk,
        merge_metadata: mergeMetadata,
        delete_from_disk: deleteFromDisk,
      });
      // Server returns numeric per-group statuses (200/400/404/500), never the string 'error'.
      if (Array.isArray(res.groups) && res.groups.length > 0) {
        for (const rg of res.groups) {
          for (const failure of rg.disk_failures || []) diskFailedIds.add(Number(failure.id));
          if (Number(rg.status) === 200 && rg.keeper) {
            markPartResolved(Number(rg.keeper));
          } else {
            const k = Number(rg?.keeper);
            if (k) {
              failedKeepers.add(k);
            } else {
              anonymousFailures += 1;
            }
          }
        }
      } else {
        for (const item of chunk) {
          markPartResolved(Number(item.keeper));
        }
      }
      applyResolvedKeepers(resolvedKeepers);
    };

    for (const groupItem of resolveGroupsPayload) {
      // A single group with more removals than the per-request cap must be split
      // across multiple requests; the server processes each group independently.
      const removeParts = [];
      for (let i = 0; i < groupItem.remove.length; i += MAX_REMOVALS_PER_REQUEST) {
        removeParts.push(groupItem.remove.slice(i, i + MAX_REMOVALS_PER_REQUEST));
      }
      const parts =
        removeParts.length > 1
          ? removeParts.map((remove) => ({
              ...groupItem,
              remove,
              expected_hashes: Object.fromEntries([groupItem.keeper, ...remove].map(id => [id, groupItem.expected_hashes[id]])),
            }))
          : [groupItem];
      if (parts.length > 1) {
        splitGroupProgress.set(Number(groupItem.keeper), { total: parts.length, ok: 0 });
      }

      for (const part of parts) {
        const removeLen = part.remove.length;
        if (
          currentChunk.length >= MAX_GROUPS_PER_REQUEST ||
          (currentRemoveCount + removeLen > MAX_REMOVALS_PER_REQUEST && currentChunk.length > 0)
        ) {
          await executeChunk(currentChunk);
          currentChunk = [];
          currentRemoveCount = 0;
        }
        currentChunk.push(part);
        currentRemoveCount += removeLen;
      }
    }
    if (currentChunk.length > 0) {
      await executeChunk(currentChunk);
    }

    const failedCount =
      [...failedKeepers].filter((k) => !resolvedKeepers.has(k)).length + anonymousFailures;

    if (resolvedKeepers.size > 0) {
      await Promise.all([actionHooks.loadMetadata(), actionHooks.loadMedia()]);
      actionHooks.updateInspector();
      if (failedCount > 0) {
        showToast(t('dedup_toast_resolved_partial', { count: failedCount }), 'error');
      } else {
        showToast(t('dedup_toast_resolved'), 'success');
      }
    } else if (failedCount > 0) {
      showToast(t('dedup_toast_resolved_partial', { count: failedCount }), 'error');
    }

    if (review.groups.length === 0) {
      review.resolving = false;
      closeDeduplicateModal();
    } else {
      rerenderReviewPreservingScroll();
    }
  } catch (error) {
    showToast(error.message || String(error), 'error');
  } finally {
    if (diskFailedIds.size > 0) {
      showToast(t('toast_disk_delete_incomplete', { count: diskFailedIds.size }), 'error');
    }
    if (review) {
      review.resolving = false;
      updateReviewActions();
    }
  }
}

let resettingDismissals = false;
let resetWarningResolver = null;

export function openResetWarningModal() {
  if (!dom.dedupResetWarningModal) return Promise.resolve(false);

  if (dom.dedupResetWarningPrompt) {
    dom.dedupResetWarningPrompt.textContent =
      t('dedup_reset_warning_prompt') ||
      'Are you sure you want to reset all dismissed duplicate pairs?';
  }
  if (dom.dedupResetWarningDesc) {
    dom.dedupResetWarningDesc.textContent =
      t('dedup_reset_warning_desc') ||
      'This clears all previously dismissed duplicate pairs in this catalog so future scans will detect them again. No files on disk will be deleted or changed.';
  }

  return new Promise((resolve) => {
    resetWarningResolver = resolve;
    dom.dedupResetWarningModal.style.display = 'flex';
    dom.cancelDedupResetWarningBtn?.focus();
  });
}

export async function resetDismissals() {
  const activeReview = review;
  if (!isDeduplicateOpen() || !activeReview || isReviewBusy()) return;

  const confirmed = await openResetWarningModal();
  if (!confirmed || review !== activeReview || isReviewBusy()) return;

  resettingDismissals = true;
  updateReviewActions();

  try {
    await api.post('/api/media/duplicates/dismissals/reset', {});
    showToast(t('dedup_toast_dismissals_reset'), 'info');
  } catch (error) {
    showToast(error.message || String(error), 'error');
    return;
  } finally {
    resettingDismissals = false;
    updateReviewActions();
  }
  if (review === activeReview) await scanDuplicates();
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

  // Reset dismissals button
  dom.dedupResetDismissalsBtn?.addEventListener('click', () => {
    resetDismissals();
  });
  dom.dedupResetDismissalsEmptyBtn?.addEventListener('click', () => {
    resetDismissals();
  });

  // Reset Warning Dialog buttons
  const dismissWarningModal = (accepted) => {
    if (resetWarningResolver) {
      const res = resetWarningResolver;
      resetWarningResolver = null;
      res(accepted);
    }
    if (dom.dedupResetWarningModal) {
      dom.dedupResetWarningModal.style.display = 'none';
    }
  };

  dom.closeDedupResetWarningBtn?.addEventListener('click', () => dismissWarningModal(false));
  dom.cancelDedupResetWarningBtn?.addEventListener('click', () => dismissWarningModal(false));
  dom.dedupResetWarningBackdrop?.addEventListener('click', () => dismissWarningModal(false));
  dom.confirmDedupResetWarningBtn?.addEventListener('click', () => dismissWarningModal(true));

  // Threshold slider
  dom.dedupVisualThresholdSlider?.addEventListener('input', (e) => {
    if (dom.dedupVisualThresholdValue) {
      dom.dedupVisualThresholdValue.textContent = e.target.value + '%';
    }
  });

  // Summary bulk actions
  dom.dedupSelectAllBtn?.addEventListener('click', () => {
    if (!review || review.scanning || review.resolving) return;
    review.groups.forEach((g) => {
      g.enabled = true;
    });
    dom.dedupGroupsList?.querySelectorAll('.dedup-group-checkbox').forEach((cb) => {
      cb.checked = true;
      cb.closest('.dedup-group-card')?.classList.remove('excluded');
    });
    updateSummaryBanner();
    updateReviewActions();
  });

  dom.dedupDeselectAllBtn?.addEventListener('click', () => {
    if (!review || review.scanning || review.resolving) return;
    review.groups.forEach((g) => {
      g.enabled = false;
    });
    dom.dedupGroupsList?.querySelectorAll('.dedup-group-checkbox').forEach((cb) => {
      cb.checked = false;
      cb.closest('.dedup-group-card')?.classList.add('excluded');
    });
    updateSummaryBanner();
    updateReviewActions();
  });

  dom.dedupDismissAllBtn?.addEventListener('click', () => {
    if (review?.groups.length && !isReviewBusy()) dismissGroups(review.groups);
  });

  // Resolve button
  dom.dedupResolveBtn?.addEventListener('click', () => {
    resolveGroups();
  });

  // Modal Escape key trap
  dom.deduplicateModal?.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') {
      if (dom.loupeModal && dom.loupeModal.style.display === 'flex') {
        return;
      }
      e.preventDefault();
      e.stopPropagation();
      closeDeduplicateModal();
    }
  });

  // Restore focus when Loupe closes
  window.addEventListener('imagine:loupeChanged', (e) => {
    if (e.detail?.mediaId === null && isDeduplicateOpen()) {
      if (lastDedupLoupeTrigger) {
        try {
          lastDedupLoupeTrigger.focus();
        } catch (_) {}
        lastDedupLoupeTrigger = null;
      }
    }
  });

  onLanguageChange(() => {
    if (isDeduplicateOpen() && review?.groups.length) {
      renderReview();
    }
  });
}
