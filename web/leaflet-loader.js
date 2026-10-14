/**
 * IMAGINE Photo Organizer - Deferred Leaflet Loader
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

let leafletPromise = null;

export function ensureLeafletLoaded() {
  if (window.L) {
    return Promise.resolve(window.L);
  }
  if (leafletPromise) {
    return leafletPromise;
  }

  leafletPromise = new Promise((resolve, reject) => {
    if (!document.querySelector('link[href*="leaflet.css"]')) {
      const link = document.createElement('link');
      link.rel = 'stylesheet';
      link.href = 'vendor/leaflet/leaflet.css';
      document.head.appendChild(link);
    }

    if (window.L) {
      resolve(window.L);
      return;
    }

    const script = document.createElement('script');
    script.src = 'vendor/leaflet/leaflet.js';
    script.async = true;
    script.onload = () => {
      if (window.L) {
        resolve(window.L);
      } else {
        leafletPromise = null;
        script.remove();
        reject(new Error('Leaflet script loaded but window.L is undefined'));
      }
    };
    script.onerror = () => {
      leafletPromise = null;
      script.remove();
      reject(new Error('Failed to load Leaflet vendor script'));
    };
    document.head.appendChild(script);
  });

  return leafletPromise;
}
