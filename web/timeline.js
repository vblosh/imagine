/**
 * IMAGINE Photo Organizer - Timeline Bar Chart & Year Navigator
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

import { state } from './state.js';
import { dom } from './dom.js';
import { t, getShortMonthName } from './i18n.js';
import {
  createTimelinePeriod,
  timelinePeriodContainsMonth,
  isSingleMonthPeriod,
  isWholeYearPeriod,
} from './timeline-range.js';
import { clearTimelineSelection } from './media-params.js';

let actionHooks = {
  loadMedia: () => {},
  updateSidebarActive: () => {},
};

export function setTimelineActionHooks(hooks) {
  actionHooks = { ...actionHooks, ...hooks };
}

function renderTimelineYearSelect(entries) {
  if (!dom.timelineYearSelect) return;

  const years = [...new Set(entries.map(entry => entry.year))].sort((a, b) => b - a);
  const select = dom.timelineYearSelect;
  select.innerHTML = '';

  const allOption = document.createElement('option');
  allOption.value = 'all';
  allOption.textContent = t('all_years');
  select.appendChild(allOption);

  const period = state.activeTimelinePeriod;
  const wholeYear = isWholeYearPeriod(period);
  const listedWholeYear = wholeYear && years.includes(period.startYear);
  if (period && !listedWholeYear) {
    const customOption = document.createElement('option');
    customOption.value = 'custom';
    customOption.textContent = t('custom_range');
    select.appendChild(customOption);
  }

  years.forEach(year => {
    const option = document.createElement('option');
    option.value = String(year);
    option.textContent = String(year);
    select.appendChild(option);
  });

  select.disabled = years.length === 0;
  select.value = listedWholeYear ? String(period.startYear) : period ? 'custom' : 'all';
}

export function renderTimeline() {
  if (!dom.timelineContainer) return;
  dom.timelineContainer.innerHTML = '';

  const entries = [...state.timelineData].sort((a, b) => b.year - a.year || b.month - a.month);
  renderTimelineYearSelect(entries);

  if (entries.length === 0) {
    dom.timelineContainer.innerHTML = `<span class="timeline-empty">${t('no_timeline_data')}</span>`;
    return;
  }

  const maxCount = Math.max(...entries.map(entry => entry.count), 1);
  let activeWrap = null;

  const entriesByYear = new Map();
  entries.forEach(entry => {
    if (!entriesByYear.has(entry.year)) entriesByYear.set(entry.year, []);
    entriesByYear.get(entry.year).push(entry);
  });

  entriesByYear.forEach((yearEntries, year) => {
    const group = document.createElement('div');
    group.className = 'timeline-year-group';
    group.dataset.year = String(year);

    const bars = document.createElement('div');
    bars.className = 'timeline-year-bars';

    yearEntries.forEach(entry => {
      const wrap = document.createElement('button');
      wrap.type = 'button';
      wrap.className = 'timeline-bar-wrap';
      wrap.dataset.year = String(entry.year);
      wrap.dataset.month = String(entry.month);

      const isActive = timelinePeriodContainsMonth(
        state.activeTimelinePeriod,
        entry.year,
        entry.month
      );
      if (isActive) {
        wrap.classList.add('active');
        if (!activeWrap) activeWrap = wrap;
      }

      const heightPercent = Math.max(10, Math.round((entry.count / maxCount) * 100));
      const monthAbbr = getShortMonthName(entry.month);
      const itemNoun = entry.count === 1 ? t('photo_singular') : t('photo_plural');
      const title = `${monthAbbr} ${entry.year}: ${entry.count} ${itemNoun}`;

      wrap.title = title;
      wrap.setAttribute('aria-label', title);
      wrap.innerHTML = `<span class="timeline-bar" style="height: ${heightPercent}%;"></span>`;

      wrap.addEventListener('click', event => {
        const point = { year: entry.year, month: entry.month };
        const current = state.activeTimelinePeriod;

        if (event.shiftKey && state.timelineAnchor) {
          state.activeTimelinePeriod = createTimelinePeriod(state.timelineAnchor, point);
        } else if (
          isSingleMonthPeriod(current) &&
          current.startYear === entry.year &&
          current.startMonth === entry.month
        ) {
          clearTimelineSelection();
        } else {
          state.activeTimelinePeriod = createTimelinePeriod(point);
          state.timelineAnchor = point;
        }

        renderTimeline();
        actionHooks.updateSidebarActive();
        actionHooks.loadMedia();
      });

      bars.appendChild(wrap);
    });

    const yearLabel = document.createElement('span');
    yearLabel.className = 'timeline-year-label';
    yearLabel.textContent = String(year);
    group.appendChild(bars);
    group.appendChild(yearLabel);
    dom.timelineContainer.appendChild(group);
  });

  if (activeWrap) {
    activeWrap.scrollIntoView({ block: 'nearest', inline: 'nearest' });
  }
}

export function bindTimelineHandlers() {
  if (dom.resetTimelineBtn) {
    dom.resetTimelineBtn.addEventListener('click', () => {
      clearTimelineSelection();
      renderTimeline();
      actionHooks.updateSidebarActive();
      actionHooks.loadMedia();
    });
  }

  if (dom.timelineYearSelect) {
    dom.timelineYearSelect.addEventListener('change', (e) => {
      const val = e.target.value;
      if (val === 'all') {
        clearTimelineSelection();
      } else if (val !== 'custom') {
        const y = parseInt(val, 10);
        state.activeTimelinePeriod = {
          startYear: y,
          startMonth: 1,
          endYear: y,
          endMonth: 12,
        };
        state.timelineAnchor = null;
      }
      renderTimeline();
      actionHooks.updateSidebarActive();
      actionHooks.loadMedia();
    });
  }

  if (dom.timelineContainer) {
    dom.timelineContainer.addEventListener(
      'wheel',
      (e) => {
        if (e.deltaY !== 0 && e.deltaX === 0) {
          e.preventDefault();
          dom.timelineContainer.scrollLeft += e.deltaY;
        }
      },
      { passive: false }
    );
  }
}
