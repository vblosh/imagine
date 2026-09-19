"""
Deterministic UI tests for Album management and filtering.
"""

import sqlite3

from playwright.sync_api import Page, expect


def test_albums_sidebar_display_and_filtering(server, page: Page):
    """Verify pre-seeded albums in sidebar, item count badges, and filtering photos by album."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    albums = page.locator("#albumsList .menu-item")
    expect(albums).to_have_count(2)

    # Check album names and counts
    best_album = page.locator("#albumsList .menu-item", has_text="Best of 2026")
    expect(best_album).to_be_visible()
    expect(best_album.locator(".count-badge")).to_have_text("2")

    vacation_album = page.locator("#albumsList .menu-item", has_text="Vacation")
    expect(vacation_album).to_be_visible()
    expect(vacation_album.locator(".count-badge")).to_have_text("2")

    # Click "Best of 2026" album to filter
    best_album.click()
    expect(page.locator("#filterLabel")).to_contain_text("Album: Best of 2026")
    expect(cards).to_have_count(2)
    names = [cards.nth(0).locator(".card-filename").text_content(), cards.nth(1).locator(".card-filename").text_content()]
    assert "mountain.bmp" in names
    assert "sunset.bmp" in names

    # Click "Best of 2026" again to toggle filter off
    best_album.click()
    expect(cards).to_have_count(6)


def test_album_order_sort_uses_album_media_position(server, page: Page):
    """Album order is available only in an album and follows album_media.position."""
    with sqlite3.connect(server["env"]["db_path"]) as conn:
        conn.execute("""
            UPDATE album_media
            SET position = CASE
                WHEN media_id = (SELECT id FROM media_items WHERE file_name = 'sunset.bmp') THEN 10
                WHEN media_id = (SELECT id FROM media_items WHERE file_name = 'mountain.bmp') THEN 20
                ELSE position
            END
            WHERE album_id = (SELECT id FROM albums WHERE name = 'Best of 2026')
        """)

    page.goto(server["url"])
    sort_select = page.locator("#sortSelect")
    album_order = sort_select.locator('option[value="album_order-asc"]')
    expect(album_order).to_be_disabled()
    expect(album_order).to_have_attribute("hidden", "")

    search_input = page.locator("#searchInput")
    search_input.fill("sunset.bmp")
    expect(page.locator(".photo-card")).to_have_count(1)

    album = page.locator("#albumsList .menu-item", has_text="Best of 2026")
    album.click()
    expect(album_order).to_be_enabled()
    expect(album_order).not_to_have_attribute("hidden", "")
    expect(album_order).to_have_text("Album order")
    expect(sort_select).to_have_value("album_order-asc")
    cards = page.locator(".photo-card")
    expect(cards).to_have_count(2)
    expect(search_input).to_have_value("")
    assert page.evaluate("() => localStorage.getItem('imagine_search_text')") is None
    expect(page.locator(".date-header")).to_have_count(0)
    expect(page.locator(".album-order-grid")).to_have_count(1)
    expect(cards.nth(0)).to_have_attribute("draggable", "true")
    expect(cards.nth(0).locator(".card-filename")).to_have_text("sunset.bmp")
    expect(cards.nth(1).locator(".card-filename")).to_have_text("mountain.bmp")

    with page.expect_response(lambda response: response.url.endswith("/order") and response.request.method == "POST") as response_info:
        cards.nth(0).drag_to(cards.nth(1))
    assert response_info.value.ok
    expect(cards.nth(0).locator(".card-filename")).to_have_text("mountain.bmp")
    expect(cards.nth(1).locator(".card-filename")).to_have_text("sunset.bmp")

    with sqlite3.connect(server["env"]["db_path"]) as conn:
        saved_order = conn.execute("""
            SELECT m.file_name
            FROM album_media am
            JOIN media_items m ON m.id = am.media_id
            WHERE am.album_id = (SELECT id FROM albums WHERE name = 'Best of 2026')
            ORDER BY am.position
        """).fetchall()
    assert [row[0] for row in saved_order] == ["mountain.bmp", "sunset.bmp"]

    album.click()
    expect(album_order).to_be_disabled()
    expect(sort_select).to_have_value("date_taken-desc")


def test_create_album_modal_validation_and_creation(server, page: Page):
    """Test Create Album modal dialog, name validation, and successful album creation."""
    page.goto(server["url"])

    modal = page.locator("#newAlbumModal")
    expect(modal).to_be_hidden()

    # Open modal
    page.locator("#newAlbumBtn").click()
    expect(modal).to_be_visible()

    # Validation: empty name triggers alert
    dialog_messages = []
    page.on("dialog", lambda dialog: (dialog_messages.append(dialog.message), dialog.accept()))

    page.locator("#createAlbumSubmitBtn").click()
    assert len(dialog_messages) == 1
    assert "Album name is required" in dialog_messages[0]
    expect(modal).to_be_visible()

    # Fill valid album details
    page.locator("#albumNameInput").fill("Autumn 2026")
    page.locator("#albumDescInput").fill("Fall foliage trip")
    page.locator("#createAlbumSubmitBtn").click()

    # Modal closes
    expect(modal).to_be_hidden()

    # Verify new album appears in sidebar list
    new_album_item = page.locator("#albumsList .menu-item", has_text="Autumn 2026")
    expect(new_album_item).to_be_visible()
    expect(new_album_item.locator(".count-badge")).to_have_text("0")


def test_create_album_modal_cancel_and_backdrop(server, page: Page):
    """Test closing the album modal via cancel button, close button, and backdrop click."""
    page.goto(server["url"])

    modal = page.locator("#newAlbumModal")

    # 1. Cancel button
    page.locator("#newAlbumBtn").click()
    expect(modal).to_be_visible()
    page.locator("#cancelAlbumBtn").click()
    expect(modal).to_be_hidden()

    # 2. Close icon button
    page.locator("#newAlbumBtn").click()
    expect(modal).to_be_visible()
    page.locator("#closeNewAlbumModalBtn").click()
    expect(modal).to_be_hidden()

    # 3. Backdrop click
    page.locator("#newAlbumBtn").click()
    expect(modal).to_be_visible()
    page.locator("#newAlbumBackdrop").click(position={"x": 10, "y": 10})
    expect(modal).to_be_hidden()


def test_delete_album(server, page: Page):
    """Deleting an album sends DELETE /api/albums/:id, removes it from the sidebar, and resets active filter."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    # 1. Create a temporary album to delete
    page.locator("#newAlbumBtn").click()
    page.locator("#albumNameInput").fill("ToDelete Album")
    page.locator("#createAlbumSubmitBtn").click()

    album_item = page.locator("#albumsList .menu-item", has_text="ToDelete Album")
    expect(album_item).to_be_visible()

    # Dismissing confirm cancels deletion
    page.once("dialog", lambda d: d.dismiss())
    album_item.locator(".delete-album-btn").click()
    expect(page.locator("#albumsList .menu-item", has_text="ToDelete Album")).to_be_visible()

    # Accepting confirm deletes the album
    page.once("dialog", lambda d: d.accept())
    album_item.locator(".delete-album-btn").click()
    expect(page.locator("#albumsList .menu-item", has_text="ToDelete Album")).to_have_count(0)

    # 2. Delete an active album and verify filter resets
    vacation_album = page.locator("#albumsList .menu-item", has_text="Vacation")
    vacation_album.click()
    expect(page.locator("#filterLabel")).to_contain_text("Album: Vacation")
    expect(cards).to_have_count(2)

    # Delete the active "Vacation" album with confirmation
    page.once("dialog", lambda d: d.accept())
    vacation_album.locator(".delete-album-btn").click()
    expect(page.locator("#albumsList .menu-item", has_text="Vacation")).to_have_count(0)
    # Filter resets back to all media
    expect(cards).to_have_count(6)


def test_add_photos_to_album_batch(server, page: Page):
    """Test adding selected photos to an album using the batch toolbar and modal."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    # 1. Create a new album "My Trip"
    page.locator("#newAlbumBtn").click()
    page.locator("#albumNameInput").fill("My Trip")
    page.locator("#createAlbumSubmitBtn").click()
    trip_album = page.locator("#albumsList .menu-item", has_text="My Trip")
    expect(trip_album).to_be_visible()
    expect(trip_album.locator(".count-badge")).to_have_text("0")

    # 2. Select two photos
    cards.nth(0).click()
    cards.nth(1).click(modifiers=["Control"])
    expect(page.locator("#batchActionBar")).to_be_visible()

    # 3. Open Add to Album modal
    page.locator("#batchAddAlbumBtn").click()
    modal = page.locator("#addToAlbumModal")
    expect(modal).to_be_visible()
    expect(page.locator("#addToAlbumTargetCount")).to_contain_text("2 selected photos")

    # 4. Select "My Trip" and confirm
    opt = page.locator("#addToAlbumSelect option", has_text="My Trip")
    val = opt.get_attribute("value")
    page.locator("#addToAlbumSelect").select_option(val)
    page.locator("#confirmAddToAlbumBtn").click()

    expect(modal).to_be_hidden()

    # Verify badge count updated
    expect(trip_album.locator(".count-badge")).to_have_text("2")

    # Click album to filter and verify 2 photos are in it
    trip_album.click()
    expect(page.locator("#filterLabel")).to_contain_text("Album: My Trip")
    expect(cards).to_have_count(2)


def test_add_photo_to_album_inspector(server, page: Page):
    """Test adding single selected photo to an album using the inspector panel."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    # 1. Select single card
    cards.nth(0).click()

    # 2. Click inspector Add to Album button
    page.locator("#inspectorAddToAlbumBtn").click()
    modal = page.locator("#addToAlbumModal")
    expect(modal).to_be_visible()
    expect(page.locator("#addToAlbumTargetCount")).to_contain_text("1 selected photo")

    # 3. Cancel modal
    page.locator("#cancelAddToAlbumBtn").click()
    expect(modal).to_be_hidden()


def test_remove_photo_from_album_inspector(server, page: Page):
    """The inspector removes one selected photo from the active album."""
    page.goto(server["url"])

    album = page.locator("#albumsList .menu-item", has_text="Best of 2026")
    album.click()
    cards = page.locator(".photo-card")
    expect(cards).to_have_count(2)

    cards.first.click()
    add_btn = page.locator("#inspectorAddToAlbumBtn")
    expect(add_btn).to_have_text("Add to Album")
    add_btn.click()
    expect(page.locator("#addToAlbumModal")).to_be_visible()
    page.locator("#cancelAddToAlbumBtn").click()

    remove_btn = page.locator("#inspectorRemoveFromAlbumBtn")
    expect(remove_btn).to_be_visible()
    expect(remove_btn).to_have_text("Remove from Album")

    with page.expect_response(lambda response: "/api/albums/" in response.url and response.url.endswith("/media") and response.request.method == "DELETE") as response_info:
        remove_btn.click()
    assert response_info.value.ok

    expect(cards).to_have_count(1)
    expect(album.locator(".count-badge")).to_have_text("1")


def test_remove_photos_from_album_batch_toolbar(server, page: Page):
    """The batch toolbar removes all selected photos from the active album."""
    page.goto(server["url"])

    album = page.locator("#albumsList .menu-item", has_text="Best of 2026")
    album.click()
    cards = page.locator(".photo-card")
    expect(cards).to_have_count(2)

    cards.nth(0).click()
    cards.nth(1).click(modifiers=["Control"])
    expect(page.locator("#inspectorAddToAlbumBtn")).to_have_text("Add to Album")
    expect(page.locator("#inspectorRemoveFromAlbumBtn")).to_be_visible()
    remove_btn = page.locator("#batchAddAlbumBtn")
    expect(remove_btn).to_have_text("Remove from Album")

    with page.expect_response(lambda response: "/api/albums/" in response.url and response.url.endswith("/media") and response.request.method == "DELETE") as response_info:
        remove_btn.click()
    assert response_info.value.ok

    expect(cards).to_have_count(0)
    expect(album.locator(".count-badge")).to_have_text("0")
    expect(page.locator("#batchActionBar")).to_be_hidden()


def test_set_album_cover_from_inspector(server, page: Page):
    """Setting a selected album photo as its cover persists the selected media ID."""
    page.goto(server["url"])

    album = page.locator("#albumsList .menu-item", has_text="Best of 2026")
    album.click()
    cards = page.locator(".photo-card")
    expect(cards).to_have_count(2)

    selected_card = cards.nth(1)
    selected_media_id = selected_card.get_attribute("data-id")
    selected_card.click()

    set_cover_btn = page.locator("#inspectorSetAsCoverBtn")
    expect(set_cover_btn).to_be_visible()
    expect(set_cover_btn).to_have_text("Set as cover")

    with page.expect_response(lambda response: "/api/albums/" in response.url and response.url.endswith("/cover") and response.request.method == "POST") as response_info:
        set_cover_btn.click()
    assert response_info.value.ok
    expect(set_cover_btn).to_have_text("Current cover")
    expect(set_cover_btn).to_be_disabled()

    albums = page.evaluate("""async () => {
        const response = await fetch('/api/albums');
        return response.json();
    }""")
    best_album = next(item for item in albums if item["name"] == "Best of 2026")
    assert str(best_album["cover_media_id"]) == selected_media_id


def test_albums_top_selector_tab_display_and_drilldown(server, page: Page):
    """Clicking Albums tab in the top selector opens the visual grid of album cards, and clicking a card drills down."""
    import re
    page.goto(server["url"])

    category_view = page.locator("#categoryViewContainer")
    media_grid = page.locator("#mediaGrid")
    content_toolbar = page.locator("#contentToolbar")
    category_toolbar = page.locator("#categoryToolbar")
    back_btn = page.locator("#categoryBackBtn")
    cards = page.locator(".photo-card")

    # Initial state: Media view active
    expect(page.locator('.tab-btn[data-tab="media"]')).to_have_class(re.compile(r"\bactive\b"))
    expect(category_view).to_be_hidden()

    # Click Albums tab on top selector
    albums_tab = page.locator('.tab-btn[data-tab="albums"]')
    expect(albums_tab).to_be_visible()
    expect(albums_tab.locator("span")).to_have_text("Albums")
    albums_tab.click()

    # Verify tab is active and category view is displayed
    expect(albums_tab).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator('.tab-btn[data-tab="media"]')).not_to_have_class(re.compile(r"\bactive\b"))
    expect(category_view).to_be_visible()
    expect(content_toolbar).to_be_hidden()
    expect(category_toolbar).to_be_visible()
    expect(page.locator("#categoryViewTitle")).to_have_text("Albums")
    expect(page.locator("#categoryViewSubtitle")).to_contain_text("2 albums")

    # Check album cards in the grid
    album_cards = page.locator(".category-card-item")
    expect(album_cards).to_have_count(2)

    best_card = page.locator('.category-card-item', has_text="Best of 2026")
    vacation_card = page.locator('.category-card-item', has_text="Vacation")
    expect(best_card).to_be_visible()
    expect(vacation_card).to_be_visible()
    expect(best_card.locator(".category-card-badge")).to_contain_text("2 photos")
    expect(vacation_card.locator(".category-card-badge")).to_contain_text("2 photos")

    # Click Best of 2026 album card to drill down
    best_card.click()
    expect(category_view).to_be_hidden()
    expect(media_grid).to_be_visible()
    expect(content_toolbar).to_be_visible()
    expect(category_toolbar).to_be_hidden()
    expect(cards).to_have_count(2)
    expect(page.locator("#filterLabel")).to_contain_text("Album: Best of 2026")
    expect(back_btn).to_be_visible()
    expect(page.locator("#categoryBackBtnLabel")).to_have_text("Back to Albums")

    # Verify sidebar album item is highlighted active
    sidebar_album = page.locator('#albumsList .menu-item', has_text="Best of 2026")
    expect(sidebar_album).to_have_class(re.compile(r"\bactive\b"))

    # Click Back to Albums button
    back_btn.click()
    expect(category_view).to_be_visible()
    expect(media_grid).to_be_hidden()
    expect(category_toolbar).to_be_visible()
    expect(content_toolbar).to_be_hidden()
    expect(albums_tab).to_have_class(re.compile(r"\bactive\b"))


def test_albums_top_selector_create_album_and_search(server, page: Page):
    """Creating an album via toolbar Add Album updates the grid, and search filters album cards."""
    page.goto(server["url"])

    page.locator('.tab-btn[data-tab="albums"]').click()
    expect(page.locator("#categoryViewTitle")).to_have_text("Albums")

    # Click Add Album in toolbar
    add_btn = page.locator("#categoryAddBtn")
    expect(add_btn).to_be_visible()
    expect(page.locator("#categoryAddBtnLabel")).to_have_text("Add Album")
    add_btn.click()

    # Modal opens
    modal = page.locator("#newAlbumModal")
    expect(modal).to_be_visible()

    # Create new album
    page.locator("#albumNameInput").fill("Holiday 2026")
    page.locator("#createAlbumSubmitBtn").click()
    expect(modal).to_be_hidden()

    # Verify new album card in grid
    holiday_card = page.locator('.category-card-item', has_text="Holiday 2026")
    expect(holiday_card).to_be_visible()
    expect(holiday_card.locator(".category-card-badge")).to_contain_text("0 photos")

    # Search filtering in Albums view
    search_input = page.locator("#searchInput")
    search_input.fill("Holiday")
    page.wait_for_timeout(350)
    expect(page.locator('.category-card-item', has_text="Holiday 2026")).to_be_visible()
    expect(page.locator('.category-card-item', has_text="Best of 2026")).to_be_hidden()

    # Clear search
    page.locator("#clearSearchBtn").click()
    page.wait_for_timeout(350)
    expect(page.locator('.category-card-item', has_text="Best of 2026")).to_be_visible()
    expect(page.locator('.category-card-item', has_text="Holiday 2026")).to_be_visible()


def test_albums_top_selector_keyboard_navigation(server, page: Page):
    """Pressing Enter on an album card drills down and Escape navigates back to Albums grid."""
    page.goto(server["url"])

    page.locator('.tab-btn[data-tab="albums"]').click()
    best_card = page.locator('.category-card-item', has_text="Best of 2026")
    expect(best_card).to_be_visible()

    # Focus card and press Enter
    best_card.focus()
    page.keyboard.press("Enter")
    expect(page.locator("#categoryViewContainer")).to_be_hidden()
    expect(page.locator(".photo-card")).to_have_count(2)
    expect(page.locator("#categoryBackBtn")).to_be_visible()

    # Press Escape to return to Albums view
    page.keyboard.press("Escape")
    expect(page.locator("#categoryViewContainer")).to_be_visible()
    expect(page.locator('.category-card-item', has_text="Best of 2026")).to_be_visible()



