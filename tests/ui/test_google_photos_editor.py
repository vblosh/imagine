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


def test_crop_undo_redo_restores_canvas(server, page: Page):
    """Cropping changes canvas dimensions; Undo restores original uncropped canvas; Redo re-crops."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    # Capture initial dimensions
    dims = page.evaluate("""() => {
        const qe = window._imagineQuickEdit.quickEditState;
        return {
            w: qe.workingCanvas.width,
            h: qe.workingCanvas.height
        };
    }""")
    assert dims["w"] > 0 and dims["h"] > 0

    # Open crop tool and select 1:1 ratio
    page.locator("#quickEditCropBtn").click()
    expect(page.locator("#cropOverlay")).to_be_visible()
    page.locator("#cropAspectRatioSelect").select_option("1:1")

    # Apply crop
    page.locator("#quickEditApplyCropBtn").click()
    expect(page.locator("#cropOverlay")).to_be_hidden()

    cropped_dims = page.evaluate("""() => {
        const qe = window._imagineQuickEdit.quickEditState;
        return {
            hasCropped: qe.hasCropped,
            w: qe.workingCanvas.width,
            h: qe.workingCanvas.height
        };
    }""")
    assert cropped_dims["hasCropped"] is True
    # In 1:1 crop, width and height must match
    assert cropped_dims["w"] == cropped_dims["h"]

    # Undo crop
    undo_btn = page.locator("#editorUndoBtn")
    expect(undo_btn).to_be_enabled()
    undo_btn.click()

    restored_dims = page.evaluate("""() => {
        const qe = window._imagineQuickEdit.quickEditState;
        return {
            hasCropped: qe.hasCropped,
            w: qe.workingCanvas.width,
            h: qe.workingCanvas.height
        };
    }""")
    assert restored_dims["hasCropped"] is False
    assert restored_dims["w"] == dims["w"]
    assert restored_dims["h"] == dims["h"]

    # Redo crop
    redo_btn = page.locator("#editorRedoBtn")
    expect(redo_btn).to_be_enabled()
    redo_btn.click()

    re_cropped_dims = page.evaluate("""() => {
        const qe = window._imagineQuickEdit.quickEditState;
        return {
            hasCropped: qe.hasCropped,
            w: qe.workingCanvas.width,
            h: qe.workingCanvas.height
        };
    }""")
    assert re_cropped_dims["hasCropped"] is True
    assert re_cropped_dims["w"] == cropped_dims["w"]
    assert re_cropped_dims["h"] == cropped_dims["h"]


def test_crop_with_straighten_and_flip(server, page: Page):
    """Applying crop when straighten and flip transforms are active resets transforms to 0."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    # Open crop tool
    page.locator("#quickEditCropBtn").click()
    expect(page.locator("#cropSubOptions")).to_be_visible()

    # Set straighten angle to 15°
    page.evaluate("""() => {
        const slider = document.getElementById('editorStraightenSlider');
        slider.value = '15';
        slider.dispatchEvent(new Event('input'));
        slider.dispatchEvent(new Event('change'));
    }""")
    expect(page.locator("#editorStraightenVal")).to_have_text("15°")

    # Flip horizontal
    page.locator("#editorFlipHBtn").click()

    # Apply crop
    page.locator("#quickEditApplyCropBtn").click()
    expect(page.locator("#cropOverlay")).to_be_hidden()

    # Active transforms must be reset to 0 to prevent double-transforms
    state_after = page.evaluate("""() => {
        const qe = window._imagineQuickEdit.quickEditState;
        return {
            rotation: qe.rotation,
            straighten: qe.straighten,
            flipH: qe.flipH,
            flipV: qe.flipV,
            hasCropped: qe.hasCropped
        };
    }""")
    assert state_after["hasCropped"] is True
    assert state_after["rotation"] == 0
    assert state_after["straighten"] == 0
    assert state_after["flipH"] is False
    assert state_after["flipV"] is False


def test_suggestions_and_crop_tab_navigation(server, page: Page):
    """Suggestions and Crop tabs open their panels and trigger quick styles and flips."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    # Open Suggestions panel
    tab_suggestions = page.locator("#editorTabSuggestions")
    panel_suggestions = page.locator("#editorPanelSuggestions")
    tab_suggestions.click()
    expect(panel_suggestions).to_be_visible()

    # Test Warm quick style
    page.locator("#panelStyleWarmBtn").click()
    expect(page.locator("#valWarmth")).to_have_text("+30")
    expect(page.locator("#valSaturation")).to_have_text("+10")

    # Test Cool quick style
    page.locator("#panelStyleCoolBtn").click()
    expect(page.locator("#valWarmth")).to_have_text("-25")

    # Test B&W Drama quick style
    page.locator("#panelStyleBwBtn").click()
    filter_val = page.evaluate("() => window._imagineQuickEdit.quickEditState.filter")
    assert filter_val == "onyx"

    # Switch to Crop panel
    tab_crop = page.locator("#editorTabCrop")
    panel_crop = page.locator("#editorPanelCrop")
    tab_crop.click()
    expect(panel_crop).to_be_visible()
    expect(panel_suggestions).to_be_hidden()

    # Click panel flip H button
    page.locator("#panelFlipHBtn").click()
    is_flip_h = page.evaluate("() => window._imagineQuickEdit.quickEditState.flipH")
    assert is_flip_h is True


def test_aspect_ratio_5_4_selection(server, page: Page):
    """5:4 aspect ratio option exists, can be selected, and formats crop box correctly."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    page.locator("#loupeQuickEditBtn").click()

    page.locator("#quickEditCropBtn").click()
    expect(page.locator("#cropSubOptions")).to_be_visible()

    aspect_select = page.locator("#cropAspectRatioSelect")
    # Verify 5:4 option exists
    option_5_4 = aspect_select.locator('option[value="5:4"]')
    expect(option_5_4).to_be_attached()

    # Select 5:4
    aspect_select.select_option("5:4")
    crop_state = page.evaluate("""() => {
        const qe = window._imagineQuickEdit.quickEditState;
        return {
            cropRatio: qe.cropRatio,
            w: qe.cropBox.w,
            h: qe.cropBox.h
        };
    }""")
    assert crop_state["cropRatio"] == "5:4"
    # Ratio width/height should be approximately 5/4 = 1.25
    ratio = crop_state["w"] / crop_state["h"]
    assert abs(ratio - 1.25) < 0.05


def test_filter_intensity_reaches_zero(server, page: Page):
    """Filter intensity slider has min=0 and can fade preset to 0%."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    page.locator("#loupeQuickEditBtn").click()

    page.locator("#editorTabFilters").click()
    expect(page.locator("#editorPanelFilters")).to_be_visible()

    # Select Vivid filter
    page.locator('.editor-filter-chip[data-filter="vivid"]').click()

    slider = page.locator("#editorFilterIntensitySlider")
    expect(slider).to_have_attribute("min", "0")

    # Set intensity to 0
    page.evaluate("""() => {
        const sl = document.getElementById('editorFilterIntensitySlider');
        sl.value = '0';
        sl.dispatchEvent(new Event('input'));
        sl.dispatchEvent(new Event('change'));
    }""")
    expect(page.locator("#editorFilterIntensityVal")).to_have_text("0%")
    intensity = page.evaluate("() => window._imagineQuickEdit.quickEditState.filterIntensity")
    assert intensity == 0


def test_onyx_filter_adjustments_and_parameters(server, page: Page):
    """Onyx filter has adjust wrapper with contrast, blacks, whites and Reel has vignette."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    page.locator("#loupeQuickEditBtn").click()

    filters_meta = page.evaluate("""() => {
        const F = window._imagineQuickEdit.FILTERS;
        return {
            onyx: F.onyx,
            reel: F.reel
        };
    }""")
    assert filters_meta["onyx"]["isBw"] is True
    assert filters_meta["onyx"]["adjust"]["contrast"] == 30
    assert filters_meta["onyx"]["adjust"]["blacks"] == -15
    assert filters_meta["onyx"]["adjust"]["whites"] == 15
    assert filters_meta["reel"]["adjust"]["vignette"] == 20

    # Select Onyx filter chip
    page.locator("#editorTabFilters").click()
    chip_onyx = page.locator('.editor-filter-chip[data-filter="onyx"]')
    chip_onyx.click()
    expect(chip_onyx).to_have_class(re.compile(r"\bactive\b"))


def test_straighten_reset_recorded_in_history(server, page: Page):
    """Straightening angle and clicking 0° reset button records previous angle in history for Undo."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    page.locator("#loupeQuickEditBtn").click()

    # Open crop tool
    page.locator("#quickEditCropBtn").click()
    expect(page.locator("#cropSubOptions")).to_be_visible()

    # Adjust straighten slider to 20°
    page.evaluate("""() => {
        const slider = document.getElementById('editorStraightenSlider');
        slider.value = '20';
        slider.dispatchEvent(new Event('input'));
        slider.dispatchEvent(new Event('change'));
    }""")
    expect(page.locator("#editorStraightenVal")).to_have_text("20°")

    # Click 0° Reset button
    page.locator("#editorStraightenResetBtn").click()
    expect(page.locator("#editorStraightenVal")).to_have_text("0°")

    # Undo must restore 20°
    undo_btn = page.locator("#editorUndoBtn")
    expect(undo_btn).to_be_enabled()
    undo_btn.click()
    expect(page.locator("#editorStraightenVal")).to_have_text("20°")
    angle_after_undo = page.evaluate("() => window._imagineQuickEdit.quickEditState.straighten")
    assert angle_after_undo == 20


def test_canvas_pointerdown_crop_handles_no_conflict(server, page: Page):
    """Interacting with crop overlay/handles does not trigger compare mode on the canvas."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    page.locator("#loupeQuickEditBtn").click()

    page.locator("#quickEditCropBtn").click()
    expect(page.locator("#cropOverlay")).to_be_visible()

    compare_badge = page.locator("#editorCompareBadge")
    expect(compare_badge).to_be_hidden()

    # Dispatch pointerdown on crop handle
    page.evaluate("""() => {
        const handle = document.querySelector('.crop-handle[data-handle="se"]');
        if (handle) {
            handle.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true, cancelable: true }));
        }
    }""")
    expect(compare_badge).to_be_hidden()
    is_comparing = page.evaluate("() => window._imagineQuickEdit.quickEditState.isComparing")
    assert is_comparing is False


def test_auto_level_horizon_detection(server, page: Page):
    """Auto Horizon Level detects horizon tilt and applies exact counter-rotation."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    # Open Horizon tool
    page.locator("#quickEditHorizonBtn").click()
    expect(page.locator("#horizonSubOptions")).to_be_visible()

    # Replace workingCanvas with a synthetic tilted horizon of +4.0° (sky on top, dark sea below)
    page.evaluate("""() => {
        const qe = window._imagineQuickEdit.quickEditState;
        const canvas = document.createElement('canvas');
        canvas.width = 640;
        canvas.height = 480;
        const ctx = canvas.getContext('2d');
        const rad = 4.0 * Math.PI / 180;
        const xc = 320;
        const yc = 240;
        for (let y = 0; y < 480; y++) {
            for (let x = 0; x < 640; x++) {
                const hy = yc + (x - xc) * Math.tan(rad);
                ctx.fillStyle = (y < hy) ? '#A0D0FF' : '#1A3320';
                ctx.fillRect(x, y, 1, 1);
            }
        }
        qe.workingCanvas = canvas;
        qe.workingCtx = ctx;
    }""")

    # Click Auto Horizon button
    page.locator("#horizonAutoBtn").click()

    # With +4.0° tilt, straighten angle must be -4° to level the image
    expect(page.locator("#horizonAngleVal")).to_have_text("-4°")
    straighten_val = page.evaluate("() => window._imagineQuickEdit.quickEditState.straighten")
    assert straighten_val == -4.0

    # Now replace with a synthetic tilted horizon of -6.0°
    page.evaluate("""() => {
        const qe = window._imagineQuickEdit.quickEditState;
        const canvas = document.createElement('canvas');
        canvas.width = 640;
        canvas.height = 480;
        const ctx = canvas.getContext('2d');
        const rad = -6.0 * Math.PI / 180;
        const xc = 320;
        const yc = 240;
        for (let y = 0; y < 480; y++) {
            for (let x = 0; x < 640; x++) {
                const hy = yc + (x - xc) * Math.tan(rad);
                ctx.fillStyle = (y < hy) ? '#A0D0FF' : '#1A3320';
                ctx.fillRect(x, y, 1, 1);
            }
        }
        qe.workingCanvas = canvas;
        qe.workingCtx = ctx;
    }""")

    # Click Auto Horizon button
    page.locator("#horizonAutoBtn").click()

    # With -6.0° tilt, straighten angle must be +6° to level the image
    expect(page.locator("#horizonAngleVal")).to_have_text("6°")
    straighten_val = page.evaluate("() => window._imagineQuickEdit.quickEditState.straighten")
    assert straighten_val == 6.0


def test_rotate_compact_buttons_and_flip_vertical(server, page: Page):
    """Rotate group has Rotate label, icon-only Rotate Left/Right buttons, and Flip Vertical button."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    rotate_group = page.locator(".quick-edit-toolbar .editor-header-row .quick-edit-group").first
    expect(rotate_group).to_be_visible()

    # 1. Label 'Rotate' is present
    label = rotate_group.locator(".quick-edit-group-label")
    expect(label).to_have_text("Rotate")

    # 2. Buttons Rotate Left and Rotate Right have icons and no text
    btn_rot_left = rotate_group.locator("#quickEditRotateLeftBtn")
    btn_rot_right = rotate_group.locator("#quickEditRotateRightBtn")
    expect(btn_rot_left).to_be_visible()
    expect(btn_rot_right).to_be_visible()
    expect(btn_rot_left).to_have_text("")
    expect(btn_rot_right).to_have_text("")
    expect(btn_rot_left.locator("svg use")).to_have_attribute("href", "#icon-rotate-ccw")
    expect(btn_rot_right.locator("svg use")).to_have_attribute("href", "#icon-rotate-cw")

    # 3. Button with icon Flip Vertical is positioned right after Rotate Right
    flip_v_btn = rotate_group.locator("#quickEditRotateRightBtn + #editorFlipVBtn")
    expect(flip_v_btn).to_be_visible()
    expect(flip_v_btn.locator("svg use")).to_have_attribute("href", "#icon-flip-v")
    expect(flip_v_btn).not_to_have_class(re.compile(r"\bactive\b"))

    # 4. Clicking Flip Vertical toggles flipV state
    flip_v_btn.click()
    expect(flip_v_btn).to_have_class(re.compile(r"\bactive\b"))
    assert page.evaluate("() => window._imagineQuickEdit.quickEditState.flipV") is True

    # Clicking again un-flips
    flip_v_btn.click()
    expect(flip_v_btn).not_to_have_class(re.compile(r"\bactive\b"))
    assert page.evaluate("() => window._imagineQuickEdit.quickEditState.flipV") is False

    # 5. Rotate buttons function properly
    btn_rot_right.click()
    assert page.evaluate("() => window._imagineQuickEdit.quickEditState.rotation") == 90
    btn_rot_left.click()
    assert page.evaluate("() => window._imagineQuickEdit.quickEditState.rotation") == 0





def test_quick_style_buttons_reflect_active_state(server, page: Page):
    """Quick style chips highlight the applied style and clear on manual edits."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    page.locator("#editorTabSuggestions").click()
    warm_btn = page.locator("#panelStyleWarmBtn")
    cool_btn = page.locator("#panelStyleCoolBtn")

    expect(warm_btn).not_to_have_class(re.compile(r"\bactive\b"))

    warm_btn.click()
    expect(warm_btn).to_have_class(re.compile(r"\bactive\b"))
    expect(cool_btn).not_to_have_class(re.compile(r"\bactive\b"))

    cool_btn.click()
    expect(cool_btn).to_have_class(re.compile(r"\bactive\b"))
    expect(warm_btn).not_to_have_class(re.compile(r"\bactive\b"))

    # 1. A manual adjustment invalidates the applied style
    page.locator("#editorTabAdjust").click()
    page.evaluate("""() => {
        const slider = document.getElementById('sliderSaturation');
        slider.value = '20';
        slider.dispatchEvent(new Event('input'));
        slider.dispatchEvent(new Event('change'));
    }""")

    page.locator("#editorTabSuggestions").click()
    expect(cool_btn).not_to_have_class(re.compile(r"\bactive\b"))

    # 2. Straighten invalidates the applied style
    warm_btn.click()
    expect(warm_btn).to_have_class(re.compile(r"\bactive\b"))
    page.evaluate("""() => {
        const slider = document.getElementById('editorStraightenSlider');
        slider.value = '5';
        slider.dispatchEvent(new Event('input'));
        slider.dispatchEvent(new Event('change'));
    }""")
    expect(warm_btn).not_to_have_class(re.compile(r"\bactive\b"))

    # 3. Rotation invalidates the applied style
    warm_btn.click()
    expect(warm_btn).to_have_class(re.compile(r"\bactive\b"))
    page.locator("#quickEditRotateRightBtn").click()
    expect(warm_btn).not_to_have_class(re.compile(r"\bactive\b"))

    # 4. Flip invalidates the applied style
    warm_btn.click()
    expect(warm_btn).to_have_class(re.compile(r"\bactive\b"))
    page.locator("#editorFlipVBtn").click()
    expect(warm_btn).not_to_have_class(re.compile(r"\bactive\b"))

    # 5. Smart Fix invalidates the applied style
    warm_btn.click()
    expect(warm_btn).to_have_class(re.compile(r"\bactive\b"))
    page.locator("#quickEditSmartFixBtn").click()
    expect(warm_btn).not_to_have_class(re.compile(r"\bactive\b"))


def test_reset_all_edits_is_undoable(server, page: Page):
    """Reset All records history so the previous edits can be restored with Undo."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    page.locator("#editorTabAdjust").click()
    page.evaluate("""() => {
        const slider = document.getElementById('sliderBrightness');
        slider.value = '40';
        slider.dispatchEvent(new Event('input'));
        slider.dispatchEvent(new Event('change'));
    }""")
    expect(page.locator("#valBrightness")).to_have_text("+40")

    page.locator("#quickEditResetBtn").click()
    expect(page.locator("#valBrightness")).to_have_text("0")

    undo_btn = page.locator("#editorUndoBtn")
    expect(undo_btn).to_be_enabled()
    undo_btn.click()
    expect(page.locator("#valBrightness")).to_have_text("+40")
    assert page.evaluate("() => window._imagineQuickEdit.quickEditState.isDirty") is True

    # Closing editor after undoing reset shows discard confirmation modal
    page.locator("#quickEditCancelBtn").click()
    expect(page.locator("#discardChangesModal")).to_be_visible()


def test_undoing_reset_sets_dirty_even_when_history_becomes_empty(server, page: Page):
    """Undoing Reset restores edits and marks session dirty even if history stack becomes empty."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    # Clear history to simulate single-item history before reset
    page.evaluate("""() => {
        window._imagineQuickEdit.quickEditState.history = [];
        window._imagineQuickEdit.quickEditState.rotation = 90;
        window._imagineQuickEdit.quickEditState.isDirty = true;
    }""")

    # Click Reset
    page.locator("#quickEditResetBtn").click()
    assert page.evaluate("() => window._imagineQuickEdit.quickEditState.rotation") == 0
    assert page.evaluate("() => window._imagineQuickEdit.quickEditState.isDirty") is False

    # Undo Reset
    page.locator("#editorUndoBtn").click()
    assert page.evaluate("() => window._imagineQuickEdit.quickEditState.rotation") == 90
    assert page.evaluate("() => window._imagineQuickEdit.quickEditState.history.length") == 0
    assert page.evaluate("() => window._imagineQuickEdit.quickEditState.isDirty") is True

    # Cancel prompt appears
    page.locator("#quickEditCancelBtn").click()
    expect(page.locator("#discardChangesModal")).to_be_visible()


def test_crop_panel_flip_buttons_update_immediately(server, page: Page):
    """Clicking panel flip buttons immediately updates active class on both panel and toolbar buttons."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    # Open Crop tab panel
    page.locator("#editorTabCrop").click()
    expect(page.locator("#editorPanelCrop")).to_be_visible()

    panel_flip_h = page.locator("#panelFlipHBtn")
    panel_flip_v = page.locator("#panelFlipVBtn")
    toolbar_flip_h = page.locator("#editorFlipHBtn")
    toolbar_flip_v = page.locator("#editorFlipVBtn")

    expect(panel_flip_h).to_be_visible()
    expect(panel_flip_v).to_be_visible()
    expect(panel_flip_h).not_to_have_class(re.compile(r"\bactive\b"))
    expect(panel_flip_v).not_to_have_class(re.compile(r"\bactive\b"))

    # 1. Click panel Flip H -> immediately active
    panel_flip_h.click()
    expect(panel_flip_h).to_have_class(re.compile(r"\bactive\b"))
    expect(toolbar_flip_h).to_have_class(re.compile(r"\bactive\b"))
    assert page.evaluate("() => window._imagineQuickEdit.quickEditState.flipH") is True

    # Click panel Flip H again -> immediately inactive
    panel_flip_h.click()
    expect(panel_flip_h).not_to_have_class(re.compile(r"\bactive\b"))
    expect(toolbar_flip_h).not_to_have_class(re.compile(r"\bactive\b"))
    assert page.evaluate("() => window._imagineQuickEdit.quickEditState.flipH") is False

    # 2. Click panel Flip V -> immediately active
    panel_flip_v.click()
    expect(panel_flip_v).to_have_class(re.compile(r"\bactive\b"))
    expect(toolbar_flip_v).to_have_class(re.compile(r"\bactive\b"))
    assert page.evaluate("() => window._imagineQuickEdit.quickEditState.flipV") is True

    # Click panel Flip V again -> immediately inactive
    panel_flip_v.click()
    expect(panel_flip_v).not_to_have_class(re.compile(r"\bactive\b"))
    expect(toolbar_flip_v).not_to_have_class(re.compile(r"\bactive\b"))
    assert page.evaluate("() => window._imagineQuickEdit.quickEditState.flipV") is False


def test_straighten_grid_overlay_syncs_with_undo(server, page: Page):
    """Undoing a straighten change hides the alignment grid when the angle returns to 0."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    page.locator("#quickEditCropBtn").click()
    expect(page.locator("#cropSubOptions")).to_be_visible()

    grid_overlay = page.locator("#straightenGridOverlay")
    expect(grid_overlay).to_be_hidden()

    page.evaluate("""() => {
        const slider = document.getElementById('editorStraightenSlider');
        slider.value = '15';
        slider.dispatchEvent(new Event('input'));
        slider.dispatchEvent(new Event('change'));
    }""")
    expect(page.locator("#editorStraightenVal")).to_have_text("15°")
    expect(grid_overlay).to_be_visible()

    page.locator("#editorUndoBtn").click()
    expect(page.locator("#editorStraightenVal")).to_have_text("0°")
    expect(grid_overlay).to_be_hidden()


def test_auto_level_horizon_no_op_on_uniform_image_leaves_clean_state(server, page: Page):
    """Auto Horizon Level on a uniform image does not push history or mark editor dirty."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    # Open Horizon tool
    page.locator("#quickEditHorizonBtn").click()
    expect(page.locator("#horizonSubOptions")).to_be_visible()

    # Replace workingCanvas with a solid uniform color (no horizon edges)
    page.evaluate("""() => {
        const qe = window._imagineQuickEdit.quickEditState;
        const canvas = document.createElement('canvas');
        canvas.width = 320;
        canvas.height = 240;
        const ctx = canvas.getContext('2d');
        ctx.fillStyle = '#808080';
        ctx.fillRect(0, 0, 320, 240);
        qe.workingCanvas = canvas;
        qe.workingCtx = ctx;
        qe.isDirty = false;
    }""")

    # Verify editor is initially clean and undo is disabled
    assert page.evaluate("() => window._imagineQuickEdit.quickEditState.isDirty") is False
    assert page.evaluate("() => window._imagineQuickEdit.quickEditState.straighten") == 0
    expect(page.locator("#editorUndoBtn")).to_be_disabled()

    # Click Auto Horizon button
    page.locator("#horizonAutoBtn").click()

    # Must remain clean, 0° angle, no undo step created
    assert page.evaluate("() => window._imagineQuickEdit.quickEditState.isDirty") is False
    assert page.evaluate("() => window._imagineQuickEdit.quickEditState.straighten") == 0
    expect(page.locator("#editorUndoBtn")).to_be_disabled()
    expect(page.locator("#horizonAngleVal")).to_have_text("0°")


def test_quick_edit_keyboard_guards_focused_button_and_select(server, page: Page):
    """When a select dropdown or button is focused, Quick Edit single-key and Space shortcuts are suppressed."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    # Open Crop tool so cropAspectRatioSelect is visible
    page.locator("#quickEditCropBtn").click()
    expect(page.locator("#cropSubOptions")).to_be_visible()
    aspect_select = page.locator("#cropAspectRatioSelect")
    expect(aspect_select).to_be_visible()

    # 1. Focus select element: single-letter shortcuts (c, s, l, r) and Space must NOT trigger Quick Edit actions
    aspect_select.focus()
    page.keyboard.press("c")
    # Quick Edit crop tab / panel should remain open, cropActive should not toggle off
    expect(page.locator("#cropSubOptions")).to_be_visible()

    page.keyboard.press("s")
    # Smart Fix should NOT have been activated
    assert page.evaluate("() => window._imagineQuickEdit.quickEditState.smartFixActive") is False

    page.keyboard.down("Space")
    # Compare badge should NOT be visible
    expect(page.locator("#editorCompareBadge")).to_be_hidden()
    page.keyboard.up("Space")

    # 2. Focus a button element: single-letter shortcuts and Space must be suppressed
    page.locator("#quickEditHorizonBtn").click()
    expect(page.locator("#horizonSubOptions")).to_be_visible()
    reset_btn = page.locator("#horizonResetBtn")
    reset_btn.focus()
    page.keyboard.press("s")
    assert page.evaluate("() => window._imagineQuickEdit.quickEditState.smartFixActive") is False

    rot_before = page.evaluate("() => window._imagineQuickEdit.quickEditState.rotation")
    page.keyboard.press("r")
    assert page.evaluate("() => window._imagineQuickEdit.quickEditState.rotation") == rot_before


