"""
Tests for UI State Persistence (Option A: Layout & View Preferences, Option B: Navigation & Filter State).
Verifies that user layout choices, view modes, tabs, and filters persist across page reloads via localStorage.
"""

import re
import pytest
from playwright.sync_api import Page, expect


def test_folders_and_tag_categories_collapse_persistence(server, page: Page):
    """Folders and tag category expanded/collapsed states persist across page reloads."""
    page.goto(server["url"])
    expect(page.locator("#mediaGrid")).to_be_visible()

    folders_tree = page.locator("#foldersTree")
    folders_header = page.locator("#foldersHeader")
    people_group = page.locator('.tag-category-group[data-category="people"]')
    people_header = people_group.locator(".category-header")
    people_items = people_group.locator(".tag-items")
    keyword_group = page.locator('.tag-category-group[data-category="keyword"]')
    keyword_header = keyword_group.locator(".category-header")
    keyword_items = keyword_group.locator(".tag-items")

    # Initial default: all collapsed
    expect(folders_tree).to_be_hidden()
    expect(people_items).to_be_hidden()
    expect(keyword_items).to_be_hidden()

    # Expand Folders and People
    folders_header.click()
    expect(folders_tree).to_be_visible()
    people_header.click()
    expect(people_items).to_be_visible()

    # Reload page
    page.reload()
    expect(page.locator("#mediaGrid")).to_be_visible()

    # Verify Folders and People stayed expanded, while Keyword remained collapsed
    reloaded_folders = page.locator("#foldersTree")
    reloaded_people_items = page.locator('.tag-category-group[data-category="people"] .tag-items')
    reloaded_keyword_items = page.locator('.tag-category-group[data-category="keyword"] .tag-items')

    expect(reloaded_folders).to_be_visible()
    expect(reloaded_people_items).to_be_visible()
    expect(reloaded_keyword_items).to_be_hidden()

    # Now collapse Folders and reload
    page.locator("#foldersHeader").click()
    expect(reloaded_folders).to_be_hidden()

    page.reload()
    expect(page.locator("#foldersTree")).to_be_hidden()
    expect(page.locator('.tag-category-group[data-category="people"] .tag-items')).to_be_visible()


def test_zoom_slider_persistence(server, page: Page):
    """Thumbnail zoom level persists across reloads and updates --thumb-size CSS variable."""
    page.goto(server["url"])
    expect(page.locator("#mediaGrid")).to_be_visible()

    zoom_slider = page.locator("#zoomSlider")
    # Change zoom to 260px
    page.evaluate("""() => {
        const slider = document.getElementById('zoomSlider');
        slider.value = '260';
        slider.dispatchEvent(new Event('input'));
    }""")

    stored_zoom = page.evaluate("() => localStorage.getItem('imagine_thumb_zoom')")
    assert stored_zoom == "260"

    # Reload page
    page.reload()
    expect(page.locator("#mediaGrid")).to_be_visible()

    reloaded_slider_val = page.locator("#zoomSlider").input_value()
    assert reloaded_slider_val == "260"
    thumb_size = page.evaluate("() => getComputedStyle(document.documentElement).getPropertyValue('--thumb-size').trim()")
    assert thumb_size == "260px"


def test_sort_preference_persistence(server, page: Page):
    """Main grid sort preference persists across reloads and correctly sorts media items."""
    page.goto(server["url"])
    expect(page.locator("#mediaGrid")).to_be_visible()

    sort_select = page.locator("#sortSelect")
    # Change sort to rating descending (highest rated first)
    sort_select.select_option("rating-desc")

    stored_sort = page.evaluate("() => localStorage.getItem('imagine_main_sort')")
    assert stored_sort == "rating-desc"

    # Reload page
    page.reload()
    expect(page.locator("#mediaGrid")).to_be_visible()

    reloaded_sort = page.locator("#sortSelect").input_value()
    assert reloaded_sort == "rating-desc"

    state_sort = page.evaluate("() => ({ sortBy: window._imagineState.sortBy, sortDesc: window._imagineState.sortDesc })")
    assert state_sort["sortBy"] == "rating"
    assert state_sort["sortDesc"] is True


def test_inspector_collapsed_state_persistence(server, page: Page):
    """Inspector collapsed state persists across reloads."""
    page.goto(server["url"])
    expect(page.locator("#mediaGrid")).to_be_visible()

    inspector = page.locator("#rightInspector")
    toggle_btn = page.locator("#toggleInspectorBtn")

    # Initially inspector is open (not collapsed)
    expect(inspector).not_to_have_class(re.compile(r"\bcollapsed\b"))

    # Collapse inspector
    toggle_btn.click()
    expect(inspector).to_have_class(re.compile(r"\bcollapsed\b"))
    stored_val = page.evaluate("() => localStorage.getItem('imagine_inspector_collapsed')")
    assert stored_val == "true"

    # Reload page
    page.reload()
    reloaded_inspector = page.locator("#rightInspector")
    expect(reloaded_inspector).to_have_class(re.compile(r"\bcollapsed\b"))

    # Expand again
    page.locator("#toggleInspectorBtn").click()
    expect(reloaded_inspector).not_to_have_class(re.compile(r"\bcollapsed\b"))

    page.reload()
    expect(page.locator("#rightInspector")).not_to_have_class(re.compile(r"\bcollapsed\b"))


def test_view_mode_map_persistence(server, page: Page):
    """Map view mode persists across reloads."""
    page.goto(server["url"])
    expect(page.locator("#mediaGrid")).to_be_visible()

    # Switch to map view
    page.locator("#viewMapBtn").click()
    expect(page.locator("#mapViewContainer")).to_be_visible()
    expect(page.locator("#viewMapBtn")).to_have_class(re.compile(r"\bactive\b"))

    stored_mode = page.evaluate("() => localStorage.getItem('imagine_view_mode')")
    assert stored_mode == "map"

    # Reload page
    page.reload()
    expect(page.locator("#mapViewContainer")).to_be_visible()
    expect(page.locator("#viewMapBtn")).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#gridScrollContainer")).to_be_hidden()

    # Switch back to grid
    page.locator("#viewGridBtn").click()
    expect(page.locator("#gridScrollContainer")).to_be_visible()

    page.reload()
    expect(page.locator("#gridScrollContainer")).to_be_visible()
    expect(page.locator("#viewGridBtn")).to_have_class(re.compile(r"\bactive\b"))


def test_active_tab_persistence(server, page: Page):
    """Active tab selection (e.g. People, Places, Events) persists across reloads."""
    page.goto(server["url"])
    expect(page.locator("#mediaGrid")).to_be_visible()

    # Switch to People tab
    page.locator('.tab-btn[data-tab="people"]').click()
    expect(page.locator("#categoryViewContainer")).to_be_visible()
    expect(page.locator('.tab-btn[data-tab="people"]')).to_have_class(re.compile(r"\bactive\b"))

    stored_tab = page.evaluate("() => localStorage.getItem('imagine_active_tab')")
    assert stored_tab == "people"

    # Reload page
    page.reload()
    expect(page.locator("#categoryViewContainer")).to_be_visible()
    expect(page.locator('.tab-btn[data-tab="people"]')).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator('.tab-btn[data-tab="media"]')).not_to_have_class(re.compile(r"\bactive\b"))

    # People category cards must be visible
    expect(page.locator(".category-card-item", has_text="Alice")).to_be_visible()


def test_nav_filter_and_search_persistence(server, page: Page):
    """Sidebar nav status filter (e.g. Picks) and search text persist across reloads."""
    page.goto(server["url"])
    expect(page.locator("#mediaGrid")).to_be_visible()

    # Click Picks filter (2 pick items in test fixtures)
    page.locator("#navPicks").click()
    expect(page.locator("#navPicks")).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator(".photo-card")).to_have_count(2)

    # Reload page
    page.reload()
    expect(page.locator("#navPicks")).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator(".photo-card")).to_have_count(2)

    # Clear via All Media
    page.locator("#navAllMedia").click()
    expect(page.locator("#navPicks")).not_to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator(".photo-card")).to_have_count(6)

    # Test search persistence
    search_input = page.locator("#searchInput")
    search_input.fill("mountain")
    page.wait_for_timeout(350)
    expect(page.locator(".photo-card")).to_have_count(1)

    page.reload()
    reloaded_input = page.locator("#searchInput")
    expect(reloaded_input).to_have_value("mountain")
    expect(page.locator(".photo-card")).to_have_count(1)

    # Clear search
    page.locator("#clearSearchBtn").click()
    expect(page.locator(".photo-card")).to_have_count(6)


def test_folder_selection_persistence_and_clear_filters(server, page: Page):
    """Active folder selection persists across reloads and clearAllFilters resets it."""
    page.goto(server["url"])
    expect(page.locator("#mediaGrid")).to_be_visible()

    # Expand folders and click 'family' folder (contains 2 photos)
    page.locator("#foldersHeader").click()
    family_folder = page.locator("#foldersTree .folder-item", has_text="family")
    family_folder.click()
    expect(family_folder).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator(".photo-card")).to_have_count(2)

    # Reload page
    page.reload()
    expect(page.locator(".photo-card")).to_have_count(2)
    # Folders section should be auto-expanded because of active folder
    reloaded_family = page.locator("#foldersTree .folder-item", has_text="family")
    expect(reloaded_family).to_be_visible()
    expect(reloaded_family).to_have_class(re.compile(r"\bactive\b"))

    # Clear filters button resets persisted filter
    page.locator("#clearFiltersBtn").click()
    expect(page.locator(".photo-card")).to_have_count(6)
    expect(reloaded_family).not_to_have_class(re.compile(r"\bactive\b"))

    # Reload again to verify clean state
    page.reload()
    expect(page.locator(".photo-card")).to_have_count(6)
