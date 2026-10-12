"""
Deterministic UI tests for Google Photos-style Editing features in Imagine:
- Adjustments panel (Light, Color, Details sliders)
- Filter presets and intensity control
- Straighten angle slider and Flip H / Flip V controls
- Horizon correction tool with dedicated slider and auto level
- Press '0' on adjustment sliders to reset to 0
- Sharpness and Vignette range from -100 to 100
- Discard unsaved changes styled modal dialog
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


def test_press_0_resets_adjustment_sliders(server, page: Page):
    """Pressing 0 key on a slider or clicking the value badge resets the slider to 0."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    page.locator("#editorTabAdjust").click()
    expect(page.locator("#editorPanelAdjust")).to_be_visible()

    # 1. Set Brightness to 45 and press '0' on the slider
    page.evaluate("""() => {
        const slider = document.getElementById('sliderBrightness');
        slider.value = '45';
        slider.dispatchEvent(new Event('input'));
        slider.dispatchEvent(new Event('change'));
    }""")
    expect(page.locator("#valBrightness")).to_have_text("+45")
    slider_brightness = page.locator("#sliderBrightness")
    slider_brightness.focus()
    page.keyboard.press("0")
    expect(page.locator("#valBrightness")).to_have_text("0")

    # 2. Set Contrast to -30 and click the value badge to reset to 0
    page.evaluate("""() => {
        const slider = document.getElementById('sliderContrast');
        slider.value = '-30';
        slider.dispatchEvent(new Event('input'));
        slider.dispatchEvent(new Event('change'));
    }""")
    expect(page.locator("#valContrast")).to_have_text("-30")
    page.locator("#valContrast").click()
    expect(page.locator("#valContrast")).to_have_text("0")


def test_sharpness_and_vignette_range_negative_to_positive(server, page: Page):
    """Sharpness and Vignette range from -100 to 100."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    page.locator("#loupeQuickEditBtn").click()

    page.locator("#editorTabAdjust").click()

    slider_sharpness = page.locator("#sliderSharpness")
    expect(slider_sharpness).to_have_attribute("min", "-100")
    expect(slider_sharpness).to_have_attribute("max", "100")

    slider_vignette = page.locator("#sliderVignette")
    expect(slider_vignette).to_have_attribute("min", "-100")
    expect(slider_vignette).to_have_attribute("max", "100")

    # Test negative sharpness (softening)
    page.evaluate("""() => {
        const slider = document.getElementById('sliderSharpness');
        slider.value = '-50';
        slider.dispatchEvent(new Event('input'));
        slider.dispatchEvent(new Event('change'));
    }""")
    expect(page.locator("#valSharpness")).to_have_text("-50")

    # Test positive sharpness
    page.evaluate("""() => {
        const slider = document.getElementById('sliderSharpness');
        slider.value = '60';
        slider.dispatchEvent(new Event('input'));
        slider.dispatchEvent(new Event('change'));
    }""")
    expect(page.locator("#valSharpness")).to_have_text("+60")

    # Test negative vignette (white corners)
    page.evaluate("""() => {
        const slider = document.getElementById('sliderVignette');
        slider.value = '-40';
        slider.dispatchEvent(new Event('input'));
        slider.dispatchEvent(new Event('change'));
    }""")
    expect(page.locator("#valVignette")).to_have_text("-40")

    # Test positive vignette (dark corners)
    page.evaluate("""() => {
        const slider = document.getElementById('sliderVignette');
        slider.value = '70';
        slider.dispatchEvent(new Event('input'));
        slider.dispatchEvent(new Event('change'));
    }""")
    expect(page.locator("#valVignette")).to_have_text("+70")


def test_horizon_correction_tool(server, page: Page):
    """Horizon correction tool with dedicated slider, auto leveling, and grid overlay."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    horizon_btn = page.locator("#quickEditHorizonBtn")
    horizon_options = page.locator("#horizonSubOptions")
    grid_overlay = page.locator("#straightenGridOverlay")

    expect(horizon_btn).to_be_visible()
    expect(horizon_options).to_be_hidden()

    # 1. Click Horizon button -> options and grid overlay appear
    horizon_btn.click()
    expect(horizon_options).to_be_visible()
    expect(grid_overlay).to_be_visible()
    expect(horizon_btn).to_have_class(re.compile(r"\bactive\b"))

    # 2. Adjust horizon angle slider
    page.evaluate("""() => {
        const slider = document.getElementById('horizonAngleSlider');
        slider.value = '12';
        slider.dispatchEvent(new Event('input'));
        slider.dispatchEvent(new Event('change'));
    }""")
    expect(page.locator("#horizonAngleVal")).to_have_text("12°")
    expect(page.locator("#editorStraightenVal")).to_have_text("12°")

    # 3. Press 0 on horizon slider to reset
    horizon_slider = page.locator("#horizonAngleSlider")
    horizon_slider.focus()
    page.keyboard.press("0")
    expect(page.locator("#horizonAngleVal")).to_have_text("0°")

    # 4. Click Auto button to trigger auto-leveling
    auto_btn = page.locator("#horizonAutoBtn")
    auto_btn.click()

    # 5. Reset via 0° reset button
    page.locator("#horizonResetBtn").click()
    expect(page.locator("#horizonAngleVal")).to_have_text("0°")

    # 6. Toggle Horizon button off
    horizon_btn.click()
    expect(horizon_options).to_be_hidden()


def test_discard_unsaved_changes_modal(server, page: Page):
    """Styled Discard Unsaved Changes modal dialog appears when cancelling with unsaved edits."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    loupe = page.locator("#loupeModal")
    expect(loupe).to_be_visible()

    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    # Make an edit (dirty state)
    page.locator("#editorTabAdjust").click()
    page.evaluate("""() => {
        const slider = document.getElementById('sliderBrightness');
        slider.value = '30';
        slider.dispatchEvent(new Event('input'));
        slider.dispatchEvent(new Event('change'));
    }""")

    modal = page.locator("#discardChangesModal")
    expect(modal).to_be_hidden()

    # Click Cancel -> styled modal appears instead of window.confirm
    page.locator("#quickEditCancelBtn").click()
    expect(modal).to_be_visible()

    # 1. Click "Keep Editing" -> modal closes, quick edit remains open
    page.locator("#cancelDiscardChangesBtn").click()
    expect(modal).to_be_hidden()
    expect(loupe).to_have_class(re.compile(r"\bis-quick-editing\b"))

    # 2. Click Cancel again -> modal reappears
    page.locator("#quickEditCancelBtn").click()
    expect(modal).to_be_visible()

    # 3. Click "Discard Changes" -> modal closes, quick edit closes and reverts
    page.locator("#confirmDiscardChangesBtn").click()
    expect(modal).to_be_hidden()
    expect(loupe).not_to_have_class(re.compile(r"\bis-quick-editing\b"))
    expect(page.locator("#quickEditToolbar")).to_be_hidden()


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
