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
