"""
Deterministic UI tests for Sorting and Search functionality.
"""

from playwright.sync_api import Page, expect


def test_sort_dropdown_options(server, page: Page):
    """Testing all sorting options in the sort dropdown."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    # 1. File Name A-Z (file_name-asc)
    page.locator("#sortSelect").select_option("file_name-asc")
    # Expected order: beach.bmp, birthday.bmp, forest.bmp, mountain.bmp, portrait.bmp, sunset.bmp
    expect(cards.nth(0).locator(".card-filename")).to_have_text("beach.bmp")
    expect(cards.nth(1).locator(".card-filename")).to_have_text("birthday.bmp")
    expect(cards.nth(5).locator(".card-filename")).to_have_text("sunset.bmp")

    # 2. Rating High to Low (rating-desc)
    page.locator("#sortSelect").select_option("rating-desc")
    # mountain.bmp has rating 5, sunset.bmp has rating 4
    expect(cards.nth(0).locator(".card-filename")).to_have_text("mountain.bmp")
    expect(cards.nth(1).locator(".card-filename")).to_have_text("sunset.bmp")

    # 3. Date Oldest first (date_taken-asc)
    page.locator("#sortSelect").select_option("date_taken-asc")
    # forest.bmp is Dec 10, 2025 (oldest)
    expect(cards.nth(0).locator(".card-filename")).to_have_text("forest.bmp")

    # 4. Date Newest first (date_taken-desc)
    page.locator("#sortSelect").select_option("date_taken-desc")
    # mountain.bmp is Feb 15, 2026 (newest)
    expect(cards.nth(0).locator(".card-filename")).to_have_text("mountain.bmp")

    # 5. File Size Largest (file_size-desc)
    page.locator("#sortSelect").select_option("file_size-desc")
    # Date groups ordered by first appearance:
    # Feb 2026: mountain.bmp (6.2MB), sunset.bmp (2.76MB)
    # Jan 2026: beach.bmp (4.3MB), birthday.bmp (2.25MB)
    # Dec 2025: forest.bmp (2.88MB), portrait.bmp (2.4MB)
    expect(cards.nth(0).locator(".card-filename")).to_have_text("mountain.bmp")
    expect(cards.nth(1).locator(".card-filename")).to_have_text("sunset.bmp")
    expect(cards.nth(2).locator(".card-filename")).to_have_text("beach.bmp")
    expect(cards.nth(4).locator(".card-filename")).to_have_text("forest.bmp")
    expect(cards.nth(5).locator(".card-filename")).to_have_text("portrait.bmp")


def test_realtime_search_and_clear(server, page: Page):
    """Searching by filename, camera, lens with debounce, and clearing search."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    search_input = page.locator("#searchInput")

    # 1. Search by filename: "beach"
    search_input.fill("beach")
    # Expect 1 card after debounce
    expect(cards).to_have_count(1)
    expect(cards.first.locator(".card-filename")).to_have_text("beach.bmp")
    expect(page.locator("#filterLabel")).to_contain_text('Search: "beach"')

    # 2. Clear search via #clearSearchBtn
    page.locator("#clearSearchBtn").click()
    expect(search_input).to_have_value("")
    expect(cards).to_have_count(6)

    # 3. Search by camera make: "Sony" (matches mountain.bmp and birthday.bmp)
    search_input.fill("Sony")
    expect(cards).to_have_count(2)
    card_names = [cards.nth(0).locator(".card-filename").text_content(), cards.nth(1).locator(".card-filename").text_content()]
    assert "mountain.bmp" in card_names
    assert "birthday.bmp" in card_names

    # 4. Search by lens: "85mm" (matches portrait.bmp)
    search_input.fill("85mm")
    expect(cards).to_have_count(1)
    expect(cards.first.locator(".card-filename")).to_have_text("portrait.bmp")
