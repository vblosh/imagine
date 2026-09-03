"""
Deterministic UI tests for Card Selection and Keyboard Shortcuts.
"""

import re
from playwright.sync_api import Page, expect


def test_single_and_multi_card_selection(server, page: Page):
    """Test single click selection, click replacing selection, Ctrl+click multi-selection, and outside deselect."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    card0 = cards.nth(0)
    card1 = cards.nth(1)

    # 1. Single click card 0
    card0.click()
    expect(card0).to_have_class(re.compile(r"\bselected\b"))
    expect(card1).not_to_have_class(re.compile(r"\bselected\b"))
    expect(page.locator("#inspectorSelection")).to_be_visible()

    # 2. Single click card 1 replaces selection
    card1.click()
    expect(card1).to_have_class(re.compile(r"\bselected\b"))
    expect(card0).not_to_have_class(re.compile(r"\bselected\b"))

    # 3. Ctrl+click card 0 to multi-select
    card0.click(modifiers=["Control"])
    expect(card0).to_have_class(re.compile(r"\bselected\b"))
    expect(card1).to_have_class(re.compile(r"\bselected\b"))
    expect(page.locator("#batchActionBar")).to_be_visible()
    expect(page.locator("#batchSelectedCount")).to_have_text("2 selected")

    # 4. Deselection by clicking outside cards on gridScrollContainer background
    page.locator("#gridScrollContainer").click(position={"x": 5, "y": 5})
    expect(card0).not_to_have_class(re.compile(r"\bselected\b"))
    expect(card1).not_to_have_class(re.compile(r"\bselected\b"))
    expect(page.locator("#batchActionBar")).to_be_hidden()
    expect(page.locator("#inspectorNoSelection")).to_be_visible()


def test_shift_range_selection(server, page: Page):
    """Shift+click selects a contiguous range of photo cards."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    # Click first card
    cards.nth(0).click()
    expect(cards.nth(0)).to_have_class(re.compile(r"\bselected\b"))

    # Shift+click 4th card (index 3)
    cards.nth(3).click(modifiers=["Shift"])

    # Indices 0, 1, 2, 3 should all be selected
    for i in range(4):
        expect(cards.nth(i)).to_have_class(re.compile(r"\bselected\b"))

    # Indices 4, 5 should NOT be selected
    expect(cards.nth(4)).not_to_have_class(re.compile(r"\bselected\b"))
    expect(cards.nth(5)).not_to_have_class(re.compile(r"\bselected\b"))

    expect(page.locator("#batchSelectedCount")).to_have_text("4 selected")


def test_keyboard_select_all_and_deselect(server, page: Page):
    """Pressing Ctrl+A selects all cards; pressing Escape deselects all cards."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    # Click first card to establish focus in main container
    cards.nth(0).click()

    # Press Ctrl+A to select all
    page.keyboard.press("Control+a")

    selected_cards = page.locator(".photo-card.selected")
    expect(selected_cards).to_have_count(6)
    expect(page.locator("#batchSelectedCount")).to_have_text("6 selected")

    # Press Escape to clear selection
    page.keyboard.press("Escape")
    expect(page.locator(".photo-card.selected")).to_have_count(0)
    expect(page.locator("#batchActionBar")).to_be_hidden()


def test_keyboard_rating_shortcuts(server, page: Page):
    """Pressing number keys 1-5 sets rating; pressing 0 clears rating."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="birthday.bmp")
    card.click()

    # Initially 0 stars
    expect(card.locator(".card-stars span.active")).to_have_count(0)

    # Press '3' to set 3 stars
    page.keyboard.press("3")
    expect(card.locator(".card-stars span.active")).to_have_count(3)
    expect(page.locator("#inspectorRating span.active")).to_have_count(3)

    # Press '5' to set 5 stars
    page.keyboard.press("5")
    expect(card.locator(".card-stars span.active")).to_have_count(5)

    # Press '0' to clear rating
    page.keyboard.press("0")
    expect(card.locator(".card-stars span.active")).to_have_count(0)


def test_keyboard_flagging_shortcuts(server, page: Page):
    """Pressing 'p' flags Pick; 'x' flags Reject; 'u' unflags."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="sunset.bmp")
    card.click()

    # sunset.bmp is initially unflagged
    expect(card.locator(".flag-badge")).to_have_count(0)

    # Press 'p' to flag as Pick
    page.keyboard.press("p")
    expect(card.locator(".flag-badge.pick")).to_be_visible()
    expect(page.locator("#inspectorFlag .flag-pick")).to_have_class(re.compile(r"\bactive\b"))

    # Press 'x' to flag as Reject
    page.keyboard.press("x")
    expect(card.locator(".flag-badge.reject")).to_be_visible()
    expect(page.locator("#inspectorFlag .flag-reject")).to_have_class(re.compile(r"\bactive\b"))

    # Press 'u' to Unflag
    page.keyboard.press("u")
    expect(card.locator(".flag-badge")).to_have_count(0)
    expect(page.locator("#inspectorFlag .flag-pick")).not_to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#inspectorFlag .flag-reject")).not_to_have_class(re.compile(r"\bactive\b"))


def test_keyboard_open_and_close_loupe(server, page: Page):
    """Pressing Enter opens Loupe for selected card; Escape closes Loupe."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.click()

    loupe = page.locator("#loupeModal")
    expect(loupe).to_be_hidden()

    # Press Enter
    page.keyboard.press("Enter")
    expect(loupe).to_be_visible()
    expect(page.locator("#loupeFileName")).to_have_text("mountain.bmp")

    # Press Escape to close loupe
    page.keyboard.press("Escape")
    expect(loupe).to_be_hidden()


def test_multi_selection_batch_keyboard_shortcuts(server, page: Page):
    """Pressing rating and flagging keys with multiple cards selected updates all selected cards."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    # Focus and select all
    cards.first.click()
    page.keyboard.press("Control+a")
    expect(page.locator(".photo-card.selected")).to_have_count(6)

    # Press '4' to rate all 6 photos with 4 stars
    page.keyboard.press("4")
    for i in range(6):
        expect(cards.nth(i).locator(".card-stars span.active")).to_have_count(4)

    # Press 'p' to flag all 6 photos as Pick
    page.keyboard.press("p")
    for i in range(6):
        expect(cards.nth(i).locator(".flag-badge.pick")).to_be_visible()

    # Press 'u' to unflag all 6 photos
    page.keyboard.press("u")
    for i in range(6):
        expect(cards.nth(i).locator(".flag-badge")).to_have_count(0)

    # Press Escape to deselect all
    page.keyboard.press("Escape")
    expect(page.locator(".photo-card.selected")).to_have_count(0)

