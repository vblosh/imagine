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


def test_delete_modal_checkbox_ui_toggle(server, page: Page):
    """Also delete from disk checkbox updates warning text, button label, and resets on close."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="sunset.bmp")
    card.click()

    page.locator("#inspectorDeleteBtn").click()
    modal = page.locator("#deleteMediaModal")
    expect(modal).to_be_visible()

    checkbox = page.locator("#deleteFromDiskCheckbox")
    warning_text = page.locator("#deleteMediaWarningText")
    confirm_btn = page.locator("#confirmDeleteMediaBtn")

    # Unchecked by default
    expect(checkbox).to_be_visible()
    expect(checkbox).not_to_be_checked()
    expect(warning_text).to_contain_text("The original files on disk will not be deleted")
    expect(confirm_btn).to_have_text("Delete from Catalog")

    # Check the checkbox
    checkbox.check()
    expect(checkbox).to_be_checked()
    expect(warning_text).to_contain_text("Warning: This will permanently delete the original file(s) from disk")
    expect(confirm_btn).to_have_text("Delete from Disk")

    # Uncheck again
    checkbox.uncheck()
    expect(checkbox).not_to_be_checked()
    expect(warning_text).to_contain_text("The original files on disk will not be deleted")
    expect(confirm_btn).to_have_text("Delete from Catalog")

    # Check and cancel modal; re-opening should reset checkbox to unchecked
    checkbox.check()
    page.locator("#cancelDeleteMediaBtn").click()
    expect(modal).to_be_hidden()

    page.locator("#inspectorDeleteBtn").click()
    expect(modal).to_be_visible()
    expect(checkbox).not_to_be_checked()
    expect(confirm_btn).to_have_text("Delete from Catalog")


def test_delete_from_catalog_and_disk(server, page: Page):
    """Checking 'also delete from disk' removes the photo from catalog and deletes the file from disk."""
    import os
    sunset_path = os.path.join(server["env"]["photos_dir"], "nature", "sunset.bmp")
    assert os.path.isfile(sunset_path)

    page.goto(server["url"])
    card = page.locator(".photo-card", has_text="sunset.bmp")
    card.click()

    page.locator("#inspectorDeleteBtn").click()
    modal = page.locator("#deleteMediaModal")
    expect(modal).to_be_visible()

    page.locator("#deleteFromDiskCheckbox").check()
    page.locator("#confirmDeleteMediaBtn").click()

    expect(modal).to_be_hidden()
    expect(page.locator(".photo-card", has_text="sunset.bmp")).to_have_count(0)
    expect(page.locator(".photo-card")).to_have_count(5)
    expect(page.locator("#totalMediaCount")).to_have_text("5")

    # File on disk should be removed
    assert not os.path.exists(sunset_path)


def test_delete_from_catalog_keeps_file_on_disk(server, page: Page):
    """Leaving 'also delete from disk' unchecked keeps the original file on disk."""
    import os
    mountain_path = os.path.join(server["env"]["photos_dir"], "nature", "mountain.bmp")
    assert os.path.isfile(mountain_path)

    page.goto(server["url"])
    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.click()

    page.locator("#inspectorDeleteBtn").click()
    modal = page.locator("#deleteMediaModal")
    expect(modal).to_be_visible()

    # Leave checkbox unchecked
    expect(page.locator("#deleteFromDiskCheckbox")).not_to_be_checked()
    page.locator("#confirmDeleteMediaBtn").click()

    expect(modal).to_be_hidden()
    expect(page.locator(".photo-card", has_text="mountain.bmp")).to_have_count(0)
    expect(page.locator(".photo-card")).to_have_count(5)

    # File on disk must still exist
    assert os.path.isfile(mountain_path)


def test_batch_delete_from_disk(server, page: Page):
    """Batch deleting with 'also delete from disk' removes multiple files from disk."""
    import os
    sunset_path = os.path.join(server["env"]["photos_dir"], "nature", "sunset.bmp")
    birthday_path = os.path.join(server["env"]["photos_dir"], "family", "birthday.bmp")
    assert os.path.isfile(sunset_path)
    assert os.path.isfile(birthday_path)

    page.goto(server["url"])
    card1 = page.locator(".photo-card", has_text="sunset.bmp")
    card2 = page.locator(".photo-card", has_text="birthday.bmp")
    card1.click()
    card2.click(modifiers=["Control"])

    page.locator("#batchDeleteBtn").click()
    modal = page.locator("#deleteMediaModal")
    expect(modal).to_be_visible()

    page.locator("#deleteFromDiskCheckbox").check()
    page.locator("#confirmDeleteMediaBtn").click()

    expect(modal).to_be_hidden()
    expect(page.locator(".photo-card", has_text="sunset.bmp")).to_have_count(0)
    expect(page.locator(".photo-card", has_text="birthday.bmp")).to_have_count(0)
    expect(page.locator(".photo-card")).to_have_count(4)

    assert not os.path.exists(sunset_path)
    assert not os.path.exists(birthday_path)

