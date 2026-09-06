/**
 * IMAGINE Photo Organizer - Quick Edit Mode
 * Pure Vanilla JavaScript (Offline-ready, no external dependencies)
 *
 * Supports:
 * - Rotate Left (-90°) and Rotate Right (+90°)
 * - Interactive Crop tool with 8 resize handles, rule-of-thirds grid, and aspect ratios
 * - Auto Smart Fix (Dynamic range stretching, auto contrast, midtone gamma exposure, color cast neutralization, vibrance)
 * - Compare (before/after view)
 * - Revert / Reset
 * - Save (in-place overwrite with thumbnail & DB update) and Save Copy
 */

import { normalizeMediaItem } from "./api.js";
import { state } from "./state.js";
import { dom, showToast } from "./dom.js";
import { renderGrid } from "./media-grid.js";

export const quickEditState = {
  isOpen: false,
  isDirty: false,
  photoId: null,
  originalImage: null,
  workingCanvas: null,
  workingCtx: null,
  rotation: 0,
  cropActive: false,
  cropRatio: "original",
  cropBox: { x: 0, y: 0, w: 0, h: 0 },
  cropDragMode: null,
  cropDragStart: { x: 0, y: 0, boxX: 0, boxY: 0, boxW: 0, boxH: 0 },
  smartFixActive: false,
  smartFixIntensity: 100,
  isComparing: false,
  smartFixLUT: null,
};

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
  const clipHigh = sampledCount * 0.995;

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

function applySmartFixToImageData(srcData, dstData, lut, intensity) {
  const { lutR, lutG, lutB } = lut;
  const src = srcData.data;
  const dst = dstData.data;
  const len = src.length;
  const alpha = intensity / 100;
  const oneMinusAlpha = 1 - alpha;

  for (let i = 0; i < len; i += 4) {
    let r = lutR[src[i]];
    let g = lutG[src[i + 1]];
    let b = lutB[src[i + 2]];

    const max = Math.max(r, g, b);
    const min = Math.min(r, g, b);
    const sat = max > 0 ? (max - min) / max : 0;
    const boost = (1 - sat) * 0.12;
    const avg = (r + g + b) / 3;
    r = Math.min(255, Math.max(0, r + (r - avg) * boost));
    g = Math.min(255, Math.max(0, g + (g - avg) * boost));
    b = Math.min(255, Math.max(0, b + (b - avg) * boost));

    dst[i] = Math.round(src[i] * oneMinusAlpha + r * alpha);
    dst[i + 1] = Math.round(src[i + 1] * oneMinusAlpha + g * alpha);
    dst[i + 2] = Math.round(src[i + 2] * oneMinusAlpha + b * alpha);
    dst[i + 3] = src[i + 3];
  }
}

export function openQuickEdit() {
  if (state.loupeIndex < 0 || state.loupeIndex >= state.mediaItems.length) return;
  const item = state.mediaItems[state.loupeIndex];

  quickEditState.isOpen = true;
  quickEditState.isDirty = false;
  quickEditState.photoId = item.id;
  quickEditState.rotation = 0;
  quickEditState.cropActive = false;
  quickEditState.smartFixActive = false;
  quickEditState.smartFixIntensity = 100;
  quickEditState.isComparing = false;
  quickEditState.smartFixLUT = null;

  if (dom.loupeModal) dom.loupeModal.classList.add("is-quick-editing");
  if (dom.loupeQuickEditBtn) dom.loupeQuickEditBtn.classList.add("active");
  if (dom.quickEditToolbar) dom.quickEditToolbar.style.display = "flex";
  if (dom.quickEditContainer) dom.quickEditContainer.style.display = "flex";
  if (dom.loupeImg) dom.loupeImg.style.display = "none";

  updateQuickEditToolbarUI();

  const img = new Image();
  img.crossOrigin = "anonymous";
  img.onload = () => {
    quickEditState.originalImage = img;
    initWorkingCanvas(img);
    renderQuickEditCanvas();
  };
  img.src = `/api/photos/${item.id}/original`;
}

export function closeQuickEdit(promptIfDirty = true) {
  if (!quickEditState.isOpen) return;

  if (promptIfDirty && quickEditState.isDirty) {
    if (!window.confirm("Discard unsaved changes?")) {
      return;
    }
  }

  quickEditState.isOpen = false;
  quickEditState.isDirty = false;
  quickEditState.cropActive = false;

  if (dom.loupeModal) dom.loupeModal.classList.remove("is-quick-editing");
  if (dom.loupeQuickEditBtn) dom.loupeQuickEditBtn.classList.remove("active");
  if (dom.quickEditToolbar) dom.quickEditToolbar.style.display = "none";
  if (dom.quickEditContainer) dom.quickEditContainer.style.display = "none";
  if (dom.cropOverlay) dom.cropOverlay.style.display = "none";
  if (dom.loupeImg) dom.loupeImg.style.display = "";

  quickEditState.originalImage = null;
  quickEditState.workingCanvas = null;
  quickEditState.workingCtx = null;
  quickEditState.smartFixLUT = null;
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

export function rotateLeft() {
  if (!quickEditState.isOpen) return;
  quickEditState.rotation = (quickEditState.rotation - 90 + 360) % 360;
  quickEditState.isDirty = true;
  if (quickEditState.cropActive) {
    initCropBox();
  }
  renderQuickEditCanvas();
}

export function rotateRight() {
  if (!quickEditState.isOpen) return;
  quickEditState.rotation = (quickEditState.rotation + 90) % 360;
  quickEditState.isDirty = true;
  if (quickEditState.cropActive) {
    initCropBox();
  }
  renderQuickEditCanvas();
}

export function toggleCrop() {
  if (!quickEditState.isOpen) return;
  quickEditState.cropActive = !quickEditState.cropActive;
  updateQuickEditToolbarUI();

  if (quickEditState.cropActive) {
    initCropBox();
    if (dom.cropOverlay) dom.cropOverlay.style.display = "block";
  } else {
    if (dom.cropOverlay) dom.cropOverlay.style.display = "none";
  }
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

export function applyCrop() {
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
  croppedCtx.drawImage(canvas, srcX, srcY, srcW, srcH, 0, 0, srcW, srcH);

  quickEditState.workingCanvas = croppedCanvas;
  quickEditState.workingCtx = croppedCtx;
  quickEditState.rotation = 0;
  quickEditState.cropActive = false;
  quickEditState.isDirty = true;
  quickEditState.smartFixLUT = null;

  if (dom.cropOverlay) dom.cropOverlay.style.display = "none";
  updateQuickEditToolbarUI();
  renderQuickEditCanvas();
}

export function cancelCrop() {
  if (!quickEditState.isOpen) return;
  quickEditState.cropActive = false;
  if (dom.cropOverlay) dom.cropOverlay.style.display = "none";
  updateQuickEditToolbarUI();
}

export function toggleSmartFix() {
  if (!quickEditState.isOpen) return;
  quickEditState.smartFixActive = !quickEditState.smartFixActive;
  quickEditState.isDirty = true;
  updateQuickEditToolbarUI();
  renderQuickEditCanvas();
}

export function setSmartFixIntensity(val) {
  quickEditState.smartFixIntensity = Math.max(10, Math.min(100, val));
  if (dom.smartFixIntensitySlider) dom.smartFixIntensitySlider.value = quickEditState.smartFixIntensity;
  if (dom.smartFixIntensityVal) dom.smartFixIntensityVal.textContent = `${quickEditState.smartFixIntensity}%`;
  quickEditState.isDirty = true;
  renderQuickEditCanvas();
}

export function startCompare() {
  if (!quickEditState.isOpen) return;
  quickEditState.isComparing = true;
  if (dom.quickEditCompareBtn) dom.quickEditCompareBtn.classList.add("active");
  renderQuickEditCanvas();
}

export function stopCompare() {
  if (!quickEditState.isOpen) return;
  quickEditState.isComparing = false;
  if (dom.quickEditCompareBtn) dom.quickEditCompareBtn.classList.remove("active");
  renderQuickEditCanvas();
}

export function resetAllEdits() {
  if (!quickEditState.isOpen || !quickEditState.originalImage) return;

  initWorkingCanvas(quickEditState.originalImage);
  quickEditState.rotation = 0;
  quickEditState.cropActive = false;
  quickEditState.smartFixActive = false;
  quickEditState.smartFixIntensity = 100;
  quickEditState.isDirty = false;
  quickEditState.smartFixLUT = null;

  if (dom.cropOverlay) dom.cropOverlay.style.display = "none";
  updateQuickEditToolbarUI();
  renderQuickEditCanvas();
}

export function renderQuickEditCanvas() {
  if (!quickEditState.isOpen || !dom.quickEditCanvas || !quickEditState.workingCanvas) return;

  const canvas = dom.quickEditCanvas;
  const ctx = canvas.getContext("2d");
  const rot = quickEditState.rotation;
  const srcCanvas = quickEditState.workingCanvas;

  const is90or270 = rot === 90 || rot === 270;
  const targetW = is90or270 ? srcCanvas.height : srcCanvas.width;
  const targetH = is90or270 ? srcCanvas.width : srcCanvas.height;

  canvas.width = targetW;
  canvas.height = targetH;

  if (quickEditState.isComparing && quickEditState.originalImage) {
    canvas.width = quickEditState.originalImage.naturalWidth || quickEditState.originalImage.width;
    canvas.height = quickEditState.originalImage.naturalHeight || quickEditState.originalImage.height;
    ctx.drawImage(quickEditState.originalImage, 0, 0);
    return;
  }

  ctx.save();
  ctx.translate(targetW / 2, targetH / 2);
  ctx.rotate((rot * Math.PI) / 180);
  ctx.drawImage(srcCanvas, -srcCanvas.width / 2, -srcCanvas.height / 2);
  ctx.restore();

  if (quickEditState.smartFixActive) {
    if (!quickEditState.smartFixLUT) {
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

    const imgData = ctx.getImageData(0, 0, canvas.width, canvas.height);
    const outData = ctx.createImageData(canvas.width, canvas.height);
    applySmartFixToImageData(imgData, outData, quickEditState.smartFixLUT, quickEditState.smartFixIntensity);
    ctx.putImageData(outData, 0, 0);
  }

  if (quickEditState.cropActive) {
    updateCropOverlayDOM();
  }
}

export async function saveEdits(mode = "overwrite") {
  if (!quickEditState.isOpen || !dom.quickEditCanvas) return;

  const id = quickEditState.photoId;
  const canvas = dom.quickEditCanvas;

  try {
    if (dom.quickEditSaveBtn) dom.quickEditSaveBtn.disabled = true;
    if (dom.quickEditSaveCopyBtn) dom.quickEditSaveCopyBtn.disabled = true;

    const dataUrl = canvas.toDataURL("image/jpeg", 0.95);

    const res = await fetch(`/api/photos/${id}/edit`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        mode,
        image_data: dataUrl,
      }),
    });

    if (!res.ok) {
      const err = await res.json().catch(() => ({ error: res.statusText }));
      throw new Error(err.error || "Failed to save edit");
    }

    const rawItem = await res.json();
    const updatedItem = normalizeMediaItem(rawItem);
    const cacheBuster = `?t=${Date.now()}`;

    if (mode === "overwrite") {
      const idx = state.mediaItems.findIndex(m => m.id === id);
      if (idx !== -1) {
        state.mediaItems[idx] = updatedItem;
      }
      if (dom.loupeImg) dom.loupeImg.src = `/api/photos/${id}/original${cacheBuster}`;
      const cardImg = document.querySelector(`.photo-card[data-id="${id}"] img`);
      if (cardImg) cardImg.src = `/api/thumbnails/${updatedItem.content_hash}/256${cacheBuster}`;
      if (dom.inspectorImg && state.selectedIds.has(id)) {
        dom.inspectorImg.src = `/api/photos/${id}/original${cacheBuster}`;
      }
      showToast("Photo edited and saved successfully");
    } else {
      state.mediaItems.unshift(updatedItem);
      state.totalCount++;
      if (dom.totalMediaCount) dom.totalMediaCount.textContent = state.totalCount;
      renderGrid();
      if (window._imagineApp?.renderCurrentView) {
        window._imagineApp.renderCurrentView();
      }
      showToast(`Saved as new copy: ${updatedItem.file_name}`);
    }

    quickEditState.isDirty = false;
    closeQuickEdit(false);
  } catch (err) {
    alert(`Error saving edits: ${err.message}`);
  } finally {
    if (dom.quickEditSaveBtn) dom.quickEditSaveBtn.disabled = false;
    if (dom.quickEditSaveCopyBtn) dom.quickEditSaveCopyBtn.disabled = false;
  }
}

export function updateQuickEditToolbarUI() {
  if (dom.quickEditCropBtn) {
    dom.quickEditCropBtn.classList.toggle("active", quickEditState.cropActive);
  }
  if (dom.cropSubOptions) {
    dom.cropSubOptions.style.display = quickEditState.cropActive ? "flex" : "none";
  }
  if (dom.cropAspectRatioSelect) {
    dom.cropAspectRatioSelect.value = quickEditState.cropRatio;
  }

  if (dom.quickEditSmartFixBtn) {
    dom.quickEditSmartFixBtn.classList.toggle("active", quickEditState.smartFixActive);
  }
  if (dom.smartFixSubOptions) {
    dom.smartFixSubOptions.style.display = quickEditState.smartFixActive ? "flex" : "none";
  }
  if (dom.smartFixIntensitySlider) {
    dom.smartFixIntensitySlider.value = quickEditState.smartFixIntensity;
  }
  if (dom.smartFixIntensityVal) {
    dom.smartFixIntensityVal.textContent = `${quickEditState.smartFixIntensity}%`;
  }
}

export function setupCropMouseListeners() {
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

    quickEditState.cropDragStart = {
      x: e.clientX,
      y: e.clientY,
      boxX: quickEditState.cropBox.x,
      boxY: quickEditState.cropBox.y,
      boxW: quickEditState.cropBox.w,
      boxH: quickEditState.cropBox.h,
    };
    e.preventDefault();
  });

  window.addEventListener("pointermove", (e) => {
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
    }

    quickEditState.cropBox = {
      x: Math.round(newX),
      y: Math.round(newY),
      w: Math.round(newW),
      h: Math.round(newH),
    };

    updateCropOverlayDOM();
    e.preventDefault();
  });

  window.addEventListener("pointerup", () => {
    quickEditState.cropDragMode = null;
  });
}
