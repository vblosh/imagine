"""
Deterministic UI tests for Section 3.3 Accessibility (a11y):
1. Modal Dialog Semantics & Focus Trapping (web/modals.js)
2. Keyboard Roving Tabindex in Media Grid (web/media-grid.js)
"""

from playwright.sync_api import Page, expect


def test_modal_dialog_semantics(server, page: Page):
    """Verify that all modal dialogs have role='dialog', aria-modal='true', and aria-labelledby."""
    page.goto(server["url"])

    modal_ids = [
        "importModal",
        "settingsModal",
        "newAlbumModal",
        "addToAlbumModal",
        "newTagModal",
        "deleteMediaModal",
        "deleteTagModal",
        "batchDateModal",
        "batchMoveModal",
        "iosInstallModal",
        "semanticIndexModal",
    ]

    for mid in modal_ids:
        modal = page.locator(f"#{mid}")
        expect(modal).to_have_attribute("role", "dialog")
        expect(modal).to_have_attribute("aria-modal", "true")
        labelled_by = modal.get_attribute("aria-labelledby")
        assert labelled_by, f"Modal #{mid} is missing aria-labelledby"
        heading = page.locator(f"#{labelled_by}")
        expect(heading).to_have_count(1)


def test_modal_focus_trapping_and_restoration(server, page: Page):
    """Verify that opening a modal traps Tab/Shift+Tab focus inside it and restores focus on close."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    # Focus and select the first photo card
    first_card = cards.first
    first_card.click()
    first_card.focus()

    # Open Settings Modal
    page.locator("#settingsBtn").click()
    settings_modal = page.locator("#settingsModal")
    expect(settings_modal).to_be_visible()

    # Verify focus moved into the modal
    focused_id = page.evaluate("() => document.activeElement ? document.activeElement.id : null")
    assert focused_id in ["settingsApiTokenInput", "closeSettingsModalBtn", "settingsForm"], f"Unexpected focused element: {focused_id}"

    # Cycle focus through all focusable elements in Settings modal
    # Focusable in settingsModal: closeSettingsModalBtn, settingsApiTokenInput, clearApiTokenBtn, cancelSettingsBtn, saveSettingsBtn
    save_btn = page.locator("#saveSettingsBtn")
    save_btn.focus()
    expect(save_btn).to_be_focused()

    # Press Tab from the last button -> focus must wrap to the first focusable element inside the modal
    page.keyboard.press("Tab")
    is_inside_modal = page.evaluate("() => document.getElementById('settingsModal').contains(document.activeElement)")
    assert is_inside_modal, "Tab focus escaped the modal dialog!"

    # Verify focus is not in background media grid
    is_in_grid = page.evaluate("() => document.getElementById('mediaGrid').contains(document.activeElement)")
    assert not is_in_grid, "Focus leaked into the background grid while modal was active!"

    # Now focus the first element in modal and press Shift+Tab -> must wrap to the last element
    close_btn = page.locator("#closeSettingsModalBtn")
    close_btn.focus()
    page.keyboard.press("Shift+Tab")

    is_inside_modal_shift = page.evaluate("() => document.getElementById('settingsModal').contains(document.activeElement)")
    assert is_inside_modal_shift, "Shift+Tab focus escaped the modal dialog!"
    expect(save_btn).to_be_focused()

    # Close the modal via Escape key
    page.keyboard.press("Escape")
    expect(settings_modal).to_be_hidden()

    # Verify focus is restored to the selected photo card
    expect(first_card).to_be_focused()


def test_delete_modal_focus_trapping_and_restoration(server, page: Page):
    """Verify focus trapping and focus restoration for Delete Media modal."""
    page.goto(server["url"])

    second_card = page.locator(".photo-card").nth(1)
    second_card.click()
    second_card.focus()
    expect(second_card).to_be_focused()

    # Scenario 1: Open Delete modal via keyboard shortcut (Delete key) while photo card is focused
    page.keyboard.press("Delete")
    delete_modal = page.locator("#deleteMediaModal")
    expect(delete_modal).to_be_visible()

    # Focus is inside modal
    is_inside = page.evaluate("() => document.getElementById('deleteMediaModal').contains(document.activeElement)")
    assert is_inside, "Focus is not inside deleteMediaModal on open"

    # Tab through focusable elements inside the modal
    for _ in range(5):
        page.keyboard.press("Tab")
        is_still_inside = page.evaluate("() => document.getElementById('deleteMediaModal').contains(document.activeElement)")
        assert is_still_inside, "Tab focus escaped deleteMediaModal"

    # Close modal via Escape
    page.keyboard.press("Escape")
    expect(delete_modal).to_be_hidden()

    # Focus should return to the second card because it was active when Delete was pressed
    expect(second_card).to_be_focused()

    # Scenario 2: Open Delete modal via inspector delete button
    inspector_del_btn = page.locator("#inspectorDeleteBtn")
    inspector_del_btn.click()
    expect(delete_modal).to_be_visible()

    # Close via Cancel button
    page.locator("#cancelDeleteMediaBtn").click()
    expect(delete_modal).to_be_hidden()

    # Focus returns to the selected card
    expect(second_card).to_be_focused()


def test_media_grid_roving_tabindex_structure(server, page: Page):
    """Verify that photo cards have article role, aria-selected, aria-label, and roving tabindex."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    # All cards should have role="article"
    for i in range(6):
        card = cards.nth(i)
        expect(card).to_have_attribute("role", "article")
        expect(card).to_have_attribute("aria-selected", "false")
        label = card.get_attribute("aria-label")
        assert label, f"Card {i} is missing aria-label"

    # Exactly one card has tabindex="0", and the rest have tabindex="-1"
    tabindex_0_cards = page.locator(".photo-card[tabindex='0']")
    tabindex_neg1_cards = page.locator(".photo-card[tabindex='-1']")
    expect(tabindex_0_cards).to_have_count(1)
    expect(tabindex_neg1_cards).to_have_count(5)


def test_media_grid_arrow_navigation_roving_tabindex(server, page: Page):
    """Verify arrow key navigation (Right, Left, Down, Up, Home, End) updates focus and roving tabindex."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    # Focus the first card
    cards.first.click()
    cards.first.focus()
    expect(cards.first).to_be_focused()
    expect(cards.first).to_have_attribute("tabindex", "0")
    expect(cards.first).to_have_attribute("aria-selected", "true")

    # ArrowRight moves focus to card 1 (second card)
    page.keyboard.press("ArrowRight")
    expect(cards.nth(1)).to_be_focused()
    expect(cards.nth(1)).to_have_attribute("tabindex", "0")
    expect(cards.first).to_have_attribute("tabindex", "-1")
    expect(cards.nth(1)).to_have_attribute("aria-selected", "true")
    expect(cards.first).to_have_attribute("aria-selected", "false")

    # ArrowLeft moves focus back to card 0 (first card)
    page.keyboard.press("ArrowLeft")
    expect(cards.first).to_be_focused()
    expect(cards.first).to_have_attribute("tabindex", "0")
    expect(cards.nth(1)).to_have_attribute("tabindex", "-1")

    # End moves focus to the last card (index 5)
    page.keyboard.press("End")
    expect(cards.nth(5)).to_be_focused()
    expect(cards.nth(5)).to_have_attribute("tabindex", "0")
    expect(cards.nth(5)).to_have_attribute("aria-selected", "true")

    # Home moves focus back to the first card (index 0)
    page.keyboard.press("Home")
    expect(cards.first).to_be_focused()
    expect(cards.first).to_have_attribute("tabindex", "0")

    # ArrowDown moves focus down to a subsequent row card
    page.keyboard.press("ArrowDown")
    down_focused = page.evaluate("() => Array.from(document.querySelectorAll('.photo-card')).indexOf(document.activeElement)")
    assert down_focused > 0, f"ArrowDown did not navigate forward: index {down_focused}"
    expect(cards.nth(down_focused)).to_have_attribute("tabindex", "0")

    # ArrowUp moves focus back up
    page.keyboard.press("ArrowUp")
    up_focused = page.evaluate("() => Array.from(document.querySelectorAll('.photo-card')).indexOf(document.activeElement)")
    assert up_focused < down_focused, f"ArrowUp did not navigate back: index {up_focused}"


def test_media_grid_ctrl_and_shift_arrow_navigation(server, page: Page):
    """Verify Ctrl+Arrow moves focus without changing selection, and Shift+Arrow extends range selection."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    # 1. Select the first card
    cards.first.click()
    expect(cards.first).to_have_attribute("aria-selected", "true")
    expect(cards.first).to_have_attribute("tabindex", "0")

    # 2. Control+ArrowRight moves focus only, preserving first card selection
    page.keyboard.press("Control+ArrowRight")
    expect(cards.nth(1)).to_be_focused()
    expect(cards.nth(1)).to_have_attribute("tabindex", "0")
    expect(cards.first).to_have_attribute("aria-selected", "true")
    expect(cards.nth(1)).to_have_attribute("aria-selected", "false")

    # 3. Focus back on card 0
    cards.first.click()
    expect(cards.first).to_be_focused()

    # 4. Shift+ArrowRight extends selection to include both card 0 and card 1
    page.keyboard.press("Shift+ArrowRight")
    expect(cards.nth(1)).to_be_focused()
    expect(cards.first).to_have_attribute("aria-selected", "true")
    expect(cards.nth(1)).to_have_attribute("aria-selected", "true")
