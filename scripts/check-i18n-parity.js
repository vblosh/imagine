#!/usr/bin/env node

/**
 * IMAGINE - i18n locale key parity and duplicate check
 */

import fs from 'node:fs';
import { SUPPORTED_LANGUAGES } from '../web/i18n.js';
import en from '../web/locales/en.js';
import es from '../web/locales/es.js';
import de from '../web/locales/de.js';
import ru from '../web/locales/ru.js';
import zh from '../web/locales/zh.js';

const locales = { en, es, de, ru, zh };
const langs = Object.keys(SUPPORTED_LANGUAGES);
const keysByLang = {};
let hasError = false;

langs.forEach(l => {
  if (!locales[l]) {
    console.error(`Missing translation dictionary for locale: ${l}`);
    hasError = true;
    return;
  }
  keysByLang[l] = new Set(Object.keys(locales[l]));
});

if (hasError) process.exit(1);

const allKeys = new Set();
langs.forEach(l => keysByLang[l].forEach(k => allKeys.add(k)));

langs.forEach(l => {
  const missing = [...allKeys].filter(k => !keysByLang[l].has(k));
  if (missing.length > 0) {
    console.error(`Locale '${l}' is missing ${missing.length} keys: ${missing.slice(0, 10).join(', ')}...`);
    hasError = true;
  }
});

// Also check each individual locale source file for duplicate keys
langs.forEach(l => {
  const content = fs.readFileSync(`web/locales/${l}.js`, 'utf8');
  const lines = content.split('\n');
  const seen = new Map();
  lines.forEach((line, idx) => {
    const keyMatch = line.match(/^\s*([a-zA-Z0-9_]+):/);
    if (keyMatch) {
      const key = keyMatch[1];
      if (seen.has(key)) {
        console.error(`Duplicate key '${key}' in locale '${l}' at line ${idx + 1} (first seen at line ${seen.get(key)})`);
        hasError = true;
      } else {
        seen.set(key, idx + 1);
      }
    }
  });
});

if (hasError) {
  process.exit(1);
} else {
  console.log(`i18n key parity check passed! All ${langs.length} locales have identical ${allKeys.size} keys with zero duplicates.`);
  process.exit(0);
}
