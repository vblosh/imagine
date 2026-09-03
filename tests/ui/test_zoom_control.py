"""
Deterministic UI tests for the Zoom Control slider.
"""

from playwright.sync_api import Page, expect


def test_zoom_slider_updates_css_variable_and_card_size(server, page: Page):
    """Adjusting the zoom slider updates --thumb-size CSS variable and card widths."""
    page.goto(server["url"])

    slider = page.locator("#zoomSlider")
    expect(slider).to_be_visible()

    card = page.locator(".photo-card").first
    expect(card).to_be_visible()

    # Initial default value is 180
    expect(slider).to_have_value("180")

    # Adjust zoom slider to 280px
    page.evaluate("""() => {
        const slider = document.getElementById('zoomSlider');
        slider.value = '280';
        slider.dispatchEvent(new Event('input'));
    }""")

    # Verify CSS variable updated
    thumb_size_280 = page.evaluate("getComputedStyle(document.documentElement).getPropertyValue('--thumb-size').trim()")
    assert thumb_size_280 == "280px"

    box_280 = card.bounding_box()
    assert box_280 is not None
    # Grid column expands at least to the minmax min size
    assert box_280["width"] >= 280

    # Adjust zoom slider to minimum 120px
    page.evaluate("""() => {
        const slider = document.getElementById('zoomSlider');
        slider.value = '120';
        slider.dispatchEvent(new Event('input'));
    }""")

    thumb_size_120 = page.evaluate("getComputedStyle(document.documentElement).getPropertyValue('--thumb-size').trim()")
    assert thumb_size_120 == "120px"

    box_120 = card.bounding_box()
    assert box_120 is not None
    assert box_120["width"] >= 120

    # Card width at 280px must be significantly larger than at 120px
    assert box_280["width"] > box_120["width"]
