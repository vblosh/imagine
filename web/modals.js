/**
 * IMAGINE Photo Organizer - Modal Dialogs Management
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

import {
  api,
  escapeHtml,
  formatDate,
  formatBytes,
  normalizeImportProgress,
  getOriginalMediaUrl
} from './api.js';
import { state } from './state.js';
import { dom, showToast } from './dom.js';
import { loadMetadata, loadMedia, updateBatchBar } from './media-grid.js';
import { updateInspector } from './inspector.js';
import { closeLoupe } from './loupe.js';

// Internal pending state
let pendingAddToAlbumIds = [];
let pendingTagTargetMediaIds = [];
let pendingDeleteMediaIds = [];
let pendingDeleteTag = null;

// --- Import Modal & Workflow ---
export function openImportModal() {
  if (dom.importModal) dom.importModal.style.display = 'flex';
  if (state.isImporting) {
    if (dom.importProgressBox) dom.importProgressBox.style.display = 'flex';
    if (dom.startImportBtn) dom.startImportBtn.disabled = true;
  } else {
    if (dom.startImportBtn) dom.startImportBtn.disabled = false;
    if (dom.importPathInput) dom.importPathInput.focus();
  }
}

export function closeImportModal() {
  if (dom.importModal) dom.importModal.style.display = 'none';
  if (dom.importProgressBox) dom.importProgressBox.style.display = 'none';
  if (!state.isImporting && state.importPollInterval) {
    clearTimeout(state.importPollInterval);
    state.importPollInterval = null;
  }
}

export async function cancelImport() {
  if (state.isImporting) {
    try {
      if (dom.importStatusCounts) {
        dom.importStatusCounts.textContent = 'Cancelling import...';
      }
      await api.post('/api/import/cancel');
    } catch (err) {
      console.warn('Failed to cancel import:', err);
    }
  }
  closeImportModal();
}

export async function pollImportProgress() {
  if (!state.isImporting) return;

  try {
    const rawProg = await api.get('/api/import/progress');
    if (!state.isImporting) return;
    const prog = normalizeImportProgress(rawProg);

    const total = prog.total_files || 0;
    const processed = prog.processed_files || 0;
    const imported = prog.imported_files || 0;
    const skipped = prog.skipped_files || 0;
    const failed = prog.failed_files || 0;

    if (total > 0 && dom.importProgressBar) {
      const percent = Math.min(100, Math.round((processed / total) * 100));
      dom.importProgressBar.style.width = `${percent}%`;
    }

    if (prog.is_running && total === 0) {
      if (dom.importProgressBar) dom.importProgressBar.style.width = '10%';
      if (dom.importStatusCounts) {
        dom.importStatusCounts.textContent = 'Scanning folders...';
      }
      if (dom.importCurrentFile) {
        dom.importCurrentFile.textContent = prog.current_file || 'Scanning folders...';
      }
    } else {
      if (dom.importStatusCounts) {
        dom.importStatusCounts.textContent =
          `Processed: ${processed} / ${total} (Imported: ${imported}, Skipped: ${skipped}, Failed: ${failed})`;
      }
      if (dom.importCurrentFile) {
        dom.importCurrentFile.textContent = prog.current_file || '';
      }
    }

    if (prog.is_running) {
      state.importPollInterval = setTimeout(pollImportProgress, 500);
    } else {
      state.importPollInterval = null;
      state.isImporting = false;
      if (dom.importProgressBar) dom.importProgressBar.style.width = '100%';
      const summaryMsg = (total === 0 && imported === 0 && skipped === 0)
        ? 'Import finished: no new photos found.'
        : `Completed! ${imported} imported, ${skipped} skipped.`;
      if (dom.importStatusCounts) dom.importStatusCounts.textContent = summaryMsg;
      if (dom.startImportBtn) dom.startImportBtn.disabled = false;
      setTimeout(() => {
        if (dom.importModal) dom.importModal.style.display = 'none';
        if (dom.importProgressBox) dom.importProgressBox.style.display = 'none';
        loadMetadata();
        loadMedia();
        showToast(summaryMsg, 'success');
      }, 1200);
    }
  } catch (pollErr) {
    console.warn('Progress poll error:', pollErr);
    if (state.isImporting) {
      state.importPollInterval = setTimeout(pollImportProgress, 1000);
    }
  }
}

export async function triggerImport(path, recursive) {
  try {
    state.isImporting = true;
    if (dom.importProgressBox) dom.importProgressBox.style.display = 'flex';
    if (dom.importProgressBar) dom.importProgressBar.style.width = '5%';
    if (dom.importStatusCounts) dom.importStatusCounts.textContent = 'Import started...';
    if (dom.startImportBtn) dom.startImportBtn.disabled = true;

    await api.post('/api/import', { path, recursive });

    // Poll progress via recursive setTimeout
    if (state.importPollInterval) {
      clearTimeout(state.importPollInterval);
      state.importPollInterval = null;
    }
    state.importPollInterval = setTimeout(pollImportProgress, 500);

  } catch (err) {
    state.isImporting = false;
    alert(`Import error: ${err.message}`);
    if (dom.startImportBtn) dom.startImportBtn.disabled = false;
    if (dom.importProgressBox) dom.importProgressBox.style.display = 'none';
  }
}

// --- New Album Modal ---
export function openAlbumModal() {
  if (dom.newAlbumModal) dom.newAlbumModal.style.display = 'flex';
  if (dom.albumNameInput) {
    dom.albumNameInput.value = '';
    dom.albumNameInput.focus();
  }
  if (dom.albumDescInput) dom.albumDescInput.value = '';
}

export function closeAlbumModal() {
  if (dom.newAlbumModal) dom.newAlbumModal.style.display = 'none';
}

export async function submitCreateAlbum() {
  const name = dom.albumNameInput ? dom.albumNameInput.value.trim() : '';
  if (!name) {
    alert('Album name is required');
    return;
  }
  try {
    await api.post('/api/albums', {
      name,
      description: dom.albumDescInput ? dom.albumDescInput.value.trim() : ''
    });
    closeAlbumModal();
    loadMetadata();
  } catch (err) {
    alert(`Failed to create album: ${err.message}`);
  }
}

// --- Add to Album Modal ---
export function openAddToAlbumModal(mediaIds) {
  if (!mediaIds || mediaIds.length === 0) return;
  pendingAddToAlbumIds = mediaIds;
  const count = mediaIds.length;
  if (dom.addToAlbumTargetCount) {
    dom.addToAlbumTargetCount.textContent = `Add ${count} selected photo${count > 1 ? 's' : ''} to:`;
  }

  if (!state.albums || state.albums.length === 0) {
    if (dom.addToAlbumSelectGroup) dom.addToAlbumSelectGroup.style.display = 'none';
    if (dom.noAlbumsNotice) dom.noAlbumsNotice.style.display = 'block';
    if (dom.confirmAddToAlbumBtn) dom.confirmAddToAlbumBtn.disabled = true;
  } else {
    if (dom.addToAlbumSelectGroup) dom.addToAlbumSelectGroup.style.display = 'flex';
    if (dom.noAlbumsNotice) dom.noAlbumsNotice.style.display = 'none';
    if (dom.confirmAddToAlbumBtn) dom.confirmAddToAlbumBtn.disabled = false;

    if (dom.addToAlbumSelect) {
      dom.addToAlbumSelect.innerHTML = state.albums.map(album =>
        `<option value="${album.id}">${escapeHtml(album.name)} (${album.item_count || 0} photos)</option>`
      ).join('');
    }
  }

  if (dom.addToAlbumModal) {
    dom.addToAlbumModal.style.display = 'flex';
  }
}

export function closeAddToAlbumModal() {
  if (dom.addToAlbumModal) {
    dom.addToAlbumModal.style.display = 'none';
  }
  pendingAddToAlbumIds = [];
}

export async function submitAddToAlbum() {
  if (pendingAddToAlbumIds.length === 0) {
    closeAddToAlbumModal();
    return;
  }
  const albumId = dom.addToAlbumSelect ? parseInt(dom.addToAlbumSelect.value, 10) : NaN;
  if (!albumId) return;

  try {
    await api.post(`/api/albums/${albumId}/media`, {
      media_ids: pendingAddToAlbumIds
    });
    closeAddToAlbumModal();
    await loadMetadata();
    if (state.activeAlbumId === albumId) {
      await loadMedia();
    }
  } catch (err) {
    alert(`Failed to add photos to album: ${err.message}`);
  }
}

// --- Tag Modal Dialog ---
export function openTagModal(targetMediaIds = null) {
  if (!dom.newTagModal) return;

  if (targetMediaIds === null) {
    pendingTagTargetMediaIds = state.selectedIds.size > 0 ? Array.from(state.selectedIds) : [];
  } else {
    pendingTagTargetMediaIds = Array.isArray(targetMediaIds) ? [...targetMediaIds] : [];
  }

  const hasTargets = pendingTagTargetMediaIds.length > 0;
  if (dom.tagModalPhotoTarget) {
    dom.tagModalPhotoTarget.style.display = hasTargets ? 'flex' : 'none';
  }
  if (dom.tagModalApplyGroup) {
    dom.tagModalApplyGroup.style.display = hasTargets ? 'block' : 'none';
  }
  if (dom.tagModalApplyToPhotoCheckbox) {
    dom.tagModalApplyToPhotoCheckbox.checked = true;
  }

  if (hasTargets) {
    const firstId = pendingTagTargetMediaIds[0];
    const firstItem = state.mediaItems.find(m => m.id === firstId);
    const count = pendingTagTargetMediaIds.length;

    if (dom.tagModalHeading) {
      dom.tagModalHeading.textContent = count === 1 ? 'Add Tag to Photo' : `Add Tag to ${count} Photos`;
    }
    if (dom.createTagSubmitBtn) {
      dom.createTagSubmitBtn.textContent = count === 1 ? 'Add Tag to Photo' : 'Add Tag to Photos';
    }
    if (dom.tagModalApplyLabel) {
      dom.tagModalApplyLabel.textContent = count === 1
        ? 'Attach tag to selected photo'
        : `Attach tag to ${count} selected photos`;
    }

    if (firstItem) {
      if (dom.tagModalPhotoThumb) {
        dom.tagModalPhotoThumb.src = firstItem.content_hash
          ? `/api/thumbnails/${encodeURIComponent(firstItem.content_hash)}/256`
          : getOriginalMediaUrl(firstItem);
        dom.tagModalPhotoThumb.onerror = () => {
          dom.tagModalPhotoThumb.src = getOriginalMediaUrl(firstItem);
        };
      }
      if (dom.tagModalPhotoName) {
        dom.tagModalPhotoName.textContent = count === 1 ? (firstItem.file_name || 'Selected Photo') : `${count} photos selected`;
      }
      if (dom.tagModalPhotoMeta) {
        if (count === 1) {
          const metaParts = [];
          if (firstItem.date_taken) metaParts.push(formatDate(firstItem.date_taken));
          if (firstItem.width && firstItem.height) metaParts.push(`${firstItem.width} × ${firstItem.height}`);
          if (firstItem.file_size) metaParts.push(formatBytes(firstItem.file_size));
          dom.tagModalPhotoMeta.textContent = metaParts.join(' • ') || 'Photo details';
        } else {
          dom.tagModalPhotoMeta.textContent = `${count} items in current selection`;
        }
      }
      if (dom.tagModalPhotoCurrentTags) {
        if (count === 1 && firstItem.tags && firstItem.tags.length > 0) {
          dom.tagModalPhotoCurrentTags.innerHTML = firstItem.tags
            .map(t => `<span class="tag-badge" data-category="${escapeHtml((t.category || 'keyword').toLowerCase())}">${escapeHtml(t.name)}</span>`)
            .join('');
        } else if (count === 1) {
          dom.tagModalPhotoCurrentTags.innerHTML = '<span class="tag-target-no-tags">No tags yet</span>';
        } else {
          dom.tagModalPhotoCurrentTags.innerHTML = `<span class="tag-target-no-tags">${count} photos selected</span>`;
        }
      }
    }
    if (dom.tagModalPhotoCountBadge) {
      if (count > 1) {
        dom.tagModalPhotoCountBadge.style.display = 'inline-block';
        dom.tagModalPhotoCountBadge.textContent = `+${count - 1}`;
      } else {
        dom.tagModalPhotoCountBadge.style.display = 'none';
      }
    }
  } else {
    if (dom.tagModalHeading) dom.tagModalHeading.textContent = 'Create Keyword Tag';
    if (dom.createTagSubmitBtn) dom.createTagSubmitBtn.textContent = 'Create Tag';
  }

  // Reset inputs
  if (dom.tagNameInput) {
    dom.tagNameInput.value = '';
  }
  if (dom.tagModalClearInputBtn) {
    dom.tagModalClearInputBtn.style.display = 'none';
  }
  if (dom.tagDetectedBadge) {
    dom.tagDetectedBadge.style.display = 'none';
  }

  // Default category: active category tab if filtered, else 'keyword'
  let initialCat = 'keyword';
  if (['people', 'places', 'events', 'keyword'].includes(state.activeTab)) {
    initialCat = state.activeTab;
  }
  setModalCategory(initialCat);

  // Populate datalist suggestions
  updateModalTagSuggestions();

  // Render search help chips
  renderModalSearchHelp();

  dom.newTagModal.style.display = 'flex';
  if (dom.tagNameInput) {
    dom.tagNameInput.focus();
  }
}

export function closeTagModal() {
  if (dom.newTagModal) {
    dom.newTagModal.style.display = 'none';
  }
  pendingTagTargetMediaIds = [];
}

export function setModalCategory(category) {
  const cat = (category || 'keyword').toLowerCase();
  if (dom.tagCategorySelect && dom.tagCategorySelect.value !== cat) {
    dom.tagCategorySelect.value = cat;
  }
  if (dom.tagCategoryCards) {
    dom.tagCategoryCards.querySelectorAll('.category-card').forEach(btn => {
      if (btn.getAttribute('data-cat') === cat) {
        btn.classList.add('active');
      } else {
        btn.classList.remove('active');
      }
    });
  }
  renderModalSearchHelp();
}

export function updateModalTagSuggestions() {
  if (!dom.tagModalSuggestions) return;
  dom.tagModalSuggestions.innerHTML = state.tags
    .map(t => `<option value="${escapeHtml(t.name)}">`)
    .join('');
}

export function getCategoryColor(category) {
  switch ((category || '').toLowerCase()) {
    case 'people': return '#5cd65c';
    case 'events': return '#d966ff';
    case 'places': return '#4da6ff';
    default: return '#aaaaaa';
  }
}

export function getCategoryDisplayName(category) {
  switch ((category || '').toLowerCase()) {
    case 'people': return 'People';
    case 'events': return 'Events';
    case 'places': return 'Places';
    default: return 'Keyword';
  }
}

export function renderModalSearchHelp() {
  if (!dom.tagSearchHelpChips || !dom.tagNameInput) return;

  const query = dom.tagNameInput.value.trim().toLowerCase();
  const currentCat = dom.tagCategorySelect ? dom.tagCategorySelect.value.toLowerCase() : 'keyword';

  if (!query) {
    // Mode 1: No query -> Show quick pick tags from the active category
    const categoryTags = state.tags.filter(t => (t.category || 'keyword').toLowerCase() === currentCat);
    if (dom.tagSearchHelpTitle) {
      dom.tagSearchHelpTitle.textContent = `${getCategoryDisplayName(currentCat)} Tags (${categoryTags.length})`;
    }
    if (dom.tagSearchHelpSub) {
      dom.tagSearchHelpSub.textContent = 'Click to select tag';
    }
    if (dom.tagDetectedBadge) {
      dom.tagDetectedBadge.style.display = 'none';
    }

    if (categoryTags.length === 0) {
      dom.tagSearchHelpChips.innerHTML = `<div class="tag-help-empty">No tags in ${getCategoryDisplayName(currentCat)} yet. Type above to create one.</div>`;
    } else {
      dom.tagSearchHelpChips.innerHTML = categoryTags
        .map(t => `
          <button type="button" class="tag-badge tag-help-chip" data-category="${escapeHtml((t.category || 'keyword').toLowerCase())}" data-tag-name="${escapeHtml(t.name)}" title="Select ${escapeHtml(t.name)}">
            ${escapeHtml(t.name)}
            <span class="chip-count">(${t.media_count || 0})</span>
          </button>
        `).join('');
    }
  } else {
    // Mode 2: Query present -> Search help across all tags
    const matchingTags = state.tags.filter(t => t.name.toLowerCase().includes(query));
    const exactMatch = state.tags.find(t => t.name.toLowerCase() === query);

    if (exactMatch) {
      if (dom.tagDetectedBadge) {
        dom.tagDetectedBadge.style.display = 'inline-block';
        dom.tagDetectedBadge.textContent = `Existing in ${getCategoryDisplayName(exactMatch.category)}`;
        dom.tagDetectedBadge.style.borderLeft = `2px solid ${getCategoryColor(exactMatch.category)}`;
      }
      if (dom.tagCategorySelect && dom.tagCategorySelect.value !== exactMatch.category.toLowerCase()) {
        setModalCategory(exactMatch.category.toLowerCase());
      }
    } else {
      if (dom.tagDetectedBadge) {
        dom.tagDetectedBadge.style.display = 'inline-block';
        dom.tagDetectedBadge.textContent = `New tag in ${getCategoryDisplayName(currentCat)}`;
        dom.tagDetectedBadge.style.borderLeft = `2px solid ${getCategoryColor(currentCat)}`;
      }
    }

    if (dom.tagSearchHelpTitle) {
      dom.tagSearchHelpTitle.textContent = matchingTags.length > 0
        ? `Matching Tags (${matchingTags.length})`
        : 'No Matching Existing Tags';
    }
    if (dom.tagSearchHelpSub) {
      dom.tagSearchHelpSub.textContent = matchingTags.length > 0 ? 'Click to select' : 'Will create new tag';
    }

    let html = '';
    if (matchingTags.length > 0) {
      html = matchingTags.map(t => {
        const isExact = t.name.toLowerCase() === query;
        return `
          <button type="button" class="tag-badge tag-help-chip ${isExact ? 'active-match' : ''}" data-category="${escapeHtml((t.category || 'keyword').toLowerCase())}" data-tag-name="${escapeHtml(t.name)}" title="Select ${escapeHtml(t.name)}">
            <span class="chip-cat-prefix">${getCategoryDisplayName(t.category)}:</span>
            <strong>${escapeHtml(t.name)}</strong>
            <span class="chip-count">(${t.media_count || 0})</span>
          </button>
        `;
      }).join('');
    }

    if (!exactMatch) {
      html += `<div class="tag-help-create-prompt">Press Enter or click <strong>${dom.createTagSubmitBtn ? dom.createTagSubmitBtn.textContent : 'Create Tag'}</strong> to add "<strong>${escapeHtml(dom.tagNameInput.value.trim())}</strong>" to <em>${getCategoryDisplayName(currentCat)}</em></div>`;
    }

    dom.tagSearchHelpChips.innerHTML = html;
  }

  // Attach click listeners to chips
  dom.tagSearchHelpChips.querySelectorAll('.tag-help-chip').forEach(chip => {
    chip.addEventListener('click', () => {
      const tagName = chip.getAttribute('data-tag-name');
      const cat = chip.getAttribute('data-category');
      if (tagName && dom.tagNameInput) {
        dom.tagNameInput.value = tagName;
        if (dom.tagModalClearInputBtn) dom.tagModalClearInputBtn.style.display = 'inline-block';
      }
      if (cat) {
        setModalCategory(cat);
      }
      renderModalSearchHelp();
      if (dom.tagNameInput) dom.tagNameInput.focus();
    });
  });
}

export async function submitCreateTag() {
  const name = dom.tagNameInput ? dom.tagNameInput.value.trim() : '';
  if (!name) {
    alert('Tag name is required');
    return;
  }
  const category = dom.tagCategorySelect ? dom.tagCategorySelect.value : 'keyword';

  if (dom.tagModalApplyToPhotoCheckbox && dom.tagModalApplyToPhotoCheckbox.checked && pendingTagTargetMediaIds.length > 0) {
    try {
      await api.post('/api/media/batch-tags', { ids: pendingTagTargetMediaIds, name, category });
    } catch (e) {
      console.warn('Failed to batch attach tag to photos:', e);
    }
  } else {
    try {
      await api.post('/api/tags', {
        name,
        category
      });
    } catch (err) {
      if (!err.message || !err.message.includes('already exists')) {
        console.warn('Create tag API notice:', err);
      }
    }
  }

  closeTagModal();
  await loadMetadata();
  updateInspector();
  if (state.activeTab === category) {
    await loadMedia();
  }
}

// --- Delete Media Modal ---
export function openDeleteMediaModal(mediaIds) {
  if (!mediaIds || mediaIds.length === 0) return;
  pendingDeleteMediaIds = mediaIds;
  const count = mediaIds.length;
  if (dom.deleteMediaPromptText) {
    dom.deleteMediaPromptText.textContent = count === 1
      ? 'Are you sure you want to delete this photo from the catalog?'
      : `Are you sure you want to delete ${count} selected photos from the catalog?`;
  }
  if (dom.deleteFromDiskCheckbox) {
    dom.deleteFromDiskCheckbox.checked = false;
  }
  if (dom.deleteMediaWarningText) {
    dom.deleteMediaWarningText.textContent =
      'This removes photo metadata, ratings, tags, and album associations from the catalog database. The original files on disk will not be deleted.';
    dom.deleteMediaWarningText.style.color = 'var(--text-dim)';
  }
  if (dom.confirmDeleteMediaBtn) {
    dom.confirmDeleteMediaBtn.textContent = 'Delete from Catalog';
  }
  if (dom.deleteMediaModal) {
    dom.deleteMediaModal.style.display = 'flex';
  }
}

export function closeDeleteMediaModal() {
  if (dom.deleteMediaModal) {
    dom.deleteMediaModal.style.display = 'none';
  }
  if (dom.deleteFromDiskCheckbox) {
    dom.deleteFromDiskCheckbox.checked = false;
  }
  if (dom.deleteMediaWarningText) {
    dom.deleteMediaWarningText.textContent =
      'This removes photo metadata, ratings, tags, and album associations from the catalog database. The original files on disk will not be deleted.';
    dom.deleteMediaWarningText.style.color = 'var(--text-dim)';
  }
  if (dom.confirmDeleteMediaBtn) {
    dom.confirmDeleteMediaBtn.textContent = 'Delete from Catalog';
  }
  pendingDeleteMediaIds = [];
}

export async function submitDeleteMedia() {
  if (pendingDeleteMediaIds.length === 0) {
    closeDeleteMediaModal();
    return;
  }

  const idsToDelete = [...pendingDeleteMediaIds];
  const deleteFromDisk = !!(dom.deleteFromDiskCheckbox && dom.deleteFromDiskCheckbox.checked);
  const failedIds = [];
  try {
    await api.post('/api/media/batch-delete', { ids: idsToDelete, delete_from_disk: deleteFromDisk });
  } catch (e) {
    for (const id of idsToDelete) {
      try {
        await api.del(`/api/media/${id}${deleteFromDisk ? '?delete_from_disk=true' : ''}`);
      } catch (err) {
        console.warn('Failed to delete media', id, err);
        failedIds.push(id);
      }
    }
  }

  if (failedIds.length > 0) {
    showToast(`${failedIds.length} photos could not be deleted`, 'error');
  } else if (deleteFromDisk) {
    showToast(`Deleted ${idsToDelete.length} photo${idsToDelete.length === 1 ? '' : 's'} from catalog and disk`, 'success');
  }

  const actuallyDeletedIds = idsToDelete.filter(id => !failedIds.includes(id));

  // If loupe was viewing one of the deleted items, close loupe
  if (state.loupeIndex >= 0) {
    const loupeItem = state.mediaItems[state.loupeIndex];
    if (loupeItem && actuallyDeletedIds.includes(loupeItem.id)) {
      closeLoupe();
    }
  }

  actuallyDeletedIds.forEach(id => state.selectedIds.delete(id));
  closeDeleteMediaModal();

  await loadMedia();
  await loadMetadata();
  updateBatchBar();
  updateInspector();
}

// --- Delete Tag Modal ---
export function openDeleteTagModal(tag) {
  if (!tag) return;
  if (typeof tag === 'number' || typeof tag === 'string') {
    const found = (state.tags || []).find(t => t.id === tag || t.id === Number(tag));
    pendingDeleteTag = found || { id: tag, name: String(tag) };
  } else {
    pendingDeleteTag = tag;
  }
  const tagName = pendingDeleteTag.name || 'this tag';
  if (dom.deleteTagPromptText) {
    dom.deleteTagPromptText.textContent = `Are you sure you want to delete tag "${tagName}"?`;
  }
  if (dom.deleteTagWarningText) {
    dom.deleteTagWarningText.textContent =
      'This removes the tag from all associated photos in the catalog database. The original files on disk will not be deleted.';
  }
  if (dom.deleteTagModal) {
    dom.deleteTagModal.style.display = 'flex';
  }
}

export function closeDeleteTagModal() {
  if (dom.deleteTagModal) {
    dom.deleteTagModal.style.display = 'none';
  }
  pendingDeleteTag = null;
}

export async function submitDeleteTag() {
  if (!pendingDeleteTag) {
    closeDeleteTagModal();
    return;
  }

  const tag = pendingDeleteTag;
  closeDeleteTagModal();

  try {
    await api.del(`/api/tags/${tag.id}`);
    if (state.activeTagId === tag.id) {
      state.activeTagId = null;
    }
    await loadMetadata();
    loadMedia();
    updateInspector();
    showToast(`Tag "${tag.name}" deleted.`, 'info');
  } catch (err) {
    console.error('Failed to delete tag:', err);
    showToast('Failed to delete tag: ' + (err.message || 'Server error'), 'error');
  }
}

