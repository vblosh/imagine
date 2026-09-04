"""
Deterministic UI tests for Deleting Photos from Catalog.
"""

from playwright.sync_api import Page, expect


def test_inspector_delete_modal_cancel_and_confirm(server, page: Page):
    """Clicking delete in inspector opens confirmation modal, can be cancelled or confirmed."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)
    expect(page.locator("#totalMediaCount")).to_have_text("6")

    # Select sunset.bmp
    card = page.locator(".photo-card", has_text="sunset.bmp")
    card.click()

    expect(page.locator("#inspectorSelection")).to_be_visible()
    expect(page.locator("#inspectorDeleteBtn")).to_be_visible()

    modal = page.locator("#deleteMediaModal")
    expect(modal).to_be_hidden()

    # Click delete button in inspector
    page.locator("#inspectorDeleteBtn").click()
    expect(modal).to_be_visible()
    expect(page.locator("#deleteMediaPromptText")).to_contain_text("delete this photo")

    # Cancel deletion
    page.locator("#cancelDeleteMediaBtn").click()
    expect(modal).to_be_hidden()
    expect(page.locator(".photo-card", has_text="sunset.bmp")).to_have_count(1)
    expect(page.locator("#totalMediaCount")).to_have_text("6")

    # Click delete again and confirm
    page.locator("#inspectorDeleteBtn").click()
    expect(modal).to_be_visible()
    page.locator("#confirmDeleteMediaBtn").click()

    expect(modal).to_be_hidden()
    # Verify photo removed from grid
    expect(page.locator(".photo-card", has_text="sunset.bmp")).to_have_count(0)
    expect(cards).to_have_count(5)
    expect(page.locator("#totalMediaCount")).to_have_text("5")
    # Inspector should return to no selection
    expect(page.locator("#inspectorNoSelection")).to_be_visible()


def test_batch_delete_photos(server, page: Page):
    """Batch delete removes multiple selected photos simultaneously."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    card1 = page.locator(".photo-card", has_text="sunset.bmp")
    card2 = page.locator(".photo-card", has_text="birthday.bmp")

    # Select two cards
    card1.click()
    card2.click(modifiers=["Control"])

    expect(page.locator("#batchActionBar")).to_be_visible()
    expect(page.locator("#batchDeleteBtn")).to_be_visible()

    modal = page.locator("#deleteMediaModal")
    expect(modal).to_be_hidden()

    # Click batch delete button
    page.locator("#batchDeleteBtn").click()
    expect(modal).to_be_visible()
    expect(page.locator("#deleteMediaPromptText")).to_contain_text("delete 2 selected photos")

    # Confirm deletion
    page.locator("#confirmDeleteMediaBtn").click()
    expect(modal).to_be_hidden()

    expect(page.locator(".photo-card", has_text="sunset.bmp")).to_have_count(0)
    expect(page.locator(".photo-card", has_text="birthday.bmp")).to_have_count(0)
    expect(cards).to_have_count(4)
    expect(page.locator("#totalMediaCount")).to_have_text("4")
    expect(page.locator("#batchActionBar")).to_be_hidden()


def test_keyboard_delete_shortcut_and_escape(server, page: Page):
    """Pressing Delete key opens delete confirmation; Escape closes modal."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="sunset.bmp")
    card.click()

    modal = page.locator("#deleteMediaModal")
    expect(modal).to_be_hidden()

    # Press Delete key
    page.keyboard.press("Delete")
    expect(modal).to_be_visible()

    # Press Escape key
    page.keyboard.press("Escape")
    expect(modal).to_be_hidden()
    expect(card).to_be_visible()


def test_loupe_delete_photo(server, page: Page):
    """Deleting a photo from within the Loupe view closes loupe and removes card."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="sunset.bmp")
    card.dblclick()

    loupe = page.locator("#loupeModal")
    expect(loupe).to_be_visible()
    expect(page.locator("#loupeDeleteBtn")).to_be_visible()

    modal = page.locator("#deleteMediaModal")
    expect(modal).to_be_hidden()

    # Click delete in loupe
    page.locator("#loupeDeleteBtn").click()
    expect(modal).to_be_visible()

    # Confirm deletion
    page.locator("#confirmDeleteMediaBtn").click()
    expect(modal).to_be_hidden()
    expect(loupe).to_be_hidden()
    expect(page.locator(".photo-card", has_text="sunset.bmp")).to_have_count(0)
