/**
 * IMAGINE Photo Organizer - API & Data Normalization Module
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

export const api = {
  async get(endpoint, params = {}, options = {}) {
    const url = new URL(endpoint, window.location.origin);
    Object.keys(params).forEach(k => {
      if (params[k] !== undefined && params[k] !== null && params[k] !== '') {
        url.searchParams.append(k, params[k]);
      }
    });
    const fetchOpts = {};
    if (options && options.signal) {
      fetchOpts.signal = options.signal;
    }
    const res = await fetch(url.toString(), fetchOpts);
    if (!res.ok) {
      const err = await res.json().catch(() => ({ error: res.statusText }));
      throw new Error(err.error || 'Request failed');
    }
    return res.json();
  },

  async post(endpoint, data = {}) {
    const res = await fetch(endpoint, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(data)
    });
    if (!res.ok) {
      const err = await res.json().catch(() => ({ error: res.statusText }));
      throw new Error(err.error || 'Request failed');
    }
    return res.json();
  },

  async del(endpoint) {
    const res = await fetch(endpoint, { method: 'DELETE' });
    if (!res.ok) {
      const err = await res.json().catch(() => ({ error: res.statusText }));
      throw new Error(err.error || 'Delete failed');
    }
    return res.json();
  }
};

// --- Utility Functions ---
export function escapeHtml(str) {
  if (str === null || str === undefined) return '';
  return String(str)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#039;');
}

export function numeric(value) {
  if (value === null || value === undefined) return null;
  if (typeof value === 'string' && value.trim() === '') return null;
  const n = Number(value);
  return Number.isFinite(n) ? n : null;
}

export function formatBytes(bytes) {
  const value = Number(bytes);
  if (!Number.isFinite(value) || value <= 0) return '0 B';
  const k = 1024;
  const sizes = ['B', 'KB', 'MB', 'GB', 'TB'];
  const i = Math.min(Math.floor(Math.log(value) / Math.log(k)), sizes.length - 1);
  return parseFloat((value / Math.pow(k, i)).toFixed(1)) + ' ' + sizes[i];
}

export function formatDate(timestamp) {
  const ts = numeric(timestamp);
  if (ts === null || ts <= 0) return 'Unknown Date';
  const d = new Date(ts * 1000);
  return d.toLocaleDateString(undefined, {
    timeZone: 'UTC',
    year: 'numeric',
    month: 'short',
    day: 'numeric'
  });
}

export function formatDateTime(timestamp) {
  const ts = numeric(timestamp);
  if (ts === null || ts <= 0) return 'Unknown Date';
  const d = new Date(ts * 1000);
  return d.toLocaleString(undefined, {
    timeZone: 'UTC',
    year: 'numeric',
    month: 'short',
    day: 'numeric',
    hour: '2-digit',
    minute: '2-digit'
  });
}

export function formatExposureTime(sec) {
  const s = numeric(sec);
  if (s === null || s <= 0) return '';
  if (s >= 1) return s.toFixed(1) + 's';
  const fraction = Math.round(1 / s);
  return `1/${fraction}s`;
}

export function getMonthName(monthNumber) {
  const months = [
    'January', 'February', 'March', 'April', 'May', 'June',
    'July', 'August', 'September', 'October', 'November', 'December'
  ];
  return months[monthNumber - 1] || '';
}

export function hasValidGps(item) {
  if (!item || !item.exif || !item.exif.has_gps) return false;
  const lat = numeric(item.exif.latitude);
  const lon = numeric(item.exif.longitude);
  if (lat !== null && lon !== null && lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180) {
    item.exif.latitude = lat;
    item.exif.longitude = lon;
    return true;
  }
  return false;
}

// --- Response Schema Validation & Normalization ---
export function normalizeExif(raw) {
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) {
    return {};
  }
  return {
    ...raw,
    has_gps: Boolean(raw.has_gps)
  };
}

export function normalizeTag(raw) {
  if (!raw || typeof raw !== 'object') return null;
  const id = Number(raw.id);
  if (!Number.isFinite(id)) return null;
  return {
    ...raw,
    id,
    name: String(raw.name || ''),
    category: typeof raw.category === 'string' ? raw.category : 'keyword',
    parent_id: raw.parent_id != null ? Number(raw.parent_id) : null,
    media_count: Number.isFinite(Number(raw.media_count)) ? Number(raw.media_count) : 0
  };
}

export function normalizeAlbum(raw) {
  if (!raw || typeof raw !== 'object') return null;
  const id = Number(raw.id);
  if (!Number.isFinite(id)) return null;
  return {
    ...raw,
    id,
    name: String(raw.name || ''),
    description: String(raw.description || ''),
    is_smart: Boolean(raw.is_smart),
    query_json: typeof raw.query_json === 'string' ? raw.query_json : '',
    cover_media_id: raw.cover_media_id != null ? Number(raw.cover_media_id) : null,
    item_count: Number.isFinite(Number(raw.item_count)) ? Number(raw.item_count) : 0,
    created_at: Number.isFinite(Number(raw.created_at)) ? Number(raw.created_at) : 0
  };
}

export function normalizeTimelineEntry(raw) {
  if (!raw || typeof raw !== 'object') return null;
  const year = Number(raw.year);
  const month = Number(raw.month);
  if (!Number.isFinite(year) || !Number.isFinite(month)) return null;
  return {
    ...raw,
    year,
    month,
    count: Number.isFinite(Number(raw.count)) ? Number(raw.count) : 0
  };
}

export function normalizeCatalogStats(raw) {
  if (!raw || typeof raw !== 'object') {
    return {
      total_media: 0,
      total_size_bytes: 0,
      total_tags: 0,
      total_albums: 0,
      earliest_date: 0,
      latest_date: 0
    };
  }
  return {
    ...raw,
    total_media: Number.isFinite(Number(raw.total_media)) ? Number(raw.total_media) : 0,
    total_size_bytes: Number.isFinite(Number(raw.total_size_bytes)) ? Number(raw.total_size_bytes) : 0,
    total_tags: Number.isFinite(Number(raw.total_tags)) ? Number(raw.total_tags) : 0,
    total_albums: Number.isFinite(Number(raw.total_albums)) ? Number(raw.total_albums) : 0,
    earliest_date: Number.isFinite(Number(raw.earliest_date)) ? Number(raw.earliest_date) : 0,
    latest_date: Number.isFinite(Number(raw.latest_date)) ? Number(raw.latest_date) : 0
  };
}

export function normalizeImportProgress(raw) {
  if (!raw || typeof raw !== 'object') {
    return {
      is_running: false,
      total_files: 0,
      processed_files: 0,
      imported_files: 0,
      skipped_files: 0,
      failed_files: 0,
      current_file: ''
    };
  }
  return {
    ...raw,
    is_running: Boolean(raw.is_running),
    total_files: Number.isFinite(Number(raw.total_files)) ? Number(raw.total_files) : 0,
    processed_files: Number.isFinite(Number(raw.processed_files)) ? Number(raw.processed_files) : 0,
    imported_files: Number.isFinite(Number(raw.imported_files)) ? Number(raw.imported_files) : 0,
    skipped_files: Number.isFinite(Number(raw.skipped_files)) ? Number(raw.skipped_files) : 0,
    failed_files: Number.isFinite(Number(raw.failed_files)) ? Number(raw.failed_files) : 0,
    current_file: String(raw.current_file || '')
  };
}

export function normalizeMediaItem(raw) {
  if (!raw || typeof raw !== 'object') return null;
  const id = Number(raw.id);
  if (!Number.isFinite(id)) return null;

  const rating = Number.isInteger(raw.rating) ? (raw.rating >= 0 && raw.rating <= 5 ? raw.rating : 0) : 0;
  const flag = [-1, 0, 1].includes(raw.flag) ? raw.flag : 0;
  const exif = raw.exif && typeof raw.exif === 'object' && !Array.isArray(raw.exif) ? normalizeExif(raw.exif) : {};

  return {
    ...raw,
    id,
    file_name: typeof raw.file_name === 'string' ? raw.file_name : String(raw.file_name || ''),
    file_path: typeof raw.file_path === 'string' ? raw.file_path : '',
    file_size: Number.isFinite(Number(raw.file_size)) ? Number(raw.file_size) : 0,
    file_modified_time: Number.isFinite(Number(raw.file_modified_time)) ? Number(raw.file_modified_time) : 0,
    content_hash: typeof raw.content_hash === 'string' ? raw.content_hash : '',
    width: Number.isFinite(Number(raw.width)) ? Number(raw.width) : 0,
    height: Number.isFinite(Number(raw.height)) ? Number(raw.height) : 0,
    date_taken: Number.isFinite(Number(raw.date_taken)) ? Number(raw.date_taken) : 0,
    rating,
    flag,
    exif,
    thumb_small: typeof raw.thumb_small === 'string' ? raw.thumb_small : '',
    thumb_large: typeof raw.thumb_large === 'string' ? raw.thumb_large : '',
    caption: typeof raw.caption === 'string' ? raw.caption : '',
    tags: Array.isArray(raw.tags) ? raw.tags.map(normalizeTag).filter(Boolean) : []
  };
}
