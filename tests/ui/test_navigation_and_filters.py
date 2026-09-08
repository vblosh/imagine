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

    # Verify navigation count badges
    expect(page.locator("#totalMediaCount")).to_have_text("6")
    expect(page.locator("#totalPicksCount")).to_have_text("2")
    expect(page.locator("#totalRejectsCount")).to_have_text("2")
    expect(page.locator("#totalNotRejectsCount")).to_have_text("4")
    expect(page.locator("#totalUnratedCount")).to_have_text("2")

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

    # 3. Not Rejects filter (flag != -1: mountain.bmp, sunset.bmp, portrait.bmp, birthday.bmp)
    page.locator("#navNotRejects").click()
    expect(page.locator("#navNotRejects")).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#filterLabel")).to_contain_text("Not Rejects")
    expect(cards).to_have_count(4)
    not_reject_names = [cards.nth(i).locator(".card-filename").text_content() for i in range(4)]
    assert "mountain.bmp" in not_reject_names
    assert "sunset.bmp" in not_reject_names
    assert "portrait.bmp" in not_reject_names
    assert "birthday.bmp" in not_reject_names
    assert "beach.bmp" not in not_reject_names
    assert "forest.bmp" not in not_reject_names

    # 4. Unrated filter (rating = 0: birthday.bmp, forest.bmp)
    page.locator("#navUnrated").click()
    expect(page.locator("#navUnrated")).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#filterLabel")).to_contain_text("Unrated Photos")
    expect(cards).to_have_count(2)
    names = [cards.nth(0).locator(".card-filename").text_content(), cards.nth(1).locator(".card-filename").text_content()]
    assert "birthday.bmp" in names
    assert "forest.bmp" in names

    # 5. Return to All Media
    page.locator("#navAllMedia").click()
    expect(page.locator("#navAllMedia")).to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(6)


def test_view_tabs_category_filtering(server, page: Page):
    """Clicking top tabs (People, Places, Events) opens grid with photo and name; clicking photo selects and shows photos."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    category_view = page.locator("#categoryViewContainer")
    category_cards = page.locator(".category-card-item")
    back_btn = page.locator("#categoryBackBtn")

    # 1. Switch to People tab -> opens People Grid with cards for Alice and Bob
    page.locator('.tab-btn[data-tab="people"]').click()
    expect(page.locator('.tab-btn[data-tab="people"]')).to_have_class(re.compile(r"\bactive\b"))
    expect(category_view).to_be_visible()
    expect(category_cards).to_have_count(2)

    alice_card = page.locator('.category-card-item', has_text="Alice")
    bob_card = page.locator('.category-card-item', has_text="Bob")
    expect(alice_card).to_be_visible()
    expect(bob_card).to_be_visible()
    # Check that each card has photo thumbnail and name
    expect(alice_card.locator(".category-card-name")).to_have_text("Alice")
    expect(bob_card.locator(".category-card-name")).to_have_text("Bob")

    # Click on Alice's card/photo -> selects Alice and shows photos from Alice
    alice_card.click()
    expect(category_view).to_be_hidden()
    expect(cards).to_have_count(1)
    expect(cards.first.locator(".card-filename")).to_have_text("birthday.bmp")
    expect(back_btn).to_be_visible()
    expect(back_btn).to_contain_text("Back to People")

    # Click Back to People -> returns to People grid
    back_btn.click()
    expect(category_view).to_be_visible()
    expect(category_cards).to_have_count(2)

    # 2. Switch to Places tab -> opens Places Grid with cards for Alps and Beach
    page.locator('.tab-btn[data-tab="places"]').click()
    expect(page.locator('.tab-btn[data-tab="places"]')).to_have_class(re.compile(r"\bactive\b"))
    expect(category_view).to_be_visible()
    expect(category_cards).to_have_count(2)

    alps_card = page.locator('.category-card-item', has_text="Alps")
    beach_card = page.locator('.category-card-item', has_text="Beach")
    expect(alps_card).to_be_visible()
    expect(beach_card).to_be_visible()

    # Click on Alps -> shows mountain.bmp
    alps_card.click()
    expect(category_view).to_be_hidden()
    expect(cards).to_have_count(1)
    expect(cards.first.locator(".card-filename")).to_have_text("mountain.bmp")
    expect(back_btn).to_contain_text("Back to Places")

    # Click Places tab on top -> returns to Places grid
    page.locator('.tab-btn[data-tab="places"]').click()
    expect(category_view).to_be_visible()
    expect(category_cards).to_have_count(2)

    # 3. Switch to Events tab -> opens Events Grid with card for Birthday 2026
    page.locator('.tab-btn[data-tab="events"]').click()
    expect(page.locator('.tab-btn[data-tab="events"]')).to_have_class(re.compile(r"\bactive\b"))
    expect(category_view).to_be_visible()
    expect(category_cards).to_have_count(1)

    event_card = page.locator('.category-card-item', has_text="Birthday 2026")
    expect(event_card).to_be_visible()

    # Click on Birthday 2026 -> shows birthday.bmp
    event_card.click()
    expect(category_view).to_be_hidden()
    expect(cards).to_have_count(1)
    expect(cards.first.locator(".card-filename")).to_have_text("birthday.bmp")

    # 4. Switch back to Media tab -> returns to all 6 photos
    page.locator('.tab-btn[data-tab="media"]').click()
    expect(page.locator('.tab-btn[data-tab="media"]')).to_have_class(re.compile(r"\bactive\b"))
    expect(category_view).to_be_hidden()
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

    # Expand folders header
    page.locator("#foldersHeader").click()

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


def test_folders_and_tags_visible_for_all_images_without_scroll(server, page: Page):
    """Verify that all folders and tags across the entire catalog are visible immediately on load, even before scrolling or loading paginated media."""
    # Route media request to only return 1 item (from 'nature' folder) initially, with total=6
    original_route = page.route
    def handle_media(route):
        url = route.request.url
        if "offset=0" in url and "folder=" not in url and "tag_id=" not in url and "search=" not in url:
            # Fetch normal response and slice items to only 1 item
            response = route.fetch()
            data = response.json()
            data["items"] = data["items"][:1]  # Only 1 item (mountain.bmp in 'nature')
            data["total"] = 6
            route.fulfill(response=response, json=data)
        else:
            route.continue_()

    page.route("**/api/media*", handle_media)
    page.goto(server["url"])

    # Grid only has 1 card initially (simulating first page with limit)
    expect(page.locator(".photo-card")).to_have_count(1)

    # BUT the folders sidebar must already show ALL folders (both 'family' and 'nature'), not just 'nature'
    page.locator("#foldersHeader").click()
    folders = page.locator("#foldersTree .folder-item")
    expect(folders).to_have_count(2)
    expect(page.locator("#foldersTree .folder-item", has_text="family")).to_be_visible()
    expect(page.locator("#foldersTree .folder-item", has_text="nature")).to_be_visible()

    # And tags sidebar must show all tags across categories
    expect(page.locator("#tagCategoryPeople")).to_contain_text("Alice")
    expect(page.locator("#tagCategoryPlaces")).to_contain_text("Alps")
    expect(page.locator("#tagCategoryEvents")).to_contain_text("Birthday 2026")
    expect(page.locator("#tagCategoryKeyword")).to_contain_text("Sunset")

    # Clicking 'family' folder (which had 0 photos in the initial 1-photo page) works and loads family photos
    family_folder = page.locator("#foldersTree .folder-item", has_text="family")
    family_folder.click()
    expect(family_folder).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator(".photo-card")).to_have_count(2)

    # Both folders remain visible
    expect(folders).to_have_count(2)
    expect(page.locator("#foldersTree .folder-item", has_text="nature")).to_be_visible()


def test_combine_navigation_filters_albums_and_tags(server, page: Page):
    """Test combining first NAVIGATION filter (Photos/media type) AND another (Picks/flag), albums, and tags with AND logic."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    # 1. Apply first NAVIGATION filter: Photos
    page.locator("#navPhotos").click()
    expect(page.locator("#navPhotos")).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#filterLabel")).to_contain_text("Photos")
    expect(cards).to_have_count(6)

    # 2. Combine with another NAVIGATION filter: Picks (flag = 1)
    # Both navPhotos and navPicks must now be active simultaneously
    page.locator("#navPicks").click()
    expect(page.locator("#navPhotos")).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#navPicks")).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#filterLabel")).to_contain_text("Photos • Picks")
    expect(cards).to_have_count(2)
    names = [cards.nth(0).locator(".card-filename").text_content(), cards.nth(1).locator(".card-filename").text_content()]
    assert "mountain.bmp" in names
    assert "portrait.bmp" in names

    # 3. Combine with Album: Best of 2026 (contains mountain.bmp and sunset.bmp)
    best_album = page.locator("#albumsList .menu-item", has_text="Best of 2026")
    best_album.click()
    expect(page.locator("#navPhotos")).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#navPicks")).to_have_class(re.compile(r"\bactive\b"))
    expect(best_album).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#filterLabel")).to_contain_text("Photos • Picks • Album: Best of 2026")
    # Only mountain.bmp is a Photo, Pick, AND in Best of 2026
    expect(cards).to_have_count(1)
    expect(cards.first.locator(".card-filename")).to_have_text("mountain.bmp")

    # 4. Combine with Tag: Sunset (tagged on mountain.bmp and sunset.bmp)
    page.locator('.tag-category-group[data-category="keyword"] .category-header').click()
    sunset_tag = page.locator("#tagCategoryKeyword .tag-item", has_text="Sunset")
    sunset_tag.click()
    expect(page.locator("#navPhotos")).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#navPicks")).to_have_class(re.compile(r"\bactive\b"))
    expect(best_album).to_have_class(re.compile(r"\bactive\b"))
    expect(sunset_tag).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#filterLabel")).to_contain_text("Photos • Picks • Album: Best of 2026 • Tag: Sunset")
    expect(cards).to_have_count(1)
    expect(cards.first.locator(".card-filename")).to_have_text("mountain.bmp")

    # 5. Independent toggle-off: toggle Tag off
    sunset_tag.click()
    expect(sunset_tag).not_to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#navPhotos")).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#navPicks")).to_have_class(re.compile(r"\bactive\b"))
    expect(best_album).to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(1)

    # Toggle Album off
    best_album.click()
    expect(best_album).not_to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#navPhotos")).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#navPicks")).to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(2)

    # Toggle Picks off
    page.locator("#navPicks").click()
    expect(page.locator("#navPicks")).not_to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#navPhotos")).to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(6)

    # Toggle Photos off -> returns to All Media
    page.locator("#navPhotos").click()
    expect(page.locator("#navPhotos")).not_to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#navAllMedia")).to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(6)

    # 6. Test direct combination of Album + Tag
    vacation_album = page.locator("#albumsList .menu-item", has_text="Vacation")
    vacation_album.click()
    expect(cards).to_have_count(2)  # beach.bmp, birthday.bmp

    page.locator('.tag-category-group[data-category="people"] .category-header').click()
    alice_tag = page.locator("#tagCategoryPeople .tag-item", has_text="Alice")
    alice_tag.click()
    expect(vacation_album).to_have_class(re.compile(r"\bactive\b"))
    expect(alice_tag).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#filterLabel")).to_contain_text("Album: Vacation • Tag: Alice")
    expect(cards).to_have_count(1)
    expect(cards.first.locator(".card-filename")).to_have_text("birthday.bmp")

    # Clear all filters with Clear Filter button
    page.locator("#clearFiltersBtn").click()
    expect(page.locator("#navAllMedia")).to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(6)


def test_ctrl_click_multiple_tags_or_filter(server, page: Page):
    """Test selecting multiple tags across People/Places/Events/Keywords using Ctrl-click filters them with OR logic."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    page.locator('.tag-category-group[data-category="people"] .category-header').click()
    page.locator('.tag-category-group[data-category="places"] .category-header').click()
    alice_tag = page.locator("#tagCategoryPeople .tag-item", has_text="Alice")
    bob_tag = page.locator("#tagCategoryPeople .tag-item", has_text="Bob")
    beach_tag = page.locator("#tagCategoryPlaces .tag-item", has_text="Beach")

    # 1. Click Alice (People)
    alice_tag.click()
    expect(alice_tag).to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(1)
    expect(cards.first.locator(".card-filename")).to_have_text("birthday.bmp")

    # 2. Ctrl-click Bob (People) -> OR filter: Alice OR Bob
    bob_tag.click(modifiers=["Control"])
    expect(alice_tag).to_have_class(re.compile(r"\bactive\b"))
    expect(bob_tag).to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(2)
    names = [cards.nth(0).locator(".card-filename").text_content(), cards.nth(1).locator(".card-filename").text_content()]
    assert "birthday.bmp" in names
    assert "portrait.bmp" in names
    expect(page.locator("#filterLabel")).to_contain_text("Filter (OR)")

    # 3. Ctrl-click Beach (Places) -> OR filter: Alice OR Bob OR Beach
    beach_tag.click(modifiers=["Control"])
    expect(beach_tag).to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(3)
    names = [cards.nth(i).locator(".card-filename").text_content() for i in range(3)]
    assert "birthday.bmp" in names
    assert "portrait.bmp" in names
    assert "beach.bmp" in names

    # 4. Ctrl-click Alice to deselect it -> OR filter: Bob OR Beach
    alice_tag.click(modifiers=["Control"])
    expect(alice_tag).not_to_have_class(re.compile(r"\bactive\b"))
    expect(bob_tag).to_have_class(re.compile(r"\bactive\b"))
    expect(beach_tag).to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(2)
    names = [cards.nth(0).locator(".card-filename").text_content(), cards.nth(1).locator(".card-filename").text_content()]
    assert "portrait.bmp" in names
    assert "beach.bmp" in names


def test_ctrl_click_multiple_folders_or_filter(server, page: Page):
    """Test selecting multiple folders using Ctrl-click filters them with OR logic."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    page.locator("#foldersHeader").click()
    family_folder = page.locator("#foldersTree .folder-item", has_text="family")
    nature_folder = page.locator("#foldersTree .folder-item", has_text="nature")

    # 1. Click family folder
    family_folder.click()
    expect(family_folder).to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(2)

    # 2. Ctrl-click nature folder -> family OR nature (all 6 items)
    nature_folder.click(modifiers=["Control"])
    expect(family_folder).to_have_class(re.compile(r"\bactive\b"))
    expect(nature_folder).to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(6)
    expect(page.locator("#filterLabel")).to_contain_text("Filter (OR)")

    # 3. Ctrl-click family folder to deselect it -> only nature folder active (4 items)
    family_folder.click(modifiers=["Control"])
    expect(family_folder).not_to_have_class(re.compile(r"\bactive\b"))
    expect(nature_folder).to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(4)


def test_ctrl_click_mixed_tags_and_folders_or_filter(server, page: Page):
    """Test selecting tags and folders together using Ctrl-click filters with OR logic."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    page.locator('.tag-category-group[data-category="people"] .category-header').click()
    page.locator("#foldersHeader").click()
    bob_tag = page.locator("#tagCategoryPeople .tag-item", has_text="Bob")
    nature_folder = page.locator("#foldersTree .folder-item", has_text="nature")

    # 1. Click Bob (portrait.bmp)
    bob_tag.click()
    expect(bob_tag).to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(1)
    expect(cards.first.locator(".card-filename")).to_have_text("portrait.bmp")

    # 2. Ctrl-click nature folder -> Bob OR nature folder
    # nature folder has 4 items (mountain, sunset, beach, forest), plus Bob (portrait.bmp) in family folder = 5 items total
    nature_folder.click(modifiers=["Control"])
    expect(bob_tag).to_have_class(re.compile(r"\bactive\b"))
    expect(nature_folder).to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(5)
    names = [cards.nth(i).locator(".card-filename").text_content() for i in range(5)]
    assert "portrait.bmp" in names
    assert "mountain.bmp" in names
    assert "sunset.bmp" in names
    assert "beach.bmp" in names
    assert "forest.bmp" in names
    assert "birthday.bmp" not in names
    expect(page.locator("#filterLabel")).to_contain_text("Filter (OR)")

    # 3. Clear filters button clears both
    page.locator("#clearFiltersBtn").click()
    expect(bob_tag).not_to_have_class(re.compile(r"\bactive\b"))
    expect(nature_folder).not_to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(6)


def test_shift_click_range_selection(server, page: Page):
    """Test Shift-clicking to select a range of items in the sidebar."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    page.locator('.tag-category-group[data-category="people"] .category-header').click()
    alice_tag = page.locator("#tagCategoryPeople .tag-item", has_text="Alice")
    bob_tag = page.locator("#tagCategoryPeople .tag-item", has_text="Bob")

    # 1. Click Alice (first item)
    alice_tag.click()
    expect(alice_tag).to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(1)

    # 2. Shift-click Bob (second item) -> both Alice and Bob selected
    bob_tag.click(modifiers=["Shift"])
    expect(alice_tag).to_have_class(re.compile(r"\bactive\b"))
    expect(bob_tag).to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(2)

    # 3. Normal click on Bob deselects the multi-selection and leaves only Bob
    bob_tag.click()
    expect(alice_tag).not_to_have_class(re.compile(r"\bactive\b"))
    expect(bob_tag).to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(1)
    expect(cards.first.locator(".card-filename")).to_have_text("portrait.bmp")


def test_ctrl_click_across_all_categories_and_folders(server, page: Page):
    """Test selecting items from People, Places, Events, Keywords, and Folders simultaneously using Ctrl-click."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    page.locator('.tag-category-group[data-category="people"] .category-header').click()
    page.locator('.tag-category-group[data-category="places"] .category-header').click()
    page.locator('.tag-category-group[data-category="events"] .category-header').click()
    page.locator('.tag-category-group[data-category="keyword"] .category-header').click()
    page.locator("#foldersHeader").click()

    alice_tag = page.locator("#tagCategoryPeople .tag-item", has_text="Alice")
    beach_tag = page.locator("#tagCategoryPlaces .tag-item", has_text="Beach")
    bday_tag = page.locator("#tagCategoryEvents .tag-item", has_text="Birthday 2026")
    sunset_tag = page.locator("#tagCategoryKeyword .tag-item", has_text="Sunset")
    family_folder = page.locator("#foldersTree .folder-item", has_text="family")

    # Click Alice (People)
    alice_tag.click()
    expect(alice_tag).to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(1)

    # Ctrl-click Beach (Places)
    beach_tag.click(modifiers=["Control"])
    expect(beach_tag).to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(2)  # birthday.bmp, beach.bmp

    # Ctrl-click Birthday 2026 (Events)
    bday_tag.click(modifiers=["Control"])
    expect(bday_tag).to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(2)  # birthday.bmp (has both Alice & Birthday 2026), beach.bmp

    # Ctrl-click Sunset (Keywords)
    sunset_tag.click(modifiers=["Control"])
    expect(sunset_tag).to_have_class(re.compile(r"\bactive\b"))
    # Sunset is on mountain.bmp and sunset.bmp -> total now 4 (birthday, beach, mountain, sunset)
    expect(cards).to_have_count(4)

    # Ctrl-click family folder
    family_folder.click(modifiers=["Control"])
    expect(family_folder).to_have_class(re.compile(r"\bactive\b"))
    # family folder adds portrait.bmp -> total now 5
    expect(cards).to_have_count(5)

    # All selected elements are highlighted active
    expect(alice_tag).to_have_class(re.compile(r"\bactive\b"))
    expect(beach_tag).to_have_class(re.compile(r"\bactive\b"))
    expect(bday_tag).to_have_class(re.compile(r"\bactive\b"))
    expect(sunset_tag).to_have_class(re.compile(r"\bactive\b"))
    expect(family_folder).to_have_class(re.compile(r"\bactive\b"))

    expect(page.locator("#filterLabel")).to_contain_text("Filter (OR)")

    # Clear filters button clears all
    page.locator("#clearFiltersBtn").click()
    expect(cards).to_have_count(6)
    expect(alice_tag).not_to_have_class(re.compile(r"\bactive\b"))
    expect(beach_tag).not_to_have_class(re.compile(r"\bactive\b"))
    expect(bday_tag).not_to_have_class(re.compile(r"\bactive\b"))
    expect(sunset_tag).not_to_have_class(re.compile(r"\bactive\b"))
    expect(family_folder).not_to_have_class(re.compile(r"\bactive\b"))


def test_long_folder_name_icon_does_not_disappear(server, page: Page):
    """Verify that for long folder names like 'Deutsche orden Schloß', the folder icon does not disappear."""
    def handle_folders(route):
        route.fulfill(json=["family", "nature", "Deutsche orden Schloß", "A very long folder name that overflows the left panel width"])

    page.route("**/api/folders*", handle_folders)
    page.goto(server["url"])

    expect(page.locator(".photo-card")).to_have_count(6)

    page.locator("#foldersHeader").click()
    folder_item = page.locator("#foldersTree .folder-item", has_text="Deutsche orden Schloß")
    expect(folder_item).to_be_visible()

    # The folder icon (SVG) must be visible and have non-zero dimensions (~13px)
    icon = folder_item.locator("svg")
    expect(icon).to_be_visible()
    bb = icon.bounding_box()
    assert bb is not None, "Folder icon bounding box should exist"
    assert bb["width"] >= 12, f"Folder icon width should be >= 12px, got {bb['width']}"
    assert bb["height"] >= 12, f"Folder icon height should be >= 12px, got {bb['height']}"

    # Even if sidebar is made narrow (160px), folder icon must still be visible and not disappear
    sidebar = page.locator("#leftSidebar")
    sidebar.evaluate("el => el.style.width = '160px'")

    bb_narrow = icon.bounding_box()
    assert bb_narrow is not None
    assert bb_narrow["width"] >= 12, f"Folder icon must not disappear when sidebar is narrow, got {bb_narrow['width']}"
    expect(icon).to_be_visible()


def test_left_sidebar_resize_drag_and_reset(server, page: Page):
    """Dragging the right resizer of the left sidebar expands and shrinks it horizontally; double-click resets."""
    page.goto(server["url"])

    sidebar = page.locator("#leftSidebar")
    resizer = page.locator("#sidebarResizerRight")

    expect(sidebar).to_be_visible()
    expect(resizer).to_be_visible()

    initial_bb = sidebar.bounding_box()
    assert initial_bb is not None
    initial_width = initial_bb["width"]
    assert 235 <= initial_width <= 245, f"Expected default width around 240px, got {initial_width}"

    # 1. Drag resizer 80px to the right (widening)
    r_bb = resizer.bounding_box()
    assert r_bb is not None
    page.mouse.move(r_bb["x"] + r_bb["width"] / 2, r_bb["y"] + 100)
    page.mouse.down()
    page.mouse.move(r_bb["x"] + r_bb["width"] / 2 + 80, r_bb["y"] + 100)
    page.mouse.up()

    widened_bb = sidebar.bounding_box()
    assert widened_bb is not None
    assert widened_bb["width"] > initial_width + 60, f"Sidebar should widen by ~80px, got {widened_bb['width']}"

    # 2. Drag resizer 50px to the left (shrinking)
    r_bb = resizer.bounding_box()
    assert r_bb is not None
    page.mouse.move(r_bb["x"] + r_bb["width"] / 2, r_bb["y"] + 100)
    page.mouse.down()
    page.mouse.move(r_bb["x"] + r_bb["width"] / 2 - 50, r_bb["y"] + 100)
    page.mouse.up()

    shrunk_bb = sidebar.bounding_box()
    assert shrunk_bb is not None
    assert shrunk_bb["width"] < widened_bb["width"] - 30

    # 3. Double-click resizer to reset to default
    resizer.dblclick()
    reset_bb = sidebar.bounding_box()
    assert reset_bb is not None
    assert abs(reset_bb["width"] - initial_width) < 2, f"Double-click should reset width to default, got {reset_bb['width']}"


def test_left_sidebar_keyboard_controls(server, page: Page):
    """Keyboard Arrow keys on focused sidebar resizer adjust width, and Enter resets."""
    page.goto(server["url"])

    sidebar = page.locator("#leftSidebar")
    resizer = page.locator("#sidebarResizerRight")

    initial_bb = sidebar.bounding_box()
    assert initial_bb is not None
    initial_width = initial_bb["width"]

    resizer.focus()

    # ArrowRight expands width by 10px per press
    resizer.press("ArrowRight")
    resizer.press("ArrowRight")
    expanded_bb = sidebar.bounding_box()
    assert expanded_bb is not None
    assert expanded_bb["width"] >= initial_width + 18

    # ArrowLeft shrinks width
    resizer.press("ArrowLeft")
    decreased_bb = sidebar.bounding_box()
    assert decreased_bb is not None
    assert decreased_bb["width"] < expanded_bb["width"]

    # Enter resets width
    resizer.press("Enter")
    reset_bb = sidebar.bounding_box()
    assert reset_bb is not None
    assert abs(reset_bb["width"] - initial_width) < 2


def test_left_sidebar_resize_persistence(server, page: Page):
    """Resized sidebar width persists across page reloads in localStorage."""
    page.goto(server["url"])

    sidebar = page.locator("#leftSidebar")
    resizer = page.locator("#sidebarResizerRight")

    initial_bb = sidebar.bounding_box()
    assert initial_bb is not None
    initial_width = initial_bb["width"]

    # Drag resizer 90px to the right
    r_bb = resizer.bounding_box()
    assert r_bb is not None
    page.mouse.move(r_bb["x"] + r_bb["width"] / 2, r_bb["y"] + 120)
    page.mouse.down()
    page.mouse.move(r_bb["x"] + r_bb["width"] / 2 + 90, r_bb["y"] + 120)
    page.mouse.up()

    widened_bb = sidebar.bounding_box()
    assert widened_bb is not None
    saved_val = page.evaluate("() => localStorage.getItem('imagine_sidebar_width')")
    assert saved_val is not None
    assert int(saved_val) > initial_width + 70

    # Reload page
    page.reload()
    reloaded_sidebar = page.locator("#leftSidebar")
    reloaded_bb = reloaded_sidebar.bounding_box()
    assert reloaded_bb is not None
    assert abs(reloaded_bb["width"] - widened_bb["width"]) < 3, "Resized width should persist after reload"

    # Double click to reset
    page.locator("#sidebarResizerRight").dblclick()
    cleared_val = page.evaluate("() => localStorage.getItem('imagine_sidebar_width')")
    assert cleared_val is None


def test_folders_collapse_expand(server, page: Page):
    """Folders section starts collapsed, and clicking header expands and collapses it."""
    page.goto(server["url"])

    header = page.locator("#foldersHeader")
    tree = page.locator("#foldersTree")
    arrow = page.locator("#foldersArrow")

    # Initially folders tree is hidden, arrow is ▶
    expect(tree).to_be_hidden()
    expect(arrow).to_have_text("▶")

    # Click header to expand
    header.click()
    expect(tree).to_be_visible()
    expect(arrow).to_have_text("▼")

    # Folders are visible inside
    expect(tree.locator(".folder-item", has_text="family")).to_be_visible()
    expect(tree.locator(".folder-item", has_text="nature")).to_be_visible()

    # Click header to collapse again
    header.click()
    expect(tree).to_be_hidden()
    expect(arrow).to_have_text("▶")





