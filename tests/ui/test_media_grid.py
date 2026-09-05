"""
Deterministic UI tests for the Media Grid component.
"""

from playwright.sync_api import Page, expect


def test_media_grid_date_grouping(server, page: Page):
    """Verify that photo cards are grouped by month & year with correct header counts."""
    page.goto(server["url"])

    # 6 cards total across 3 date groups
    expect(page.locator(".photo-card")).to_have_count(6)

    # Verify date headers exist
    headers = page.locator(".date-header")
    expect(headers).to_have_count(3)

    # Check header titles and item counts
    expect(headers.nth(0)).to_contain_text("February 2026")
    expect(headers.nth(0).locator(".group-count")).to_have_text("2 items")

    expect(headers.nth(1)).to_contain_text("January 2026")
    expect(headers.nth(1).locator(".group-count")).to_have_text("2 items")

    expect(headers.nth(2)).to_contain_text("December 2025")
    expect(headers.nth(2).locator(".group-count")).to_have_text("2 items")


def test_photo_card_dom_structure_and_badges(server, page: Page):
    """Verify photo card structure: thumbnail, filename, date, star ratings, and pick/reject badges."""
    page.goto(server["url"])

    # Find the card for mountain.bmp (rating 5, pick)
    mountain_card = page.locator(".photo-card", has_text="mountain.bmp")
    expect(mountain_card).to_be_visible()

    # Verify thumbnail element
    thumb_img = mountain_card.locator(".photo-thumb-wrap img")
    expect(thumb_img).to_be_visible()
    expect(thumb_img).to_have_attribute("alt", "mountain.bmp")
    src = thumb_img.get_attribute("src")
    assert "/api/thumbnails/" in src or "/api/photos/" in src

    # Filename label
    expect(mountain_card.locator(".card-filename")).to_have_text("mountain.bmp")

    # Star rating: 5 active stars
    active_stars = mountain_card.locator(".card-stars span.active")
    expect(active_stars).to_have_count(5)

    # Flag badge: Pick
    pick_badge = mountain_card.locator(".flag-badge.pick")
    expect(pick_badge).to_be_visible()
    expect(pick_badge).to_have_text("✔")

    # Check beach.bmp (rating 3, reject)
    beach_card = page.locator(".photo-card", has_text="beach.bmp")
    expect(beach_card).to_be_visible()
    expect(beach_card.locator(".card-stars span.active")).to_have_count(3)
    reject_badge = beach_card.locator(".flag-badge.reject")
    expect(reject_badge).to_be_visible()
    expect(reject_badge).to_have_text("✖")

    # Check birthday.bmp (rating 0, unflagged)
    bday_card = page.locator(".photo-card", has_text="birthday.bmp")
    expect(bday_card).to_be_visible()
    expect(bday_card.locator(".card-stars span.active")).to_have_count(0)
    expect(bday_card.locator(".flag-badge")).to_have_count(0)


def test_thumbnail_error_fallback(server, page: Page):
    """Verify that if thumbnail load fails, image onerror falls back to original photo."""
    page.goto(server["url"])

    mountain_card = page.locator(".photo-card", has_text="mountain.bmp")
    thumb_img = mountain_card.locator(".photo-thumb-wrap img")
    card_id = mountain_card.get_attribute("data-id")

    # Dispatch native 'error' event to trigger inline onerror handler
    thumb_img.dispatch_event("error")

    # Verify src fell back to original endpoint
    expect(thumb_img).to_have_attribute("src", f"/api/photos/{card_id}/original")


def test_incremental_grid_append_no_flicker(server, page: Page):
    """Verify that appendMediaToGrid incrementally appends cards without wiping the DOM (eliminating flicker)."""
    page.goto(server["url"])
    expect(page.locator(".photo-card")).to_have_count(6)

    # Tag an existing card element with an in-memory JS expando property
    page.evaluate("""() => {
        const firstCard = document.querySelector('.photo-card');
        firstCard.__preserved = true;
    }""")

    # Call appendMediaToGrid with an item in an existing group (February 2026) and a new group (November 2025)
    page.evaluate("""() => {
        const newItemExisting = {
            id: 9991,
            file_name: 'incremental_feb.jpg',
            file_path: '/photos/incremental_feb.jpg',
            media_type: 'photo',
            date_taken: 1771156800,
            created_at: '2026-02-15T12:00:00Z',
            rating: 4,
            flag: 1
        };
        const newItemNew = {
            id: 9992,
            file_name: 'incremental_nov.jpg',
            file_path: '/photos/incremental_nov.jpg',
            media_type: 'photo',
            date_taken: 1762776000,
            created_at: '2025-11-10T12:00:00Z',
            rating: 0,
            flag: 0
        };
        window._imagineState.mediaItems.push(newItemExisting, newItemNew);
        window._imagineApp.appendMediaToGrid([newItemExisting, newItemNew]);
    }""")

    # Verify total cards increased from 6 to 8
    expect(page.locator(".photo-card")).to_have_count(8)

    # Verify the original card retained its identity without being re-created/destroyed
    is_preserved = page.evaluate("() => document.querySelector('.photo-card').__preserved === true")
    assert is_preserved is True, "Original DOM card was wiped or re-rendered, causing flicker!"

    # Verify the new items are in the DOM
    expect(page.locator(".photo-card", has_text="incremental_feb.jpg")).to_be_visible()
    expect(page.locator(".photo-card", has_text="incremental_nov.jpg")).to_be_visible()

    # Verify February 2026 header now counts 3 items (was 2)
    feb_header = page.locator(".date-group[data-group-key='February 2026'] .group-count")
    expect(feb_header).to_have_text("3 items")

    # Verify November 2025 date group was newly created
    expect(page.locator(".date-header", has_text="November 2025")).to_be_visible()

