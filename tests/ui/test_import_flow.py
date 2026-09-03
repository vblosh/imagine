"""
Deterministic UI tests for the Import flow.
"""

from playwright.sync_api import Page, expect


def test_import_modal_open_close_cancel_and_backdrop(empty_server, page: Page):
    """Test opening import modal, and closing it via Cancel button and backdrop click."""
    page.goto(empty_server["url"])

    import_modal = page.locator("#importModal")
    expect(import_modal).to_be_hidden()

    # 1. Open via top bar button
    page.locator("#importBtn").click()
    expect(import_modal).to_be_visible()

    # 2. Close via Cancel button
    page.locator("#cancelImportBtn").click()
    expect(import_modal).to_be_hidden()

    # 3. Open again and close via backdrop click
    page.locator("#importBtn").click()
    expect(import_modal).to_be_visible()
    page.locator("#importBackdrop").click(position={"x": 10, "y": 10})
    expect(import_modal).to_be_hidden()


def test_import_validation_empty_path(empty_server, page: Page):
    """Submitting empty directory path shows an alert dialog."""
    page.goto(empty_server["url"])

    dialog_message = []
    page.on("dialog", lambda dialog: (dialog_message.append(dialog.message), dialog.accept()))

    page.locator("#importBtn").click()
    expect(page.locator("#importModal")).to_be_visible()

    # Clear input and click Start Import
    page.locator("#importPathInput").fill("")
    page.locator("#startImportBtn").click()

    # Alert should have fired
    assert len(dialog_message) == 1
    assert "Please provide a directory path" in dialog_message[0]
    expect(page.locator("#importModal")).to_be_visible()


def test_successful_folder_import(empty_server, page: Page):
    """Importing photos from a valid folder updates progress, closes modal, and renders cards in grid."""
    page.goto(empty_server["url"])

    import_dir = empty_server["env"]["import_dir"]

    # Open import modal
    page.locator("#importBtn").click()
    expect(page.locator("#importModal")).to_be_visible()

    # Fill directory path
    page.locator("#importPathInput").fill(import_dir)

    # Click start import
    page.locator("#startImportBtn").click()

    # Progress box appears
    expect(page.locator("#importProgressBox")).to_be_visible()

    # Modal auto-closes after completion
    expect(page.locator("#importModal")).to_be_hidden(timeout=10000)

    # Media grid now contains the 2 imported photos
    expect(page.locator(".photo-card")).to_have_count(2)

    # Total count updates
    expect(page.locator("#totalMediaCount")).to_have_text("2")

    # Empty state is hidden
    expect(page.locator("#emptyState")).to_be_hidden()


def test_import_invalid_directory_path(empty_server, page: Page):
    """Submitting a non-existent directory path displays an error alert and restores import UI state."""
    page.goto(empty_server["url"])

    page.locator("#importBtn").click()
    expect(page.locator("#importModal")).to_be_visible()

    # Enter non-existent directory path
    page.locator("#importPathInput").fill("/path/that/does/not/exist/for/import_test_xyz")

    with page.expect_event("dialog") as dialog_info:
        page.locator("#startImportBtn").click()

    dialog = dialog_info.value
    assert "Import error" in dialog.message
    dialog.accept()

    # Modal remains open, Start Import button re-enabled, progress box hidden
    expect(page.locator("#importModal")).to_be_visible()
    expect(page.locator("#startImportBtn")).to_be_enabled()
    expect(page.locator("#importProgressBox")).to_be_hidden()

    # Close modal
    page.locator("#cancelImportBtn").click()
    expect(page.locator("#importModal")).to_be_hidden()

