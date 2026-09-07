"""
Deterministic UI tests for DOM startup validation and unguarded listener defenses.
"""

import pytest
from playwright.sync_api import Page, expect


def test_validate_required_dom_passes_on_valid_page(server, page: Page):
    """Calling validateRequiredDom() on a complete page succeeds without error."""
    page.goto(server["url"])

    # Ensure page is loaded
    expect(page.locator("#mediaGrid")).to_be_visible()

    # Call validateRequiredDom directly
    result = page.evaluate("""() => {
        try {
            window._imagineApp.validateRequiredDom();
            return { ok: true };
        } catch (err) {
            return { ok: false, error: err.message };
        }
    }""")
    assert result["ok"] is True


def test_validate_required_dom_throws_when_required_elements_missing(server, page: Page):
    """Calling validateRequiredDom() throws descriptive error when required elements are absent."""
    page.goto(server["url"])

    # Simulate missing required element in dom object
    res = page.evaluate("""() => {
        const originalMediaGrid = window._imagineApp.dom.mediaGrid;
        const originalNavPicks = window._imagineApp.dom.navPicks;
        try {
            // Remove from dom object and DOM
            window._imagineApp.dom.mediaGrid = null;
            window._imagineApp.dom.navPicks = null;
            document.getElementById('mediaGrid').id = 'mediaGrid_temp';
            document.getElementById('navPicks').id = 'navPicks_temp';

            try {
                window._imagineApp.validateRequiredDom();
                return { threw: false };
            } catch (err) {
                return { threw: true, message: err.message };
            }
        } finally {
            document.getElementById('mediaGrid_temp').id = 'mediaGrid';
            document.getElementById('navPicks_temp').id = 'navPicks';
            window._imagineApp.dom.mediaGrid = originalMediaGrid;
            window._imagineApp.dom.navPicks = originalNavPicks;
        }
    }""")

    assert res["threw"] is True
    assert "Missing required DOM elements: mediaGrid, navPicks" in res["message"]


def test_setup_event_listeners_safe_with_missing_optional_elements(server, page: Page):
    """setupEventListeners() does not throw TypeError even if all optional controls are absent."""
    page.goto(server["url"])

    res = page.evaluate("""() => {
        const dom = window._imagineApp.dom;

        // Backup original elements
        const backup = Object.assign({}, dom);

        // Null out optional and unguarded controls identified in audit
        const optionalKeys = [
            'zoomSlider',
            'viewTabs',
            'navAllMedia',
            'navPicks',
            'navRejects',
            'navUnrated',
            'toggleInspectorBtn',
            'closeInspectorBtn',
            'openLoupeFromInspector',
            'inspectorFlag',
            'addTagBtn',
            'addTagInput',
            'addTagCategorySelect',
            'inspectorAddToAlbumBtn',
            'batchClearBtn',
            'batchPickBtn',
            'batchRejectBtn',
            'batchDeleteBtn',
            'batchRating',
            'batchAddTagBtn',
            'batchAddAlbumBtn',
            'batchDateBtn',
            'closeBatchDateModalBtn',
            'cancelBatchDateBtn',
            'batchDateBackdrop',
            'confirmBatchDateBtn',
            'batchDateInput',
            'batchDateShiftHours',
            'shiftMinus24Btn',
            'shiftMinus1Btn',
            'shiftPlus1Btn',
            'shiftPlus24Btn',
            'batchDateTimezoneSelect',
            'batchDateTzFrom',
            'batchDateTzTo',
            'batchDateModeShift',
            'batchDateModeExact',
            'inspectorDeleteBtn',
            'loupeDeleteBtn',
            'loupePrevBtn',
            'loupeNextBtn',
            'loupeCloseBtn',
            'loupeBackdrop',
            'loupeZoomInBtn',
            'loupeZoomOutBtn',
            'loupeZoomResetBtn',
            'loupeZoomSlider',
            'loupeFlag',
            'importBtn',
            'emptyImportBtn',
            'closeImportModalBtn',
            'cancelImportBtn',
            'importBackdrop',
            'startImportBtn',
            'newAlbumBtn',
            'closeNewAlbumModalBtn',
            'cancelAlbumBtn',
            'newAlbumBackdrop',
            'createAlbumSubmitBtn',
            'newTagBtn',
            'closeNewTagModalBtn',
            'cancelTagBtn',
            'newTagBackdrop',
            'tagModalClearInputBtn',
            'tagCategoryCards',
            'tagCategorySelect',
            'tagNameInput',
            'createTagSubmitBtn',
            'addToAlbumSelect',
            'closeAddToAlbumModalBtn',
            'cancelAddToAlbumBtn',
            'addToAlbumBackdrop',
            'confirmAddToAlbumBtn',
            'closeDeleteMediaModalBtn',
            'cancelDeleteMediaBtn',
            'deleteMediaBackdrop',
            'confirmDeleteMediaBtn',
            'deleteFromDiskCheckbox',
            'deleteMediaWarningText'
        ];

        for (const k of optionalKeys) {
            dom[k] = null;
        }

        try {
            window._imagineApp.setupEventListeners();
            return { ok: true };
        } catch (err) {
            return { ok: false, error: err.message, stack: err.stack };
        } finally {
            Object.assign(dom, backup);
        }
    }""")

    assert res["ok"] is True, f"setupEventListeners failed with missing elements: {res.get('error')}"
