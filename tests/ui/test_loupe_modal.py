"""
Deterministic UI tests for the Loupe fullscreen modal viewer.
"""

import re
from playwright.sync_api import Page, expect


def test_open_loupe_via_double_click_and_inspector_overlay(server, page: Page):
    """Opening loupe modal via card double-click and via the inspector overlay button."""
    page.goto(server["url"])

    loupe = page.locator("#loupeModal")
    expect(loupe).to_be_hidden()

    # 1. Open via double click on mountain.bmp (loads 1024 thumbnail by default)
    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    expect(loupe).to_be_visible()
    expect(page.locator("#loupeFileName")).to_have_text("mountain.bmp")
    expect(page.locator("#loupeIndex")).to_contain_text("1 / 6")
    expect(page.locator("#loupeImg")).to_have_attribute("src", re.compile(r"/api/thumbnails/.+/1024"))

    # Clicking on the image loads the full-resolution original
    page.locator("#loupeImg").click()
    expect(page.locator("#loupeImg")).to_have_attribute("src", re.compile(r"/api/photos/\d+/original"))

    # Close loupe
    page.locator("#loupeCloseBtn").click()
    expect(loupe).to_be_hidden()

    # 2. Open via inspector overlay button
    card.click()
    expect(page.locator("#openLoupeFromInspector")).to_be_visible()
    page.locator("#openLoupeFromInspector").click()
    expect(loupe).to_be_visible()
    expect(page.locator("#loupeFileName")).to_have_text("mountain.bmp")
    expect(page.locator("#loupeImg")).to_have_attribute("src", re.compile(r"/api/thumbnails/.+/1024"))
    page.locator("#loupeCloseBtn").click()
    expect(loupe).to_be_hidden()


def test_loupe_navigation_buttons_and_arrow_keys(server, page: Page):
    """Navigating through photos using next/prev buttons and left/right arrow keys."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    # Open loupe on first photo
    cards.first.dblclick()
    loupe = page.locator("#loupeModal")
    expect(loupe).to_be_visible()
    expect(page.locator("#loupeIndex")).to_have_text("1 / 6")

    # Click Next button
    page.locator("#loupeNextBtn").click()
    expect(page.locator("#loupeIndex")).to_have_text("2 / 6")

    # Click Prev button
    page.locator("#loupePrevBtn").click()
    expect(page.locator("#loupeIndex")).to_have_text("1 / 6")

    # Press ArrowRight key
    page.keyboard.press("ArrowRight")
    expect(page.locator("#loupeIndex")).to_have_text("2 / 6")

    # Press ArrowLeft key
    page.keyboard.press("ArrowLeft")
    expect(page.locator("#loupeIndex")).to_have_text("1 / 6")


def test_loupe_arrow_navigation_preserves_original_image_mode(server, page: Page):
    """When user clicks image to load original, navigating with left/right arrows continues loading originals."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    # 1. Open loupe on first photo (initially 1024 thumbnail)
    cards.first.dblclick()
    loupe = page.locator("#loupeModal")
    expect(loupe).to_be_visible()
    loupe_img = page.locator("#loupeImg")
    expect(loupe_img).to_have_attribute("src", re.compile(r"/api/thumbnails/.+/1024"))

    # 2. Click on image to load full original
    loupe_img.click()
    expect(loupe_img).to_have_attribute("src", re.compile(r"/api/photos/\d+/original"))

    # 3. Navigate right with ArrowRight -> new image should directly show original
    page.keyboard.press("ArrowRight")
    expect(page.locator("#loupeIndex")).to_have_text("2 / 6")
    expect(loupe_img).to_have_attribute("src", re.compile(r"/api/photos/\d+/original"))

    # 4. Navigate right again with Next button -> 3rd image also shows original
    page.locator("#loupeNextBtn").click()
    expect(page.locator("#loupeIndex")).to_have_text("3 / 6")
    expect(loupe_img).to_have_attribute("src", re.compile(r"/api/photos/\d+/original"))

    # 5. Navigate left with ArrowLeft -> previous image also shows original
    page.keyboard.press("ArrowLeft")
    expect(page.locator("#loupeIndex")).to_have_text("2 / 6")
    expect(loupe_img).to_have_attribute("src", re.compile(r"/api/photos/\d+/original"))

    # 6. Close loupe and re-open on photo 1 -> should reset back to 1024 thumbnail default
    page.keyboard.press("Escape")
    expect(loupe).to_be_hidden()

    cards.first.dblclick()
    expect(loupe).to_be_visible()
    expect(loupe_img).to_have_attribute("src", re.compile(r"/api/thumbnails/.+/1024"))


def test_loupe_rating_and_flagging(server, page: Page):
    """Rating and flagging photos directly inside the loupe modal toolbar."""
    page.goto(server["url"])

    # Open loupe on birthday.bmp (initially rating 0, unflagged)
    card = page.locator(".photo-card", has_text="birthday.bmp")
    card.dblclick()
    loupe = page.locator("#loupeModal")
    expect(loupe).to_be_visible()

    # Rating inside loupe
    expect(page.locator("#loupeRating span.active")).to_have_count(0)
    page.locator('#loupeRating span[data-star="5"]').click()
    expect(page.locator("#loupeRating span.active")).to_have_count(5)

    # Flag Pick inside loupe
    pick_btn = page.locator("#loupeFlag .flag-pick")
    expect(pick_btn).not_to_have_class(re.compile(r"\bactive\b"))
    pick_btn.click()
    expect(pick_btn).to_have_class(re.compile(r"\bactive\b"))

    # Close loupe and verify updates on grid card
    page.keyboard.press("Escape")
    expect(loupe).to_be_hidden()
    expect(card.locator(".card-stars span.active")).to_have_count(5)
    expect(card.locator(".flag-badge.pick")).to_be_visible()


def test_loupe_close_mechanisms(server, page: Page):
    """Closing the loupe modal via Close button, Escape key, and backdrop click."""
    page.goto(server["url"])

    loupe = page.locator("#loupeModal")
    card = page.locator(".photo-card").first

    # 1. Close via #loupeCloseBtn
    card.dblclick()
    expect(loupe).to_be_visible()
    page.locator("#loupeCloseBtn").click()
    expect(loupe).to_be_hidden()

    # 2. Close via Escape key
    card.dblclick()
    expect(loupe).to_be_visible()
    page.keyboard.press("Escape")
    expect(loupe).to_be_hidden()

    # 3. Close via Backdrop click
    card.dblclick()
    expect(loupe).to_be_visible()
    page.locator("#loupeBackdrop").click(position={"x": 10, "y": 10})
    expect(loupe).to_be_hidden()


def test_loupe_zoom_buttons_and_slider(server, page: Page):
    """Zooming in/out using the toolbar buttons and slider."""
    page.goto(server["url"])

    card = page.locator(".photo-card").first
    card.dblclick()
    loupe = page.locator("#loupeModal")
    expect(loupe).to_be_visible()

    zoom_in_btn = page.locator("#loupeZoomInBtn")
    zoom_out_btn = page.locator("#loupeZoomOutBtn")
    zoom_reset_btn = page.locator("#loupeZoomResetBtn")
    zoom_slider = page.locator("#loupeZoomSlider")
    img = page.locator("#loupeImg")

    # Initial state: 100% zoom, zoom out disabled
    expect(zoom_reset_btn).to_have_text("100%")
    expect(zoom_slider).to_have_value("100")
    expect(zoom_out_btn).to_be_disabled()

    # Click Zoom In (+)
    zoom_in_btn.click()
    expect(zoom_reset_btn).to_have_text("125%")
    expect(zoom_slider).to_have_value("125")
    expect(zoom_out_btn).to_be_enabled()
    expect(img).to_have_attribute("style", re.compile(r"scale\(1\.25\)"))

    # Click Zoom In again
    zoom_in_btn.click()
    expect(zoom_reset_btn).to_have_text("150%")
    expect(img).to_have_attribute("style", re.compile(r"scale\(1\.5\)"))

    # Click Zoom Out (-)
    zoom_out_btn.click()
    expect(zoom_reset_btn).to_have_text("125%")

    # Click Zoom Reset button (resets to 100%)
    zoom_reset_btn.click()
    expect(zoom_reset_btn).to_have_text("100%")
    expect(zoom_out_btn).to_be_disabled()

    # Use slider to set zoom to 250%
    page.evaluate("""() => {
        const slider = document.getElementById('loupeZoomSlider');
        slider.value = '250';
        slider.dispatchEvent(new Event('input'));
    }""")
    expect(zoom_reset_btn).to_have_text("250%")
    expect(img).to_have_attribute("style", re.compile(r"scale\(2\.5\)"))


def test_loupe_zoom_keyboard_shortcuts(server, page: Page):
    """Zooming in/out using keyboard shortcuts (+, -, z, Ctrl+0)."""
    page.goto(server["url"])

    card = page.locator(".photo-card").first
    card.dblclick()
    expect(page.locator("#loupeModal")).to_be_visible()

    zoom_reset_btn = page.locator("#loupeZoomResetBtn")
    expect(zoom_reset_btn).to_have_text("100%")

    # Press '+' or '=' to zoom in
    page.keyboard.press("=")
    expect(zoom_reset_btn).to_have_text("125%")

    page.keyboard.press("+")
    expect(zoom_reset_btn).to_have_text("150%")

    # Press '-' to zoom out
    page.keyboard.press("-")
    expect(zoom_reset_btn).to_have_text("125%")

    # Press 'z' to reset zoom
    page.keyboard.press("z")
    expect(zoom_reset_btn).to_have_text("100%")

    # Press 'z' again to toggle to 200%
    page.keyboard.press("z")
    expect(zoom_reset_btn).to_have_text("200%")

    # Press 'Control+0' to reset to 100%
    page.keyboard.press("Control+0")
    expect(zoom_reset_btn).to_have_text("100%")


def test_loupe_zoom_double_click_and_reset_on_nav(server, page: Page):
    """Double-clicking toggles zoom, and navigating between photos resets zoom."""
    page.goto(server["url"])

    card = page.locator(".photo-card").first
    card.dblclick()
    expect(page.locator("#loupeModal")).to_be_visible()

    viewport = page.locator("#loupeImageViewport")
    zoom_reset_btn = page.locator("#loupeZoomResetBtn")
    expect(zoom_reset_btn).to_have_text("100%")

    # Double click on the viewport image to zoom to 200%
    viewport.dblclick()
    expect(zoom_reset_btn).to_have_text("200%")

    # Double click again to reset to 100%
    viewport.dblclick()
    expect(zoom_reset_btn).to_have_text("100%")

    # Zoom in to 200% again, then navigate to next photo
    viewport.dblclick()
    expect(zoom_reset_btn).to_have_text("200%")
    page.locator("#loupeNextBtn").click()

    # Next photo should have reset zoom back to 100%
    expect(page.locator("#loupeIndex")).to_have_text("2 / 6")
    expect(zoom_reset_btn).to_have_text("100%")


def test_loupe_zoom_pan_drag(server, page: Page):
    """Dragging while zoomed in pans the image."""
    page.goto(server["url"])

    card = page.locator(".photo-card").first
    card.dblclick()
    expect(page.locator("#loupeModal")).to_be_visible()

    # Zoom in to 300% via slider
    page.evaluate("""() => {
        const slider = document.getElementById('loupeZoomSlider');
        slider.value = '300';
        slider.dispatchEvent(new Event('input'));
    }""")
    img = page.locator("#loupeImg")
    expect(img).to_have_attribute("style", re.compile(r"scale\(3\)"))

    viewport = page.locator("#loupeImageViewport")
    box = viewport.bounding_box()
    assert box is not None

    start_x = box["x"] + box["width"] / 2
    start_y = box["y"] + box["height"] / 2

    # Drag image by 60px horizontally and 40px vertically
    page.mouse.move(start_x, start_y)
    page.mouse.down()
    page.mouse.move(start_x - 60, start_y - 40)
    page.mouse.up()

    # Verify transform has non-zero translation
    style = img.get_attribute("style") or ""
    assert "translate(" in style
    assert "translate(0px, 0px)" not in style


def test_loupe_zoom_mouse_wheel(server, page: Page):
    """Mouse wheel scrolling over the image zooms in and out."""
    page.goto(server["url"])

    card = page.locator(".photo-card").first
    card.dblclick()
    expect(page.locator("#loupeModal")).to_be_visible()

    zoom_reset_btn = page.locator("#loupeZoomResetBtn")
    viewport = page.locator("#loupeImageViewport")
    expect(zoom_reset_btn).to_have_text("100%")

    box = viewport.bounding_box()
    assert box is not None

    page.mouse.move(box["x"] + box["width"] / 2, box["y"] + box["height"] / 2)
    # Scroll up (negative deltaY zooms in)
    page.mouse.wheel(delta_x=0, delta_y=-100)
    expect(zoom_reset_btn).not_to_have_text("100%")

    # Scroll down (positive deltaY zooms out back to 100%)
    page.mouse.wheel(delta_x=0, delta_y=300)
    expect(zoom_reset_btn).to_have_text("100%")


