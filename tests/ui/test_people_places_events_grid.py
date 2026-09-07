"""
Deterministic UI tests for People, Places, and Events Grid views and photo drill-down,
inspired by Photoshop Elements Organizer.
"""

import re
from playwright.sync_api import Page, expect


def test_people_grid_view_and_drilldown(server, page: Page):
    """Clicking People opens grid with one photo and name for each person; clicking opens photos."""
    page.goto(server["url"])

    category_view = page.locator("#categoryViewContainer")
    media_grid = page.locator("#mediaGrid")
    back_btn = page.locator("#categoryBackBtn")
    cards = page.locator(".photo-card")

    # Initially in Media view
    expect(cards).to_have_count(6)
    expect(category_view).to_be_hidden()

    # Click People tab on top
    page.locator('.tab-btn[data-tab="people"]').click()
    expect(page.locator('.tab-btn[data-tab="people"]')).to_have_class(re.compile(r"\bactive\b"))
    expect(category_view).to_be_visible()
    expect(page.locator("#categoryViewTitle")).to_have_text("People")
    expect(page.locator("#categoryViewSubtitle")).to_contain_text("2 people")

    # Verify People cards in grid
    people_cards = page.locator(".category-card-item")
    expect(people_cards).to_have_count(2)

    alice_card = page.locator('.category-card-item', has_text="Alice")
    bob_card = page.locator('.category-card-item', has_text="Bob")
    expect(alice_card).to_be_visible()
    expect(bob_card).to_be_visible()

    # Verify each card has photo thumb wrapper, badge, and name
    expect(alice_card.locator(".category-card-name")).to_have_text("Alice")
    expect(alice_card.locator(".category-card-badge")).to_contain_text("photo")
    expect(bob_card.locator(".category-card-name")).to_have_text("Bob")
    expect(bob_card.locator(".category-card-badge")).to_contain_text("photo")

    # Click on Alice's photo -> select Alice and show photos from Alice
    alice_card.click()
    expect(category_view).to_be_hidden()
    expect(media_grid).to_be_visible()
    expect(cards).to_have_count(1)
    expect(cards.first.locator(".card-filename")).to_have_text("birthday.bmp")

    # Verify sidebar tag is highlighted
    expect(page.locator("#tagCategoryPeople .tag-item.active")).to_contain_text("Alice")

    # Verify back button and filter label
    expect(back_btn).to_be_visible()
    expect(back_btn).to_contain_text("Back to People")
    expect(page.locator("#filterLabel")).to_contain_text("People: Alice")

    # Click Back to People -> returns to People grid
    back_btn.click()
    expect(category_view).to_be_visible()
    expect(people_cards).to_have_count(2)
    expect(back_btn).to_be_hidden()

    # Click on Bob's card -> select Bob and show photos from Bob
    bob_card.click()
    expect(category_view).to_be_hidden()
    expect(cards).to_have_count(1)
    expect(cards.first.locator(".card-filename")).to_have_text("portrait.bmp")
    expect(page.locator("#tagCategoryPeople .tag-item.active")).to_contain_text("Bob")

    # Clicking People tab on top also returns to People grid
    page.locator('.tab-btn[data-tab="people"]').click()
    expect(category_view).to_be_visible()
    expect(people_cards).to_have_count(2)


def test_places_and_events_grid_views(server, page: Page):
    """Test Places and Events grid views and photo drill-down."""
    page.goto(server["url"])

    category_view = page.locator("#categoryViewContainer")
    cards = page.locator(".photo-card")
    back_btn = page.locator("#categoryBackBtn")

    # 1. Places Grid
    page.locator('.tab-btn[data-tab="places"]').click()
    expect(category_view).to_be_visible()
    expect(page.locator("#categoryViewTitle")).to_have_text("Places")

    places_cards = page.locator(".category-card-item")
    expect(places_cards).to_have_count(2)

    alps_card = page.locator('.category-card-item', has_text="Alps")
    beach_card = page.locator('.category-card-item', has_text="Beach")
    expect(alps_card).to_be_visible()
    expect(beach_card).to_be_visible()

    # Drill down to Beach
    beach_card.click()
    expect(category_view).to_be_hidden()
    expect(cards).to_have_count(1)
    expect(cards.first.locator(".card-filename")).to_have_text("beach.bmp")
    expect(back_btn).to_contain_text("Back to Places")

    # Back to Places
    back_btn.click()
    expect(category_view).to_be_visible()
    expect(places_cards).to_have_count(2)

    # 2. Events Grid
    page.locator('.tab-btn[data-tab="events"]').click()
    expect(category_view).to_be_visible()
    expect(page.locator("#categoryViewTitle")).to_have_text("Events")

    event_card = page.locator('.category-card-item', has_text="Birthday 2026")
    expect(event_card).to_be_visible()

    # Drill down to Birthday 2026
    event_card.click()
    expect(category_view).to_be_hidden()
    expect(cards).to_have_count(1)
    expect(cards.first.locator(".card-filename")).to_have_text("birthday.bmp")
    expect(back_btn).to_contain_text("Back to Events")

    # Click Events tab on top -> back to Events grid
    page.locator('.tab-btn[data-tab="events"]').click()
    expect(category_view).to_be_visible()
    expect(page.locator(".category-card-item")).to_have_count(1)


def test_category_grid_search_filter(server, page: Page):
    """Typing in search box while in People grid filters visible person cards by name."""
    page.goto(server["url"])

    category_view = page.locator("#categoryViewContainer")
    page.locator('.tab-btn[data-tab="people"]').click()
    expect(category_view).to_be_visible()

    people_cards = page.locator(".category-card-item")
    expect(people_cards).to_have_count(2)

    # Type "Ali" into search box
    search_input = page.locator("#searchInput")
    search_input.fill("Ali")

    # Only Alice should remain visible
    expect(page.locator('.category-card-item', has_text="Alice")).to_be_visible()
    expect(page.locator('.category-card-item', has_text="Bob")).to_be_hidden()

    # Clear search
    page.locator("#clearSearchBtn").click()
    expect(page.locator('.category-card-item', has_text="Alice")).to_be_visible()
    expect(page.locator('.category-card-item', has_text="Bob")).to_be_visible()


def test_escape_key_navigates_back_to_category_grid(server, page: Page):
    """When in photos of a Person, Place, or Event and Back button is active, pressing Escape goes back."""
    page.goto(server["url"])

    category_view = page.locator("#categoryViewContainer")
    back_btn = page.locator("#categoryBackBtn")
    cards = page.locator(".photo-card")

    # 1. Test in People
    page.locator('.tab-btn[data-tab="people"]').click()
    expect(category_view).to_be_visible()

    # Click on Alice's card -> opens Alice's photos, Back button active
    page.locator('.category-card-item', has_text="Alice").click()
    expect(category_view).to_be_hidden()
    expect(back_btn).to_be_visible()
    expect(cards).to_have_count(1)

    # Press Escape -> goes back to People grid!
    page.keyboard.press("Escape")
    expect(category_view).to_be_visible()
    expect(back_btn).to_be_hidden()
    expect(page.locator(".category-card-item")).to_have_count(2)

    # 2. Test in Places with a selected photo card
    page.locator('.tab-btn[data-tab="places"]').click()
    expect(category_view).to_be_visible()

    # Click on Alps -> opens Alps photos
    page.locator('.category-card-item', has_text="Alps").click()
    expect(category_view).to_be_hidden()
    expect(back_btn).to_be_visible()

    # Select the photo card
    cards.first.click()
    expect(cards.first).to_have_class(re.compile(r"\bselected\b"))

    # Press Escape -> goes back to Places grid!
    page.keyboard.press("Escape")
    expect(category_view).to_be_visible()
    expect(back_btn).to_be_hidden()
    expect(page.locator(".category-card-item")).to_have_count(2)

    # 3. Test in Events
    page.locator('.tab-btn[data-tab="events"]').click()
    expect(category_view).to_be_visible()

    page.locator('.category-card-item', has_text="Birthday 2026").click()
    expect(category_view).to_be_hidden()
    expect(back_btn).to_be_visible()

    # Press Escape -> goes back to Events grid!
    page.keyboard.press("Escape")
    expect(category_view).to_be_visible()
    expect(back_btn).to_be_hidden()
    expect(page.locator(".category-card-item")).to_have_count(1)


def test_category_grid_skips_non_photo_for_cover(media_server, page: Page):
    """
    If in People, Places, or Events first media is not photo, skip it till a photo is found.
    When an event has both a newer video and an older photo, the card thumbnail must use the photo.
    When an event has only a video, it falls back to the video thumbnail.
    """
    import sqlite3
    db_path = media_server["env"]["db_path"]
    conn = sqlite3.connect(db_path)
    cur = conn.cursor()

    # Get video item (drone_flight.mp4, date_taken=1771200000)
    cur.execute("SELECT id, content_hash FROM media_items WHERE media_type = 'video' LIMIT 1")
    video_row = cur.fetchone()
    assert video_row is not None
    video_id, video_hash = video_row

    # Get photo item from Birthday 2026 (birthday.bmp, date_taken=1767628800)
    cur.execute("SELECT id, content_hash FROM media_items WHERE file_name = 'birthday.bmp'")
    photo_row = cur.fetchone()
    assert photo_row is not None
    photo_id, photo_hash = photo_row

    # Get tag id for 'Birthday 2026' (events)
    cur.execute("SELECT id FROM tags WHERE name = 'Birthday 2026' AND category = 'events'")
    bday_tag_id = cur.fetchone()[0]

    # Tag the video with 'Birthday 2026' as well (video is newer than the photo)
    cur.execute("INSERT INTO media_tags (media_id, tag_id) VALUES (?, ?)", (video_id, bday_tag_id))

    # Create a tag that ONLY has the video: "Drone Expo"
    cur.execute("INSERT INTO tags (name, category) VALUES ('Drone Expo', 'events')")
    drone_tag_id = cur.lastrowid
    cur.execute("INSERT INTO media_tags (media_id, tag_id) VALUES (?, ?)", (video_id, drone_tag_id))

    conn.commit()
    conn.close()

    # Load page
    page.goto(media_server["url"])

    # Switch to Events tab
    page.locator('.tab-btn[data-tab="events"]').click()
    expect(page.locator("#categoryViewContainer")).to_be_visible()

    # Both events should be present
    bday_card = page.locator('.category-card-item', has_text="Birthday 2026")
    drone_card = page.locator('.category-card-item', has_text="Drone Expo")
    expect(bday_card).to_be_visible()
    expect(drone_card).to_be_visible()

    # Birthday 2026 has both video (newer) and photo (older).
    # Its thumbnail MUST use the photo's hash (birthday.bmp), NOT the video's hash!
    bday_thumb = bday_card.locator("img.category-card-thumb")
    expect(bday_thumb).to_be_visible()
    expect(bday_thumb).to_have_attribute("src", re.compile(rf"/api/thumbnails/{photo_hash}/256"))

    # Drone Expo has ONLY the video. It falls back to the video thumbnail.
    drone_thumb = drone_card.locator("img.category-card-thumb")
    expect(drone_thumb).to_be_visible()
    expect(drone_thumb).to_have_attribute("src", re.compile(rf"/api/thumbnails/{video_hash}/256"))


