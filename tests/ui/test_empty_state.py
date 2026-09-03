"""
Deterministic UI tests for Empty State features.
"""

from playwright.sync_api import Page, expect


def test_empty_catalog_ui_state(empty_server, page: Page):
    """Verify that an empty catalog correctly renders the empty state UI."""
    page.goto(empty_server["url"])

    # Empty state container is visible
    empty_state = page.locator("#emptyState")
    expect(empty_state).to_be_visible()

    # Empty state header and description
    expect(page.locator("#emptyState h3")).to_have_text("No Photos Found")
    expect(page.locator("#emptyState p")).to_contain_text("Import photos from a folder on your computer")

    # Total media count badge is 0
    expect(page.locator("#totalMediaCount")).to_have_text("0")

    # Inspector shows no-selection prompt
    expect(page.locator("#inspectorNoSelection")).to_be_visible()
    expect(page.locator("#inspectorSelection")).to_be_hidden()

    # Timeline container shows no timeline data
    expect(page.locator("#timelineContainer")).to_contain_text("No timeline data")

    # Albums list shows empty message
    expect(page.locator("#albumsList")).to_contain_text("No albums")

    # Folders tree shows empty message
    expect(page.locator("#foldersTree")).to_contain_text("No folders")


def test_empty_state_import_button_opens_modal(empty_server, page: Page):
    """Clicking 'Import Folder' inside the empty state opens the import modal dialog."""
    page.goto(empty_server["url"])

    import_modal = page.locator("#importModal")
    expect(import_modal).to_be_hidden()

    # Click the empty state import button
    page.locator("#emptyImportBtn").click()

    # Modal dialog is now visible
    expect(import_modal).to_be_visible()
    expect(page.locator("#importPathInput")).to_be_visible()

    # Close modal via close button
    page.locator("#closeImportModalBtn").click()
    expect(import_modal).to_be_hidden()
