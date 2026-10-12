"""
Deterministic UI tests for Google Photos-style Editing features in Imagine:
- Adjustments panel (Light, Color, Details sliders)
- Filter presets and intensity control
- Straighten angle slider and Flip H / Flip V controls
- Non-destructive parametric Undo/Redo (buttons and Ctrl+Z / Ctrl+Y)
- Hold-to-compare (Spacebar and comparison badge)
- Saving photo with Google Photos edits applied
"""

import re
from playwright.sync_api import Page, expect


def test_editor_tab_switching(server, page: Page):
    """Switching between Adjust and Filters expandable tool panels."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    expect(page.locator("#loupeModal")).to_be_visible()

    # Open Quick Edit
    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    tab_adjust = page.locator("#editorTabAdjust")
    tab_filters = page.locator("#editorTabFilters")
    panel_adjust = page.locator("#editorPanelAdjust")
    panel_filters = page.locator("#editorPanelFilters")

    # Initially both expandable panels are hidden
    expect(panel_adjust).to_be_hidden()
    expect(panel_filters).to_be_hidden()

    # 1. Click Adjust Tab -> Adjust panel opens
    tab_adjust.click()
    expect(panel_adjust).to_be_visible()
    expect(panel_filters).to_be_hidden()
    expect(tab_adjust).to_have_class(re.compile(r"\bactive\b"))

    # 2. Click Filters Tab -> Filters panel opens, Adjust panel closes
    tab_filters.click()
    expect(panel_filters).to_be_visible()
    expect(panel_adjust).to_be_hidden()
    expect(tab_filters).to_have_class(re.compile(r"\bactive\b"))
    expect(tab_adjust).not_to_have_class(re.compile(r"\bactive\b"))

    # 3. Click Filters Tab again -> toggles off (collapses panel)
    tab_filters.click()
    expect(panel_filters).to_be_hidden()
    expect(panel_adjust).to_be_hidden()
    expect(tab_filters).not_to_have_class(re.compile(r"\bactive\b"))


def test_adjustments_and_reset(server, page: Page):
    """Adjusting Light, Color, and Details sliders, and resetting them."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    # Open Adjust panel
    page.locator("#editorTabAdjust").click()
    expect(page.locator("#editorPanelAdjust")).to_be_visible()

    # Adjust Brightness
    page.evaluate("""() => {
        const slider = document.getElementById('sliderBrightness');
        slider.value = '35';
        slider.dispatchEvent(new Event('input'));
        slider.dispatchEvent(new Event('change'));
    }""")
    expect(page.locator("#valBrightness")).to_have_text("+35")

    # Adjust Warmth
    page.evaluate("""() => {
        const slider = document.getElementById('sliderWarmth');
        slider.value = '-25';
        slider.dispatchEvent(new Event('input'));
        slider.dispatchEvent(new Event('change'));
    }""")
    expect(page.locator("#valWarmth")).to_have_text("-25")

    # Adjust Saturation
    page.evaluate("""() => {
        const slider = document.getElementById('sliderSaturation');
        slider.value = '40';
        slider.dispatchEvent(new Event('input'));
        slider.dispatchEvent(new Event('change'));
    }""")
    expect(page.locator("#valSaturation")).to_have_text("+40")

    # Reset Adjustments
    page.locator("#editorResetAdjustBtn").click()
    expect(page.locator("#valBrightness")).to_have_text("0")
    expect(page.locator("#valWarmth")).to_have_text("0")
    expect(page.locator("#valSaturation")).to_have_text("0")


def test_filters_and_intensity(server, page: Page):
    """Selecting filter presets and adjusting intensity slider."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    # Open Filters panel
    page.locator("#editorTabFilters").click()
    expect(page.locator("#editorPanelFilters")).to_be_visible()

    intensity_control = page.locator("#editorFilterIntensityContainer")
    expect(intensity_control).to_be_hidden()

    # Select "vivid" preset chip
    chip_vivid = page.locator('.editor-filter-chip[data-filter="vivid"]')
    chip_vivid.click()
    expect(chip_vivid).to_have_class(re.compile(r"\bactive\b"))
    expect(intensity_control).to_be_visible()

    # Change intensity to 80%
    page.evaluate("""() => {
        const slider = document.getElementById('editorFilterIntensitySlider');
        slider.value = '80';
        slider.dispatchEvent(new Event('input'));
        slider.dispatchEvent(new Event('change'));
    }""")
    expect(page.locator("#editorFilterIntensityVal")).to_have_text("80%")

    # Select "onyx" B&W preset chip
    chip_onyx = page.locator('.editor-filter-chip[data-filter="onyx"]')
    chip_onyx.click()
    expect(chip_onyx).to_have_class(re.compile(r"\bactive\b"))
    expect(chip_vivid).not_to_have_class(re.compile(r"\bactive\b"))

    # Select "none" (Original)
    chip_none = page.locator('.editor-filter-chip[data-filter="none"]')
    chip_none.click()
    expect(chip_none).to_have_class(re.compile(r"\bactive\b"))
    expect(intensity_control).to_be_hidden()


def test_crop_straighten_and_flips(server, page: Page):
    """Straighten angle slider and Flip Horizontal / Vertical controls."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    # Open Crop tool
    page.locator("#quickEditCropBtn").click()
    expect(page.locator("#cropSubOptions")).to_be_visible()
    expect(page.locator("#cropOverlay")).to_be_visible()

    # Adjust Straighten Angle
    page.evaluate("""() => {
        const slider = document.getElementById('editorStraightenSlider');
        slider.value = '15';
        slider.dispatchEvent(new Event('input'));
        slider.dispatchEvent(new Event('change'));
    }""")
    expect(page.locator("#editorStraightenVal")).to_have_text("15°")
    expect(page.locator("#straightenGridOverlay")).to_be_visible()

    # Reset Straighten
    page.locator("#editorStraightenResetBtn").click()
    expect(page.locator("#editorStraightenVal")).to_have_text("0°")

    # Flip Horizontal and Vertical buttons
    flip_h_btn = page.locator("#editorFlipHBtn")
    flip_v_btn = page.locator("#editorFlipVBtn")
    expect(flip_h_btn).to_be_visible()
    expect(flip_v_btn).to_be_visible()

    flip_h_btn.click()
    flip_v_btn.click()

    # Close Crop tool
    page.locator("#quickEditCropBtn").click()
    expect(page.locator("#cropSubOptions")).to_be_hidden()
    expect(page.locator("#straightenGridOverlay")).to_be_hidden()


def test_undo_redo_and_shortcuts(server, page: Page):
    """Parametric Undo/Redo via toolbar buttons and Ctrl+Z / Ctrl+Y keyboard shortcuts."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    undo_btn = page.locator("#editorUndoBtn")
    redo_btn = page.locator("#editorRedoBtn")

    # Initially undo and redo should be disabled
    expect(undo_btn).to_be_disabled()
    expect(redo_btn).to_be_disabled()

    # Open Adjust tab and adjust Brightness to 50
    page.locator("#editorTabAdjust").click()
    page.evaluate("""() => {
        const slider = document.getElementById('sliderBrightness');
        slider.value = '50';
        slider.dispatchEvent(new Event('input'));
        slider.dispatchEvent(new Event('change'));
    }""")
    expect(page.locator("#valBrightness")).to_have_text("+50")
    expect(undo_btn).to_be_enabled()
    expect(redo_btn).to_be_disabled()

    # Click Undo button
    undo_btn.click()
    expect(page.locator("#valBrightness")).to_have_text("0")
    expect(undo_btn).to_be_disabled()
    expect(redo_btn).to_be_enabled()

    # Click Redo button
    redo_btn.click()
    expect(page.locator("#valBrightness")).to_have_text("+50")
    expect(undo_btn).to_be_enabled()
    expect(redo_btn).to_be_disabled()

    # Test Ctrl+Z shortcut
    page.keyboard.press("Control+z")
    expect(page.locator("#valBrightness")).to_have_text("0")

    # Test Ctrl+Y shortcut
    page.keyboard.press("Control+y")
    expect(page.locator("#valBrightness")).to_have_text("+50")


def test_hold_to_compare_spacebar(server, page: Page):
    """Holding Spacebar shows original image and ORIGINAL badge."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    compare_badge = page.locator("#editorCompareBadge")
    expect(compare_badge).to_be_hidden()

    # Apply Smart Fix
    page.locator("#quickEditSmartFixBtn").click()

    # Press and hold Spacebar
    page.keyboard.down("Space")
    expect(compare_badge).to_be_visible()

    # Release Spacebar
    page.keyboard.up("Space")
    expect(compare_badge).to_be_hidden()


def test_save_copy_with_adjustments_and_filter(server, page: Page):
    """Saving edited photo with both adjustments and filter applied as new copy."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    loupe = page.locator("#loupeModal")
    expect(loupe).to_be_visible()

    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    # 1. Adjust brightness
    page.locator("#editorTabAdjust").click()
    page.evaluate("""() => {
        const slider = document.getElementById('sliderBrightness');
        slider.value = '25';
        slider.dispatchEvent(new Event('input'));
        slider.dispatchEvent(new Event('change'));
    }""")

    # 2. Select filter
    page.locator("#editorTabFilters").click()
    page.locator('.editor-filter-chip[data-filter="vivid"]').click()

    # 3. Save copy
    save_copy_btn = page.locator("#quickEditSaveCopyBtn")
    save_copy_btn.click()

    # Expect toast notification
    expect(page.locator("#toastContainer .toast", has_text="Saved as new copy")).to_be_visible()

    # Loupe exits edit mode
    expect(loupe).not_to_have_class(re.compile(r"\bis-quick-editing\b"))
    expect(page.locator("#quickEditToolbar")).to_be_hidden()
