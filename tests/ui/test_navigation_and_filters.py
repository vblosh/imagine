"""
Deterministic UI tests for Navigation, Sidebar Quick Filters, Tabs, and Folder Tree.
"""

import re
from playwright.sync_api import Page, expect


def test_sidebar_quick_filters(server, page: Page):
    """Test All Media, Picks, Rejects, and Unrated sidebar filters."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    # 1. Picks filter (flag = 1: mountain.bmp, portrait.bmp)
    page.locator("#navPicks").click()
    expect(page.locator("#navPicks")).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#filterLabel")).to_contain_text("Picks")
    expect(cards).to_have_count(2)
    names = [cards.nth(0).locator(".card-filename").text_content(), cards.nth(1).locator(".card-filename").text_content()]
    assert "mountain.bmp" in names
    assert "portrait.bmp" in names

    # 2. Rejects filter (flag = -1: beach.bmp, forest.bmp)
    page.locator("#navRejects").click()
    expect(page.locator("#navRejects")).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#filterLabel")).to_contain_text("Rejects")
    expect(cards).to_have_count(2)
    names = [cards.nth(0).locator(".card-filename").text_content(), cards.nth(1).locator(".card-filename").text_content()]
    assert "beach.bmp" in names
    assert "forest.bmp" in names

    # 3. Unrated filter (rating = 0: birthday.bmp, forest.bmp)
    page.locator("#navUnrated").click()
    expect(page.locator("#navUnrated")).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#filterLabel")).to_contain_text("Unrated Photos")
    expect(cards).to_have_count(2)
    names = [cards.nth(0).locator(".card-filename").text_content(), cards.nth(1).locator(".card-filename").text_content()]
    assert "birthday.bmp" in names
    assert "forest.bmp" in names

    # 4. Return to All Media
    page.locator("#navAllMedia").click()
    expect(page.locator("#navAllMedia")).to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(6)


def test_view_tabs_category_filtering(server, page: Page):
    """Switching top-bar view tabs (Media, People, Places, Events) filters by tag category."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    # 1. Switch to People tab
    page.locator('.tab-btn[data-tab="people"]').click()
    expect(page.locator('.tab-btn[data-tab="people"]')).to_have_class(re.compile(r"\bactive\b"))
    # People category contains both birthday.bmp (Alice) and portrait.bmp (Bob)
    expect(cards).to_have_count(2)
    people_names = [cards.nth(0).locator(".card-filename").text_content(), cards.nth(1).locator(".card-filename").text_content()]
    assert "birthday.bmp" in people_names
    assert "portrait.bmp" in people_names

    # 2. Switch to Places tab
    page.locator('.tab-btn[data-tab="places"]').click()
    expect(page.locator('.tab-btn[data-tab="places"]')).to_have_class(re.compile(r"\bactive\b"))
    # Places category contains both mountain.bmp (Alps) and beach.bmp (Beach)
    expect(cards).to_have_count(2)
    places_names = [cards.nth(0).locator(".card-filename").text_content(), cards.nth(1).locator(".card-filename").text_content()]
    assert "mountain.bmp" in places_names
    assert "beach.bmp" in places_names

    # 3. Switch to Events tab
    page.locator('.tab-btn[data-tab="events"]').click()
    expect(page.locator('.tab-btn[data-tab="events"]')).to_have_class(re.compile(r"\bactive\b"))
    # Events tag is "Birthday 2026", tagged on birthday.bmp
    expect(cards).to_have_count(1)
    expect(cards.first.locator(".card-filename")).to_have_text("birthday.bmp")

    # 4. Switch back to Media tab
    page.locator('.tab-btn[data-tab="media"]').click()
    expect(page.locator('.tab-btn[data-tab="media"]')).to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(6)


def test_clear_filters_button(server, page: Page):
    """When a filter is active, clicking #clearFiltersBtn resets the filter."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)
    clear_btn = page.locator("#clearFiltersBtn")
    expect(clear_btn).to_be_hidden()

    # Apply Picks filter
    page.locator("#navPicks").click()
    expect(clear_btn).to_be_visible()
    expect(cards).to_have_count(2)

    # Click Clear Filter
    clear_btn.click()
    expect(clear_btn).to_be_hidden()
    expect(cards).to_have_count(6)


def test_folder_tree_navigation(server, page: Page):
    """Clicking a folder item in the sidebar filters media grid, marks it active, toggles off, and preserves other folders."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    folders = page.locator("#foldersTree .folder-item")
    expect(folders).to_have_count(2)

    # Find folder ending with 'family' and 'nature'
    family_folder = page.locator("#foldersTree .folder-item", has_text="family")
    nature_folder = page.locator("#foldersTree .folder-item", has_text="nature")
    expect(family_folder).to_be_visible()
    expect(nature_folder).to_be_visible()

    # 1. Click family folder
    family_folder.click()
    expect(family_folder).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#filterLabel")).to_contain_text("Folder: family")
    # Family contains 2 items: birthday.bmp and portrait.bmp
    expect(cards).to_have_count(2)
    names = [cards.nth(0).locator(".card-filename").text_content(), cards.nth(1).locator(".card-filename").text_content()]
    assert "birthday.bmp" in names
    assert "portrait.bmp" in names

    # Crucial: verify nature folder did NOT disappear from sidebar tree
    expect(folders).to_have_count(2)
    expect(nature_folder).to_be_visible()

    # 2. Click family folder again to toggle off
    family_folder.click()
    expect(family_folder).not_to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(6)

    # 3. Click nature folder and clear via clear filter button
    nature_folder.click()
    expect(nature_folder).to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(4)

    page.locator("#clearFiltersBtn").click()
    expect(nature_folder).not_to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(6)
