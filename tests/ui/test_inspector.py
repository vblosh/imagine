"""
Deterministic UI tests for the Inspector panel.
"""

import re
from playwright.sync_api import Page, expect


def test_inspector_toggle_and_collapse(server, page: Page):
    """Test collapsing and expanding the right inspector panel."""
    page.goto(server["url"])

    inspector = page.locator("#rightInspector")
    expect(inspector).to_be_visible()
    expect(inspector).not_to_have_class(re.compile(r"\bcollapsed\b"))

    # Click toggle inspector button to collapse
    page.locator("#toggleInspectorBtn").click()
    expect(inspector).to_have_class(re.compile(r"\bcollapsed\b"))

    # Click toggle inspector button again to expand
    page.locator("#toggleInspectorBtn").click()
    expect(inspector).not_to_have_class(re.compile(r"\bcollapsed\b"))

    # Click close button inside inspector to collapse
    page.locator("#closeInspectorBtn").click()
    expect(inspector).to_have_class(re.compile(r"\bcollapsed\b"))


def test_inspector_metadata_display(server, page: Page):
    """Selecting a photo populates File Information and Camera/Exposure EXIF metadata."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.click()

    expect(page.locator("#inspectorNoSelection")).to_be_hidden()
    expect(page.locator("#inspectorSelection")).to_be_visible()

    # File information
    expect(page.locator("#infoFileName")).to_have_text("mountain.bmp")
    expect(page.locator("#infoDimensions")).to_have_text("1920 × 1080 px")
    expect(page.locator("#infoFileSize")).to_contain_text("B")
    expect(page.locator("#infoDateTaken")).to_contain_text("2026")
    expect(page.locator("#infoFilePath")).to_contain_text("nature/mountain.bmp")

    # Camera & Exposure (EXIF)
    expect(page.locator("#infoCamera")).to_have_text("Sony ILCE-7M4")
    expect(page.locator("#infoLens")).to_have_text("FE 24-70mm F2.8 GM II")
    expect(page.locator("#infoExposure")).to_have_text("1/500s")
    expect(page.locator("#infoAperture")).to_have_text("f/2.8")
    expect(page.locator("#infoIso")).to_have_text("ISO 100")
    expect(page.locator("#infoFocal")).to_have_text("35.0 mm")
    expect(page.locator("#infoGps")).to_contain_text("46.50000, 11.35000")


def test_inspector_rating_and_flag_updates(server, page: Page):
    """Clicking star rating or flag buttons in inspector updates the photo and reflected on the grid card."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="sunset.bmp")
    card.click()

    # sunset.bmp initial rating is 4, flag is 0
    expect(card.locator(".card-stars span.active")).to_have_count(4)
    expect(page.locator("#inspectorRating span.active")).to_have_count(4)

    # Click 2nd star in inspector to change rating to 2
    page.locator('#inspectorRating span[data-star="2"]').click()
    expect(page.locator("#inspectorRating span.active")).to_have_count(2)
    expect(card.locator(".card-stars span.active")).to_have_count(2)

    # Click Reject button in inspector
    expect(card.locator(".flag-badge")).to_have_count(0)
    page.locator("#inspectorFlag .flag-reject").click()
    expect(card.locator(".flag-badge.reject")).to_be_visible()
    expect(page.locator("#inspectorFlag .flag-reject")).to_have_class(re.compile(r"\bactive\b"))

    # Click Reject again to toggle off (unflag)
    page.locator("#inspectorFlag .flag-reject").click()
    expect(card.locator(".flag-badge")).to_have_count(0)
    expect(page.locator("#inspectorFlag .flag-reject")).not_to_have_class(re.compile(r"\bactive\b"))


def test_inspector_tags_inline_add_and_remove(server, page: Page):
    """Adding a new tag inline in inspector and removing a tag badge."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.click()

    tags_container = page.locator("#inspectorTags")
    # Pre-seeded tags: Alps, Sunset
    expect(tags_container).to_contain_text("Alps")
    expect(tags_container).to_contain_text("Sunset")

    # Add tag "Dolomites" inline
    page.locator("#addTagInput").fill("Dolomites")
    page.locator("#addTagBtn").click()

    # Verify "Dolomites" tag badge is created
    dolomites_badge = tags_container.locator(".tag-badge", has_text="Dolomites")
    expect(dolomites_badge).to_be_visible()

    # Remove "Dolomites" tag badge
    dolomites_badge.locator(".remove-tag").click()
    expect(tags_container.locator(".tag-badge", has_text="Dolomites")).to_have_count(0)


def test_file_info_and_camera_exposure_panels_resizable_attributes(server, page: Page):
    """File Information and Camera & Exposure panels have resizable DOM attributes and CSS."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.click()

    file_info = page.locator("#fileInfoSection")
    exif_sec = page.locator("#exifSection")

    expect(file_info).to_be_visible()
    expect(file_info).to_have_class(re.compile(r"\bresizable-panel\b"))
    expect(file_info.locator("#fileInfoBody")).to_be_visible()
    expect(file_info.locator("#fileInfoResizer")).to_be_visible()

    expect(exif_sec).to_be_visible()
    expect(exif_sec).to_have_class(re.compile(r"\bresizable-panel\b"))
    expect(exif_sec.locator("#exifBody")).to_be_visible()
    expect(exif_sec.locator("#exifResizer")).to_be_visible()

    # CSS resize property is vertical
    file_info_resize = page.eval_on_selector("#fileInfoSection", "el => window.getComputedStyle(el).resize")
    assert file_info_resize == "vertical"

    exif_resize = page.eval_on_selector("#exifSection", "el => window.getComputedStyle(el).resize")
    assert exif_resize == "vertical"

    # Separator roles
    expect(page.locator("#fileInfoResizer")).to_have_attribute("role", "separator")
    expect(page.locator("#exifResizer")).to_have_attribute("role", "separator")
    expect(page.locator("#inspectorResizerLeft")).to_have_attribute("role", "separator")


def test_file_info_panel_resize_drag_and_reset(server, page: Page):
    """Dragging File Information resizer expands and shrinks height; double-click resets."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.click()
    expect(page.locator("#inspectorSelection")).to_be_visible()

    file_info = page.locator("#fileInfoSection")
    resizer = page.locator("#fileInfoResizer")
    expect(file_info).to_be_visible()
    initial_bb = file_info.bounding_box()
    assert initial_bb is not None
    initial_height = initial_bb["height"]

    # Drag down by 60px to expand
    r_bb = resizer.bounding_box()
    assert r_bb is not None
    page.mouse.move(r_bb["x"] + r_bb["width"] / 2, r_bb["y"] + r_bb["height"] / 2)
    page.mouse.down()
    page.mouse.move(r_bb["x"] + r_bb["width"] / 2, r_bb["y"] + r_bb["height"] / 2 + 60)
    page.mouse.up()

    expanded_bb = file_info.bounding_box()
    assert expanded_bb is not None
    assert expanded_bb["height"] > initial_height + 45

    # Drag up by 100px to shrink
    r_bb = resizer.bounding_box()
    assert r_bb is not None
    page.mouse.move(r_bb["x"] + r_bb["width"] / 2, r_bb["y"] + r_bb["height"] / 2)
    page.mouse.down()
    page.mouse.move(r_bb["x"] + r_bb["width"] / 2, r_bb["y"] + r_bb["height"] / 2 - 100)
    page.mouse.up()

    shrunk_bb = file_info.bounding_box()
    assert shrunk_bb is not None
    assert shrunk_bb["height"] < initial_height

    # Content is scrollable when shrunk
    scrollable = page.eval_on_selector("#fileInfoBody", "el => el.scrollHeight > el.clientHeight")
    assert scrollable is True

    # Double click resizer to reset
    resizer.dblclick()
    reset_bb = file_info.bounding_box()
    assert reset_bb is not None
    assert abs(reset_bb["height"] - initial_height) < 2


def test_camera_exposure_panel_resize_drag_and_reset(server, page: Page):
    """Dragging Camera & Exposure resizer expands height; double-click resets."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.click()
    expect(page.locator("#inspectorSelection")).to_be_visible()

    exif_sec = page.locator("#exifSection")
    resizer = page.locator("#exifResizer")
    expect(exif_sec).to_be_visible()
    initial_bb = exif_sec.bounding_box()
    assert initial_bb is not None
    initial_height = initial_bb["height"]

    # Drag down by 50px to expand
    r_bb = resizer.bounding_box()
    assert r_bb is not None
    page.mouse.move(r_bb["x"] + r_bb["width"] / 2, r_bb["y"] + r_bb["height"] / 2)
    page.mouse.down()
    page.mouse.move(r_bb["x"] + r_bb["width"] / 2, r_bb["y"] + r_bb["height"] / 2 + 50)
    page.mouse.up()

    expanded_bb = exif_sec.bounding_box()
    assert expanded_bb is not None
    assert expanded_bb["height"] > initial_height + 35

    # Double-click to reset
    resizer.dblclick()
    reset_bb = exif_sec.bounding_box()
    assert reset_bb is not None
    assert abs(reset_bb["height"] - initial_height) < 2


def test_inspector_horizontal_width_resize_drag(server, page: Page):
    """Dragging the left inspector edge resizes the panel horizontally; double-click resets."""
    page.goto(server["url"])

    inspector = page.locator("#rightInspector")
    resizer = page.locator("#inspectorResizerLeft")
    initial_bb = inspector.bounding_box()
    assert initial_bb is not None
    initial_width = initial_bb["width"]

    # Drag left edge 70px to the left (widening)
    r_bb = resizer.bounding_box()
    assert r_bb is not None
    page.mouse.move(r_bb["x"] + r_bb["width"] / 2, r_bb["y"] + r_bb["height"] / 2)
    page.mouse.down()
    page.mouse.move(r_bb["x"] + r_bb["width"] / 2 - 70, r_bb["y"] + r_bb["height"] / 2)
    page.mouse.up()

    widened_bb = inspector.bounding_box()
    assert widened_bb is not None
    assert widened_bb["width"] > initial_width + 50

    # Double-click left resizer to reset
    resizer.dblclick()
    reset_bb = inspector.bounding_box()
    assert reset_bb is not None
    assert abs(reset_bb["width"] - initial_width) < 2


def test_panel_resize_keyboard_controls(server, page: Page):
    """Keyboard Arrow keys on focused resizer adjust height, and Enter resets."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.click()
    expect(page.locator("#inspectorSelection")).to_be_visible()

    file_info = page.locator("#fileInfoSection")
    resizer = page.locator("#fileInfoResizer")
    expect(file_info).to_be_visible()
    initial_bb = file_info.bounding_box()
    assert initial_bb is not None
    initial_height = initial_bb["height"]

    resizer.focus()
    # Press ArrowDown twice (+20px)
    resizer.press("ArrowDown")
    resizer.press("ArrowDown")

    expanded_bb = file_info.bounding_box()
    assert expanded_bb is not None
    assert expanded_bb["height"] >= initial_height + 15

    # Press ArrowUp once (-10px)
    resizer.press("ArrowUp")
    up_bb = file_info.bounding_box()
    assert up_bb is not None
    assert up_bb["height"] < expanded_bb["height"]

    # Press Enter to reset
    resizer.press("Enter")
    reset_bb = file_info.bounding_box()
    assert reset_bb is not None
    assert abs(reset_bb["height"] - initial_height) < 2


def test_panel_resize_persistence_across_page_reloads(server, page: Page):
    """Resized panel height and inspector width persist in localStorage across reloads."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.click()
    expect(page.locator("#inspectorSelection")).to_be_visible()

    file_info = page.locator("#fileInfoSection")
    resizer = page.locator("#fileInfoResizer")
    expect(file_info).to_be_visible()
    initial_bb = file_info.bounding_box()
    assert initial_bb is not None

    # Drag down by 70px
    r_bb = resizer.bounding_box()
    assert r_bb is not None
    page.mouse.move(r_bb["x"] + r_bb["width"] / 2, r_bb["y"] + r_bb["height"] / 2)
    page.mouse.down()
    page.mouse.move(r_bb["x"] + r_bb["width"] / 2, r_bb["y"] + r_bb["height"] / 2 + 70)
    page.mouse.up()

    saved_bb = file_info.bounding_box()
    assert saved_bb is not None
    expected_height = saved_bb["height"]

    # Reload page
    page.reload()
    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.click()
    expect(page.locator("#inspectorSelection")).to_be_visible()

    reloaded_file_info = page.locator("#fileInfoSection")
    reloaded_bb = reloaded_file_info.bounding_box()
    assert reloaded_bb is not None
    assert abs(reloaded_bb["height"] - expected_height) < 2


def test_inspector_rename_file(server, page: Page):
    """Test renaming a file via inspector inline form and persistence."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="sunset.bmp")
    card.click()
    expect(page.locator("#inspectorSelection")).to_be_visible()
    expect(page.locator("#infoFileName")).to_have_text("sunset.bmp")

    # Cancel rename test
    page.locator("#renameFileBtn").click()
    expect(page.locator("#renameFileForm")).to_be_visible()
    expect(page.locator("#renameFileInput")).to_have_value("sunset.bmp")
    page.locator("#cancelRenameBtn").click()
    expect(page.locator("#renameFileForm")).to_be_hidden()
    expect(page.locator("#infoFileName")).to_have_text("sunset.bmp")

    # Perform rename
    page.locator("#renameFileBtn").click()
    expect(page.locator("#renameFileForm")).to_be_visible()
    page.locator("#renameFileInput").fill("sunset_evening.bmp")
    page.locator("#saveRenameBtn").click()

    # Verify updated in inspector and on photo card
    expect(page.locator("#renameFileForm")).to_be_hidden()
    expect(page.locator("#infoFileName")).to_have_text("sunset_evening.bmp")
    expect(page.locator("#infoFilePath")).to_contain_text("sunset_evening.bmp")
    expect(page.locator(".photo-card", has_text="sunset_evening.bmp")).to_be_visible()

    # Reload page to verify persistence in database
    page.reload()
    reloaded_card = page.locator(".photo-card", has_text="sunset_evening.bmp")
    expect(reloaded_card).to_be_visible()
    reloaded_card.click()
    expect(page.locator("#infoFileName")).to_have_text("sunset_evening.bmp")


def test_inspector_change_date(server, page: Page):
    """Test changing date taken in inspector and verifying grid date group update and display."""
    page.goto(server["url"])

    # Initially mountain.bmp is in February 2026 date group
    expect(page.locator(".date-group[data-group-key='February 2026'] .photo-card", has_text="mountain.bmp")).to_be_visible()

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.click()
    expect(page.locator("#inspectorSelection")).to_be_visible()

    # Click edit date button
    page.locator("#editDateTakenBtn").click()
    expect(page.locator("#editDateTakenForm")).to_be_visible()

    # Fill new date: 2024-05-20T10:30
    page.locator("#editDateTakenInput").fill("2024-05-20T10:30")
    page.locator("#saveDateTakenBtn").click()

    expect(page.locator("#editDateTakenForm")).to_be_hidden()
    expect(page.locator("#infoDateTaken")).to_contain_text("2024")
    expect(page.locator("#infoDateTaken")).to_contain_text("May 20")

    # Grid must immediately move the thumbnail into May 2024 date group and remove from February 2026
    expect(page.locator(".date-group[data-group-key='May 2024'] .photo-card", has_text="mountain.bmp")).to_be_visible()
    expect(page.locator(".date-group[data-group-key='February 2026'] .photo-card", has_text="mountain.bmp")).to_have_count(0)

    # Reload page to verify persistence in database and grid
    page.reload()
    expect(page.locator(".date-group[data-group-key='May 2024'] .photo-card", has_text="mountain.bmp")).to_be_visible()
    expect(page.locator(".date-group[data-group-key='February 2026'] .photo-card", has_text="mountain.bmp")).to_have_count(0)
    page.locator(".photo-card", has_text="mountain.bmp").click()
    expect(page.locator("#infoDateTaken")).to_contain_text("2024")
    expect(page.locator("#infoDateTaken")).to_contain_text("May 20")


def test_inspector_change_caption(server, page: Page):
    """Test editing photo caption in inspector."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.click()
    expect(page.locator("#inspectorSelection")).to_be_visible()

    # Click edit caption button
    page.locator("#editCaptionBtn").click()
    expect(page.locator("#editCaptionForm")).to_be_visible()

    page.locator("#editCaptionInput").fill("Majestic snow capped peak")
    page.locator("#saveCaptionBtn").click()

    expect(page.locator("#editCaptionForm")).to_be_hidden()
    expect(page.locator("#infoCaption")).to_have_text("Majestic snow capped peak")

    # Reload page to verify persistence
    page.reload()
    page.locator(".photo-card", has_text="mountain.bmp").click()
    expect(page.locator("#infoCaption")).to_have_text("Majestic snow capped peak")


def test_inspector_move_button_and_outside_rejection(server, page: Page):
    """Test Move button in inspector panel moves photo within photos-dir and rejects moving outside."""
    import os
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.click()
    expect(page.locator("#inspectorSelection")).to_be_visible()

    # Move button in actions section and inline move button next to Path are both present
    move_btn = page.locator("#inspectorMoveBtn")
    expect(move_btn).to_be_visible()
    move_path_btn = page.locator("#inspectorMovePathBtn")
    expect(move_path_btn).to_be_visible()

    # 1. Attempting to move outside photo-dir is rejected
    move_btn.click()
    modal = page.locator("#batchMoveModal")
    expect(modal).to_be_visible()
    expect(page.locator("#batchMoveModalTitle")).to_have_text("Move Photo")
    expect(page.locator("#batchMoveTargetCount")).to_contain_text("1 selected photo")

    page.locator("#batchMovePathInput").fill("../outside_library")
    page.locator("#confirmBatchMoveBtn").click()

    expect(page.locator(".toast-error")).to_be_visible()
    expect(page.locator(".toast-error")).to_contain_text("inside the photos directory")
    # Modal stays open so user can correct destination or cancel without losing context
    expect(modal).to_be_visible()
    # Cards and inspector selection remain intact (photos do not disappear)
    expect(card).to_have_class(re.compile(r"\bselected\b"))
    expect(page.locator("#infoFileName")).to_have_text("mountain.bmp")
    page.locator("#cancelBatchMoveBtn").click()
    expect(modal).to_be_hidden()

    # 2. Reopen via inline move button next to Path and move to valid folder inside photos directory
    move_path_btn.click()
    expect(modal).to_be_visible()
    page.locator("#batchMovePathInput").fill("nature/alpine")
    page.locator("#confirmBatchMoveBtn").click()
    expect(modal).to_be_hidden()

    expect(page.locator(".toast-success")).to_be_visible()
    expect(page.locator(".toast-success")).to_contain_text("Moved 1 photo to nature/alpine")

    # Path in inspector updates to new folder
    page.wait_for_timeout(500)
    expect(page.locator("#infoFilePath")).to_contain_text("alpine")

    # Verify file moved on disk
    photos_dir = server["env"]["photos_dir"]
    assert os.path.isfile(os.path.join(photos_dir, "nature", "alpine", "mountain.bmp"))
    assert not os.path.exists(os.path.join(photos_dir, "nature", "mountain.bmp"))



