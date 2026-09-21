/**
 * Helpers for the inclusive month range selected by the timeline.
 */

function monthIndex(year, month) {
  return Number(year) * 12 + Number(month) - 1;
}

export function createTimelinePeriod(first, second = first) {
  if (!first || !second) return null;
  const firstIndex = monthIndex(first.year, first.month);
  const secondIndex = monthIndex(second.year, second.month);
  const start = firstIndex <= secondIndex ? first : second;
  const end = firstIndex <= secondIndex ? second : first;
  return {
    startYear: Number(start.year),
    startMonth: Number(start.month),
    endYear: Number(end.year),
    endMonth: Number(end.month)
  };
}

export function getTimelinePeriodBounds(period) {
  if (!period) return null;
  return {
    from: Math.floor(Date.UTC(period.startYear, period.startMonth - 1, 1, 0, 0, 0) / 1000),
    to: Math.floor(Date.UTC(period.endYear, period.endMonth, 1, 0, 0, 0) / 1000) - 1
  };
}

export function timelinePeriodContainsMonth(period, year, month) {
  if (!period) return false;
  const value = monthIndex(year, month);
  return value >= monthIndex(period.startYear, period.startMonth)
    && value <= monthIndex(period.endYear, period.endMonth);
}

export function timelinePeriodContainsTimestamp(period, timestamp) {
  const bounds = getTimelinePeriodBounds(period);
  return Boolean(bounds && timestamp >= bounds.from && timestamp <= bounds.to);
}

export function isSingleMonthPeriod(period) {
  return Boolean(period
    && period.startYear === period.endYear
    && period.startMonth === period.endMonth);
}

export function isWholeYearPeriod(period) {
  return Boolean(period
    && period.startYear === period.endYear
    && period.startMonth === 1
    && period.endMonth === 12);
}
