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

    # 1. Open via double click on mountain.bmp
    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.dblclick()
    expect(loupe).to_be_visible()
    expect(page.locator("#loupeFileName")).to_have_text("mountain.bmp")
    expect(page.locator("#loupeIndex")).to_contain_text("1 / 6")
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
