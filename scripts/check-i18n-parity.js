#!/usr/bin/env node

/**
 * IMAGINE - i18n locale key parity and duplicate check
 */

import { translations, SUPPORTED_LANGUAGES } from '../web/i18n.js';
import fs from 'node:fs';

const langs = Object.keys(SUPPORTED_LANGUAGES);
const keysByLang = {};
let hasError = false;

langs.forEach(l => {
  if (!translations[l]) {
    console.error(`Missing translation dictionary for locale: ${l}`);
    hasError = true;
    return;
  }
  keysByLang[l] = new Set(Object.keys(translations[l]));
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

// Also check source code for duplicate keys
const content = fs.readFileSync('web/i18n.js', 'utf8');
const lines = content.split('\n');
const transStart = lines.findIndex(line => line.includes('export const translations'));
const langBlocks = {};
let currentLang = null;

lines.slice(transStart).forEach((line, idx) => {
  const langMatch = line.match(/^\s*(en|es|de|ru|zh):\s*\{/);
  if (langMatch) {
    currentLang = langMatch[1];
    langBlocks[currentLang] = [];
    return;
  }
  if (currentLang) {
    const keyMatch = line.match(/^\s*([a-zA-Z0-9_]+):/);
    if (keyMatch) {
      langBlocks[currentLang].push({ key: keyMatch[1], line: transStart + idx + 1 });
    }
  }
});

for (const lang of Object.keys(langBlocks)) {
  const seen = new Map();
  langBlocks[lang].forEach(item => {
    if (seen.has(item.key)) {
      console.error(`Duplicate key '${item.key}' in locale '${lang}' at line ${item.line} (first seen at line ${seen.get(item.key)})`);
      hasError = true;
    } else {
      seen.set(item.key, item.line);
    }
  });
}

if (hasError) {
  process.exit(1);
} else {
  console.log(`i18n key parity check passed! All ${langs.length} locales have identical ${allKeys.size} keys with zero duplicates.`);
  process.exit(0);
}
