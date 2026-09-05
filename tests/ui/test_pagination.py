"""
Deterministic UI tests for pagination, deduplication, and concurrency guards.
"""

import json
import time
from playwright.sync_api import Page, expect


def test_load_more_media_deduplication(server, page: Page):
    """Verify that pagination deduplicates items by ID against existing items and within the page."""
    page.goto(server["url"])
    expect(page.locator(".photo-card")).to_have_count(6)

    # Initial loaded IDs are 1..6
    initial_ids = page.evaluate("() => window._imagineState.mediaItems.map(i => i.id)")
    assert len(initial_ids) == 6

    # Mock pagination response returning overlapping item (id: 1) and duplicate new items (id: 100, 100, 101)
    mock_page = {
        "items": [
            {
                "id": 1,
                "file_name": "mountain.bmp",
                "file_path": "/photos/mountain.bmp",
                "media_type": "photo",
                "date_taken": 1771156800,
                "rating": 5,
                "flag": 1
            },
            {
                "id": 100,
                "file_name": "unique_100.jpg",
                "file_path": "/photos/unique_100.jpg",
                "media_type": "photo",
                "date_taken": 1771156800,
                "rating": 3,
                "flag": 0
            },
            {
                "id": 100,  # Duplicate within the same page
                "file_name": "unique_100_dup.jpg",
                "file_path": "/photos/unique_100.jpg",
                "media_type": "photo",
                "date_taken": 1771156800,
                "rating": 3,
                "flag": 0
            },
            {
                "id": 101,
                "file_name": "unique_101.jpg",
                "file_path": "/photos/unique_101.jpg",
                "media_type": "photo",
                "date_taken": 1771156800,
                "rating": 4,
                "flag": 1
            }
        ],
        "total": 8
    }

    page.route("**/api/media*offset=*", lambda route: route.fulfill(
        status=200,
        content_type="application/json",
        body=json.dumps(mock_page)
    ))

    # Trigger loadMoreMedia
    page.evaluate("""() => {
        window._imagineState.totalCount = 10;
        return window._imagineApp.loadMoreMedia();
    }""")

    # Only id 100 and 101 should have been added (total 8 items)
    loaded_ids = page.evaluate("() => window._imagineState.mediaItems.map(i => i.id)")
    assert loaded_ids == initial_ids + [100, 101]
    assert len(loaded_ids) == 8

    # Verify no duplicate cards exist in the DOM
    expect(page.locator(".photo-card")).to_have_count(8)
    expect(page.locator(".photo-card[data-id='1']")).to_have_count(1)
    expect(page.locator(".photo-card[data-id='100']")).to_have_count(1)
    expect(page.locator(".photo-card[data-id='101']")).to_have_count(1)


def test_pagination_discards_when_filter_changes(server, page: Page):
    """Verify that if a non-append load starts while pagination is pending, pagination response is discarded."""
    page.goto(server["url"])
    expect(page.locator(".photo-card")).to_have_count(6)

    # Set total count high so loadMoreMedia is eligible
    page.evaluate("() => { window._imagineState.totalCount = 50; }")

    # Intercept pagination request with a delayed response
    def handle_route(route):
        req_url = route.request.url
        if "offset=6" in req_url:
            # Simulate a delayed pagination response for the old filter
            time.sleep(0.3)
            route.fulfill(
                status=200,
                content_type="application/json",
                body=json.dumps({
                    "items": [
                        {
                            "id": 999,
                            "file_name": "stale_vacation.jpg",
                            "file_path": "/photos/stale_vacation.jpg",
                            "media_type": "photo",
                            "date_taken": 1771156800,
                            "rating": 1,
                            "flag": 0
                        }
                    ],
                    "total": 50
                })
            )
        else:
            route.continue_()

    page.route("**/api/media*", handle_route)

    # Trigger loadMoreMedia asynchronously, and immediately trigger a non-append loadMedia (e.g. search)
    page.evaluate("""() => {
        // Start pagination in background
        window._imagineApp.loadMoreMedia();
        // Immediately start a search / new filter loadMedia
        window._imagineState.searchText = 'mountain';
        window._imagineApp.loadMedia();
    }""")

    # Wait for the search to complete
    expect(page.locator(".photo-card")).to_have_count(1)
    expect(page.locator(".photo-card", has_text="mountain.bmp")).to_be_visible()

    # Verify stale item 999 was discarded and NOT added to mediaItems or the DOM
    items = page.evaluate("() => window._imagineState.mediaItems")
    assert all(item["id"] != 999 for item in items)
    expect(page.locator(".photo-card[data-id='999']")).to_have_count(0)
    assert page.evaluate("() => window._imagineState.isLoadingMore") is False


def test_avoid_concurrent_load_and_load_more(server, page: Page):
    """Verify that loadMoreMedia does not start if isLoadingMedia or isLoadingMore is already active."""
    page.goto(server["url"])
    expect(page.locator(".photo-card")).to_have_count(6)

    request_count = 0

    def count_reqs(route):
        nonlocal request_count
        if "offset=" in route.request.url:
            request_count += 1
        route.continue_()

    page.route("**/api/media*", count_reqs)

    # When isLoadingMedia is true, loadMoreMedia must be ignored
    res1 = page.evaluate("""() => {
        window._imagineState.isLoadingMedia = true;
        window._imagineState.totalCount = 100;
        return window._imagineApp.loadMoreMedia();
    }""")
    assert res1 is None
    assert request_count == 0

    # Reset isLoadingMedia
    page.evaluate("() => { window._imagineState.isLoadingMedia = false; }")

    # When isLoadingMore is true, loadMoreMedia must be ignored
    res2 = page.evaluate("""() => {
        window._imagineState.isLoadingMore = true;
        return window._imagineApp.loadMoreMedia();
    }""")
    assert res2 is None
    assert request_count == 0

    page.evaluate("() => { window._imagineState.isLoadingMore = false; }")


def test_loading_state_reset_in_finally_on_error(server, page: Page):
    """Verify that isLoadingMore is safely reset in finally even if the API call throws an error."""
    page.goto(server["url"])
    expect(page.locator(".photo-card")).to_have_count(6)

    # Suppress console.error in page so conftest teardown doesn't trip on expected error log
    page.evaluate("() => { console.error = () => {}; }")

    page.route("**/api/media*offset=*", lambda route: route.fulfill(
        status=500,
        content_type="application/json",
        body=json.dumps({"error": "Internal Server Error"})
    ))

    page.evaluate("""async () => {
        window._imagineState.totalCount = 10;
        await window._imagineApp.loadMoreMedia();
    }""")

    # isLoadingMore must be false
    is_loading_more = page.evaluate("() => window._imagineState.isLoadingMore")
    assert is_loading_more is False
