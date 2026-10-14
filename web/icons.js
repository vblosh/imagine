/**
 * IMAGINE Photo Organizer - SVG Icons Helper
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

export function icon(name, options = {}, heightArg) {
  let opts = options;
  if (typeof options === 'number') {
    opts = { width: options, height: heightArg || options };
  }
  const { className = '', width = 16, height = 16, strokeWidth = 2, extraAttrs = '' } = opts || {};
  const cls = className ? ` class="${className}"` : '';
  const sw = strokeWidth ? ` stroke-width="${strokeWidth}"` : '';
  const extra = extraAttrs ? ` ${extraAttrs}` : '';
  const a11yAttrs = /(?:aria-label|aria-labelledby|role=["']img["'])/.test(extraAttrs)
    ? ''
    : ' aria-hidden="true" focusable="false"';
  return `<svg${cls} viewBox="0 0 24 24" width="${width}" height="${height}" fill="none" stroke="currentColor"${sw}${a11yAttrs}${extra}><use href="#icon-${name}"></use></svg>`;
}
