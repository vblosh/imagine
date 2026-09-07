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

