"""
Deterministic UI tests for Sorting and Search functionality.
"""

import json
import time
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

    # 5. Search by tag: "Alice" (matches birthday.bmp)
    search_input.fill("Alice")
    expect(cards).to_have_count(1)
    expect(cards.first.locator(".card-filename")).to_have_text("birthday.bmp")

    # 6. Search by tag: "Alps" (matches mountain.bmp)
    search_input.fill("Alps")
    expect(cards).to_have_count(1)
    expect(cards.first.locator(".card-filename")).to_have_text("mountain.bmp")


def test_sparkle_ai_toggle_button_position_and_no_world_icon(server, page: Page):
    """The Sparkle / AI toggle button is positioned to the right of the search input field, and no world icon is in the language selector."""
    page.goto(server["url"])

    search_input = page.locator("#searchInput")
    semantic_toggle = page.locator("#semanticToggle")
    expect(search_input).to_be_visible()
    expect(semantic_toggle).to_be_visible()

    input_box = search_input.bounding_box()
    toggle_box = semantic_toggle.bounding_box()
    assert input_box is not None and toggle_box is not None
    # Semantic toggle is to the right of the input field
    assert toggle_box["x"] >= input_box["x"] + input_box["width"] - 2, (
        f"Toggle (x={toggle_box['x']}) should be to the right of search input (right edge={input_box['x'] + input_box['width']})"
    )

    # World icon left of language selector is removed
    expect(page.locator(".lang-selector .lang-icon")).to_have_count(0)
    expect(page.locator(".lang-selector svg")).to_have_count(0)
    expect(page.locator("#langSelect")).to_be_visible()


def test_search_input_debounce_and_enter_key(server, page: Page):
    """Typing in search input sets a debounce timer, and pressing Enter triggers search immediately."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    search_input = page.locator("#searchInput")

    # Focus and type rapidly
    search_input.click()
    search_input.type("beach", delay=10)

    # Pressing Enter triggers immediate search
    search_input.press("Enter")
    expect(cards).to_have_count(1)
    expect(cards.first.locator(".card-filename")).to_have_text("beach.bmp")

    # Clear search
    page.locator("#clearSearchBtn").click()
    expect(cards).to_have_count(6)
    expect(search_input).to_have_value("")


def test_semantic_search_debounce_delay_constants_and_logic(server, page: Page):
    """Semantic search uses 2000ms debounce while standard search uses 500ms debounce."""
    page.goto(server["url"])

    delays = page.evaluate("""() => {
        const app = window._imagineApp;
        const stdConst = app.STANDARD_SEARCH_DEBOUNCE_MS;
        const semConst = app.SEMANTIC_SEARCH_DEBOUNCE_MS;

        // With semantic search OFF
        app.state.semanticSearchEnabled = false;
        const delayOff = app.getSearchDebounceDelay(true);

        // With semantic search ON
        app.state.semanticSearchEnabled = true;
        const delayOn = app.getSearchDebounceDelay(true);

        return { stdConst, semConst, delayOff, delayOn };
    }""")

    assert delays["stdConst"] == 500
    assert delays["semConst"] == 2000
    assert delays["delayOff"] == 500
    assert delays["delayOn"] == 2000


def test_semantic_search_sentence_typing_debounced_and_proceeds_on_enter(server, page: Page):
    """When semantic search is on and user enters a sentence with pauses, search does not
    fire repeatedly on pauses below debounce time, and proceeds immediately on Enter."""
    page.route("**/api/semantic/status*", lambda route: route.fulfill(
        status=200,
        content_type="application/json",
        body=json.dumps({"ready": True, "built": True, "indexSize": 10, "modelId": "clip-test"})
    ))

    semantic_calls = []

    def handle_semantic_search(route):
        post_data = route.request.post_data_json or {}
        semantic_calls.append(post_data.get("query", ""))
        route.fulfill(
            status=200,
            content_type="application/json",
            body=json.dumps({"media_items": [], "total": 0})
        )

    page.route("**/api/semantic/search*", handle_semantic_search)
    page.goto(server["url"])

    # Toggle semantic search ON
    page.locator("#semanticToggle").click()
    page.evaluate("() => { window.__TEST_SEARCH_DEBOUNCE_DELAY__ = 250; }")

    search_input = page.locator("#searchInput")
    search_input.click()

    # User enters words with short pauses between them (< 250ms debounce)
    # word 1
    search_input.press_sequentially("golden", delay=15)
    time.sleep(0.08)  # 80ms pause
    assert len(semantic_calls) == 0, "Search should not run during pause shorter than debounce"

    # word 2
    search_input.press_sequentially(" retriever", delay=15)
    time.sleep(0.08)  # 80ms pause
    assert len(semantic_calls) == 0, "Search should not run during second pause"

    # word 3
    search_input.press_sequentially(" in snow", delay=15)
    time.sleep(0.05)  # 50ms pause
    assert len(semantic_calls) == 0, "Search should not run during third pause"

    # Press Enter: triggers search immediately
    search_input.press("Enter")
    assert len(semantic_calls) == 1, "Enter should proceed search immediately"
    assert semantic_calls[0] == "golden retriever in snow"


def test_semantic_search_sentence_typing_proceeds_after_debouncing_pause(server, page: Page):
    """When semantic search is on, search automatically proceeds after debouncing time."""
    page.route("**/api/semantic/status*", lambda route: route.fulfill(
        status=200,
        content_type="application/json",
        body=json.dumps({"ready": True, "built": True, "indexSize": 10, "modelId": "clip-test"})
    ))

    semantic_calls = []

    def handle_semantic_search(route):
        post_data = route.request.post_data_json or {}
        semantic_calls.append(post_data.get("query", ""))
        route.fulfill(
            status=200,
            content_type="application/json",
            body=json.dumps({"media_items": [], "total": 0})
        )

    page.route("**/api/semantic/search*", handle_semantic_search)
    page.goto(server["url"])

    # Toggle semantic search ON
    page.locator("#semanticToggle").click()
    page.evaluate("() => { window.__TEST_SEARCH_DEBOUNCE_DELAY__ = 150; }")

    search_input = page.locator("#searchInput")
    search_input.click()
    search_input.press_sequentially("sunset over mountains", delay=10)

    # Immediately after typing, debounce hasn't elapsed yet
    assert len(semantic_calls) == 0

    # Wait for debounce time to elapse (150ms delay + buffer)
    page.wait_for_timeout(400)

    # Now debounce timer fired and search proceeded
    assert len(semantic_calls) == 1
    assert semantic_calls[0] == "sunset over mountains"


def test_semantic_search_probability_rank_grid_and_ui(server, page: Page):
    """Semantic search renders a dedicated grid sorted by probability rank with badges and hidden filters."""
    page.route("**/api/semantic/status*", lambda route: route.fulfill(
        status=200,
        content_type="application/json",
        body=json.dumps({"ready": True, "built": True, "indexSize": 6, "modelId": "clip-vit-base"})
    ))

    def handle_semantic_search(route):
        media_items = [
            {"id": 4, "file_name": "sunset.bmp", "date_taken": 1770000000, "similarity_score": 0.94},
            {"id": 1, "file_name": "beach.bmp", "date_taken": 1768000000, "similarity_score": 0.88},
            {"id": 5, "file_name": "forest.bmp", "date_taken": 1765000000, "similarity_score": 0.72},
        ]
        items = [
            {"mediaId": 4, "score": 0.94},
            {"mediaId": 1, "score": 0.88},
            {"mediaId": 5, "score": 0.72},
        ]
        route.fulfill(
            status=200,
            content_type="application/json",
            body=json.dumps({"items": items, "media_items": media_items, "total": 3})
        )

    page.route("**/api/semantic/search*", handle_semantic_search)
    page.goto(server["url"])

    # Normal grid has date groups and 6 cards
    expect(page.locator(".photo-card")).to_have_count(6)
    expect(page.locator(".date-group")).to_have_count(3)
    expect(page.locator("#sortSelect")).to_be_visible()
    expect(page.locator(".bottom-scrub-bar")).to_be_visible()

    # Turn on semantic search
    page.locator("#semanticToggle").click()
    page.locator("#searchInput").fill("golden beach sunset")
    page.locator("#searchInput").press("Enter")

    # Semantic search toolbar is visible
    toolbar = page.locator("#semanticSearchToolbar")
    expect(toolbar).to_be_visible()
    expect(page.locator("#semanticSearchTitle")).to_have_text('AI Search: "golden beach sunset"')
    expect(page.locator("#semanticSearchCount")).to_contain_text("3 items")
    expect(page.locator("#semanticSearchHint")).to_have_text("(sorted by probability rank)")

    # Secondary controls & filters are hidden
    expect(page.locator(".sort-selector")).to_be_hidden()
    expect(page.locator(".bottom-scrub-bar")).to_be_hidden()
    expect(page.locator("#filterIndicator")).to_be_hidden()
    expect(page.locator("#viewModeToggle")).to_be_hidden()

    # Flat grid: NO date groups
    expect(page.locator(".date-group")).to_have_count(0)
    expect(page.locator(".semantic-search-grid")).to_be_visible()

    # 3 cards ordered by probability rank
    search_cards = page.locator(".photo-card")
    expect(search_cards).to_have_count(3)
    expect(search_cards.nth(0).locator(".card-filename")).to_have_text("sunset.bmp")
    expect(search_cards.nth(0).locator(".similarity-rank-badge")).to_have_text("#1")
    expect(search_cards.nth(1).locator(".card-filename")).to_have_text("beach.bmp")
    expect(search_cards.nth(1).locator(".similarity-rank-badge")).to_have_text("#2")
    expect(search_cards.nth(2).locator(".card-filename")).to_have_text("forest.bmp")
    expect(search_cards.nth(2).locator(".similarity-rank-badge")).to_have_text("#3")

    # Clicking/selecting a card should never unhide the sort selector
    search_cards.nth(0).click()
    expect(page.locator(".sort-selector")).to_be_hidden()


def test_loupe_navigation_in_semantic_search_probability_rank_order(server, page: Page):
    """Loupe viewer navigates sequentially through semantic search results in probability rank order."""
    page.route("**/api/semantic/status*", lambda route: route.fulfill(
        status=200,
        content_type="application/json",
        body=json.dumps({"ready": True, "built": True, "indexSize": 6, "modelId": "clip-vit-base"})
    ))

    def handle_semantic_search(route):
        media_items = [
            {"id": 4, "file_name": "sunset.bmp", "date_taken": 1770000000, "similarity_score": 0.94},
            {"id": 1, "file_name": "beach.bmp", "date_taken": 1768000000, "similarity_score": 0.88},
            {"id": 5, "file_name": "forest.bmp", "date_taken": 1765000000, "similarity_score": 0.72},
        ]
        items = [
            {"mediaId": 4, "score": 0.94},
            {"mediaId": 1, "score": 0.88},
            {"mediaId": 5, "score": 0.72},
        ]
        route.fulfill(
            status=200,
            content_type="application/json",
            body=json.dumps({"items": items, "media_items": media_items, "total": 3})
        )

    page.route("**/api/semantic/search*", handle_semantic_search)
    page.goto(server["url"])

    page.locator("#semanticToggle").click()
    page.locator("#searchInput").fill("golden beach sunset")
    page.locator("#searchInput").press("Enter")

    search_cards = page.locator(".photo-card")
    expect(search_cards).to_have_count(3)

    # Double click the rank #1 card to open Loupe
    search_cards.nth(0).dblclick()
    loupe_modal = page.locator("#loupeModal")
    expect(loupe_modal).to_be_visible()

    # Loupe index indicates Rank #1 of 3
    expect(page.locator("#loupeFileName")).to_have_text("sunset.bmp")
    expect(page.locator("#loupeIndex")).to_contain_text("Rank #1 of 3")
    expect(page.locator("#loupeIndex")).to_contain_text("golden beach sunset")

    # Press ArrowRight to move to Rank #2
    page.keyboard.press("ArrowRight")
    expect(page.locator("#loupeFileName")).to_have_text("beach.bmp")
    expect(page.locator("#loupeIndex")).to_contain_text("Rank #2 of 3")

    # Press ArrowRight to move to Rank #3
    page.keyboard.press("ArrowRight")
    expect(page.locator("#loupeFileName")).to_have_text("forest.bmp")
    expect(page.locator("#loupeIndex")).to_contain_text("Rank #3 of 3")

    # Press ArrowLeft to go back to Rank #2
    page.keyboard.press("ArrowLeft")
    expect(page.locator("#loupeFileName")).to_have_text("beach.bmp")
    expect(page.locator("#loupeIndex")).to_contain_text("Rank #2 of 3")

    # Press Escape closes Loupe, preserving the search grid
    page.keyboard.press("Escape")
    expect(loupe_modal).to_be_hidden()
    expect(page.locator(".semantic-search-grid")).to_be_visible()
    expect(page.locator(".photo-card")).to_have_count(3)


def test_close_semantic_search_restores_normal_grid(server, page: Page):
    """Clicking close search button restores normal grid view, sort selector, timeline, and date headers."""
    page.route("**/api/semantic/status*", lambda route: route.fulfill(
        status=200,
        content_type="application/json",
        body=json.dumps({"ready": True, "built": True, "indexSize": 6, "modelId": "clip-vit-base"})
    ))

    def handle_semantic_search(route):
        media_items = [
            {"id": 4, "file_name": "sunset.bmp", "date_taken": 1770000000, "similarity_score": 0.94},
            {"id": 1, "file_name": "beach.bmp", "date_taken": 1768000000, "similarity_score": 0.88},
        ]
        items = [
            {"mediaId": 4, "score": 0.94},
            {"mediaId": 1, "score": 0.88},
        ]
        route.fulfill(
            status=200,
            content_type="application/json",
            body=json.dumps({"items": items, "media_items": media_items, "total": 2})
        )

    page.route("**/api/semantic/search*", handle_semantic_search)
    page.goto(server["url"])

    page.locator("#semanticToggle").click()
    page.locator("#searchInput").fill("sunset")
    page.locator("#searchInput").press("Enter")

    expect(page.locator(".photo-card")).to_have_count(2)
    expect(page.locator("#semanticSearchToolbar")).to_be_visible()
    expect(page.locator(".sort-selector")).to_be_hidden()
    expect(page.locator(".bottom-scrub-bar")).to_be_hidden()

    # Click Close Search button
    page.locator("#closeSemanticSearchBtn").click()

    # Toolbar hidden, controls restored
    expect(page.locator("#semanticSearchToolbar")).to_be_hidden()
    expect(page.locator(".sort-selector")).to_be_visible()
    expect(page.locator(".bottom-scrub-bar")).to_be_visible()
    expect(page.locator("#filterIndicator")).to_be_visible()
    expect(page.locator("#searchInput")).to_have_value("")

    # Normal grid restored with date groups and 6 photos
    expect(page.locator(".photo-card")).to_have_count(6)
    expect(page.locator(".date-group")).to_have_count(3)
    expect(page.locator(".similarity-rank-badge")).to_have_count(0)


def test_escape_key_closes_semantic_search_grid(server, page: Page):
    """Pressing Escape when Loupe is closed exits semantic search and restores normal grid."""
    page.route("**/api/semantic/status*", lambda route: route.fulfill(
        status=200,
        content_type="application/json",
        body=json.dumps({"ready": True, "built": True, "indexSize": 6, "modelId": "clip-vit-base"})
    ))

    def handle_semantic_search(route):
        media_items = [
            {"id": 4, "file_name": "sunset.bmp", "date_taken": 1770000000, "similarity_score": 0.94},
        ]
        items = [
            {"mediaId": 4, "score": 0.94},
        ]
        route.fulfill(
            status=200,
            content_type="application/json",
            body=json.dumps({"items": items, "media_items": media_items, "total": 1})
        )

    page.route("**/api/semantic/search*", handle_semantic_search)
    page.goto(server["url"])

    page.locator("#semanticToggle").click()
    page.locator("#searchInput").fill("sunset")
    page.locator("#searchInput").press("Enter")

    expect(page.locator(".photo-card")).to_have_count(1)
    expect(page.locator("#semanticSearchToolbar")).to_be_visible()

    # Unfocus search input to simulate pressing Escape anywhere in grid
    page.locator(".photo-card").first.click()

    # First Escape clears card selection
    page.keyboard.press("Escape")

    # Second Escape closes search
    page.keyboard.press("Escape")

    expect(page.locator("#semanticSearchToolbar")).to_be_hidden()
    expect(page.locator(".photo-card")).to_have_count(6)
    expect(page.locator(".date-group")).to_have_count(3)


