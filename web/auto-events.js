/** Local metadata based event suggestions and review/apply dialog. */
import { api, escapeHtml, formatDate } from './api.js';
import { state } from './state.js';
import { dom, showToast } from './dom.js';
import { t, getLocaleCode, onLanguageChange } from './i18n.js';
import { loadMedia, loadMetadata } from './media-grid.js';
import { updateInspector } from './inspector.js';

let requestSerial = 0;
let opener = null;
let review = null;

function dateLabel(timestamp) {
  return formatDate(timestamp);
}

function suggestedName(group, index, groupCount) {
  const source = group.name_source || 'date';
  if (source === 'event' && group.name_value) return truncateUtf8(group.name_value, 100);
  const date = dateLabel(group.start_date);
  let name = group.name_value ? `${group.name_value} — ${date}` : `${t('auto_event_default_name', { date })}`;
  if (groupCount > 1 && source !== 'event') {
    const start = new Date(Number(group.start_date) * 1000);
    name += ` ${start.toLocaleTimeString(getLocaleCode(), { timeZone: 'UTC', hour: '2-digit', minute: '2-digit' })}`;
  }
  return truncateUtf8(name, 100);
}

function sourceLabel(source) {
  const labels = {
    event: t('cat_events'), caption: t('caption'), place: t('cat_places'),
    folder: t('folder'), date: t('date')
  };
  return labels[source] || source || t('date');
}

function byteLength(value) {
  return new TextEncoder().encode(value).length;
}

function truncateUtf8(value, maxBytes) {
  let result = '';
  let size = 0;
  for (const character of String(value)) {
    const characterSize = byteLength(character);
    if (size + characterSize > maxBytes) break;
    result += character;
    size += characterSize;
  }
  return result;
}

function thumbUrl(photo) {
  if (photo.content_hash) return `/api/thumbnails/${encodeURIComponent(photo.content_hash)}/256`;
  return photo.thumb_small || '';
}

function groupPhotoIds(group) {
  const checked = group.photos.filter(photo => photo.included && !group.completedIds.has(Number(photo.id)));
  return checked.map(photo => Number(photo.id));
}

function closeReview() {
  review = null;
  requestSerial += 1;
  if (dom.autoEventModal) dom.autoEventModal.style.display = 'none';
  if (opener && typeof opener.focus === 'function') opener.focus();
  opener = null;
}

function setState(stateName, message = '') {
  if (!dom.autoEventLoading || !dom.autoEventGroups) return;
  dom.autoEventLoading.style.display = stateName === 'loading' ? 'block' : 'none';
  dom.autoEventEmpty.style.display = stateName === 'empty' ? 'block' : 'none';
  dom.autoEventError.style.display = stateName === 'error' ? 'block' : 'none';
  dom.autoEventError.textContent = message;
  dom.autoEventGroups.style.display = stateName === 'ready' ? 'block' : 'none';
}

function renderReview() {
  if (!review) return;
  const groups = review.groups;
  setState(groups.length ? 'ready' : 'empty');
  dom.autoEventGroups.innerHTML = groups.map((group, index) => {
    const ids = group.photos.map(photo => Number(photo.id));
    const available = ids.filter(id => !group.completedIds.has(id));
    const included = group.photos.filter(photo => photo.included && !group.completedIds.has(Number(photo.id)));
    const checked = group.enabled && included.length > 0;
    const locked = group.completedIds.size > 0;
    const invalid = byteLength(group.name.trim()) === 0 || byteLength(group.name.trim()) > 100;
    const photoHtml = group.photos.map(photo => {
      const id = Number(photo.id);
      const complete = group.completedIds.has(id);
      return `<label class="auto-event-photo${complete ? ' completed' : ''}" data-media-id="${id}">
        <input class="auto-event-photo-checkbox" type="checkbox" data-media-id="${id}" ${photo.included ? 'checked' : ''} ${complete ? 'disabled' : ''}>
        ${thumbUrl(photo) ? `<img src="${escapeHtml(thumbUrl(photo))}" alt="${escapeHtml(photo.file_name || '')}" loading="lazy">` : ''}
        <span>${escapeHtml(photo.file_name || String(id))}</span>
      </label>`;
    }).join('');
    return `<section class="auto-event-group${checked ? '' : ' disabled'}" data-group-index="${index}">
      <div class="auto-event-group-header">
        <input class="auto-event-group-checkbox" type="checkbox" ${checked ? 'checked' : ''} ${available.length ? '' : 'disabled'} aria-label="${escapeHtml(t('auto_event_include'))}">
        <input class="auto-event-name${invalid ? ' invalid' : ''}" type="text" value="${escapeHtml(group.name)}" maxlength="100" ${locked ? 'disabled' : ''} aria-label="${escapeHtml(t('auto_event_group'))}">
        <span class="auto-event-count">${escapeHtml(t('auto_event_photos', { count: included.length }))}</span>
        <button type="button" class="btn btn-xs btn-secondary auto-event-toggle-photos">${escapeHtml(t(group.expanded ? 'auto_event_hide_photos' : 'auto_event_show_photos'))}</button>
      </div>
      <div class="auto-event-meta">${escapeHtml(dateLabel(group.start_date))}${group.end_date !== group.start_date ? ` – ${escapeHtml(dateLabel(group.end_date))}` : ''} · ${escapeHtml(t('auto_event_reason', { source: sourceLabel(group.name_source) }))}</div>
      <div class="auto-event-evidence">${[...(group.people || []).map(x => `${t('cat_people')}: ${x}`), ...(group.places || []).map(x => `${t('cat_places')}: ${x}`), ...(group.keywords || []).map(x => `${t('cat_keyword')}: ${x}`)].map(x => `<span>${escapeHtml(x)}</span>`).join('')}</div>
      <div class="auto-event-photos" ${group.expanded ? '' : 'style="display:none"'}>${photoHtml}</div>
    </section>`;
  }).join('');
  const skipped = review.skipped || [];
  dom.autoEventSkipped.style.display = skipped.length ? 'block' : 'none';
  dom.autoEventSkipped.textContent = skipped.length
    ? t('auto_event_skipped', { details: skipped.map(item => `${item.id} (${item.reason})`).join(', ') }) : '';
  updateApplyButton(groups);
}

function focusDescriptor(element) {
  if (!element) return null;
  const group = element.closest('.auto-event-group');
  if (!group) return null;
  const index = group.dataset.groupIndex;
  if (element.matches('.auto-event-group-checkbox')) return { type: 'group', index };
  if (element.matches('.auto-event-toggle-photos')) return { type: 'toggle', index };
  if (element.matches('.auto-event-photo-checkbox')) {
    return { type: 'photo', index, mediaId: element.dataset.mediaId };
  }
  return null;
}

function restoreFocus(descriptor) {
  if (!descriptor) return;
  const group = dom.autoEventGroups.querySelector(`.auto-event-group[data-group-index="${descriptor.index}"]`);
  if (!group) return;
  let target = null;
  if (descriptor.type === 'group') target = group.querySelector('.auto-event-group-checkbox');
  if (descriptor.type === 'toggle') target = group.querySelector('.auto-event-toggle-photos');
  if (descriptor.type === 'photo') target = group.querySelector(`.auto-event-photo-checkbox[data-media-id="${descriptor.mediaId}"]`);
  target?.focus();
}

function rerenderReview(focusSource = null) {
  const descriptor = focusDescriptor(focusSource || document.activeElement);
  renderReview();
  bindReviewEvents();
  restoreFocus(descriptor);
}

function updateApplyButton(groups = review ? review.groups : []) {
  const pending = groups.some(group => groupPhotoIds(group).length > 0);
  const partial = groups.some(group => group.completedIds.size > 0 && groupPhotoIds(group).length > 0);
  dom.applyAutoEventsBtn.style.display = pending && !partial ? 'inline-flex' : 'none';
  dom.applyAutoEventsBtn.disabled = !pending || groups.some(group => {
    const idsForGroup = groupPhotoIds(group);
    return group.enabled && idsForGroup.length > 0 && (byteLength(group.name.trim()) === 0 || byteLength(group.name.trim()) > 100);
  });
  dom.retryAutoEventsBtn.style.display = partial ? 'inline-flex' : 'none';
  dom.retryAutoEventsBtn.disabled = !partial || groups.some(group => {
    const idsForGroup = groupPhotoIds(group);
    return group.enabled && idsForGroup.length > 0 && (byteLength(group.name.trim()) === 0 || byteLength(group.name.trim()) > 100);
  });
}

function bindReviewEvents() {
  dom.autoEventGroups.querySelectorAll('.auto-event-group').forEach((element, index) => {
    const group = review.groups[index];
    const groupCheckbox = element.querySelector('.auto-event-group-checkbox');
    const nameInput = element.querySelector('.auto-event-name');
    groupCheckbox?.addEventListener('change', () => { group.enabled = groupCheckbox.checked; rerenderReview(groupCheckbox); });
    nameInput?.addEventListener('input', () => {
      group.name = nameInput.value;
      nameInput.classList.toggle('invalid', byteLength(group.name.trim()) === 0 || byteLength(group.name.trim()) > 100);
      updateApplyButton();
    });
    element.querySelector('.auto-event-toggle-photos')?.addEventListener('click', (event) => { group.expanded = !group.expanded; rerenderReview(event.currentTarget); });
    element.querySelectorAll('.auto-event-photo-checkbox').forEach(input => input.addEventListener('change', () => {
      const photo = group.photos.find(item => Number(item.id) === Number(input.dataset.mediaId));
      if (photo && !group.completedIds.has(Number(photo.id))) photo.included = input.checked;
      rerenderReview(input);
    }));
  });
}

async function applyGroups() {
  if (!review || review.applying) return;
  review.applying = true;
  dom.applyAutoEventsBtn.disabled = true;
  dom.retryAutoEventsBtn.disabled = true;
  dom.cancelAutoEventsBtn.disabled = true;
  const failures = [];
  try {
    for (const group of review.groups) {
      const ids = groupPhotoIds(group);
      if (!group.enabled || !ids.length) continue;
      try {
        const result = await api.post('/api/media/batch-tags', { ids, name: group.name.trim(), category: 'events' });
        const failed = new Set((result.failed_ids || []).map(Number));
        ids.forEach(id => { if (!failed.has(id)) group.completedIds.add(id); });
        if (failed.size) failures.push(`${group.name}: ${failed.size}`);
      } catch (error) {
        failures.push(`${group.name}: ${error.message}`);
      }
    }
    if (failures.length) {
      if (review.groups.some(group => group.completedIds.size > 0)) {
        await loadMetadata();
        await loadMedia();
        updateInspector();
      }
      showToast(`${t('auto_event_retry')}: ${failures.join(', ')}`, 'error');
      rerenderReview();
    } else {
      await loadMetadata();
      await loadMedia();
      updateInspector();
      showToast(t('auto_event_applied'), 'success');
      closeReview();
    }
  } finally {
    if (review) {
      review.applying = false;
      dom.cancelAutoEventsBtn.disabled = false;
      rerenderReview();
    }
  }
}

export async function openAutoEventForIds(ids, button = null) {
  const unique = [...new Set((ids || []).map(Number).filter(Number.isInteger))];
  if (!unique.length || review?.applying) return;
  opener = button || document.activeElement;
  const serial = ++requestSerial;
  review = { groups: [], skipped: [], applying: false };
  dom.autoEventModal.style.display = 'flex';
  dom.autoEventModal.setAttribute('aria-busy', 'true');
  dom.cancelAutoEventsBtn?.focus();
  setState('loading');
  try {
    const result = await api.post('/api/media/event-suggestions', { ids: unique });
    if (serial !== requestSerial || !review) return;
    review.groups = (result.groups || []).map(group => ({
      ...group,
      photos: (group.photos || []).map(photo => ({ ...photo, included: true })),
      name: suggestedName(group, 0, result.groups.length),
      enabled: true,
      expanded: false,
      completedIds: new Set()
    }));
    review.skipped = result.skipped || [];
    dom.autoEventModal.removeAttribute('aria-busy');
    renderReview();
    bindReviewEvents();
    dom.cancelAutoEventsBtn?.focus();
  } catch (error) {
    if (serial !== requestSerial || !review) return;
    dom.autoEventModal.removeAttribute('aria-busy');
    setState('error', error.message || String(error));
  }
}

export function setupAutoEventListeners() {
  dom.batchAutoEventBtn?.addEventListener('click', event => openAutoEventForIds([...state.selectedIds], event.currentTarget));
  dom.inspectorAutoEventBtn?.addEventListener('click', event => {
    const ids = state.selectedIds.size ? [...state.selectedIds] : (state.lastSelectedId ? [state.lastSelectedId] : []);
    openAutoEventForIds(ids, event.currentTarget);
  });
  dom.applyAutoEventsBtn?.addEventListener('click', applyGroups);
  dom.retryAutoEventsBtn?.addEventListener('click', applyGroups);
  dom.cancelAutoEventsBtn?.addEventListener('click', closeReview);
  dom.closeAutoEventModalBtn?.addEventListener('click', () => { if (!review?.applying) closeReview(); });
  dom.autoEventBackdrop?.addEventListener('click', () => { if (!review?.applying) closeReview(); });
  onLanguageChange(() => { if (review && !review.applying) rerenderReview(); });
}

export function isAutoEventOpen() { return Boolean(review); }
export function cancelAutoEventReview() { if (review && !review.applying) closeReview(); }
