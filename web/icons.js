/**
 * IMAGINE Photo Organizer - SVG Icons Helper
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

export function icon(name, options = {}) {
  let opts = options;
  if (typeof options === 'number') {
    opts = { width: options, height: arguments[2] || options };
  }
  const { className = '', width = 16, height = 16, strokeWidth = 2, extraAttrs = '' } = opts;
  const cls = className ? ` class="${className}"` : '';
  const sw = strokeWidth ? ` stroke-width="${strokeWidth}"` : '';
  const extra = extraAttrs ? ` ${extraAttrs}` : '';
  return `<svg${cls} viewBox="0 0 24 24" width="${width}" height="${height}" fill="none" stroke="currentColor"${sw}${extra}><use href="#icon-${name}"></use></svg>`;
}
