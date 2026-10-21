/**
 * IMAGINE Photo Organizer - Grid Keyboard Accessibility & Roving Tabindex
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

import { state } from './state.js';
import { dom } from './dom.js';
import { getCardElement } from './media-cards.js';
import { handleCardSelection } from './media-selection.js';

function findCardInNextRow(currentCard, cards, currentIndex) {
  const currentRect = currentCard.getBoundingClientRect();
  const currentCenterX = currentRect.left + currentRect.width / 2;

  let nextRowCards = [];
  let minRowTop = Infinity;

  for (let i = currentIndex + 1; i < cards.length; i++) {
    const card = cards[i];
    const rect = card.getBoundingClientRect();
    if (rect.top >= currentRect.bottom - 4) {
      if (rect.top < minRowTop - 10) {
        minRowTop = rect.top;
        nextRowCards = [{ index: i, rect }];
      } else if (Math.abs(rect.top - minRowTop) <= 10) {
        nextRowCards.push({ index: i, rect });
      } else if (nextRowCards.length > 0 && rect.top > minRowTop + 10) {
        break;
      }
    }
  }

  if (nextRowCards.length > 0) {
    let bestIndex = nextRowCards[0].index;
    let minDiff = Infinity;
    for (const item of nextRowCards) {
      const centerX = item.rect.left + item.rect.width / 2;
      const diff = Math.abs(centerX - currentCenterX);
      if (diff < minDiff) {
        minDiff = diff;
        bestIndex = item.index;
      }
    }
    return bestIndex;
  }

  const gridEl = (currentCard && typeof currentCard.closest === 'function' ? currentCard.closest('.group-cards') : null) || dom.mediaGrid;
  const gridComputed = window.getComputedStyle && gridEl ? window.getComputedStyle(gridEl) : null;
  const colsTemplate = gridComputed ? gridComputed.getPropertyValue('grid-template-columns') : '';
  const colCount = colsTemplate && colsTemplate !== 'none' ? colsTemplate.trim().split(/\s+/).length : 4;
  return Math.min(cards.length - 1, currentIndex + colCount);
}

function findCardInPreviousRow(currentCard, cards, currentIndex) {
  const currentRect = currentCard.getBoundingClientRect();
  const currentCenterX = currentRect.left + currentRect.width / 2;

  let prevRowCards = [];
  let maxRowBottom = -Infinity;

  for (let i = currentIndex - 1; i >= 0; i--) {
    const card = cards[i];
    const rect = card.getBoundingClientRect();
    if (rect.bottom <= currentRect.top + 4) {
      if (rect.bottom > maxRowBottom + 10) {
        maxRowBottom = rect.bottom;
        prevRowCards = [{ index: i, rect }];
      } else if (Math.abs(rect.bottom - maxRowBottom) <= 10) {
        prevRowCards.push({ index: i, rect });
      } else if (prevRowCards.length > 0 && rect.bottom < maxRowBottom - 10) {
        break;
      }
    }
  }

  if (prevRowCards.length > 0) {
    let bestIndex = prevRowCards[0].index;
    let minDiff = Infinity;
    for (const item of prevRowCards) {
      const centerX = item.rect.left + item.rect.width / 2;
      const diff = Math.abs(centerX - currentCenterX);
      if (diff < minDiff) {
        minDiff = diff;
        bestIndex = item.index;
      }
    }
    return bestIndex;
  }

  const gridEl = (currentCard && typeof currentCard.closest === 'function' ? currentCard.closest('.group-cards') : null) || dom.mediaGrid;
  const gridComputed = window.getComputedStyle && gridEl ? window.getComputedStyle(gridEl) : null;
  const colsTemplate = gridComputed ? gridComputed.getPropertyValue('grid-template-columns') : '';
  const colCount = colsTemplate && colsTemplate !== 'none' ? colsTemplate.trim().split(/\s+/).length : 4;
  return Math.max(0, currentIndex - colCount);
}

export function updateRovingTabindex(preferredCard = null) {
  if (!dom.mediaGrid) return null;
  const cards = Array.from(dom.mediaGrid.querySelectorAll('.photo-card'));
  if (cards.length === 0) return null;

  let activeCard = preferredCard;
  if (!activeCard) {
    if (
      document.activeElement &&
      document.activeElement.classList.contains('photo-card') &&
      dom.mediaGrid.contains(document.activeElement)
    ) {
      activeCard = document.activeElement;
    } else if (state.lastSelectedId !== null) {
      activeCard = getCardElement(state.lastSelectedId);
    } else if (state.selectedIds && state.selectedIds.size > 0) {
      const firstSelectedId = state.selectedIds.values().next().value;
      activeCard = getCardElement(firstSelectedId);
    }
  }

  if (!activeCard || !cards.includes(activeCard)) {
    activeCard = cards[0];
  }

  for (const card of cards) {
    if (card === activeCard) {
      card.setAttribute('tabindex', '0');
    } else {
      card.setAttribute('tabindex', '-1');
    }
  }
  return activeCard;
}

export function handleGridArrowNavigation(event) {
  if (!dom.mediaGrid) return;
  if (state.loupeIndex >= 0 || (dom.loupeModal && dom.loupeModal.style.display === 'flex')) return;
  const activeModal = document.querySelector(
    '.modal:not([style*="display: none"]):not([style*="display:none"])'
  );
  if (activeModal) return;

  const currentCard = event.target.closest('.photo-card');
  if (!currentCard || !dom.mediaGrid.contains(currentCard)) return;

  const key = event.key;
  if (!['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', 'Home', 'End'].includes(key)) {
    return;
  }

  const cards = Array.from(dom.mediaGrid.querySelectorAll('.photo-card'));
  if (cards.length === 0) return;
  const currentIndex = cards.indexOf(currentCard);
  if (currentIndex === -1) return;

  let targetIndex = currentIndex;
  if (key === 'ArrowLeft') {
    targetIndex = Math.max(0, currentIndex - 1);
  } else if (key === 'ArrowRight') {
    targetIndex = Math.min(cards.length - 1, currentIndex + 1);
  } else if (key === 'Home') {
    targetIndex = 0;
  } else if (key === 'End') {
    targetIndex = cards.length - 1;
  } else if (key === 'ArrowDown') {
    targetIndex = findCardInNextRow(currentCard, cards, currentIndex);
  } else if (key === 'ArrowUp') {
    targetIndex = findCardInPreviousRow(currentCard, cards, currentIndex);
  }

  if (targetIndex !== currentIndex && targetIndex >= 0 && targetIndex < cards.length) {
    event.preventDefault();
    const targetCard = cards[targetIndex];
    updateRovingTabindex(targetCard);
    targetCard.focus();
    if (typeof targetCard.scrollIntoView === 'function') {
      targetCard.scrollIntoView({ block: 'nearest', inline: 'nearest' });
    }

    const cardId = parseInt(targetCard.dataset.id, 10);
    if (!isNaN(cardId)) {
      if (event.ctrlKey || event.metaKey) {
        // Move focus only, do not change selection
      } else if (event.shiftKey) {
        // Shift range selection
        handleCardSelection(cardId, { ctrlKey: false, metaKey: false, shiftKey: true });
      } else {
        // Single selection
        handleCardSelection(cardId, { ctrlKey: false, metaKey: false, shiftKey: false });
      }
    }
  }
}

let gridA11yInitialized = false;

export function initGridAccessibility() {
  if (gridA11yInitialized || !dom.mediaGrid) return;
  gridA11yInitialized = true;

  dom.mediaGrid.addEventListener('keydown', handleGridArrowNavigation);
  dom.mediaGrid.addEventListener('focusin', (e) => {
    const card = e.target.closest('.photo-card');
    if (card) {
      updateRovingTabindex(card);
    }
  });
}
