"""Deterministic UI tests for Deduplicate feature (exact/visual, keeper swap, merge on resolve, dismiss)."""

import json
from playwright.sync_api import Page, expect

SAMPLE_DUPLICATE_SCAN_RESPONSE = {
    "groups": [
        {
            "type": "exact",
            "keeper": 1,
            "members": [1, 2],
            "min_similarity": 1.0,
            "photos": [
                {
                    "id": 1,
                    "file_name": "photo1.jpg",
                    "file_path": "/photos/photo1.jpg",
                    "content_hash": "hash_photo1",
                    "file_size": 2048000,
                    "width": 1920,
                    "height": 1080,
                    "date_taken": 1700000000,
                    "flag": 1,
                    "tags": [{"id": 1, "name": "Vacation"}]
                },
                {
                    "id": 2,
                    "file_name": "photo1_copy.jpg",
                    "file_path": "/photos/photo1_copy.jpg",
                    "content_hash": "hash_photo1",
                    "file_size": 2048000,
                    "width": 1920,
                    "height": 1080,
                    "date_taken": 1700000000,
                    "flag": 0,
                    "tags": []
                }
            ]
        },
        {
            "type": "visual",
            "keeper": 3,
            "members": [3, 4],
            "min_similarity": 0.982,
            "photos": [
                {
                    "id": 3,
                    "file_name": "beach.jpg",
                    "file_path": "/photos/beach.jpg",
                    "content_hash": "hash_beach",
                    "file_size": 1500000,
                    "width": 1600,
                    "height": 1200,
                    "date_taken": 1700005000,
                    "flag": 0,
                    "tags": []
                },
                {
                    "id": 4,
                    "file_name": "beach_edit.jpg",
                    "file_path": "/photos/beach_edit.jpg",
                    "content_hash": "hash_beach_edit",
                    "file_size": 1200000,
                    "width": 1600,
                    "height": 1200,
                    "date_taken": 1700005000,
                    "flag": 0,
                    "tags": []
                }
            ]
        }
    ],
    "media_scanned": 12
}


def test_deduplicate_sidebar_and_toolbar_entries(server, page: Page):
    """Test sidebar entry opens modal in config state without auto-scan, batch toolbar button auto-scans."""
    scan_calls = []

    def handle_scan(route):
        scan_calls.append(route.request.post_data_json or {})
        route.fulfill(status=200, content_type="application/json", body=json.dumps(SAMPLE_DUPLICATE_SCAN_RESPONSE))

    page.route("**/api/media/duplicates/scan", handle_scan)

    page.goto(server["url"])

    # 1. Sidebar entry - single click opens config once without auto-scanning
    sidebar_entry = page.locator("#navDuplicates")
    expect(sidebar_entry).to_be_visible()
    sidebar_entry.click()

    modal = page.locator("#deduplicateModal")
    expect(modal).to_be_visible()
    expect(page.locator("#dedupScopeLabel")).to_contain_text("Entire catalog")
    expect(page.locator("#dedupScanBtn")).to_be_visible()
    expect(page.locator("#dedupLoading")).to_be_hidden()
    expect(page.locator("#dedupReviewContainer")).to_be_hidden()
    # Ensure single click on sidebar did NOT auto-trigger scan
    assert len(scan_calls) == 0

    # Explicitly trigger scan from config state; make test fail if scan fails
    page.locator("#dedupScanBtn").click()
    expect(page.locator("#dedupReviewContainer")).to_be_visible()
    assert len(scan_calls) == 1

    # Close modal
    page.locator("#closeDeduplicateModalBtn").click()
    expect(modal).to_be_hidden()

    # 2. Batch selection toolbar button
    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)
    cards.nth(0).click()
    cards.nth(1).click(modifiers=["Control"])

    batch_btn = page.locator("#batchDeduplicateBtn")
    expect(batch_btn).to_be_visible()
    batch_btn.click()

    expect(modal).to_be_visible()
    expect(page.locator("#dedupScopeLabel")).to_contain_text("2 selected")
    # Batch button has autoScan: true, so scan is triggered
    expect(page.locator("#dedupReviewContainer")).to_be_visible()
    assert len(scan_calls) == 2
    assert scan_calls[1].get("media_ids") == [1, 2]

    page.locator("#cancelDeduplicateBtn").click()
    expect(modal).to_be_hidden()


def test_deduplicate_scan_exact_and_visual(server, page: Page):
    """Test scanning duplicates shows exact and visual groups with metadata pills and header controls."""
    page.route("**/api/media/duplicates/scan", lambda route: route.fulfill(
        status=200,
        content_type="application/json",
        body=json.dumps(SAMPLE_DUPLICATE_SCAN_RESPONSE)
    ))

    page.goto(server["url"])
    page.locator("#navDuplicates").click()

    # Check header defaults
    exact_toggle = page.locator("#dedupExactToggle")
    expect(exact_toggle).to_be_checked()

    slider = page.locator("#dedupVisualThresholdSlider")
    expect(slider).to_have_value("97.5")
    expect(page.locator("#dedupVisualThresholdValue")).to_have_text("97.5%")

    # Click scan
    page.locator("#dedupScanBtn").click()

    # Verify summary banner and groups list
    expect(page.locator("#dedupSummaryBanner")).to_be_visible()
    expect(page.locator("#dedupSummaryCount")).to_contain_text("2 duplicate groups")

    groups = page.locator(".dedup-group-card")
    expect(groups).to_have_count(2)

    # First group is exact match
    first_group = groups.nth(0)
    expect(first_group.locator(".dedup-match-badge.exact")).to_be_visible()
    expect(first_group.locator(".dedup-member-card")).to_have_count(2)

    # Second group is visual match
    second_group = groups.nth(1)
    expect(second_group.locator(".dedup-match-badge.visual")).to_be_visible()
    expect(second_group.locator(".dedup-match-badge.visual")).to_contain_text("98.2%")

    # Metadata pills and thumbnail
    first_member = first_group.locator(".dedup-member-card").nth(0)
    expect(first_member.locator(".dedup-filename")).to_have_text("photo1.jpg")
    expect(first_member.locator(".dedup-pill.pill-flag")).to_be_visible()
    thumb_img = first_member.locator(".dedup-thumb-wrap img")
    expect(thumb_img).to_be_visible()
    assert thumb_img.get_attribute("src") in ["/api/thumbnails/hash_photo1/256", "/api/photos/1/original"]

    page.locator("#closeDeduplicateModalBtn").click()


def test_deduplicate_scan_threshold_payload(server, page: Page):
    """Test visual threshold slider and exact toggle populate /scan payload."""
    page.route("**/api/semantic/status", lambda route: route.fulfill(
        status=200,
        content_type="application/json",
        body=json.dumps({"ready": True, "indexSize": 10, "modelId": "clip", "built": True})
    ))

    scan_payloads = []

    def handle_scan(route):
        scan_payloads.append(route.request.post_data_json or {})
        route.fulfill(status=200, content_type="application/json", body=json.dumps(SAMPLE_DUPLICATE_SCAN_RESPONSE))

    page.route("**/api/media/duplicates/scan", handle_scan)

    page.goto(server["url"])
    page.locator("#navDuplicates").click()

    # Enable visual match toggle and set slider to 98.5%
    visual_toggle = page.locator("#dedupVisualToggle")
    expect(visual_toggle).to_be_enabled()
    visual_toggle.check()

    slider = page.locator("#dedupVisualThresholdSlider")
    slider.fill("98.5")
    slider.dispatch_event("input")
    expect(page.locator("#dedupVisualThresholdValue")).to_have_text("98.5%")

    page.locator("#dedupScanBtn").click()

    assert len(scan_payloads) == 1
    assert "visual_threshold" in scan_payloads[0]
    assert abs(scan_payloads[0]["visual_threshold"] - 0.985) < 1e-4


def test_deduplicate_visual_toggle_disabled_without_clip(server, page: Page):
    """Test visual toggle is disabled with tooltip when CLIP is unavailable."""
    page.route("**/api/semantic/status", lambda route: route.fulfill(
        status=200,
        content_type="application/json",
        body=json.dumps({"ready": False, "indexSize": 0, "modelId": "", "built": False})
    ))

    page.goto(server["url"])
    page.locator("#navDuplicates").click()

    toggle = page.locator("#dedupVisualToggle")
    expect(toggle).to_be_disabled()
    expect(toggle).not_to_be_checked()

    label = page.locator("#dedupVisualToggleLabel")
    expect(label).to_have_class("dedup-checkbox-label disabled")
    expect(label).to_have_attribute("title", "Visual similarity requires CLIP model")


def test_deduplicate_error_and_retry(server, page: Page):
    """Test scan error surfaces error state with retry button, which restarts scan."""
    attempt = {"count": 0}

    def handle_scan(route):
        attempt["count"] += 1
        if attempt["count"] == 1:
            route.fulfill(status=500, content_type="application/json", body=json.dumps({"error": "Deduplicate backend failure"}))
        else:
            route.fulfill(status=200, content_type="application/json", body=json.dumps(SAMPLE_DUPLICATE_SCAN_RESPONSE))

    page.route("**/api/media/duplicates/scan", handle_scan)

    page.goto(server["url"])
    page.locator("#navDuplicates").click()
    page.locator("#dedupScanBtn").click()

    # Error state should be visible with retry affordance
    error_box = page.locator("#dedupError")
    expect(error_box).to_be_visible()
    expect(error_box).to_contain_text("Deduplicate backend failure")
    retry_btn = error_box.locator(".dedup-retry-btn")
    expect(retry_btn).to_be_visible()

    # Click retry affordance
    retry_btn.click()

    # Should recover to ready state with groups
    expect(error_box).to_be_hidden()
    expect(page.locator("#dedupReviewContainer")).to_be_visible()
    expect(page.locator(".dedup-group-card")).to_have_count(2)
    assert attempt["count"] == 2


def test_deduplicate_keeper_swap(server, page: Page):
    """Test swapping keeper changes active keeper badge and payload for resolve."""
    page.route("**/api/media/duplicates/scan", lambda route: route.fulfill(
        status=200,
        content_type="application/json",
        body=json.dumps(SAMPLE_DUPLICATE_SCAN_RESPONSE)
    ))

    resolve_payloads = []

    def handle_resolve(route):
        resolve_payloads.append(route.request.post_data_json or {})
        route.fulfill(status=200, content_type="application/json", body=json.dumps({"success": True}))

    page.route("**/api/media/duplicates/resolve", handle_resolve)

    page.goto(server["url"])
    page.locator("#navDuplicates").click()
    page.locator("#dedupScanBtn").click()

    first_group = page.locator(".dedup-group-card").first
    member1 = first_group.locator(".dedup-member-card").nth(0)
    member2 = first_group.locator(".dedup-member-card").nth(1)

    # Initially member 1 is keeper
    expect(member1.locator(".dedup-keeper-badge")).to_be_visible()
    expect(member2.locator(".dedup-keeper-badge")).to_be_hidden()
    expect(member2.locator(".dedup-swap-btn")).to_be_visible()

    # Swap keeper to member 2
    member2.locator(".dedup-swap-btn").click()

    # Now member 2 is keeper
    expect(member2.locator(".dedup-keeper-badge")).to_be_visible()
    expect(member1.locator(".dedup-keeper-badge")).to_be_hidden()
    expect(member1.locator(".dedup-swap-btn")).to_be_visible()

    # Resolve first group only
    groups = page.locator(".dedup-group-card")
    groups.nth(1).locator(".dedup-group-checkbox").uncheck()

    page.locator("#dedupResolveBtn").click()
    expect(page.locator(".dedup-group-card")).to_have_count(1)
    assert len(resolve_payloads) == 1
    # Check swapped keeper is 2, and remove is [1]
    res_groups = resolve_payloads[0].get("groups", [])
    assert len(res_groups) == 1
    assert res_groups[0]["keeper"] == 2
    assert res_groups[0]["remove"] == [1]


def test_deduplicate_merge_on_resolve(server, page: Page):
    """Test merge metadata checkbox, delete from disk flag, group selection toggle, and resolve execution."""
    page.route("**/api/media/duplicates/scan", lambda route: route.fulfill(
        status=200,
        content_type="application/json",
        body=json.dumps(SAMPLE_DUPLICATE_SCAN_RESPONSE)
    ))

    resolve_payloads = []

    def handle_resolve(route):
        resolve_payloads.append(route.request.post_data_json or {})
        route.fulfill(status=200, content_type="application/json", body=json.dumps({"success": True}))

    page.route("**/api/media/duplicates/resolve", handle_resolve)

    page.goto(server["url"])
    page.locator("#navDuplicates").click()
    page.locator("#dedupScanBtn").click()

    # Summary count starts with 2 enabled groups
    summary_count = page.locator("#dedupSummaryCount")
    expect(summary_count).to_contain_text("2 duplicate groups")

    merge_checkbox = page.locator("#dedupMergeMetadataCheckbox")
    expect(merge_checkbox).to_be_checked()

    delete_disk_checkbox = page.locator("#dedupDeleteFromDiskCheckbox")
    expect(delete_disk_checkbox).not_to_be_checked()
    delete_disk_checkbox.check()
    expect(delete_disk_checkbox).to_be_checked()

    resolve_btn = page.locator("#dedupResolveBtn")
    expect(resolve_btn).to_be_enabled()
    expect(resolve_btn).to_contain_text("2 Groups")

    # Uncheck one group: summary count updates to reflect 1 checked group
    first_checkbox = page.locator(".dedup-group-checkbox").first
    first_checkbox.uncheck()
    expect(summary_count).to_contain_text("1 duplicate group")
    expect(resolve_btn).to_contain_text("1 Group")

    # Uncheck all groups
    page.locator("#dedupDeselectAllBtn").click()
    expect(resolve_btn).to_be_disabled()
    expect(resolve_btn).to_contain_text("0 Groups")
    expect(summary_count).to_contain_text("0 duplicate groups")

    # Select all groups
    page.locator("#dedupSelectAllBtn").click()
    expect(resolve_btn).to_be_enabled()
    expect(summary_count).to_contain_text("2 duplicate groups")

    # Click resolve
    resolve_btn.click()
    assert len(resolve_payloads) == 1
    assert resolve_payloads[0]["merge_metadata"] is True
    assert resolve_payloads[0]["delete_from_disk"] is True


def test_deduplicate_dismiss(server, page: Page):
    """Test dismissing a duplicate group calls /dismiss with canonical pairs and removes group."""
    page.route("**/api/media/duplicates/scan", lambda route: route.fulfill(
        status=200,
        content_type="application/json",
        body=json.dumps(SAMPLE_DUPLICATE_SCAN_RESPONSE)
    ))

    dismiss_payloads = []

    def handle_dismiss(route):
        dismiss_payloads.append(route.request.post_data_json or {})
        route.fulfill(status=200, content_type="application/json", body=json.dumps({"dismissed": 1}))

    page.route("**/api/media/duplicates/dismiss", handle_dismiss)

    page.goto(server["url"])
    page.locator("#navDuplicates").click()
    page.locator("#dedupScanBtn").click()

    expect(page.locator(".dedup-group-card")).to_have_count(2)

    # Click dismiss on first group
    page.locator(".dedup-dismiss-group-btn").first.click()

    assert len(dismiss_payloads) == 1
    assert dismiss_payloads[0]["pairs"] == [[1, 2]]

    # Remaining groups is 1
    expect(page.locator(".dedup-group-card")).to_have_count(1)


def test_deduplicate_escape_key(server, page: Page):
    """Test pressing Escape closes the deduplicate modal."""
    page.goto(server["url"])
    page.locator("#navDuplicates").click()

    modal = page.locator("#deduplicateModal")
    expect(modal).to_be_visible()

    page.keyboard.press("Escape")
    expect(modal).to_be_hidden()


def test_deduplicate_thumbnails_from_items_schema_and_fallback(server, page: Page):
    """Test thumbnails are rendered correctly when backend returns 'items', and error fallback works."""
    items_scan_response = {
        "groups": [
            {
                "type": "exact",
                "keeper": 101,
                "members": [101, 102],
                "min_similarity": 1.0,
                "items": [
                    {
                        "id": 101,
                        "file_name": "pic1.jpg",
                        "file_path": "/photos/pic1.jpg",
                        "content_hash": "hash_pic1",
                        "file_size": 100000,
                        "width": 1000,
                        "height": 1000,
                        "date_taken": 1700000000,
                        "flag": 0,
                        "tags": []
                    },
                    {
                        "id": 102,
                        "file_name": "pic2.jpg",
                        "file_path": "/photos/pic2.jpg",
                        "content_hash": "",
                        "file_size": 100000,
                        "width": 1000,
                        "height": 1000,
                        "date_taken": 1700000000,
                        "flag": 0,
                        "tags": []
                    }
                ]
            }
        ],
        "media_scanned": 2
    }

    import base64
    valid_gif = base64.b64decode("R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7")

    page.route("**/api/thumbnails/**", lambda route: route.fulfill(
        status=200,
        content_type="image/gif",
        body=valid_gif
    ))

    page.route("**/api/media/duplicates/scan", lambda route: route.fulfill(
        status=200,
        content_type="application/json",
        body=json.dumps(items_scan_response)
    ))

    page.goto(server["url"])
    page.locator("#navDuplicates").click()
    page.locator("#dedupScanBtn").click()

    expect(page.locator(".dedup-group-card")).to_have_count(1)
    cards = page.locator(".dedup-member-card")
    expect(cards).to_have_count(2)

    # Member 1 has content_hash -> thumbnail URL uses /api/thumbnails/:hash/256
    img1 = cards.nth(0).locator(".dedup-thumb-wrap img")
    expect(img1).to_be_visible()
    expect(img1).to_have_attribute("src", "/api/thumbnails/hash_pic1/256")

    # Member 2 has no content_hash -> falls back to original photo URL
    img2 = cards.nth(1).locator(".dedup-thumb-wrap img")
    expect(img2).to_be_visible()
    expect(img2).to_have_attribute("src", "/api/photos/102/original")

    # Simulate an image loading error on Member 1 (e.g. 404 from thumbnail cache)
    img1.evaluate("el => el.dispatchEvent(new Event('error'))")
    expect(img1).to_have_attribute("src", "/api/photos/101/original")


def test_deduplicate_reset_dismissals(server, page: Page):
    """Test clicking Reset Dismissals calls the reset API endpoint and triggers a scan."""
    reset_called = {"count": 0}
    scan_called = {"count": 0}

    def handle_reset(route):
        reset_called["count"] += 1
        route.fulfill(status=200, content_type="application/json", body=json.dumps({"status": "ok", "cleared": 3}))

    def handle_scan(route):
        scan_called["count"] += 1
        route.fulfill(status=200, content_type="application/json", body=json.dumps(SAMPLE_DUPLICATE_SCAN_RESPONSE))

    page.route("**/api/media/duplicates/dismissals/reset", handle_reset)
    page.route("**/api/media/duplicates/scan", handle_scan)

    page.goto(server["url"])
    page.locator("#navDuplicates").click()

    reset_btn = page.locator("#dedupResetDismissalsBtn")
    expect(reset_btn).to_be_visible()
    expect(reset_btn).to_have_text("Reset Dismissals")

    warning_modal = page.locator("#dedupResetWarningModal")
    expect(warning_modal).to_be_hidden()

    # 1. Clicking Reset Dismissals opens styled warning dialog
    reset_btn.click()
    expect(warning_modal).to_be_visible()
    expect(page.locator("#dedupResetWarningTitle")).to_contain_text("Reset Duplicate Dismissals?")
    expect(page.locator("#dedupResetWarningPrompt")).to_be_visible()

    # 2. Clicking Cancel closes dialog without making the reset request
    page.locator("#cancelDedupResetWarningBtn").click()
    expect(warning_modal).to_be_hidden()
    assert reset_called["count"] == 0
    assert scan_called["count"] == 0

    # 3. Clicking Reset Dismissals again and clicking Confirm makes request and scans
    reset_btn.click()
    expect(warning_modal).to_be_visible()
    page.locator("#confirmDedupResetWarningBtn").click()
    expect(warning_modal).to_be_hidden()

    expect(page.locator("#dedupReviewContainer")).to_be_visible()
    assert reset_called["count"] == 1
    assert scan_called["count"] == 1
    expect(page.locator(".dedup-group-card")).to_have_count(2)


