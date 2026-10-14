/**
 * IMAGINE Photo Organizer - Internationalization (i18n) Module
 * Supports English (en), Spanish (es), German (de), Russian (ru), and Chinese (zh).
 * Pure Vanilla JavaScript (Offline-ready, no dependencies)
 */

import en from './locales/en.js';

export const SUPPORTED_LANGUAGES = {
  en: { code: 'en', name: 'English', locale: 'en-US' },
  es: { code: 'es', name: 'Español', locale: 'es-ES' },
  de: { code: 'de', name: 'Deutsch', locale: 'de-DE' },
  ru: { code: 'ru', name: 'Русский', locale: 'ru-RU' },
  zh: { code: 'zh', name: '中文', locale: 'zh-CN' }
};

export const translations = {
  en
};

const loadedLocales = new Set(['en']);
const loadingPromises = new Map();

export async function loadLocale(lang) {
  if (!SUPPORTED_LANGUAGES[lang]) return translations.en;
  if (loadedLocales.has(lang)) return translations[lang];
  if (loadingPromises.has(lang)) return loadingPromises.get(lang);

  const promise = (async () => {
    try {
      const mod = await import(`./locales/${lang}.js`);
      translations[lang] = mod.default || mod[lang] || mod;
      loadedLocales.add(lang);
      return translations[lang];
    } catch (err) {
      console.error(`Failed to load locale "${lang}":`, err);
      return translations.en;
    } finally {
      loadingPromises.delete(lang);
    }
  })();

  loadingPromises.set(lang, promise);
  return promise;
}

let currentLanguage = 'en';
const languageChangeListeners = new Set();
let languageRequestSerial = 0;
let pendingLanguageRequests = 0;
let i18nInitialized = false;

export function getCurrentLanguage() {
  return currentLanguage;
}

export function getLocaleCode(lang = currentLanguage) {
  return (SUPPORTED_LANGUAGES[lang] && SUPPORTED_LANGUAGES[lang].locale) || 'en-US';
}

export function onLanguageChange(callback) {
  if (typeof callback === 'function') {
    languageChangeListeners.add(callback);
    return () => languageChangeListeners.delete(callback);
  }
  return () => {};
}

export function t(key, params = {}) {
  const langDict = translations[currentLanguage] || translations.en;
  let text = langDict[key];
  if (text === undefined) {
    text = translations.en[key];
  }
  if (text === undefined) {
    return key;
  }
  if (typeof text !== 'string') {
    return text;
  }
  return text.replace(/\{(\w+)\}/g, (match, paramKey) => {
    return params[paramKey] !== undefined ? params[paramKey] : match;
  });
}

export function getMonthName(monthNum, lang = currentLanguage) {
  const dict = translations[lang] || translations.en;
  const list = dict.months || translations.en.months;
  const m = Number(monthNum);
  if (!Number.isInteger(m) || m < 1 || m > 12 || !Array.isArray(list)) return '';
  return list[m - 1] || '';
}

export function getShortMonthName(monthNum, lang = currentLanguage) {
  const dict = translations[lang] || translations.en;
  const list = dict.months_short || translations.en.months_short;
  const m = Number(monthNum);
  if (!Number.isInteger(m) || m < 1 || m > 12 || !Array.isArray(list)) return '';
  return list[m - 1] || '';
}

export function applyTranslationsToDom() {
  // 1. Text content
  document.querySelectorAll('[data-i18n]').forEach(el => {
    const key = el.getAttribute('data-i18n');
    if (key) {
      el.textContent = t(key);
    }
  });

  // 2. Titles / tooltips
  document.querySelectorAll('[data-i18n-title]').forEach(el => {
    const key = el.getAttribute('data-i18n-title');
    if (key) {
      el.title = t(key);
    }
  });

  // 3. Placeholders
  document.querySelectorAll('[data-i18n-placeholder]').forEach(el => {
    const key = el.getAttribute('data-i18n-placeholder');
    if (key) {
      el.placeholder = t(key);
    }
  });

  // 4. Aria labels
  document.querySelectorAll('[data-i18n-aria-label]').forEach(el => {
    const key = el.getAttribute('data-i18n-aria-label');
    if (key) {
      el.setAttribute('aria-label', t(key));
    }
  });

  // 5. Optgroup labels
  document.querySelectorAll('optgroup[data-i18n-label]').forEach(el => {
    const key = el.getAttribute('data-i18n-label');
    if (key) {
      const translated = t(key);
      el.label = translated;
      el.setAttribute('label', translated);
    }
  });

  // 6. Update <html lang="...">
  document.documentElement.lang = currentLanguage;

  // 7. Update selector if present
  const langSelect = document.getElementById('langSelect');
  if (langSelect && langSelect.value !== currentLanguage) {
    langSelect.value = currentLanguage;
  }
}

export async function setLanguage(lang) {
  if (!SUPPORTED_LANGUAGES[lang]) {
    console.warn(`Unsupported language "${lang}", falling back to "en"`);
    lang = 'en';
  }
  try {
    localStorage.setItem('imagine_language', lang);
  } catch (_) {}

  const currentSerial = ++languageRequestSerial;
  pendingLanguageRequests++;
  document.documentElement.setAttribute('data-i18n-loading', 'true');

  try {
    await loadLocale(lang);
  } finally {
    pendingLanguageRequests--;
    if (pendingLanguageRequests === 0 || currentSerial === languageRequestSerial) {
      document.documentElement.removeAttribute('data-i18n-loading');
    }
  }

  // If a newer request was initiated, abort this stale request
  if (currentSerial !== languageRequestSerial) {
    return;
  }

  // If loading failed and the requested locale is not loaded, fallback to 'en'
  if (!loadedLocales.has(lang)) {
    lang = 'en';
  }

  currentLanguage = lang;
  applyTranslationsToDom();

  // Notify registered callbacks
  for (const cb of languageChangeListeners) {
    try {
      cb(currentLanguage);
    } catch (e) {
      console.error('Error in language change listener:', e);
    }
  }

  // Dispatch global custom event
  window.dispatchEvent(new CustomEvent('imagine:languageChanged', { detail: { language: currentLanguage } }));
}

export async function initI18n() {
  let initial = 'en';
  try {
    const urlParams = new URLSearchParams(window.location.search);
    const queryLang = urlParams.get('lang');
    if (queryLang && SUPPORTED_LANGUAGES[queryLang]) {
      initial = queryLang;
    } else {
      const stored = localStorage.getItem('imagine_language');
      if (stored && SUPPORTED_LANGUAGES[stored]) {
        initial = stored;
      }
    }
  } catch (_) {}

  currentLanguage = initial;
  const langSelect = document.getElementById('langSelect');
  if (langSelect) {
    langSelect.value = currentLanguage;
    if (!i18nInitialized) {
      langSelect.addEventListener('change', async (e) => {
        await setLanguage(e.target.value);
      });
    }
  }

  if (initial !== 'en') {
    document.documentElement.setAttribute('data-i18n-loading', 'true');
    try {
      await loadLocale(initial);
    } finally {
      document.documentElement.removeAttribute('data-i18n-loading');
    }
    if (!loadedLocales.has(initial)) {
      currentLanguage = 'en';
    }
  }

  i18nInitialized = true;
  applyTranslationsToDom();
}
