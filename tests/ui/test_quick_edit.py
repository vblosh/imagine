"""
Deterministic UI tests for Quick Edit mode in the Original (Loupe) view.
"""

import re
from playwright.sync_api import Page, expect


def test_open_and_close_quick_edit(server, page: Page):
    """Opening and closing quick edit mode via button and E shortcut."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()

    loupe = page.locator("#loupeModal")
    expect(loupe).to_be_visible()

    quick_edit_btn = page.locator("#loupeQuickEditBtn")
    expect(quick_edit_btn).to_be_visible()

    # 1. Click Quick Edit button
    quick_edit_btn.click()
    expect(loupe).to_have_class(re.compile(r"\bis-quick-editing\b"))
    expect(page.locator("#quickEditToolbar")).to_be_visible()
    expect(page.locator("#quickEditContainer")).to_be_visible()
    expect(page.locator("#quickEditCanvas")).to_be_visible()

    # Normal loupe nav buttons should be hidden in quick edit mode
    expect(page.locator("#loupePrevBtn")).to_be_hidden()
    expect(page.locator("#loupeNextBtn")).to_be_hidden()

    # 2. Click Cancel to exit
    page.locator("#quickEditCancelBtn").click()
    expect(loupe).not_to_have_class(re.compile(r"\bis-quick-editing\b"))
    expect(page.locator("#quickEditToolbar")).to_be_hidden()

    # 3. Press E key to toggle Quick Edit
    page.keyboard.press("e")
    expect(loupe).to_have_class(re.compile(r"\bis-quick-editing\b"))
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    # Press Escape to exit Quick Edit
    page.keyboard.press("Escape")
    expect(loupe).not_to_have_class(re.compile(r"\bis-quick-editing\b"))
    expect(page.locator("#quickEditToolbar")).to_be_hidden()


def test_rotate_left_and_right(server, page: Page):
    """Rotating image 90 degrees left and right in Quick Edit."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()

    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    canvas = page.locator("#quickEditCanvas")
    expect(canvas).to_be_visible()

    orig_w = int(canvas.evaluate("el => el.width"))
    orig_h = int(canvas.evaluate("el => el.height"))

    # 1. Click Rotate Right
    page.locator("#quickEditRotateRightBtn").click()
    new_w = int(canvas.evaluate("el => el.width"))
    new_h = int(canvas.evaluate("el => el.height"))
    assert new_w == orig_h
    assert new_h == orig_w

    # 2. Click Rotate Left
    page.locator("#quickEditRotateLeftBtn").click()
    restored_w = int(canvas.evaluate("el => el.width"))
    restored_h = int(canvas.evaluate("el => el.height"))
    assert restored_w == orig_w
    assert restored_h == orig_h

    # 3. Test keyboard shortcuts R and L
    page.keyboard.press("r")
    assert int(canvas.evaluate("el => el.width")) == orig_h
    page.keyboard.press("l")
    assert int(canvas.evaluate("el => el.width")) == orig_w


def test_crop_tool_flow(server, page: Page):
    """Interactive crop overlay, aspect ratios, apply and cancel crop."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()

    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    crop_btn = page.locator("#quickEditCropBtn")
    crop_overlay = page.locator("#cropOverlay")
    crop_sub_options = page.locator("#cropSubOptions")

    expect(crop_overlay).to_be_hidden()
    expect(crop_sub_options).to_be_hidden()

    # 1. Toggle Crop on
    crop_btn.click()
    expect(crop_overlay).to_be_visible()
    expect(crop_sub_options).to_be_visible()
    expect(page.locator("#cropBox")).to_be_visible()
    expect(page.locator(".crop-handle")).to_have_count(8)

    # 2. Select 1:1 Square aspect ratio
    page.locator("#cropAspectRatioSelect").select_option("1:1")
    badge_text = page.locator("#cropDimensionsBadge").text_content()
    w_str, h_str = [s.strip() for s in badge_text.split("×")]
    assert int(w_str) == int(h_str)

    # 3. Apply Crop
    page.locator("#quickEditApplyCropBtn").click()
    expect(crop_overlay).to_be_hidden()
    expect(crop_sub_options).to_be_hidden()

    canvas = page.locator("#quickEditCanvas")
    cropped_w = int(canvas.evaluate("el => el.width"))
    cropped_h = int(canvas.evaluate("el => el.height"))
    assert cropped_w == cropped_h


def test_smart_fix_and_compare(server, page: Page):
    """Toggling Smart Fix, intensity slider, and compare button."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()

    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    sf_btn = page.locator("#quickEditSmartFixBtn")
    sf_sub = page.locator("#smartFixSubOptions")
    expect(sf_sub).to_be_hidden()

    # 1. Toggle Smart Fix on
    sf_btn.click()
    expect(sf_btn).to_have_class(re.compile(r"\bactive\b"))
    expect(sf_sub).to_be_visible()

    # 2. Change intensity
    slider = page.locator("#smartFixIntensitySlider")
    slider.fill("75")
    slider.dispatch_event("input")
    expect(page.locator("#smartFixIntensityVal")).to_have_text("75%")

    # 3. Compare button
    compare_btn = page.locator("#quickEditCompareBtn")
    compare_btn.dispatch_event("pointerdown")
    expect(compare_btn).to_have_class(re.compile(r"\bactive\b"))
    compare_btn.dispatch_event("pointerup")
    expect(compare_btn).not_to_have_class(re.compile(r"\bactive\b"))


def test_reset_all_edits(server, page: Page):
    """Resetting edits reverts all adjustments back to original state."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()

    page.locator("#loupeQuickEditBtn").click()
    canvas = page.locator("#quickEditCanvas")
    orig_w = int(canvas.evaluate("el => el.width"))

    # Apply rotation and Smart Fix
    page.locator("#quickEditRotateRightBtn").click()
    page.locator("#quickEditSmartFixBtn").click()
    expect(page.locator("#quickEditSmartFixBtn")).to_have_class(re.compile(r"\bactive\b"))
    assert int(canvas.evaluate("el => el.width")) != orig_w

    # Click Reset
    page.locator("#quickEditResetBtn").click()
    assert int(canvas.evaluate("el => el.width")) == orig_w
    expect(page.locator("#quickEditSmartFixBtn")).not_to_have_class(re.compile(r"\bactive\b"))


def test_save_overwrite_and_save_copy(server, page: Page):
    """Saving edits in-place (overwrite) and as a new copy."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="birthday.bmp")
    card.dblclick()

    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    canvas = page.locator("#quickEditCanvas")
    orig_w = int(canvas.evaluate("el => el.width"))
    orig_h = int(canvas.evaluate("el => el.height"))

    # 1. Rotate 90 degrees and Save
    page.locator("#quickEditRotateRightBtn").click()
    page.locator("#quickEditSaveBtn").click()

    # Verify toast alert
    expect(page.locator("#toastContainer .toast", has_text="Photo edited and saved successfully")).to_be_visible()

    # Loupe should return to normal viewer with updated image
    loupe = page.locator("#loupeModal")
    expect(loupe).not_to_have_class(re.compile(r"\bis-quick-editing\b"))

    # 2. Open quick edit again and Save Copy
    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    page.locator("#quickEditRotateLeftBtn").click()
    page.locator("#quickEditSaveCopyBtn").click()

    expect(page.locator("#toastContainer .toast", has_text="Saved as new copy")).to_be_visible()

    # Close loupe and verify new card appears in media grid
    page.keyboard.press("Escape")
    expect(loupe).to_be_hidden()
    expect(page.locator(".photo-card", has_text="birthday_edited")).to_be_visible()


def test_quick_edit_disabled_on_video_and_audio(media_server, page: Page):
    """Quick edit button and shortcuts must be disabled for video and audio files."""
    page.goto(media_server["url"])

    loupe = page.locator("#loupeModal")
    quick_edit_btn = page.locator("#loupeQuickEditBtn")
    toolbar = page.locator("#quickEditToolbar")

    # 1. Video item
    video_card = page.locator(".photo-card", has_text="drone_flight.mp4")
    video_card.dblclick()
    expect(loupe).to_be_visible()

    # Quick Edit button should be hidden
    expect(quick_edit_btn).to_be_hidden()

    # Pressing E should NOT activate Quick Edit
    page.keyboard.press("e")
    expect(toolbar).to_be_hidden()
    expect(loupe).not_to_have_class(re.compile(r"\bis-quick-editing\b"))

    page.keyboard.press("Escape")
    expect(loupe).to_be_hidden()

    # 2. Audio item
    audio_card = page.locator(".photo-card", has_text="ambient_track.wav")
    audio_card.dblclick()
    expect(loupe).to_be_visible()

    # Quick Edit button should be hidden
    expect(quick_edit_btn).to_be_hidden()

    # Pressing E should NOT activate Quick Edit
    page.keyboard.press("e")
    expect(toolbar).to_be_hidden()
    expect(loupe).not_to_have_class(re.compile(r"\bis-quick-editing\b"))

    page.keyboard.press("Escape")
    expect(loupe).to_be_hidden()

    # 3. Photo item
    photo_card = page.locator(".photo-card", has_text="birthday.bmp")
    photo_card.dblclick()
    expect(loupe).to_be_visible()

    # Quick Edit button should be visible for photo
    expect(quick_edit_btn).to_be_visible()

    # Pressing E should open Quick Edit
    page.keyboard.press("e")
    expect(toolbar).to_be_visible()
    expect(loupe).to_have_class(re.compile(r"\bis-quick-editing\b"))

    page.keyboard.press("Escape")
    expect(toolbar).to_be_hidden()


def test_after_save_original_file_not_loaded_from_browser_cache(server, page: Page):
    """Verify that after saving an overwrite edit, subsequent views (loupe, quick edit, inspector)
    load the fresh updated image and do not load the stale pre-edit file from browser cache."""
    page.goto(server["url"])

    # Use mountain.bmp which is 800x600 (not square)
    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()

    loupe = page.locator("#loupeModal")
    expect(loupe).to_be_visible()

    # Open quick edit
    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()

    canvas = page.locator("#quickEditCanvas")
    orig_w = int(canvas.evaluate("el => el.width"))
    orig_h = int(canvas.evaluate("el => el.height"))
    assert orig_w > orig_h  # mountain.bmp is landscape

    # Rotate right 90 degrees: dimensions become orig_h x orig_w
    page.locator("#quickEditRotateRightBtn").click()
    assert int(canvas.evaluate("el => el.width")) == orig_h
    assert int(canvas.evaluate("el => el.height")) == orig_w

    # Save with overwrite
    page.locator("#quickEditSaveBtn").click()
    expect(page.locator("#toastContainer .toast", has_text="Photo edited and saved successfully")).to_be_visible()
    expect(loupe).not_to_have_class(re.compile(r"\bis-quick-editing\b"))

    # Loupe image should have the updated src and dimensions
    loupe_img = page.locator("#loupeImg")
    expect(loupe_img).to_be_visible()
    assert int(loupe_img.evaluate("el => el.naturalWidth")) == orig_h
    assert int(loupe_img.evaluate("el => el.naturalHeight")) == orig_w

    # Close loupe
    page.keyboard.press("Escape")
    expect(loupe).to_be_hidden()

    # Re-open loupe on mountain.bmp: must show the updated image, NOT the old cached image
    card.dblclick()
    expect(loupe).to_be_visible()
    assert int(loupe_img.evaluate("el => el.naturalWidth")) == orig_h
    assert int(loupe_img.evaluate("el => el.naturalHeight")) == orig_w

    # Open Quick Edit again on the photo: canvas must be initialized with orig_h x orig_w, NOT orig_w x orig_h from cache
    page.locator("#loupeQuickEditBtn").click()
    expect(page.locator("#quickEditToolbar")).to_be_visible()
    assert int(canvas.evaluate("el => el.width")) == orig_h
    assert int(canvas.evaluate("el => el.height")) == orig_w

    # Close quick edit and loupe
    page.keyboard.press("Escape")
    page.keyboard.press("Escape")
    expect(loupe).to_be_hidden()


