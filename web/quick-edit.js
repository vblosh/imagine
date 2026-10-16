/**
 * IMAGINE Photo Organizer - Google Photos-Style Photo Editor
 * Pure Vanilla JavaScript (Offline-ready, zero external dependencies)
 *
 * Features:
 * - 6 exclusive edit modes (Crop, Horizon, Smart Fix, Suggestions, Adjust, Filters):
 *   one active at a time, global Apply/Cancel, a single undo step per applied mode
 * - Suggestions: One-tap Auto Enhance (Smart Fix) with intensity slider, Quick style looks (Warm, Cool, B&W Drama)
 * - Crop tools (toolbar): Aspect ratios, 90° rotate (L/R), Angle straighten (-45°..+45°), Flip H & Flip V, interactive 8-handle crop box
 * - Adjust: Light (Brightness, Contrast, Highlights, Shadows, Whites, Blacks),
 *           Color (Saturation, Warmth, Tint, Vibrance),
 *           Details (Sharpness, Vignette)
 * - Filters: 12 curated photographic presets (Vivid, West, Palma, Metro, Eiffel, Modena, Reel, Bazaar, Alpaca, Vista, Onyx, Blush) with intensity slider
 * - Undo & Redo (Ctrl+Z / Ctrl+Y) with full parametric history stack
 * - Hold-to-Compare (Spacebar or mouse hold) with "ORIGINAL" badge overlay
 * - Non-destructive in-session adjustments + 60fps real-time preview pipeline + full-resolution pristine export
 * - Save (in-place overwrite) and Save Copy
 */

import { normalizeMediaItem, getOriginalMediaUrl, getAuthHeaders } from "./api.js";
import { state } from "./state.js";
import { dom, showToast } from "./dom.js";
import { renderGrid } from "./grid.js";
import { t } from "./i18n.js";
import { showModal, hideModal } from "./modals.js";

export const DEFAULT_ADJUSTMENTS = Object.freeze({
  brightness: 0,
  contrast: 0,
  highlights: 0,
  shadows: 0,
  whites: 0,
  blacks: 0,
  saturation: 0,
  warmth: 0,
  tint: 0,
  vibrance: 0,
  sharpness: 0,
  vignette: 0,
});

export const FILTERS = {
  none: { name: "Original", adjust: null },
  vivid: { name: "Vivid", adjust: { contrast: 25, saturation: 35, warmth: 5 } },
  west: { name: "West", adjust: { contrast: 10, warmth: 35, tint: 5, saturation: 10 } },
  palma: { name: "Palma", adjust: { brightness: 5, contrast: 20, saturation: 20, warmth: -10 } },
  metro: { name: "Metro", adjust: { contrast: 15, saturation: -25, tint: -10, warmth: -15 } },
  eiffel: { name: "Eiffel", adjust: { brightness: 5, contrast: -10, warmth: 25, shadows: 20, saturation: -10 } },
  modena: { name: "Modena", adjust: { contrast: 35, warmth: 15, shadows: -15, blacks: -10 } },
  reel: { name: "Reel", adjust: { contrast: 15, warmth: 10, tint: -10, saturation: -15, vignette: 20 } },
  bazaar: { name: "Bazaar", adjust: { brightness: 5, contrast: 20, saturation: 30, warmth: 30 } },
  alpaca: { name: "Alpaca", adjust: { brightness: 10, contrast: -15, saturation: -10, warmth: 15, highlights: -10 } },
  vista: { name: "Vista", adjust: { contrast: 20, saturation: 35, warmth: -5, highlights: 10 } },
  onyx: { name: "Onyx", isBw: true, adjust: { contrast: 30, blacks: -15, whites: 15 } },
  blush: { name: "Blush", adjust: { brightness: 8, contrast: -5, warmth: 15, tint: 25, saturation: 10 } },
};

export const quickEditState = {
  isOpen: false,
  isDirty: false,
  isOriginalLoaded: false,
  photoId: null,
  originalImage: null,
  workingCanvas: null,
  workingCtx: null,
  previewCanvas: null,
  hasCropped: false,
  normalizedCrop: null,
  imageLoadedPromise: null,
  imageLoadedReject: null,
  rotation: 0,
  straighten: 0,
  flipH: false,
  flipV: false,
  cropActive: false,
  cropRatio: "original",
  cropBox: { x: 0, y: 0, w: 0, h: 0 },
  cropDragMode: null,
  cropDragStart: { x: 0, y: 0, boxX: 0, boxY: 0, boxW: 0, boxH: 0 },
  smartFixActive: false,
  smartFixIntensity: 100,
  isComparing: false,
  smartFixLUT: null,
  // Mutually exclusive edit mode: null | 'crop' | 'horizon' | 'smartfix' | 'suggestions' | 'adjust' | 'filters'.
  // While a mode is active, every edit collapses into a single undo step recorded on apply.
  activeMode: null,
  modeSessionPushed: false,
  modeSessionHistoryLen: 0,
  modeEntrySnapshot: null,
  modeEntryCropBox: null,
  modeEntryCropRatio: "original",
  adjustments: { ...DEFAULT_ADJUSTMENTS },
  filter: "none",
  filterIntensity: 100,
  quickStyle: null,
  history: [],
  redoStack: [],
};

export const MAX_SMART_FIX_PREVIEW_DIM = 1200;

/* Smart Fix LUT histogram calculation */
function computeSmartFixLUT(imgData) {
  const data = imgData.data;
  const len = data.length;
  const histR = new Int32Array(256);
  const histG = new Int32Array(256);
  const histB = new Int32Array(256);

  let sumR = 0, sumG = 0, sumB = 0;
  const step = 4 * 4;
  let sampledCount = 0;
  for (let i = 0; i < len; i += step) {
    histR[data[i]]++;
    histG[data[i + 1]]++;
    histB[data[i + 2]]++;
    sumR += data[i];
    sumG += data[i + 1];
    sumB += data[i + 2];
    sampledCount++;
  }

  const avgR = sumR / (sampledCount || 1);
  const avgG = sumG / (sampledCount || 1);
  const avgB = sumB / (sampledCount || 1);
  const avgLum = 0.299 * avgR + 0.587 * avgG + 0.114 * avgB;

  const clipLow = sampledCount * 0.005;

  function findCutoffs(hist) {
    let count = 0;
    let minVal = 0;
    for (let i = 0; i < 256; i++) {
      count += hist[i];
      if (count >= clipLow) { minVal = i; break; }
    }
    count = 0;
    let maxVal = 255;
    for (let i = 255; i >= 0; i--) {
      count += hist[i];
      if (count >= clipLow) { maxVal = i; break; }
    }
    if (maxVal <= minVal) { minVal = 0; maxVal = 255; }
    return [minVal, maxVal];
  }

  const [minR, maxR] = findCutoffs(histR);
  const [minG, maxG] = findCutoffs(histG);
  const [minB, maxB] = findCutoffs(histB);

  let gamma = 1.0;
  if (avgLum < 125) {
    gamma = 0.72 + 0.28 * (avgLum / 125);
  }

  const targetAvg = Math.max(20, Math.min(235, avgLum));
  const scaleR = avgR > 10 ? 1 + 0.5 * (targetAvg / avgR - 1) : 1;
  const scaleG = avgG > 10 ? 1 + 0.5 * (targetAvg / avgG - 1) : 1;
  const scaleB = avgB > 10 ? 1 + 0.5 * (targetAvg / avgB - 1) : 1;

  const lutR = new Uint8Array(256);
  const lutG = new Uint8Array(256);
  const lutB = new Uint8Array(256);

  for (let v = 0; v < 256; v++) {
    let vr = (v - minR) / (maxR - minR || 1);
    vr = Math.max(0, Math.min(1, vr)) * scaleR;
    vr = Math.pow(Math.max(0, Math.min(1, vr)), gamma) * 255;
    lutR[v] = Math.max(0, Math.min(255, Math.round(vr)));

    let vg = (v - minG) / (maxG - minG || 1);
    vg = Math.max(0, Math.min(1, vg)) * scaleG;
    vg = Math.pow(Math.max(0, Math.min(1, vg)), gamma) * 255;
    lutG[v] = Math.max(0, Math.min(255, Math.round(vg)));

    let vb = (v - minB) / (maxB - minB || 1);
    vb = Math.max(0, Math.min(1, vb)) * scaleB;
    vb = Math.pow(Math.max(0, Math.min(1, vb)), gamma) * 255;
    lutB[v] = Math.max(0, Math.min(255, Math.round(vb)));
  }

  return { lutR, lutG, lutB };
}

/* History Stack Management */
export function captureStateSnapshot() {
  return {
    workingCanvas: quickEditState.workingCanvas,
    rotation: quickEditState.rotation,
    straighten: quickEditState.straighten,
    flipH: quickEditState.flipH,
    flipV: quickEditState.flipV,
    hasCropped: quickEditState.hasCropped,
    normalizedCrop: quickEditState.normalizedCrop ? {
      steps: Array.isArray(quickEditState.normalizedCrop.steps)
        ? quickEditState.normalizedCrop.steps.map(s => ({ ...s }))
        : []
    } : null,
    smartFixActive: quickEditState.smartFixActive,
    smartFixIntensity: quickEditState.smartFixIntensity,
    adjustments: { ...quickEditState.adjustments },
    filter: quickEditState.filter,
    filterIntensity: quickEditState.filterIntensity,
    quickStyle: quickEditState.quickStyle,
    isDirty: quickEditState.isDirty || hasAnyEdits(),
  };
}

export function pushHistoryState() {
  // While an edit mode is active, the first mutation captures the single
  // undo entry for the whole session; further pushes are absorbed until apply.
  if (quickEditState.activeMode && quickEditState.modeSessionPushed) return;
  quickEditState.history.push(captureStateSnapshot());
  if (quickEditState.history.length > 100) quickEditState.history.shift();
  if (quickEditState.activeMode) {
    quickEditState.modeSessionPushed = true;
    quickEditState.modeSessionHistoryLen = quickEditState.history.length;
  }
  quickEditState.redoStack = [];
  quickEditState.isDirty = true;
  updateUndoRedoUI();
}

function applyStateSnapshot(snap) {
  if (!snap) return;
  if (snap.workingCanvas) {
    quickEditState.workingCanvas = snap.workingCanvas;
    quickEditState.workingCtx = snap.workingCanvas.getContext("2d");
  }
  quickEditState.rotation = snap.rotation || 0;
  quickEditState.straighten = snap.straighten || 0;
  quickEditState.flipH = !!snap.flipH;
  quickEditState.flipV = !!snap.flipV;
  quickEditState.hasCropped = !!snap.hasCropped;
  quickEditState.normalizedCrop = snap.normalizedCrop ? {
    steps: Array.isArray(snap.normalizedCrop.steps)
      ? snap.normalizedCrop.steps.map(s => ({ ...s }))
      : []
  } : null;
  quickEditState.smartFixActive = !!snap.smartFixActive;
  quickEditState.smartFixIntensity = snap.smartFixIntensity ?? 100;
  quickEditState.adjustments = { ...DEFAULT_ADJUSTMENTS, ...(snap.adjustments || {}) };
  quickEditState.filter = snap.filter || "none";
  quickEditState.filterIntensity = snap.filterIntensity ?? 100;
  quickEditState.quickStyle = snap.quickStyle || null;
  quickEditState.smartFixLUT = null;
  quickEditState.previewCanvas = null;
  quickEditState.isDirty = hasAnyEdits();
}

export function undo() {
  if (!quickEditState.isOpen || quickEditState.history.length === 0) return;
  // Undo first applies the open mode as one step, then steps back over it.
  if (quickEditState.activeMode) applyMode();
  if (quickEditState.history.length === 0) return;
  const current = captureStateSnapshot();
  quickEditState.redoStack.push(current);
  const prev = quickEditState.history.pop();
  applyStateSnapshot(prev);
  quickEditState.isDirty = hasAnyEdits();
  updateUndoRedoUI();
  updateQuickEditToolbarUI();
  renderQuickEditCanvas();
}

export function redo() {
  if (!quickEditState.isOpen || quickEditState.redoStack.length === 0) return;
  if (quickEditState.activeMode) applyMode();
  const current = captureStateSnapshot();
  quickEditState.history.push(current);
  const next = quickEditState.redoStack.pop();
  applyStateSnapshot(next);
  quickEditState.isDirty = hasAnyEdits();
  updateUndoRedoUI();
  updateQuickEditToolbarUI();
  renderQuickEditCanvas();
}

export function updateUndoRedoUI() {
  if (dom.editorUndoBtn) dom.editorUndoBtn.disabled = quickEditState.history.length === 0;
  if (dom.editorRedoBtn) dom.editorRedoBtn.disabled = quickEditState.redoStack.length === 0;
}

/* True when the current session holds any visible edit */
export function hasAnyEdits() {
  return quickEditState.hasCropped
    || quickEditState.rotation !== 0
    || quickEditState.straighten !== 0
    || quickEditState.flipH
    || quickEditState.flipV
    || quickEditState.smartFixActive
    || quickEditState.filter !== "none"
    || Object.values(quickEditState.adjustments).some(v => v !== 0);
}

/* Quick style buttons mirror the style currently in effect */
export function updateQuickStyleUI() {
  [[dom.panelStyleWarmBtn, "warm"], [dom.panelStyleCoolBtn, "cool"], [dom.panelStyleBwBtn, "bw_drama"]]
    .forEach(([btn, style]) => {
      if (btn) btn.classList.toggle("active", quickEditState.quickStyle === style);
    });
}

/* Drop the active quick style as soon as its parameters are edited by hand */
export function clearQuickStyle() {
  if (!quickEditState.quickStyle) return;
  quickEditState.quickStyle = null;
  updateQuickStyleUI();
}

export function updateStraightenGridOverlay() {
  if (!dom.straightenGridOverlay) return;
  const isCropActive = quickEditState.cropActive;
  const isHorizonOpen = dom.horizonSubOptions && dom.horizonSubOptions.style.display !== "none";
  const hasStraighten = quickEditState.straighten !== 0;
  dom.straightenGridOverlay.style.display = (isCropActive && hasStraighten) || isHorizonOpen ? "block" : "none";
}

/* Edit Mode Management: exactly one mode active at a time.
 * Activating a mode applies the previously active one. Deselecting a mode
 * (clicking its button again) applies it. Global Apply/Cancel buttons act
 * on the active mode. History entries are recorded only on apply. */
export const EDIT_MODES = ["crop", "horizon", "smartfix", "suggestions", "adjust", "filters"];

export function getActiveMode() {
  return quickEditState.activeMode;
}

function captureModeEntry() {
  quickEditState.modeEntrySnapshot = captureStateSnapshot();
  quickEditState.modeEntryCropBox = { ...quickEditState.cropBox };
  quickEditState.modeEntryCropRatio = quickEditState.cropRatio;
}

function clearModeSession() {
  quickEditState.activeMode = null;
  quickEditState.modeSessionPushed = false;
  quickEditState.modeSessionHistoryLen = 0;
  quickEditState.modeEntrySnapshot = null;
  quickEditState.modeEntryCropBox = null;
  quickEditState.modeEntryCropRatio = "original";
}

export function activateMode(mode) {
  if (!quickEditState.isOpen || !EDIT_MODES.includes(mode)) return;
  if (quickEditState.activeMode === mode) {
    applyMode();
    return;
  }
  if (quickEditState.activeMode) applyMode();
  quickEditState.activeMode = mode;
  quickEditState.modeSessionPushed = false;
  if (mode === "crop" && !quickEditState.cropActive) {
    quickEditState.cropActive = true;
    initCropBox();
    if (dom.cropOverlay) dom.cropOverlay.style.display = "block";
  }
  captureModeEntry();
  updateQuickEditToolbarUI();
  updateStraightenGridOverlay();
  renderQuickEditCanvas();
}

function snapshotsEqual(a, b) {
  if (!a || !b) return false;
  for (const k of ["rotation", "straighten", "smartFixIntensity", "filterIntensity"]) {
    if ((a[k] ?? 0) !== (b[k] ?? 0)) return false;
  }
  for (const k of ["flipH", "flipV", "hasCropped", "smartFixActive"]) {
    if (!!a[k] !== !!b[k]) return false;
  }
  if ((a.filter || "none") !== (b.filter || "none")) return false;
  if ((a.quickStyle || null) !== (b.quickStyle || null)) return false;
  const normCrop = (n) => {
    if (!n) return "";
    if (Array.isArray(n.steps)) {
      return n.steps.map(s => [s.rx, s.ry, s.rw, s.rh, s.rotation, s.straighten, s.flipH, s.flipV].join(",")).join(";");
    }
    return [n.rx, n.ry, n.rw, n.rh, n.rotation].join(",");
  };
  if (normCrop(a.normalizedCrop) !== normCrop(b.normalizedCrop)) return false;
  const adjA = a.adjustments || {};
  const adjB = b.adjustments || {};
  const adjKeys = new Set([...Object.keys(adjA), ...Object.keys(adjB)]);
  for (const k of adjKeys) {
    if ((adjA[k] ?? 0) !== (adjB[k] ?? 0)) return false;
  }
  return a.workingCanvas === b.workingCanvas;
}

/* True when the crop session changed anything that must be baked on apply. */
function cropSessionChanged() {
  const e = quickEditState.modeEntrySnapshot;
  if (!e) return true;
  if (quickEditState.rotation !== (e.rotation || 0)
      || quickEditState.straighten !== (e.straighten || 0)
      || !!quickEditState.flipH !== !!e.flipH
      || !!quickEditState.flipV !== !!e.flipV) return true;
  if (quickEditState.workingCanvas !== e.workingCanvas) return true;
  const b = quickEditState.cropBox;
  const eb = quickEditState.modeEntryCropBox;
  if (!eb) return true;
  return b.x !== eb.x || b.y !== eb.y || b.w !== eb.w || b.h !== eb.h;
}

export function applyMode() {
  const mode = quickEditState.activeMode;
  if (!quickEditState.isOpen || !mode) return;
  if (mode === "crop" && cropSessionChanged()) {
    // Bake the crop (and any pending transforms); session-aware push keeps one entry.
    pushHistoryState();
    bakeCrop();
  }
  if (quickEditState.modeSessionPushed
      && snapshotsEqual(quickEditState.modeEntrySnapshot, captureStateSnapshot())
      && quickEditState.history.length === quickEditState.modeSessionHistoryLen) {
    // No net change across the session: drop the no-op entry.
    quickEditState.history.pop();
    quickEditState.isDirty = hasAnyEdits();
    updateUndoRedoUI();
  }
  if (mode === "crop") {
    quickEditState.cropActive = false;
    if (dom.cropOverlay) dom.cropOverlay.style.display = "none";
  }
  clearModeSession();
  updateQuickEditToolbarUI();
  updateStraightenGridOverlay();
  renderQuickEditCanvas();
}

export function cancelMode() {
  const mode = quickEditState.activeMode;
  if (!quickEditState.isOpen || !mode) return;
  if (quickEditState.modeSessionPushed
      && quickEditState.history.length === quickEditState.modeSessionHistoryLen) {
    quickEditState.history.pop();
    updateUndoRedoUI();
  }
  if (quickEditState.modeEntrySnapshot) applyStateSnapshot(quickEditState.modeEntrySnapshot);
  if (mode === "crop") {
    if (quickEditState.modeEntryCropBox) quickEditState.cropBox = { ...quickEditState.modeEntryCropBox };
    if (quickEditState.modeEntryCropRatio) {
      quickEditState.cropRatio = quickEditState.modeEntryCropRatio;
      if (dom.cropAspectRatioSelect) dom.cropAspectRatioSelect.value = quickEditState.cropRatio;
    }
    updateCropOverlayDOM();
    quickEditState.cropActive = false;
    if (dom.cropOverlay) dom.cropOverlay.style.display = "none";
  }
  quickEditState.isDirty = hasAnyEdits();
  clearModeSession();
  updateQuickEditToolbarUI();
  updateStraightenGridOverlay();
  renderQuickEditCanvas();
}

/* Open / Close Quick Edit */
export function openQuickEdit() {
  if (state.loupeIndex < 0 || state.loupeIndex >= state.mediaItems.length) return;
  const item = state.mediaItems[state.loupeIndex];
  if (!item || item.media_type === "video" || item.media_type === "audio" || (item.media_type && item.media_type !== "photo")) return;

  quickEditState.isOpen = true;
  quickEditState.isDirty = false;
  quickEditState.isOriginalLoaded = false;
  quickEditState.photoId = item.id;
  quickEditState.rotation = 0;
  quickEditState.straighten = 0;
  quickEditState.flipH = false;
  quickEditState.flipV = false;
  quickEditState.cropActive = false;
  quickEditState.hasCropped = false;
  quickEditState.normalizedCrop = null;
  quickEditState.smartFixActive = false;
  quickEditState.smartFixIntensity = 100;
  quickEditState.isComparing = false;
  quickEditState.smartFixLUT = null;
  clearModeSession();
  quickEditState.adjustments = { ...DEFAULT_ADJUSTMENTS };
  quickEditState.filter = "none";
  quickEditState.filterIntensity = 100;
  quickEditState.quickStyle = null;
  quickEditState.history = [];
  quickEditState.redoStack = [];

  if (dom.loupeModal) dom.loupeModal.classList.add("is-quick-editing");
  if (dom.loupeQuickEditBtn) dom.loupeQuickEditBtn.classList.add("active");
  if (dom.quickEditToolbar) dom.quickEditToolbar.style.display = "flex";
  if (dom.quickEditContainer) dom.quickEditContainer.style.display = "flex";
  if (dom.loupeImg) dom.loupeImg.style.display = "none";
  if (dom.editorCompareBadge) dom.editorCompareBadge.style.display = "none";
  if (dom.straightenGridOverlay) dom.straightenGridOverlay.style.display = "none";

  updateUndoRedoUI();
  updateQuickEditToolbarUI();

  const w = item.width || (dom.loupeImg && dom.loupeImg.naturalWidth) || 800;
  const h = item.height || (dom.loupeImg && dom.loupeImg.naturalHeight) || 600;

  const initialCanvas = document.createElement("canvas");
  initialCanvas.width = w;
  initialCanvas.height = h;
  const initialCtx = initialCanvas.getContext("2d");

  const cardImg = document.querySelector(`.photo-card[data-id="${item.id}"] img`);
  if (dom.loupeImg && dom.loupeImg.complete && dom.loupeImg.naturalWidth > 0) {
    initialCtx.drawImage(dom.loupeImg, 0, 0, w, h);
  } else if (cardImg && cardImg.complete && cardImg.naturalWidth > 0) {
    initialCtx.drawImage(cardImg, 0, 0, w, h);
  }

  quickEditState.originalImage = initialCanvas;
  quickEditState.workingCanvas = initialCanvas;
  quickEditState.workingCtx = initialCtx;

  renderQuickEditCanvas();

  if (dom.quickEditSaveBtn) dom.quickEditSaveBtn.disabled = false;
  if (dom.quickEditSaveCopyBtn) dom.quickEditSaveCopyBtn.disabled = false;

  let resolveImageLoaded;
  let rejectImageLoaded;
  quickEditState.imageLoadedPromise = new Promise((resolve, reject) => {
    resolveImageLoaded = resolve;
    rejectImageLoaded = reject;
  });
  quickEditState.imageLoadedReject = rejectImageLoaded;
  quickEditState.imageLoadedPromise.catch(() => {});

  let fullImageLoaded = false;
  const onImageReady = (loadedImg) => {
    if (fullImageLoaded) return;
    if (!quickEditState.isOpen || quickEditState.photoId !== item.id) {
      if (rejectImageLoaded) {
        try { rejectImageLoaded(new Error("Quick edit cancelled")); } catch (_) {}
      }
      return;
    }
    fullImageLoaded = true;
    quickEditState.isOriginalLoaded = true;
    quickEditState.originalImage = loadedImg;

    const fullResUncropped = document.createElement("canvas");
    fullResUncropped.width = loadedImg.naturalWidth || loadedImg.width;
    fullResUncropped.height = loadedImg.naturalHeight || loadedImg.height;
    fullResUncropped.getContext("2d").drawImage(loadedImg, 0, 0);

    const replaceCanvas = (snap) => {
      if (snap && snap.workingCanvas === initialCanvas) {
        snap.workingCanvas = fullResUncropped;
      }
    };
    quickEditState.history.forEach(replaceCanvas);
    quickEditState.redoStack.forEach(replaceCanvas);
    if (quickEditState.modeEntrySnapshot) replaceCanvas(quickEditState.modeEntrySnapshot);

    if (!quickEditState.hasCropped) {
      initWorkingCanvas(loadedImg);
    } else if (quickEditState.normalizedCrop?.steps?.length) {
      let curCanvas = document.createElement("canvas");
      curCanvas.width = loadedImg.naturalWidth || loadedImg.width;
      curCanvas.height = loadedImg.naturalHeight || loadedImg.height;
      curCanvas.getContext("2d").drawImage(loadedImg, 0, 0);

      for (const step of quickEditState.normalizedCrop.steps) {
        const is90or270 = step.rotation === 90 || step.rotation === 270;
        const rotW = is90or270 ? curCanvas.height : curCanvas.width;
        const rotH = is90or270 ? curCanvas.width : curCanvas.height;

        const transformed = document.createElement("canvas");
        transformed.width = rotW;
        transformed.height = rotH;
        const tCtx = transformed.getContext("2d");
        tCtx.save();
        tCtx.translate(rotW / 2, rotH / 2);
        const totalAngleDeg = (step.rotation || 0) + (step.straighten || 0);
        tCtx.rotate((totalAngleDeg * Math.PI) / 180);
        const rad = Math.abs(((step.straighten || 0) * Math.PI) / 180);
        const sFactor = step.straighten
          ? Math.cos(rad) + Math.max(rotH / rotW, rotW / rotH) * Math.sin(rad)
          : 1;
        tCtx.scale((step.flipH ? -1 : 1) * sFactor, (step.flipV ? -1 : 1) * sFactor);
        tCtx.drawImage(curCanvas, -curCanvas.width / 2, -curCanvas.height / 2);
        tCtx.restore();

        const cropX = Math.round(step.rx * rotW);
        const cropY = Math.round(step.ry * rotH);
        const cropW = Math.max(1, Math.round(step.rw * rotW));
        const cropH = Math.max(1, Math.round(step.rh * rotH));

        const nextCanvas = document.createElement("canvas");
        nextCanvas.width = cropW;
        nextCanvas.height = cropH;
        nextCanvas.getContext("2d").drawImage(transformed, cropX, cropY, cropW, cropH, 0, 0, cropW, cropH);
        curCanvas = nextCanvas;
      }

      quickEditState.workingCanvas = curCanvas;
      quickEditState.workingCtx = curCanvas.getContext("2d");
      quickEditState.smartFixLUT = null;
      quickEditState.previewCanvas = null;
    }
    renderQuickEditCanvas();
    quickEditState.imageLoadedReject = null;
    if (resolveImageLoaded) resolveImageLoaded(loadedImg);
  };

  const onImageError = (err) => {
    if (fullImageLoaded) return;
    if (!quickEditState.isOpen || quickEditState.photoId !== item.id) {
      if (rejectImageLoaded) {
        try { rejectImageLoaded(new Error("Quick edit cancelled")); } catch (_) {}
      }
      return;
    }
    quickEditState.imageLoadedReject = null;
    if (rejectImageLoaded) rejectImageLoaded(err || new Error("Failed to load original image"));
  };

  const targetOriginalUrl = (dom.loupeImg && dom.loupeImg.src && dom.loupeImg.src.includes(`/api/photos/${item.id}/original`))
    ? dom.loupeImg.src
    : getOriginalMediaUrl(item);

  if (dom.loupeImg && dom.loupeImg.complete && dom.loupeImg.naturalWidth > 0 && dom.loupeImg.src.includes(`/api/photos/${item.id}/original`)) {
    onImageReady(dom.loupeImg);
  } else if (dom.loupeImg && dom.loupeImg.src.includes(`/api/photos/${item.id}/original`)) {
    dom.loupeImg.addEventListener("load", () => onImageReady(dom.loupeImg), { once: true });
    dom.loupeImg.addEventListener("error", () => onImageError(), { once: true });
  }

  const img = new Image();
  img.crossOrigin = "anonymous";
  img.onload = () => onImageReady(img);
  img.onerror = () => onImageError();
  img.src = targetOriginalUrl;
}

export function showDiscardModal() {
  if (dom.discardChangesModal) {
    showModal(dom.discardChangesModal, dom.confirmDiscardChangesBtn);
  }
}

export function hideDiscardModal() {
  if (dom.discardChangesModal) {
    hideModal(dom.discardChangesModal);
  }
}

export function closeQuickEdit(promptIfDirty = true) {
  if (!quickEditState.isOpen) return;

  // Closing the editor applies the open mode as one undo step.
  if (quickEditState.activeMode) applyMode();

  if (promptIfDirty && quickEditState.isDirty) {
    showDiscardModal();
    return;
  }

  performCloseQuickEdit();
}

export function performCloseQuickEdit() {
  hideDiscardModal();
  if (quickEditState.imageLoadedReject) {
    try {
      quickEditState.imageLoadedReject(new Error("Quick edit cancelled"));
    } catch (_) {}
    quickEditState.imageLoadedReject = null;
  }

  quickEditState.isOpen = false;
  quickEditState.isDirty = false;
  quickEditState.isOriginalLoaded = false;
  quickEditState.cropActive = false;
  quickEditState.hasCropped = false;
  quickEditState.normalizedCrop = null;
  quickEditState.imageLoadedPromise = null;
  quickEditState.history = [];
  quickEditState.redoStack = [];
  clearModeSession();

  if (dom.loupeModal) dom.loupeModal.classList.remove("is-quick-editing");
  if (dom.loupeQuickEditBtn) dom.loupeQuickEditBtn.classList.remove("active");
  if (dom.quickEditToolbar) dom.quickEditToolbar.style.display = "none";
  if (dom.quickEditContainer) dom.quickEditContainer.style.display = "none";
  if (dom.cropOverlay) dom.cropOverlay.style.display = "none";
  if (dom.editorCompareBadge) dom.editorCompareBadge.style.display = "none";
  if (dom.straightenGridOverlay) dom.straightenGridOverlay.style.display = "none";
  if (dom.horizonSubOptions) dom.horizonSubOptions.style.display = "none";
  if (dom.quickEditHorizonBtn) dom.quickEditHorizonBtn.classList.remove("active");
  if (dom.loupeImg) dom.loupeImg.style.display = "";

  if (dom.quickEditSaveBtn) dom.quickEditSaveBtn.disabled = false;
  if (dom.quickEditSaveCopyBtn) dom.quickEditSaveCopyBtn.disabled = false;
  if (dom.quickEditSmartFixBtn) dom.quickEditSmartFixBtn.classList.remove("active");
  if (dom.smartFixSubOptions) dom.smartFixSubOptions.style.display = "none";
  if (dom.cropSubOptions) dom.cropSubOptions.style.display = "none";
  if (dom.quickEditCropBtn) dom.quickEditCropBtn.classList.remove("active");

  if (dom.loupePrevBtn) dom.loupePrevBtn.style.display = "";
  if (dom.loupeNextBtn) dom.loupeNextBtn.style.display = "";

  quickEditState.originalImage = null;
  quickEditState.workingCanvas = null;
  quickEditState.workingCtx = null;
  quickEditState.smartFixLUT = null;
  quickEditState.previewCanvas = null;
}

function initWorkingCanvas(img) {
  const canvas = document.createElement("canvas");
  canvas.width = img.naturalWidth || img.width;
  canvas.height = img.naturalHeight || img.height;
  const ctx = canvas.getContext("2d");
  ctx.drawImage(img, 0, 0);

  quickEditState.workingCanvas = canvas;
  quickEditState.workingCtx = ctx;
  quickEditState.smartFixLUT = null;
}

/* Rotation and Flips */
export function rotateLeft() {
  if (!quickEditState.isOpen) return;
  pushHistoryState();
  quickEditState.rotation = (quickEditState.rotation - 90 + 360) % 360;
  clearQuickStyle();
  if (quickEditState.cropActive) {
    initCropBox();
  }
  renderQuickEditCanvas();
}

export function rotateRight() {
  if (!quickEditState.isOpen) return;
  pushHistoryState();
  quickEditState.rotation = (quickEditState.rotation + 90) % 360;
  clearQuickStyle();
  if (quickEditState.cropActive) {
    initCropBox();
  }
  renderQuickEditCanvas();
}

export function toggleFlipH() {
  if (!quickEditState.isOpen) return;
  pushHistoryState();
  quickEditState.flipH = !quickEditState.flipH;
  if (dom.editorFlipHBtn) dom.editorFlipHBtn.classList.toggle("active", quickEditState.flipH);
  clearQuickStyle();
  renderQuickEditCanvas();
}

export function toggleFlipV() {
  if (!quickEditState.isOpen) return;
  pushHistoryState();
  quickEditState.flipV = !quickEditState.flipV;
  if (dom.editorFlipVBtn) dom.editorFlipVBtn.classList.toggle("active", quickEditState.flipV);
  clearQuickStyle();
  renderQuickEditCanvas();
}

export function setStraightenAngle(angleDeg) {
  if (!quickEditState.isOpen) return;
  const nextVal = Math.max(-45, Math.min(45, parseFloat(angleDeg) || 0));
  if (nextVal !== quickEditState.straighten) {
    pushHistoryState();
  }
  quickEditState.straighten = nextVal;
  if (dom.editorStraightenSlider) dom.editorStraightenSlider.value = quickEditState.straighten;
  if (dom.editorStraightenVal) dom.editorStraightenVal.textContent = `${quickEditState.straighten}°`;
  if (dom.horizonAngleSlider) dom.horizonAngleSlider.value = quickEditState.straighten;
  if (dom.horizonAngleVal) dom.horizonAngleVal.textContent = `${quickEditState.straighten}°`;
  clearQuickStyle();
  updateStraightenGridOverlay();
  quickEditState.isDirty = true;
  renderQuickEditCanvas();
}

export function resetStraighten() {
  if (!quickEditState.isOpen) return;
  if (quickEditState.straighten !== 0) {
    setStraightenAngle(0);
  }
}

/* True while the Horizon correction sub-panel is open */
export function isHorizonToolOpen() {
  return quickEditState.activeMode === "horizon";
}

export function autoLevelHorizon() {
  if (!quickEditState.isOpen || !quickEditState.workingCanvas) return;

  const src = quickEditState.workingCanvas;
  const rot = quickEditState.rotation || 0;
  const is90or270 = rot === 90 || rot === 270;
  const origW = is90or270 ? src.height : src.width;
  const origH = is90or270 ? src.width : src.height;

  const maxDim = 320;
  let sampleW = origW;
  let sampleH = origH;
  if (sampleW > maxDim || sampleH > maxDim) {
    if (sampleW >= sampleH) {
      sampleH = Math.max(20, Math.round((sampleH * maxDim) / sampleW));
      sampleW = maxDim;
    } else {
      sampleW = Math.max(20, Math.round((sampleW * maxDim) / sampleH));
      sampleH = maxDim;
    }
  }

  const sampleCanvas = document.createElement("canvas");
  sampleCanvas.width = sampleW;
  sampleCanvas.height = sampleH;
  const sCtx = sampleCanvas.getContext("2d");

  // Render proportional sample canvas with 90° rotation if applied
  sCtx.save();
  sCtx.translate(sampleW / 2, sampleH / 2);
  sCtx.rotate((rot * Math.PI) / 180);
  const srcSampleW = is90or270 ? sampleH : sampleW;
  const srcSampleH = is90or270 ? sampleW : sampleH;
  sCtx.drawImage(src, -srcSampleW / 2, -srcSampleH / 2, srcSampleW, srcSampleH);
  sCtx.restore();

  const imgData = sCtx.getImageData(0, 0, sampleW, sampleH);
  const data = imgData.data;

  // Convert to luminance array
  const gray = new Float32Array(sampleW * sampleH);
  for (let i = 0, j = 0; i < data.length; i += 4, j++) {
    gray[j] = 0.299 * data[i] + 0.587 * data[i + 1] + 0.114 * data[i + 2];
  }

  const xc = sampleW / 2;
  const numSectors = 16;
  const sectorW = sampleW / numSectors;

  const edgeXs = [];
  const edgeYs = [];
  const edgeMags = [];
  const edgeSectors = [];

  // Gradient computation & near-horizontal edge extraction
  function findEdges(gyThresh, gxRatio) {
    edgeXs.length = 0;
    edgeYs.length = 0;
    edgeMags.length = 0;
    edgeSectors.length = 0;
    for (let y = 1; y < sampleH - 1; y++) {
      const rowOffset = y * sampleW;
      for (let x = 1; x < sampleW - 1; x++) {
        const idx = rowOffset + x;
        const gx = (gray[idx + 1] - gray[idx - 1]) * 0.5;
        const gy = (gray[idx + sampleW] - gray[idx - sampleW]) * 0.5;
        const absGx = Math.abs(gx);
        const absGy = Math.abs(gy);

        // Near-horizontal edges: vertical gradient dominates horizontal gradient
        if (absGy >= gyThresh && absGx <= absGy * gxRatio) {
          const mag = Math.hypot(gx, gy);
          const sector = Math.min(numSectors - 1, Math.max(0, Math.floor(x / sectorW)));
          edgeXs.push(x);
          edgeYs.push(y);
          edgeMags.push(mag);
          edgeSectors.push(sector);
        }
      }
    }
  }

  findEdges(18, 0.5);
  if (edgeXs.length < 40) {
    findEdges(10, 0.6);
  }

  const numEdges = edgeXs.length;
  if (numEdges < 30) {
    return;
  }

  const rowWeight = new Float32Array(sampleH);
  const rowSectors = new Uint16Array(sampleH);

  let bestAngle = 0;
  let bestScore = 0;

  // Search tilt angles in range -15°..+15° at 0.5° steps
  for (let deg = -15.0; deg <= 15.05; deg += 0.5) {
    rowWeight.fill(0);
    rowSectors.fill(0);

    const rad = (deg * Math.PI) / 180;
    const tanR = Math.tan(rad);

    for (let i = 0; i < numEdges; i++) {
      const x = edgeXs[i];
      const y = edgeYs[i];
      const y0 = Math.round(y - (x - xc) * tanR);

      if (y0 >= 5 && y0 < sampleH - 5) {
        rowWeight[y0] += edgeMags[i];
        rowSectors[y0] |= (1 << edgeSectors[i]);
      }
    }

    // Evaluate candidate lines across rows
    for (let r = 5; r < sampleH - 5; r++) {
      const mask = rowSectors[r];
      if (!mask) continue;

      let cov = 0;
      let m = mask;
      while (m > 0) {
        cov += m & 1;
        m >>= 1;
      }

      // A dominant horizon must span across significant width (>= 6 sectors out of 16)
      if (cov >= 6) {
        const covRatio = cov / numSectors;
        const score = rowWeight[r] * covRatio * covRatio;
        if (score > bestScore) {
          bestScore = score;
          bestAngle = deg;
        }
      }
    }
  }

  // Minimum confidence threshold to prevent false corrections on uniform or non-landscape images
  const CONFIDENCE_THRESHOLD = 600;
  if (bestScore < CONFIDENCE_THRESHOLD) {
    return;
  }

  const targetAngle = Math.round(bestAngle * 2) / 2;
  const correctionAngle = targetAngle === 0 ? 0 : -targetAngle;
  if (correctionAngle === quickEditState.straighten) {
    return;
  }
  setStraightenAngle(correctionAngle);
}

export function setCropAspectRatio(ratio) {
  quickEditState.cropRatio = ratio;
  if (dom.cropAspectRatioSelect) {
    dom.cropAspectRatioSelect.value = ratio;
  }
  if (quickEditState.cropActive) {
    initCropBox();
  }
}

function getActiveCanvasDisplayRect() {
  const canvas = dom.quickEditCanvas;
  if (!canvas || !dom.quickEditContainer) return { left: 0, top: 0, width: 0, height: 0 };
  const rect = canvas.getBoundingClientRect();
  const containerRect = dom.quickEditContainer.getBoundingClientRect();
  return {
    left: rect.left - containerRect.left,
    top: rect.top - containerRect.top,
    width: rect.width,
    height: rect.height,
  };
}

function initCropBox() {
  const disp = getActiveCanvasDisplayRect();
  if (disp.width <= 0 || disp.height <= 0) return;

  const pad = 20;
  const availW = Math.max(40, disp.width - pad * 2);
  const availH = Math.max(40, disp.height - pad * 2);

  let targetW = availW;
  let targetH = availH;
  const ratio = getNumericAspectRatio(quickEditState.cropRatio, disp.width, disp.height);

  if (ratio !== null) {
    if (availW / availH > ratio) {
      targetW = availH * ratio;
      targetH = availH;
    } else {
      targetW = availW;
      targetH = availW / ratio;
    }
  }

  const boxX = disp.left + (disp.width - targetW) / 2;
  const boxY = disp.top + (disp.height - targetH) / 2;

  quickEditState.cropBox = {
    x: Math.round(boxX),
    y: Math.round(boxY),
    w: Math.round(targetW),
    h: Math.round(targetH),
  };

  updateCropOverlayDOM();
}

function getNumericAspectRatio(ratioName, currentW, currentH) {
  switch (ratioName) {
    case "original": return currentW / (currentH || 1);
    case "1:1": return 1.0;
    case "4:3": return 4 / 3;
    case "16:9": return 16 / 9;
    case "3:2": return 3 / 2;
    case "5:4": return 5 / 4;
    case "free":
    default: return null;
  }
}

function updateCropOverlayDOM() {
  if (!dom.cropBox || !dom.cropOverlay) return;
  const { x, y, w, h } = quickEditState.cropBox;
  const container = dom.quickEditContainer;
  const cW = container ? container.clientWidth : window.innerWidth;
  const cH = container ? container.clientHeight : window.innerHeight;

  dom.cropBox.style.left = `${x}px`;
  dom.cropBox.style.top = `${y}px`;
  dom.cropBox.style.width = `${w}px`;
  dom.cropBox.style.height = `${h}px`;

  if (dom.cropScrimTop) {
    dom.cropScrimTop.style.left = "0";
    dom.cropScrimTop.style.top = "0";
    dom.cropScrimTop.style.width = "100%";
    dom.cropScrimTop.style.height = `${Math.max(0, y)}px`;
  }
  if (dom.cropScrimBottom) {
    dom.cropScrimBottom.style.left = "0";
    dom.cropScrimBottom.style.top = `${y + h}px`;
    dom.cropScrimBottom.style.width = "100%";
    dom.cropScrimBottom.style.height = `${Math.max(0, cH - (y + h))}px`;
  }
  if (dom.cropScrimLeft) {
    dom.cropScrimLeft.style.left = "0";
    dom.cropScrimLeft.style.top = `${y}px`;
    dom.cropScrimLeft.style.width = `${Math.max(0, x)}px`;
    dom.cropScrimLeft.style.height = `${h}px`;
  }
  if (dom.cropScrimRight) {
    dom.cropScrimRight.style.left = `${x + w}px`;
    dom.cropScrimRight.style.top = `${y}px`;
    dom.cropScrimRight.style.width = `${Math.max(0, cW - (x + w))}px`;
    dom.cropScrimRight.style.height = `${h}px`;
  }

  if (dom.cropDimensionsBadge && dom.quickEditCanvas) {
    const disp = getActiveCanvasDisplayRect();
    const scale = dom.quickEditCanvas.width / (disp.width || 1);
    const origW = Math.round(w * scale);
    const origH = Math.round(h * scale);
    dom.cropDimensionsBadge.textContent = `${origW} × ${origH}`;
  }
}

/* Bake the pending crop (and transforms) into the working canvas.
 * Called only from applyMode() after the session history entry was recorded. */
function bakeCrop() {
  if (!quickEditState.isOpen || !quickEditState.cropActive) return;

  const disp = getActiveCanvasDisplayRect();
  const { x, y, w, h } = quickEditState.cropBox;
  const canvas = dom.quickEditCanvas;
  if (!canvas || disp.width <= 0 || disp.height <= 0) return;

  const scale = canvas.width / disp.width;
  const srcX = Math.max(0, Math.round((x - disp.left) * scale));
  const srcY = Math.max(0, Math.round((y - disp.top) * scale));
  const srcW = Math.min(canvas.width - srcX, Math.round(w * scale));
  const srcH = Math.min(canvas.height - srcY, Math.round(h * scale));

  if (srcW <= 10 || srcH <= 10) return;

  const croppedCanvas = document.createElement("canvas");
  croppedCanvas.width = srcW;
  croppedCanvas.height = srcH;
  const croppedCtx = croppedCanvas.getContext("2d");

  const rot = quickEditState.rotation;
  const straighten = quickEditState.straighten;
  const flipH = quickEditState.flipH;
  const flipV = quickEditState.flipV;
  const srcCanvas = quickEditState.workingCanvas;
  const fullTransformedCanvas = document.createElement("canvas");
  fullTransformedCanvas.width = canvas.width;
  fullTransformedCanvas.height = canvas.height;
  const fullCtx = fullTransformedCanvas.getContext("2d");
  fullCtx.save();
  fullCtx.translate(canvas.width / 2, canvas.height / 2);
  const totalAngleDeg = rot + straighten;
  fullCtx.rotate((totalAngleDeg * Math.PI) / 180);
  const rad = Math.abs((straighten * Math.PI) / 180);
  const sFactor = straighten !== 0
    ? Math.cos(rad) + Math.max(canvas.height / canvas.width, canvas.width / canvas.height) * Math.sin(rad)
    : 1;
  fullCtx.scale((flipH ? -1 : 1) * sFactor, (flipV ? -1 : 1) * sFactor);
  fullCtx.drawImage(srcCanvas, -srcCanvas.width / 2, -srcCanvas.height / 2);
  fullCtx.restore();

  croppedCtx.drawImage(fullTransformedCanvas, srcX, srcY, srcW, srcH, 0, 0, srcW, srcH);

  quickEditState.workingCanvas = croppedCanvas;
  quickEditState.workingCtx = croppedCtx;
  quickEditState.rotation = 0;
  quickEditState.straighten = 0;
  quickEditState.flipH = false;
  quickEditState.flipV = false;
  quickEditState.cropActive = false;
  quickEditState.hasCropped = true;
  const prevNorm = quickEditState.normalizedCrop;
  const rx = srcX / canvas.width;
  const ry = srcY / canvas.height;
  const rw = srcW / canvas.width;
  const rh = srcH / canvas.height;
  quickEditState.normalizedCrop = {
    steps: [
      ...(prevNorm?.steps || []),
      { rx, ry, rw, rh, rotation: rot, straighten, flipH, flipV },
    ],
  };
  quickEditState.isDirty = true;
  quickEditState.smartFixLUT = null;
  quickEditState.previewCanvas = null;
  clearQuickStyle();

  // Overlay/UI sync is handled centrally by applyMode().
}

/* Suggestions & Smart Fix */
export function toggleSmartFix() {
  if (!quickEditState.isOpen) return;
  pushHistoryState();
  quickEditState.smartFixActive = !quickEditState.smartFixActive;
  clearQuickStyle();
  updateQuickEditToolbarUI();
  renderQuickEditCanvas();
}

export function setSmartFixIntensity(val) {
  const nextVal = Math.max(10, Math.min(100, val));
  if (nextVal !== quickEditState.smartFixIntensity) {
    pushHistoryState();
  }
  quickEditState.smartFixIntensity = nextVal;
  if (dom.smartFixIntensitySlider) dom.smartFixIntensitySlider.value = quickEditState.smartFixIntensity;
  if (dom.smartFixIntensityVal) dom.smartFixIntensityVal.textContent = `${quickEditState.smartFixIntensity}%`;
  clearQuickStyle();
  quickEditState.isDirty = true;
  renderQuickEditCanvas();
}

export function applyQuickStyle(styleName) {
  if (!quickEditState.isOpen) return;
  pushHistoryState();
  const nextStyle = quickEditState.quickStyle === styleName ? null : styleName;
  quickEditState.quickStyle = nextStyle;
  quickEditState.adjustments = { ...DEFAULT_ADJUSTMENTS };
  quickEditState.filter = "none";
  quickEditState.filterIntensity = 100;
  if (nextStyle === "warm") {
    quickEditState.adjustments.warmth = 30;
    quickEditState.adjustments.saturation = 10;
  } else if (nextStyle === "cool") {
    quickEditState.adjustments.warmth = -25;
    quickEditState.adjustments.tint = -5;
    quickEditState.adjustments.vibrance = 15;
  } else if (nextStyle === "bw_drama") {
    quickEditState.filter = "onyx";
    quickEditState.filterIntensity = 100;
  }
  quickEditState.isDirty = hasAnyEdits();
  updateQuickEditToolbarUI();
  renderQuickEditCanvas();
}

/* Adjustments */
export function setAdjustment(key, val) {
  if (!quickEditState.isOpen) return;
  const nextVal = parseFloat(val) || 0;
  if (nextVal !== (quickEditState.adjustments[key] ?? 0)) {
    pushHistoryState();
  }
  quickEditState.adjustments[key] = nextVal;
  clearQuickStyle();
  quickEditState.isDirty = true;
  updateAdjustmentUI(key);
  renderQuickEditCanvas();
}

export function resetAdjustments() {
  if (!quickEditState.isOpen) return;
  pushHistoryState();
  quickEditState.adjustments = { ...DEFAULT_ADJUSTMENTS };
  clearQuickStyle();
  updateQuickEditToolbarUI();
  renderQuickEditCanvas();
}

function updateAdjustmentUI(key) {
  const cap = key.charAt(0).toUpperCase() + key.slice(1);
  const sliderEl = dom[`slider${cap}`];
  const valEl = dom[`val${cap}`];
  const val = quickEditState.adjustments[key] ?? 0;
  if (sliderEl) sliderEl.value = val;
  if (valEl) {
    if (val > 0) {
      valEl.textContent = `+${val}`;
    } else {
      valEl.textContent = `${val}`;
    }
  }
}

/* Filter presets */
export function setFilter(filterName) {
  if (!quickEditState.isOpen || !FILTERS[filterName]) return;
  pushHistoryState();
  quickEditState.filter = filterName;
  clearQuickStyle();
  updateQuickEditToolbarUI();
  renderQuickEditCanvas();
}

export function setFilterIntensity(val) {
  if (!quickEditState.isOpen) return;
  const num = parseInt(val, 10);
  const nextVal = Math.max(0, Math.min(100, isNaN(num) ? 100 : num));
  if (nextVal !== quickEditState.filterIntensity) {
    pushHistoryState();
  }
  quickEditState.filterIntensity = nextVal;
  clearQuickStyle();
  if (dom.editorFilterIntensitySlider) dom.editorFilterIntensitySlider.value = quickEditState.filterIntensity;
  if (dom.editorFilterIntensityVal) dom.editorFilterIntensityVal.textContent = `${quickEditState.filterIntensity}%`;
  quickEditState.isDirty = true;
  renderQuickEditCanvas();
}

/* Compare Mode */
export function startCompare() {
  if (!quickEditState.isOpen) return;
  quickEditState.isComparing = true;
  if (dom.quickEditCompareBtn) dom.quickEditCompareBtn.classList.add("active");
  if (dom.editorCompareBadge) dom.editorCompareBadge.style.display = "block";
  renderQuickEditCanvas();
}

export function stopCompare() {
  if (!quickEditState.isOpen) return;
  quickEditState.isComparing = false;
  if (dom.quickEditCompareBtn) dom.quickEditCompareBtn.classList.remove("active");
  if (dom.editorCompareBadge) dom.editorCompareBadge.style.display = "none";
  renderQuickEditCanvas();
}

/* Reset All */
export function resetAllEdits() {
  if (!quickEditState.isOpen || !quickEditState.originalImage) return;

  // Only record history when there is actually something to revert to
  if (hasAnyEdits()) {
    // Reset is a standalone undo step even inside an open mode session.
    quickEditState.modeSessionPushed = false;
    pushHistoryState();
  }
  // Reset supersedes any open mode session (no apply/cancel semantics).
  clearModeSession();
  initWorkingCanvas(quickEditState.originalImage);
  quickEditState.rotation = 0;
  quickEditState.straighten = 0;
  quickEditState.flipH = false;
  quickEditState.flipV = false;
  quickEditState.cropActive = false;
  quickEditState.hasCropped = false;
  quickEditState.normalizedCrop = null;
  quickEditState.smartFixActive = false;
  quickEditState.smartFixIntensity = 100;
  quickEditState.adjustments = { ...DEFAULT_ADJUSTMENTS };
  quickEditState.filter = "none";
  quickEditState.filterIntensity = 100;
  quickEditState.quickStyle = null;
  quickEditState.isDirty = false;
  quickEditState.smartFixLUT = null;
  quickEditState.previewCanvas = null;

  if (dom.cropOverlay) dom.cropOverlay.style.display = "none";
  updateStraightenGridOverlay();
  updateUndoRedoUI();
  updateQuickEditToolbarUI();
  renderQuickEditCanvas();
}

/* Pixel Shader Math: Combine SmartFix, Adjustments, and Filters into single pass */
function processImageDataInPlace(imgData) {
  const data = imgData.data;
  const len = data.length;

  const {
    brightness, contrast, highlights, shadows, whites, blacks,
    saturation, warmth, tint, vibrance,
  } = quickEditState.adjustments;

  const hasSmartFix = quickEditState.smartFixActive && quickEditState.smartFixLUT;
  const sfLUT = hasSmartFix ? quickEditState.smartFixLUT : null;
  const sfAlpha = quickEditState.smartFixIntensity / 100;

  // Active filter
  const filterKey = quickEditState.filter;
  const filterDef = FILTERS[filterKey] || FILTERS.none;
  const isFilterActive = filterKey !== "none";
  const filterAlpha = quickEditState.filterIntensity / 100;

  // Contrast scale factor
  const cFactor = (259 * (contrast + 255)) / (255 * (259 - contrast) || 1);
  const bOffset = brightness * 1.28;

  // Filter adjustment overlay
  const fAdj = filterDef.adjust || {};
  const fContrastFactor = fAdj.contrast ? (259 * (fAdj.contrast + 255)) / (255 * (259 - fAdj.contrast) || 1) : 1;
  const fSatBoost = (fAdj.saturation || 0) / 100;
  const fWarmth = (fAdj.warmth || 0) * 0.4;
  const fTint = (fAdj.tint || 0) * 0.3;
  const fBrightness = (fAdj.brightness || 0) * 1.28;
  const fHighlights = fAdj.highlights || 0;
  const fShadows = fAdj.shadows || 0;
  const fWhites = fAdj.whites || 0;
  const fBlacks = fAdj.blacks || 0;

  // Pixel loop
  for (let i = 0; i < len; i += 4) {
    let r = data[i];
    let g = data[i + 1];
    let b = data[i + 2];

    // 1. Smart Fix (if active)
    if (hasSmartFix) {
      let sfr = sfLUT.lutR[r];
      let sfg = sfLUT.lutG[g];
      let sfb = sfLUT.lutB[b];
      const max = Math.max(sfr, sfg, sfb);
      const min = Math.min(sfr, sfg, sfb);
      const sat = max > 0 ? (max - min) / max : 0;
      const boost = (1 - sat) * 0.12;
      const avg = (sfr + sfg + sfb) / 3;
      sfr = Math.min(255, Math.max(0, sfr + (sfr - avg) * boost));
      sfg = Math.min(255, Math.max(0, sfg + (sfg - avg) * boost));
      sfb = Math.min(255, Math.max(0, sfb + (sfb - avg) * boost));

      r = r * (1 - sfAlpha) + sfr * sfAlpha;
      g = g * (1 - sfAlpha) + sfg * sfAlpha;
      b = b * (1 - sfAlpha) + sfb * sfAlpha;
    }

    // 2. Brightness & Contrast
    if (brightness !== 0 || contrast !== 0) {
      r = (r - 128) * cFactor + 128 + bOffset;
      g = (g - 128) * cFactor + 128 + bOffset;
      b = (b - 128) * cFactor + 128 + bOffset;
      r = Math.max(0, Math.min(255, r));
      g = Math.max(0, Math.min(255, g));
      b = Math.max(0, Math.min(255, b));
    }

    // 3. Highlights & Shadows & Whites & Blacks
    if (highlights !== 0 || shadows !== 0 || whites !== 0 || blacks !== 0) {
      const lum = 0.299 * r + 0.587 * g + 0.114 * b;
      if (lum < 128 && shadows !== 0) {
        const factor = (1 - lum / 128) * (shadows / 100) * 35;
        r += factor; g += factor; b += factor;
      } else if (lum >= 128 && highlights !== 0) {
        const factor = ((lum - 128) / 128) * (highlights / 100) * 35;
        r += factor; g += factor; b += factor;
      }
      if (blacks !== 0) {
        const factor = (1 - lum / 255) * (blacks / 100) * 25;
        r += factor; g += factor; b += factor;
      }
      if (whites !== 0) {
        const factor = (lum / 255) * (whites / 100) * 25;
        r += factor; g += factor; b += factor;
      }
      r = Math.max(0, Math.min(255, r));
      g = Math.max(0, Math.min(255, g));
      b = Math.max(0, Math.min(255, b));
    }

    // 4. Color: Warmth & Tint
    if (warmth !== 0) {
      const w = warmth * 0.35;
      r += w;
      b -= w;
    }
    if (tint !== 0) {
      const t = tint * 0.3;
      g += t;
      r -= t * 0.5;
      b -= t * 0.5;
    }
    if (warmth !== 0 || tint !== 0) {
      r = Math.max(0, Math.min(255, r));
      g = Math.max(0, Math.min(255, g));
      b = Math.max(0, Math.min(255, b));
    }

    // 5. Saturation & Vibrance
    if (saturation !== 0 || vibrance !== 0) {
      const lum = 0.299 * r + 0.587 * g + 0.114 * b;
      const max = Math.max(r, g, b);
      const min = Math.min(r, g, b);
      const curSat = max > 0 ? (max - min) / max : 0;
      const vibBoost = vibrance !== 0 ? (1 - curSat) * (vibrance / 100) : 0;
      const totalSatMult = 1 + (saturation / 100) + vibBoost;

      r = lum + (r - lum) * totalSatMult;
      g = lum + (g - lum) * totalSatMult;
      b = lum + (b - lum) * totalSatMult;
      r = Math.max(0, Math.min(255, r));
      g = Math.max(0, Math.min(255, g));
      b = Math.max(0, Math.min(255, b));
    }

    // 6. Filter Preset
    if (isFilterActive) {
      let fr = r;
      let fg = g;
      let fb = b;

      if (filterDef.isBw) {
        const lum = 0.299 * fr + 0.587 * fg + 0.114 * fb;
        fr = fg = fb = lum;
      }

      if (fBrightness) { fr += fBrightness; fg += fBrightness; fb += fBrightness; }
      if (fContrastFactor !== 1) {
        fr = (fr - 128) * fContrastFactor + 128;
        fg = (fg - 128) * fContrastFactor + 128;
        fb = (fb - 128) * fContrastFactor + 128;
      }
      if (fHighlights !== 0 || fShadows !== 0 || fWhites !== 0 || fBlacks !== 0) {
        const lum = 0.299 * fr + 0.587 * fg + 0.114 * fb;
        if (lum < 128 && fShadows !== 0) {
          const factor = (1 - lum / 128) * (fShadows / 100) * 35;
          fr += factor; fg += factor; fb += factor;
        } else if (lum >= 128 && fHighlights !== 0) {
          const factor = ((lum - 128) / 128) * (fHighlights / 100) * 35;
          fr += factor; fg += factor; fb += factor;
        }
        if (fBlacks !== 0) {
          const factor = (1 - lum / 255) * (fBlacks / 100) * 25;
          fr += factor; fg += factor; fb += factor;
        }
        if (fWhites !== 0) {
          const factor = (lum / 255) * (fWhites / 100) * 25;
          fr += factor; fg += factor; fb += factor;
        }
      }
      if (!filterDef.isBw) {
        if (fWarmth) { fr += fWarmth; fb -= fWarmth; }
        if (fTint) { fg += fTint; fr -= fTint * 0.5; fb -= fTint * 0.5; }
        if (fSatBoost) {
          const lum = 0.299 * fr + 0.587 * fg + 0.114 * fb;
          fr = lum + (fr - lum) * (1 + fSatBoost);
          fg = lum + (fg - lum) * (1 + fSatBoost);
          fb = lum + (fb - lum) * (1 + fSatBoost);
        }
      }

      r = r * (1 - filterAlpha) + fr * filterAlpha;
      g = g * (1 - filterAlpha) + fg * filterAlpha;
      b = b * (1 - filterAlpha) + fb * filterAlpha;
    }

    data[i] = Math.max(0, Math.min(255, Math.round(r)));
    data[i + 1] = Math.max(0, Math.min(255, Math.round(g)));
    data[i + 2] = Math.max(0, Math.min(255, Math.round(b)));
  }
}

/* Apply Vignette falloff overlay */
function applyVignetteOverlay(ctx, w, h, amount) {
  if (amount === 0) return;
  const radius = Math.sqrt((w / 2) ** 2 + (h / 2) ** 2);
  const grad = ctx.createRadialGradient(w / 2, h / 2, radius * 0.4, w / 2, h / 2, radius);
  const absAmount = Math.abs(amount);
  const alpha = Math.min(0.85, (absAmount / 100) * 0.85);
  const color = amount > 0 ? "0,0,0" : "255,255,255";
  grad.addColorStop(0, `rgba(${color},0)`);
  grad.addColorStop(1, `rgba(${color},${alpha})`);

  ctx.save();
  ctx.fillStyle = grad;
  ctx.fillRect(0, 0, w, h);
  ctx.restore();
}

/* Sharpness 3x3 unsharp mask convolution & softening */
function applySharpnessInPlace(imgData, amount) {
  if (amount === 0) return;
  const src = new Uint8ClampedArray(imgData.data);
  const dst = imgData.data;
  const w = imgData.width;
  const h = imgData.height;

  if (amount > 0) {
    const strength = (amount / 100) * 1.5;
    for (let y = 1; y < h - 1; y++) {
      for (let x = 1; x < w - 1; x++) {
        const idx = (y * w + x) * 4;
        for (let c = 0; c < 3; c++) {
          const center = src[idx + c];
          const up = src[((y - 1) * w + x) * 4 + c];
          const down = src[((y + 1) * w + x) * 4 + c];
          const left = src[(y * w + (x - 1)) * 4 + c];
          const right = src[(y * w + (x + 1)) * 4 + c];
          const laplacian = center * 4 - up - down - left - right;
          dst[idx + c] = Math.max(0, Math.min(255, center + laplacian * strength));
        }
      }
    }
  } else {
    // Negative sharpness: smoothing / softening
    const t = Math.min(1, Math.abs(amount) / 100) * 0.75;
    for (let y = 1; y < h - 1; y++) {
      for (let x = 1; x < w - 1; x++) {
        const idx = (y * w + x) * 4;
        for (let c = 0; c < 3; c++) {
          const center = src[idx + c];
          const up = src[((y - 1) * w + x) * 4 + c];
          const down = src[((y + 1) * w + x) * 4 + c];
          const left = src[(y * w + (x - 1)) * 4 + c];
          const right = src[(y * w + (x + 1)) * 4 + c];
          const neighbors = (up + down + left + right) * 0.25;
          dst[idx + c] = Math.max(0, Math.min(255, center * (1 - t) + neighbors * t));
        }
      }
    }
  }
}

/* Render Main Viewport Canvas */
export function renderQuickEditCanvas() {
  if (!quickEditState.isOpen || !dom.quickEditCanvas || !quickEditState.workingCanvas) return;

  const canvas = dom.quickEditCanvas;
  const ctx = canvas.getContext("2d");
  const rot = quickEditState.rotation;
  const straighten = quickEditState.straighten;
  const flipH = quickEditState.flipH;
  const flipV = quickEditState.flipV;
  const srcCanvas = quickEditState.workingCanvas;

  const is90or270 = rot === 90 || rot === 270;
  const targetW = is90or270 ? srcCanvas.height : srcCanvas.width;
  const targetH = is90or270 ? srcCanvas.width : srcCanvas.height;

  canvas.width = targetW;
  canvas.height = targetH;

  // Comparison view
  if (quickEditState.isComparing && quickEditState.originalImage) {
    canvas.width = quickEditState.originalImage.naturalWidth || quickEditState.originalImage.width;
    canvas.height = quickEditState.originalImage.naturalHeight || quickEditState.originalImage.height;
    ctx.drawImage(quickEditState.originalImage, 0, 0);
    return;
  }

  // Draw rotated, straightened, and flipped source
  ctx.save();
  ctx.translate(targetW / 2, targetH / 2);
  const totalAngleDeg = rot + straighten;
  ctx.rotate((totalAngleDeg * Math.PI) / 180);
  const rad = Math.abs((straighten * Math.PI) / 180);
  const sFactor = straighten !== 0
    ? Math.cos(rad) + Math.max(targetH / targetW, targetW / targetH) * Math.sin(rad)
    : 1;
  ctx.scale((flipH ? -1 : 1) * sFactor, (flipV ? -1 : 1) * sFactor);
  ctx.drawImage(srcCanvas, -srcCanvas.width / 2, -srcCanvas.height / 2);
  ctx.restore();

  // Smart fix LUT preparation if enabled
  if (quickEditState.smartFixActive && !quickEditState.smartFixLUT) {
    const sampleCanvas = document.createElement("canvas");
    const maxDim = 800;
    let sW = srcCanvas.width;
    let sH = srcCanvas.height;
    if (sW > maxDim || sH > maxDim) {
      if (sW > sH) {
        sH = Math.round((sH * maxDim) / sW);
        sW = maxDim;
      } else {
        sW = Math.round((sW * maxDim) / sH);
        sH = maxDim;
      }
    }
    sampleCanvas.width = sW;
    sampleCanvas.height = sH;
    const sCtx = sampleCanvas.getContext("2d");
    sCtx.drawImage(srcCanvas, 0, 0, sW, sH);
    const sampleData = sCtx.getImageData(0, 0, sW, sH);
    quickEditState.smartFixLUT = computeSmartFixLUT(sampleData);
  }

  // Check if any pixel adjustments or filters are active
  const hasAdjustments = Object.values(quickEditState.adjustments).some(v => v !== 0);
  const hasFilter = quickEditState.filter !== "none";
  const hasSmartFix = quickEditState.smartFixActive;

  if (hasAdjustments || hasFilter || hasSmartFix) {
    // For large images, apply on a scaled preview canvas for 60fps interactivity
    if (targetW > MAX_SMART_FIX_PREVIEW_DIM || targetH > MAX_SMART_FIX_PREVIEW_DIM) {
      let pW = targetW;
      let pH = targetH;
      if (pW > pH) {
        pH = Math.round((pH * MAX_SMART_FIX_PREVIEW_DIM) / pW);
        pW = MAX_SMART_FIX_PREVIEW_DIM;
      } else {
        pW = Math.round((pW * MAX_SMART_FIX_PREVIEW_DIM) / pH);
        pH = MAX_SMART_FIX_PREVIEW_DIM;
      }

      if (!quickEditState.previewCanvas) {
        quickEditState.previewCanvas = document.createElement("canvas");
      }
      const prevCanvas = quickEditState.previewCanvas;
      if (prevCanvas.width !== pW || prevCanvas.height !== pH) {
        prevCanvas.width = pW;
        prevCanvas.height = pH;
      }
      const prevCtx = prevCanvas.getContext("2d");
      prevCtx.drawImage(canvas, 0, 0, pW, pH);

      const imgData = prevCtx.getImageData(0, 0, pW, pH);
      processImageDataInPlace(imgData);
      if (quickEditState.adjustments.sharpness !== 0) {
        applySharpnessInPlace(imgData, quickEditState.adjustments.sharpness);
      }
      prevCtx.putImageData(imgData, 0, 0);

      const filterDef = FILTERS[quickEditState.filter] || FILTERS.none;
      const fAdj = filterDef.adjust || {};
      const filterAlpha = quickEditState.filterIntensity / 100;
      const filterVignette = (quickEditState.filter !== "none" && fAdj.vignette)
        ? fAdj.vignette * filterAlpha
        : 0;
      const effectiveVignette = Math.max(-100, Math.min(100, quickEditState.adjustments.vignette + filterVignette));

      if (effectiveVignette !== 0) {
        applyVignetteOverlay(prevCtx, pW, pH, effectiveVignette);
      }

      ctx.clearRect(0, 0, canvas.width, canvas.height);
      ctx.drawImage(prevCanvas, 0, 0, targetW, targetH);
    } else {
      const imgData = ctx.getImageData(0, 0, canvas.width, canvas.height);
      processImageDataInPlace(imgData);
      if (quickEditState.adjustments.sharpness !== 0) {
        applySharpnessInPlace(imgData, quickEditState.adjustments.sharpness);
      }
      ctx.putImageData(imgData, 0, 0);

      const filterDef = FILTERS[quickEditState.filter] || FILTERS.none;
      const fAdj = filterDef.adjust || {};
      const filterAlpha = quickEditState.filterIntensity / 100;
      const filterVignette = (quickEditState.filter !== "none" && fAdj.vignette)
        ? fAdj.vignette * filterAlpha
        : 0;
      const effectiveVignette = Math.max(-100, Math.min(100, quickEditState.adjustments.vignette + filterVignette));

      if (effectiveVignette !== 0) {
        applyVignetteOverlay(ctx, canvas.width, canvas.height, effectiveVignette);
      }
    }
  }

  if (quickEditState.cropActive) {
    updateCropOverlayDOM();
  }
}

/* Full resolution export canvas generation */
export function getFullResolutionExportCanvas() {
  const rot = quickEditState.rotation;
  const straighten = quickEditState.straighten;
  const flipH = quickEditState.flipH;
  const flipV = quickEditState.flipV;
  const srcCanvas = quickEditState.workingCanvas;
  const is90or270 = rot === 90 || rot === 270;
  const targetW = is90or270 ? srcCanvas.height : srcCanvas.width;
  const targetH = is90or270 ? srcCanvas.width : srcCanvas.height;

  const exportCanvas = document.createElement("canvas");
  exportCanvas.width = targetW;
  exportCanvas.height = targetH;
  const expCtx = exportCanvas.getContext("2d");

  expCtx.save();
  expCtx.translate(targetW / 2, targetH / 2);
  const totalAngleDeg = rot + straighten;
  expCtx.rotate((totalAngleDeg * Math.PI) / 180);
  const rad = Math.abs((straighten * Math.PI) / 180);
  const sFactor = straighten !== 0
    ? Math.cos(rad) + Math.max(targetH / targetW, targetW / targetH) * Math.sin(rad)
    : 1;
  expCtx.scale((flipH ? -1 : 1) * sFactor, (flipV ? -1 : 1) * sFactor);
  expCtx.drawImage(srcCanvas, -srcCanvas.width / 2, -srcCanvas.height / 2);
  expCtx.restore();

  if (quickEditState.smartFixActive && !quickEditState.smartFixLUT) {
    const sampleCanvas = document.createElement("canvas");
    const maxDim = 800;
    let sW = srcCanvas.width;
    let sH = srcCanvas.height;
    if (sW > maxDim || sH > maxDim) {
      if (sW > sH) {
        sH = Math.round((sH * maxDim) / sW);
        sW = maxDim;
      } else {
        sW = Math.round((sW * maxDim) / sH);
        sH = maxDim;
      }
    }
    sampleCanvas.width = sW;
    sampleCanvas.height = sH;
    const sCtx = sampleCanvas.getContext("2d");
    sCtx.drawImage(srcCanvas, 0, 0, sW, sH);
    const sampleData = sCtx.getImageData(0, 0, sW, sH);
    quickEditState.smartFixLUT = computeSmartFixLUT(sampleData);
  }

  const hasAdjustments = Object.values(quickEditState.adjustments).some(v => v !== 0);
  const hasFilter = quickEditState.filter !== "none";
  const hasSmartFix = quickEditState.smartFixActive;

  if (hasAdjustments || hasFilter || hasSmartFix) {
    const imgData = expCtx.getImageData(0, 0, targetW, targetH);
    processImageDataInPlace(imgData);
    if (quickEditState.adjustments.sharpness !== 0) {
      applySharpnessInPlace(imgData, quickEditState.adjustments.sharpness);
    }
    expCtx.putImageData(imgData, 0, 0);

    const filterDef = FILTERS[quickEditState.filter] || FILTERS.none;
    const fAdj = filterDef.adjust || {};
    const filterAlpha = quickEditState.filterIntensity / 100;
    const filterVignette = (quickEditState.filter !== "none" && fAdj.vignette)
      ? fAdj.vignette * filterAlpha
      : 0;
    const effectiveVignette = Math.max(-100, Math.min(100, quickEditState.adjustments.vignette + filterVignette));

    if (effectiveVignette !== 0) {
      applyVignetteOverlay(expCtx, targetW, targetH, effectiveVignette);
    }
  }

  return exportCanvas;
}

/* Save Edits (Overwrite or Save Copy) */
export async function saveEdits(mode = "overwrite") {
  if (!quickEditState.isOpen || !dom.quickEditCanvas) return;

  // Saving applies the open mode as one undo step so nothing is lost.
  if (quickEditState.activeMode) applyMode();

  if (quickEditState.isComparing) {
    stopCompare();
  }

  const id = quickEditState.photoId;
  const currentItem = state.mediaItems.find(m => m.id === id);
  if (currentItem && (currentItem.media_type === "video" || currentItem.media_type === "audio")) {
    return;
  }

  try {
    if (dom.quickEditSaveBtn) dom.quickEditSaveBtn.disabled = true;
    if (dom.quickEditSaveCopyBtn) dom.quickEditSaveCopyBtn.disabled = true;

    if (quickEditState.imageLoadedPromise) {
      await quickEditState.imageLoadedPromise;
    }

    if (!quickEditState.isOpen || !dom.quickEditCanvas || quickEditState.photoId !== id) {
      return;
    }

    if (!quickEditState.isOriginalLoaded) {
      throw new Error("Failed to load original image");
    }

    const canvas = getFullResolutionExportCanvas();

    let res;
    if (typeof canvas.toBlob === "function") {
      try {
        const blob = await new Promise((resolve) => canvas.toBlob(resolve, "image/jpeg", 0.95));
        if (blob) {
          res = await fetch(`/api/photos/${id}/edit?mode=${encodeURIComponent(mode)}`, {
            method: "POST",
            headers: {
              ...getAuthHeaders(),
              "Content-Type": "image/jpeg",
              "X-Edit-Mode": mode,
            },
            body: blob,
          });
        }
      } catch (blobErr) {
        console.warn("Failed to create blob from canvas, falling back to data URL:", blobErr);
      }
    }

    if (!res) {
      const dataUrl = canvas.toDataURL("image/jpeg", 0.95);
      res = await fetch(`/api/photos/${id}/edit`, {
        method: "POST",
        headers: { ...getAuthHeaders(), "Content-Type": "application/json" },
        body: JSON.stringify({
          mode,
          image_data: dataUrl,
        }),
      });
    }

    if (!res.ok) {
      let errMsg = res.statusText;
      try {
        const err = await res.json();
        if (err.error) errMsg = err.error;
      } catch (_) {}
      if (res.status === 413) {
        errMsg = `Payload too large (${res.statusText || "413"}). Image size exceeds server limit.`;
      }
      throw new Error(errMsg || "Failed to save edit");
    }

    const rawItem = await res.json();
    const updatedItem = normalizeMediaItem(rawItem);
    const ts = Date.now();
    if (updatedItem) {
      updatedItem._cacheBuster = ts;
    }
    const cacheBuster = `?t=${ts}`;

    if (mode === "overwrite") {
      const idx = state.mediaItems.findIndex(m => m.id === id);
      if (idx !== -1) {
        state.mediaItems[idx] = updatedItem;
      }
      const newOriginalUrl = getOriginalMediaUrl(updatedItem);
      if (dom.loupeImg) {
        dom.loupeImg.src = newOriginalUrl;
        dom.loupeImg.dataset.originalUrl = newOriginalUrl;
        dom.loupeImg.dataset.isOriginal = "true";
      }
      const cardImg = document.querySelector(`.photo-card[data-id="${id}"] img`);
      if (cardImg && updatedItem.content_hash) {
        cardImg.src = `/api/thumbnails/${encodeURIComponent(updatedItem.content_hash)}/256${cacheBuster}`;
      }
      if (dom.inspectorImg && state.selectedIds.has(id)) {
        dom.inspectorImg.src = newOriginalUrl;
      }
      showToast(t("quick_edit_saved"));
      if (state.selectedIds && state.selectedIds.has(id)) {
        if (window.imagineUI?.updateInspector) {
          window.imagineUI.updateInspector();
        }
      }
    } else {
      state.mediaItems.unshift(updatedItem);
      state.totalCount++;
      if (dom.totalMediaCount) dom.totalMediaCount.textContent = state.totalCount;
      if (state.loupeIndex >= 0) {
        const newIdx = state.mediaItems.findIndex(m => m.id === id);
        if (newIdx !== -1) {
          state.loupeIndex = newIdx;
        } else {
          state.loupeIndex++;
        }
        if (dom.loupeIndex) {
          if (state.totalCount > state.mediaItems.length) {
            dom.loupeIndex.textContent = `${state.loupeIndex + 1} / ${state.mediaItems.length} (${state.totalCount} ${t("total")})`;
          } else {
            dom.loupeIndex.textContent = `${state.loupeIndex + 1} / ${state.mediaItems.length}`;
          }
        }
      }
      renderGrid();
      if (window.imagineUI?.renderCurrentView) {
        window.imagineUI.renderCurrentView();
      }
      showToast(t("quick_edit_saved_copy", { name: updatedItem.file_name }));
      if (window.imagineUI?.loadMetadata) {
        window.imagineUI.loadMetadata();
      }
    }

    quickEditState.isDirty = false;
    closeQuickEdit(false);
  } catch (err) {
    if (err.message !== "Quick edit cancelled" && quickEditState.isOpen) {
      showToast(t("toast_failed_save_edits", { error: err.message }), "error");
      try {
        alert(`Error saving edits: ${err.message}`);
      } catch (_) {}
    }
  } finally {
    if (dom.quickEditSaveBtn) dom.quickEditSaveBtn.disabled = false;
    if (dom.quickEditSaveCopyBtn) dom.quickEditSaveCopyBtn.disabled = false;
  }
}

/* Update Toolbar UI controls */
export function updateQuickEditToolbarUI() {
  const mode = quickEditState.activeMode;

  // Mode buttons & option panels: exactly one mode visible at a time
  if (dom.quickEditCropBtn) {
    dom.quickEditCropBtn.classList.toggle("active", mode === "crop");
  }
  if (dom.cropSubOptions) {
    dom.cropSubOptions.style.display = mode === "crop" ? "flex" : "none";
  }
  if (dom.quickEditHorizonBtn) {
    dom.quickEditHorizonBtn.classList.toggle("active", mode === "horizon");
  }
  if (dom.horizonSubOptions) {
    dom.horizonSubOptions.style.display = mode === "horizon" ? "flex" : "none";
  }
  if (dom.cropAspectRatioSelect) {
    dom.cropAspectRatioSelect.value = quickEditState.cropRatio;
  }

  if (dom.quickEditSmartFixBtn) {
    dom.quickEditSmartFixBtn.classList.toggle("active", mode === "smartfix");
  }
  if (dom.smartFixSubOptions) {
    dom.smartFixSubOptions.style.display = mode === "smartfix" ? "flex" : "none";
  }
  if (dom.smartFixEnabledCheckbox) {
    dom.smartFixEnabledCheckbox.checked = quickEditState.smartFixActive;
  }
  if (dom.smartFixIntensitySlider) {
    dom.smartFixIntensitySlider.value = quickEditState.smartFixIntensity;
  }
  if (dom.smartFixIntensityVal) {
    dom.smartFixIntensityVal.textContent = `${quickEditState.smartFixIntensity}%`;
  }

  if (dom.editorTabSuggestions) {
    dom.editorTabSuggestions.classList.toggle("active", mode === "suggestions");
  }
  if (dom.editorPanelSuggestions) {
    dom.editorPanelSuggestions.style.display = mode === "suggestions" ? "flex" : "none";
  }
  if (dom.editorTabAdjust) {
    dom.editorTabAdjust.classList.toggle("active", mode === "adjust");
  }
  if (dom.editorPanelAdjust) {
    dom.editorPanelAdjust.style.display = mode === "adjust" ? "flex" : "none";
  }
  if (dom.editorTabFilters) {
    dom.editorTabFilters.classList.toggle("active", mode === "filters");
  }
  if (dom.editorPanelFilters) {
    dom.editorPanelFilters.style.display = mode === "filters" ? "flex" : "none";
  }

  // Global mode Apply/Cancel: visible only while a mode is active
  if (dom.modeApplyBtn) {
    dom.modeApplyBtn.style.display = mode ? "" : "none";
  }
  if (dom.modeCancelBtn) {
    dom.modeCancelBtn.style.display = mode ? "" : "none";
  }

  if (dom.editorFlipHBtn) {
    dom.editorFlipHBtn.classList.toggle("active", quickEditState.flipH);
  }
  if (dom.editorFlipVBtn) {
    dom.editorFlipVBtn.classList.toggle("active", quickEditState.flipV);
  }
  if (dom.editorStraightenSlider) {
    dom.editorStraightenSlider.value = quickEditState.straighten;
  }
  if (dom.editorStraightenVal) {
    dom.editorStraightenVal.textContent = `${quickEditState.straighten}°`;
  }
  if (dom.horizonAngleSlider) {
    dom.horizonAngleSlider.value = quickEditState.straighten;
  }
  if (dom.horizonAngleVal) {
    dom.horizonAngleVal.textContent = `${quickEditState.straighten}°`;
  }

  // Adjustments sliders
  for (const key of Object.keys(quickEditState.adjustments)) {
    updateAdjustmentUI(key);
  }

  // Filter chips active state
  if (dom.editorFiltersStrip) {
    const chips = dom.editorFiltersStrip.querySelectorAll(".editor-filter-chip");
    chips.forEach(chip => {
      const isChipActive = chip.dataset.filter === quickEditState.filter;
      chip.classList.toggle("active", isChipActive);
      chip.setAttribute("aria-pressed", isChipActive ? "true" : "false");
    });
  }
  if (dom.editorFilterIntensityContainer) {
    dom.editorFilterIntensityContainer.style.display = quickEditState.filter !== "none" ? "flex" : "none";
  }
  if (dom.editorFilterIntensitySlider) {
    dom.editorFilterIntensitySlider.value = quickEditState.filterIntensity;
  }
  if (dom.editorFilterIntensityVal) {
    dom.editorFilterIntensityVal.textContent = `${quickEditState.filterIntensity}%`;
  }

  // Quick style buttons active state
  updateQuickStyleUI();

  updateUndoRedoUI();
  updateStraightenGridOverlay();
}

let cropListenersInitialized = false;

/* Crop pointer / mouse listeners */
export function setupCropMouseListeners() {
  if (cropListenersInitialized) return;
  cropListenersInitialized = true;

  const onPointerUp = () => {
    quickEditState.cropDragMode = null;
    window.removeEventListener("pointermove", onPointerMove);
    window.removeEventListener("pointerup", onPointerUp);
    window.removeEventListener("pointercancel", onPointerUp);
  };

  const onPointerMove = (e) => {
    if (!quickEditState.isOpen || !quickEditState.cropActive || !quickEditState.cropDragMode) return;

    const dx = e.clientX - quickEditState.cropDragStart.x;
    const dy = e.clientY - quickEditState.cropDragStart.y;
    const { boxX, boxY, boxW, boxH } = quickEditState.cropDragStart;
    const disp = getActiveCanvasDisplayRect();
    const ratio = getNumericAspectRatio(quickEditState.cropRatio, disp.width, disp.height);

    let newX = boxX;
    let newY = boxY;
    let newW = boxW;
    let newH = boxH;

    const mode = quickEditState.cropDragMode;

    if (mode === "move") {
      newX = Math.max(disp.left, Math.min(disp.left + disp.width - boxW, boxX + dx));
      newY = Math.max(disp.top, Math.min(disp.top + disp.height - boxH, boxY + dy));
    } else {
      if (mode.includes("e")) newW = Math.max(30, boxW + dx);
      if (mode.includes("s")) newH = Math.max(30, boxH + dy);
      if (mode.includes("w")) {
        const potentialW = boxW - dx;
        if (potentialW >= 30) {
          newW = potentialW;
          newX = boxX + dx;
        }
      }
      if (mode.includes("n")) {
        const potentialH = boxH - dy;
        if (potentialH >= 30) {
          newH = potentialH;
          newY = boxY + dy;
        }
      }

      if (ratio !== null) {
        if (mode === "e" || mode === "w") {
          newH = newW / ratio;
        } else if (mode === "n" || mode === "s") {
          newW = newH * ratio;
        } else {
          if (newW / newH > ratio) {
            newW = newH * ratio;
          } else {
            newH = newW / ratio;
          }
          if (mode.includes("w")) newX = boxX + (boxW - newW);
          if (mode.includes("n")) newY = boxY + (boxH - newH);
        }
      }

      if (newX < disp.left) { newW -= (disp.left - newX); newX = disp.left; }
      if (newY < disp.top) { newH -= (disp.top - newY); newY = disp.top; }
      if (newX + newW > disp.left + disp.width) newW = disp.left + disp.width - newX;
      if (newY + newH > disp.top + disp.height) newH = disp.top + disp.height - newY;

      if (ratio !== null) {
        if (newW / newH > ratio) {
          newW = newH * ratio;
        } else {
          newH = newW / ratio;
        }
      }
    }

    quickEditState.cropBox = {
      x: Math.round(newX),
      y: Math.round(newY),
      w: Math.round(newW),
      h: Math.round(newH),
    };

    updateCropOverlayDOM();
    e.preventDefault();
  };

  window.addEventListener("pointerdown", (e) => {
    if (!quickEditState.isOpen || !quickEditState.cropActive) return;

    const handle = e.target.closest(".crop-handle");
    const box = e.target.closest("#cropBox");

    if (handle) {
      quickEditState.cropDragMode = handle.dataset.handle;
    } else if (box) {
      quickEditState.cropDragMode = "move";
    } else {
      return;
    }

    e.stopPropagation();
    e.preventDefault();
    quickEditState.cropDragStart = {
      x: e.clientX,
      y: e.clientY,
      boxX: quickEditState.cropBox.x,
      boxY: quickEditState.cropBox.y,
      boxW: quickEditState.cropBox.w,
      boxH: quickEditState.cropBox.h,
    };
    window.addEventListener("pointermove", onPointerMove);
    window.addEventListener("pointerup", onPointerUp);
    window.addEventListener("pointercancel", onPointerUp);
  });
}
