"""
Deterministic UI tests for Album management and filtering.
"""

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

    # Click delete button on the new album
    album_item.locator(".delete-album-btn").click()
    expect(page.locator("#albumsList .menu-item", has_text="ToDelete Album")).to_have_count(0)

    # 2. Delete an active album and verify filter resets
    vacation_album = page.locator("#albumsList .menu-item", has_text="Vacation")
    vacation_album.click()
    expect(page.locator("#filterLabel")).to_contain_text("Album: Vacation")
    expect(cards).to_have_count(2)

    # Delete the active "Vacation" album
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


