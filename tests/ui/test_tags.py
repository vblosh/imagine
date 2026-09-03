"""
Deterministic UI tests for Keyword Tags management, categories accordion, and filtering.
"""

from playwright.sync_api import Page, expect


def test_tag_category_accordion_toggle(server, page: Page):
    """Clicking category header collapses and expands the tag items list."""
    page.goto(server["url"])

    people_group = page.locator('.tag-category-group[data-category="people"]')
    header = people_group.locator(".category-header")
    items = people_group.locator(".tag-items")
    arrow = people_group.locator(".arrow")

    # Initially items are visible, arrow is ▼
    expect(items).to_be_visible()
    expect(arrow).to_have_text("▼")

    # Click header to collapse
    header.click()
    expect(items).to_be_hidden()
    expect(arrow).to_have_text("▶")

    # Click header to expand again
    header.click()
    expect(items).to_be_visible()
    expect(arrow).to_have_text("▼")


def test_tags_sidebar_rendering_and_filtering(server, page: Page):
    """Verify pre-seeded tags are rendered in correct categories and clicking a tag filters the media grid."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    # Verify pre-seeded tags in their respective categories
    expect(page.locator("#tagCategoryPeople")).to_contain_text("Alice")
    expect(page.locator("#tagCategoryPeople")).to_contain_text("Bob")
    expect(page.locator("#tagCategoryPlaces")).to_contain_text("Alps")
    expect(page.locator("#tagCategoryPlaces")).to_contain_text("Beach")
    expect(page.locator("#tagCategoryEvents")).to_contain_text("Birthday 2026")
    expect(page.locator("#tagCategoryKeyword")).to_contain_text("Sunset")

    # Click tag "Sunset" (in keyword category)
    sunset_tag = page.locator("#tagCategoryKeyword .tag-item", has_text="Sunset")
    expect(sunset_tag.locator(".count-badge")).to_have_text("2")
    sunset_tag.click()

    # Filtered to 2 photos with Sunset tag: mountain.bmp and sunset.bmp
    expect(page.locator("#filterLabel")).to_contain_text("Tag: Sunset")
    expect(cards).to_have_count(2)
    names = [cards.nth(0).locator(".card-filename").text_content(), cards.nth(1).locator(".card-filename").text_content()]
    assert "mountain.bmp" in names
    assert "sunset.bmp" in names

    # Click again to toggle filter off
    sunset_tag.click()
    expect(cards).to_have_count(6)


def test_create_tag_modal_validation_and_creation(server, page: Page):
    """Test Create Tag modal dialog, validation on empty name, and successful tag creation."""
    page.goto(server["url"])

    modal = page.locator("#newTagModal")
    expect(modal).to_be_hidden()

    # Open modal
    page.locator("#newTagBtn").click()
    expect(modal).to_be_visible()

    # Validation: empty name triggers alert
    dialog_messages = []
    page.on("dialog", lambda dialog: (dialog_messages.append(dialog.message), dialog.accept()))

    page.locator("#createTagSubmitBtn").click()
    assert len(dialog_messages) == 1
    assert "Tag name is required" in dialog_messages[0]
    expect(modal).to_be_visible()

    # Fill valid tag details
    page.locator("#tagNameInput").fill("Skiing")
    page.locator("#tagCategorySelect").select_option("events")
    page.locator("#createTagSubmitBtn").click()

    # Modal closes
    expect(modal).to_be_hidden()

    # Verify tag appears in Events category
    new_tag = page.locator("#tagCategoryEvents .tag-item", has_text="Skiing")
    expect(new_tag).to_be_visible()
    expect(new_tag.locator(".count-badge")).to_have_text("0")


def test_create_tag_modal_cancel_and_backdrop(server, page: Page):
    """Test closing tag modal via cancel button, close button, and backdrop click."""
    page.goto(server["url"])

    modal = page.locator("#newTagModal")

    # 1. Cancel button
    page.locator("#newTagBtn").click()
    expect(modal).to_be_visible()
    page.locator("#cancelTagBtn").click()
    expect(modal).to_be_hidden()

    # 2. Close icon button
    page.locator("#newTagBtn").click()
    expect(modal).to_be_visible()
    page.locator("#closeNewTagModalBtn").click()
    expect(modal).to_be_hidden()

    # 3. Backdrop click
    page.locator("#newTagBtn").click()
    expect(modal).to_be_visible()
    page.locator("#newTagBackdrop").click(position={"x": 10, "y": 10})
    expect(modal).to_be_hidden()
