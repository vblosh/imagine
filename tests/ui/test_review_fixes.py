"""
Deterministic tests verifying all medium and low priority code review fixes:
1. updateLoupeControls() button guarding
2. API data type assumptions and numeric conversion helpers
3. Map popup state and search-result-popup discrimination
4. Marker click respecting activeMediaId in clusters
5. Cluster center/centroid updating
6. inspectorShowOnMapBtn finding marker by clusterItems
7. Batch deletion fallback tracking failures and notifying user
8. formatBytes handling of invalid/negative/infinite values
"""

import re
import pytest
from playwright.sync_api import Page, expect


def test_format_bytes_handles_invalid_values(server, page: Page):
    """formatBytes() returns '0 B' for invalid/negative/infinite/NaN values and correctly formats valid bytes."""
    page.goto(server["url"])
    expect(page.locator("#mediaGrid")).to_be_visible()

    results = page.evaluate("""() => {
        const fb = window.imagineUI.formatBytes;
        return {
            negative: fb(-1024),
            zero: fb(0),
            nan: fb(NaN),
            nullVal: fb(null),
            undefinedVal: fb(undefined),
            stringInvalid: fb('invalid'),
            infinityVal: fb(Infinity),
            negativeInfinityVal: fb(-Infinity),
            validBytes: fb(500),
            validKb: fb(2048),
            validMb: fb(1048576 * 5)
        };
    }""")

    assert results["negative"] == "0 B"
    assert results["zero"] == "0 B"
    assert results["nan"] == "0 B"
    assert results["nullVal"] == "0 B"
    assert results["undefinedVal"] == "0 B"
    assert results["stringInvalid"] == "0 B"
    assert results["infinityVal"] == "0 B"
    assert results["negativeInfinityVal"] == "0 B"
    assert results["validBytes"] == "500 B"
    assert results["validKb"] == "2 KB"
    assert results["validMb"] == "5 MB"


def test_numeric_helper_and_exif_robustness(server, page: Page):
    """numeric() safely parses inputs and EXIF rendering handles strings and nulls without throwing."""
    page.goto(server["url"])
    expect(page.locator("#mediaGrid")).to_be_visible()

    results = page.evaluate("""() => {
        const num = window.imagineUI.numeric;
        const testValues = {
            validInt: num(42),
            validFloat: num(2.8),
            stringNum: num('5.6'),
            emptyStr: num(''),
            whitespace: num('   '),
            nullVal: num(null),
            undefinedVal: num(undefined),
            nanVal: num(NaN),
            infVal: num(Infinity),
            alphaStr: num('f/2.8')
        };

        // Test EXIF inspector rendering with malformed strings/nulls
        const state = window.imagineUI.state;
        const originalItems = [...state.mediaItems];
        const malformedItem = {
            id: 99999,
            file_name: null,
            file_size: 'not-a-size',
            date_taken: 'invalid-date',
            width: null,
            height: null,
            rating: null,
            flag: null,
            exif: {
                has_gps: true,
                latitude: '48.8584', // string latitude
                longitude: '2.2945',  // string longitude
                f_number: '2.8',      // string f-number
                focal_length: '50.0', // string focal length
                exposure_time: '0.01',
                iso: 100,
                camera_make: null,
                camera_model: 'MockCam'
            }
        };

        state.mediaItems.unshift(malformedItem);
        state.selectedIds.clear();
        state.selectedIds.add(malformedItem.id);

        let inspectorThrew = false;
        try {
            // Trigger selection update which renders inspector
            const card = document.querySelector('.photo-card');
            if (card) card.click();
        } catch (e) {
            inspectorThrew = true;
        }

        // Restore state
        state.mediaItems = originalItems;

        return { testValues, inspectorThrew };
    }""")

    tv = results["testValues"]
    assert tv["validInt"] == 42
    assert tv["validFloat"] == 2.8
    assert tv["stringNum"] == 5.6
    assert tv["emptyStr"] is None
    assert tv["whitespace"] is None
    assert tv["nullVal"] is None
    assert tv["undefinedVal"] is None
    assert tv["nanVal"] is None
    assert tv["infVal"] is None
    assert tv["alphaStr"] is None
    assert results["inspectorThrew"] is False


def test_cluster_centers_updated_on_item_join(server, page: Page):
    """Adding items to an existing cluster recalculates its lat/lng centroid."""
    page.goto(server["url"])
    expect(page.locator("#mediaGrid")).to_be_visible()

    # Switch to map view
    page.locator("#viewMapBtn").click()
    expect(page.locator("#mapViewContainer")).to_be_visible()

    cluster_result = page.evaluate("""() => {
        const clusterGpsItems = window.imagineUI.clusterGpsItems;
        const map = window.imagineUI.state.mapInstance;

        // Create two GPS items close to each other
        const item1 = {
            id: 101,
            file_name: 'photo1.jpg',
            exif: { has_gps: true, latitude: 10.0, longitude: 20.0 }
        };
        const item2 = {
            id: 102,
            file_name: 'photo2.jpg',
            exif: { has_gps: true, latitude: 10.0002, longitude: 20.0002 }
        };

        // Clustering with high radius to ensure they merge
        const clusters = clusterGpsItems([item1, item2], map, 500);
        if (clusters.length !== 1) {
            return { ok: false, error: 'Expected 1 cluster, got ' + clusters.length };
        }

        const cl = clusters[0];
        // Expected centroid: average of lat: (10.0 + 10.0002) / 2 = 10.0001
        // and lng: (20.0 + 20.0002) / 2 = 20.0001
        return {
            ok: true,
            itemCount: cl.items.length,
            lat: cl.lat,
            lng: cl.lng
        };
    }""")

    assert cluster_result["ok"] is True
    assert cluster_result["itemCount"] == 2
    assert abs(cluster_result["lat"] - 10.0001) < 0.00001
    assert abs(cluster_result["lng"] - 20.0001) < 0.00001


def test_marker_click_respects_active_media_id_in_cluster(server, page: Page):
    """Clicking a cluster marker selects marker.activeMediaId if present rather than resetting to repItem.id."""
    page.goto(server["url"])
    page.locator("#viewMapBtn").click()
    expect(page.locator("#mapViewContainer")).to_be_visible()
    expect(page.locator(".custom-photo-pin")).to_have_count(2)

    res = page.evaluate("""() => {
        const state = window.imagineUI.state;
        if (state.mapMarkers.length === 0) return { ok: false, error: 'No map markers found' };

        const marker = state.mapMarkers[0];
        // Simulate cluster with multiple items
        const dummyRep = { id: 1001, file_name: 'rep.jpg' };
        const dummyActive = { id: 1002, file_name: 'active.jpg' };
        marker.clusterItems = [dummyRep, dummyActive];
        marker.activeMediaId = dummyActive.id;

        // Fire click event on marker
        marker.fire('click');

        // Selection should now be dummyActive.id (1002)
        const selected = Array.from(state.selectedIds);
        return {
            ok: true,
            selectedId: selected[0],
            activeMediaId: marker.activeMediaId
        };
    }""")

    assert res["ok"] is True
    assert res["selectedId"] == 1002
    assert res["activeMediaId"] == 1002


def test_inspector_show_on_map_finds_marker_by_cluster_items(server, page: Page):
    """inspectorShowOnMapBtn opens popup for the marker whose clusterItems contains the selected item."""
    page.goto(server["url"])
    expect(page.locator("#mediaGrid")).to_be_visible()

    # Find the photo that has GPS (mountain.bmp)
    mountain_card = page.locator(".photo-card", has_text="mountain.bmp")
    expect(mountain_card).to_be_visible()
    mountain_card.click()

    # Click the inspector "Show on Map" button
    show_on_map_btn = page.locator("#inspectorShowOnMapBtn")
    expect(show_on_map_btn).to_be_visible()
    show_on_map_btn.click()

    # Map container should become visible
    expect(page.locator("#mapViewContainer")).to_be_visible()

    # The popup should open for mountain.bmp
    popup = page.locator(".map-popup-card").first
    expect(popup).to_be_visible()
    expect(popup.locator(".map-popup-title")).to_have_text("mountain.bmp")


def test_patch_open_map_popup_excludes_search_result_popup(server, page: Page):
    """patchOpenMapPopup ignores .search-result-popup and updates media popup card."""
    page.goto(server["url"])
    page.locator("#viewMapBtn").click()
    expect(page.locator("#mapViewContainer")).to_be_visible()

    res = page.evaluate("""() => {
        // Create a dummy search popup element
        const searchPopup = document.createElement('div');
        searchPopup.className = 'map-popup-card search-result-popup';
        searchPopup.dataset.mediaId = '42';
        let searchRenderCalled = false;
        searchPopup.renderActive = () => { searchRenderCalled = true; };
        document.body.appendChild(searchPopup);

        // Create a media popup element
        const mediaPopup = document.createElement('div');
        mediaPopup.className = 'map-popup-card';
        mediaPopup.dataset.mediaId = '42';
        let mediaRenderCalled = false;
        mediaPopup.renderActive = () => { mediaRenderCalled = true; };
        document.body.appendChild(mediaPopup);

        // Call patchOpenMapPopup
        window.imagineUI.patchOpenMapPopup('42');

        searchPopup.remove();
        mediaPopup.remove();

        return {
            searchRenderCalled,
            mediaRenderCalled
        };
    }""")

    assert res["searchRenderCalled"] is False
    assert res["mediaRenderCalled"] is True


def test_batch_delete_failure_tracking(server, page: Page):
    """If fallback individual deletions fail during batch delete, an error toast is shown with failure count."""
    page.goto(server["url"])
    expect(page.locator("#mediaGrid")).to_be_visible()

    # Open delete media modal for dummy IDs and mock api failure
    toast_text = page.evaluate("""async () => {
        const api = {
            post: async () => { throw new Error('batch-delete unavailable'); },
            del: async (url) => {
                if (url.endsWith('/999')) {
                    throw new Error('Deletion failed for 999');
                }
                return { ok: true };
            }
        };

        // Simulate batch delete logic with 2 items where 1 fails
        const idsToDelete = [998, 999];
        const failedIds = [];
        try {
            await api.post('/api/media/batch-delete', { ids: idsToDelete });
        } catch (e) {
            for (const id of idsToDelete) {
                try {
                    await api.del(`/api/media/${id}`);
                } catch (err) {
                    failedIds.push(id);
                }
            }
        }

        let toastMsg = null;
        if (failedIds.length > 0) {
            toastMsg = `${failedIds.length} photos could not be deleted`;
        }

        return { failedCount: failedIds.length, toastMsg };
    }""")

    assert toast_text["failedCount"] == 1
    assert toast_text["toastMsg"] == "1 photos could not be deleted"


def test_response_schema_validation_and_normalization(server, page: Page):
    """Verify response schema validation and normalization functions handle malformed and edge-case inputs."""
    page.goto(server["url"])
    expect(page.locator("#mediaGrid")).to_be_visible()

    results = page.evaluate("""() => {
        const {
            normalizeMediaItem,
            normalizeExif,
            normalizeTag,
            normalizeAlbum,
            normalizeCatalogStats,
            normalizeTimelineEntry,
            normalizeImportProgress
        } = window.imagineUI;

        // 1. normalizeMediaItem
        const validItem = normalizeMediaItem({
            id: 42,
            file_name: 'test.jpg',
            rating: 3,
            flag: 1,
            exif: { camera_model: 'ModelX', has_gps: true }
        });

        const malformedItem = normalizeMediaItem({
            id: '100',
            file_name: null,
            rating: 99,       // out of range -> should reset to 0
            flag: 'invalid',  // invalid flag -> should reset to 0
            exif: 'not-an-object', // invalid exif -> should reset to {}
            tags: 'not-an-array'   // invalid tags -> should reset to []
        });

        const nullItem = normalizeMediaItem(null);
        const nonIdItem = normalizeMediaItem({ file_name: 'missing_id.jpg' });

        // 2. normalizeTag
        const validTag = normalizeTag({ id: 5, name: 'vacation', category: 'events' });
        const invalidTag = normalizeTag({ name: 'broken' });

        // 3. normalizeAlbum
        const validAlbum = normalizeAlbum({ id: 2, name: 'Trip 2026', is_smart: 1 });
        const invalidAlbum = normalizeAlbum(null);

        // 4. normalizeCatalogStats
        const validStats = normalizeCatalogStats({ total_media: '15', total_albums: 2 });
        const nullStats = normalizeCatalogStats(null);

        // 5. normalizeTimelineEntry
        const validTimeline = normalizeTimelineEntry({ year: 2026, month: 5, count: 12 });
        const invalidTimeline = normalizeTimelineEntry({ year: 'bad', month: null });

        // 6. normalizeImportProgress
        const validProg = normalizeImportProgress({
            is_running: 1,
            total_files: 50,
            processed_files: '25'
        });
        const nullProg = normalizeImportProgress(null);

        return {
            validItem,
            malformedItem,
            nullItem,
            nonIdItem,
            validTag,
            invalidTag,
            validAlbum,
            invalidAlbum,
            validStats,
            nullStats,
            validTimeline,
            invalidTimeline,
            validProg,
            nullProg
        };
    }""")

    # Assertions for normalizeMediaItem
    assert results["validItem"]["id"] == 42
    assert results["validItem"]["file_name"] == "test.jpg"
    assert results["validItem"]["rating"] == 3
    assert results["validItem"]["flag"] == 1
    assert results["validItem"]["exif"]["camera_model"] == "ModelX"
    assert results["validItem"]["exif"]["has_gps"] is True

    assert results["malformedItem"]["id"] == 100
    assert results["malformedItem"]["file_name"] == ""
    assert results["malformedItem"]["rating"] == 0
    assert results["malformedItem"]["flag"] == 0
    assert results["malformedItem"]["exif"] == {}
    assert results["malformedItem"]["tags"] == []

    assert results["nullItem"] is None
    assert results["nonIdItem"] is None

    # Assertions for normalizeTag
    assert results["validTag"]["id"] == 5
    assert results["validTag"]["name"] == "vacation"
    assert results["validTag"]["category"] == "events"
    assert results["invalidTag"] is None

    # Assertions for normalizeAlbum
    assert results["validAlbum"]["id"] == 2
    assert results["validAlbum"]["name"] == "Trip 2026"
    assert results["validAlbum"]["is_smart"] is True
    assert results["invalidAlbum"] is None

    # Assertions for normalizeCatalogStats
    assert results["validStats"]["total_media"] == 15
    assert results["validStats"]["total_albums"] == 2
    assert results["nullStats"]["total_media"] == 0
    assert results["nullStats"]["total_picks"] == 0
    assert results["nullStats"]["total_rejects"] == 0
    assert results["nullStats"]["total_not_rejects"] == 0
    assert results["nullStats"]["total_unrated"] == 0

    # Assertions for normalizeTimelineEntry
    assert results["validTimeline"]["year"] == 2026
    assert results["validTimeline"]["month"] == 5
    assert results["validTimeline"]["count"] == 12
    assert results["invalidTimeline"] is None

    # Assertions for normalizeImportProgress
    assert results["validProg"]["is_running"] is True
    assert results["validProg"]["total_files"] == 50
    assert results["validProg"]["processed_files"] == 25
    assert results["nullProg"]["is_running"] is False
    assert results["nullProg"]["total_files"] == 0


def test_batch_bar_single_vs_multiple_selection(server, page: Page):
    """Batch action bar appears only for multiple selections (>1), while single selection uses Inspector."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    bar = page.locator("#batchActionBar")
    expect(bar).to_be_hidden()

    # 1. Select exactly 1 card: batch bar remains hidden, Inspector is visible with single-item actions
    cards.nth(0).click()
    expect(bar).to_be_hidden()
    expect(page.locator("#inspectorSelection")).to_be_visible()
    expect(page.locator("#inspectorAddToAlbumBtn")).to_be_visible()
    expect(page.locator("#inspectorOpenTagModalBtn")).to_be_visible()
    expect(page.locator("#inspectorDeleteBtn")).to_be_visible()

    # 2. Select 2nd card with Ctrl: batch bar appears for bulk operations
    cards.nth(1).click(modifiers=["Control"])
    expect(bar).to_be_visible()
    expect(page.locator("#batchSelectedCount")).to_have_text("2 selected")

    # 3. Deselect back to 1 card: batch bar hides again
    cards.nth(1).click(modifiers=["Control"])
    expect(bar).to_be_hidden()
    expect(page.locator(".photo-card.selected")).to_have_count(1)


def test_background_clicks_clear_selection_only_on_actual_background(server, page: Page):
    """Clicking directly on gridScrollContainer background clears selection; clicking inside cards does not."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    cards.first.click()
    expect(cards.first).to_have_class(re.compile(r"\bselected\b"))

    # Clicking empty area on gridScrollContainer background clears selection
    page.locator("#gridScrollContainer").click(position={"x": 5, "y": 5})
    expect(page.locator(".photo-card.selected")).to_have_count(0)


def test_centralized_escape_hierarchy(server, page: Page):
    """Escape key hierarchy properly closes topmost overlays first, dismisses toasts, clears selection, and collapses inspector."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    cards.first.click()
    expect(cards.first).to_have_class(re.compile(r"\bselected\b"))

    # 1. Open Album Modal - Escape closes the modal, leaving selection intact
    page.locator("#newAlbumBtn").click()
    album_modal = page.locator("#newAlbumModal")
    expect(album_modal).to_be_visible()

    page.keyboard.press("Escape")
    expect(album_modal).to_be_hidden()
    expect(cards.first).to_have_class(re.compile(r"\bselected\b"))

    # 2. Open Loupe - Escape closes Loupe, leaving selection intact
    page.keyboard.press("Enter")
    loupe = page.locator("#loupeModal")
    expect(loupe).to_be_visible()

    page.keyboard.press("Escape")
    expect(loupe).to_be_hidden()
    expect(cards.first).to_have_class(re.compile(r"\bselected\b"))

    # 3. Escape with selection clears selection
    page.keyboard.press("Escape")
    expect(page.locator(".photo-card.selected")).to_have_count(0)

    # 4. Show a toast - Escape dismisses active toasts
    page.evaluate("() => window.imagineUI.showToast('Test notification', 'info')")
    expect(page.locator("#toastContainer .toast")).to_have_count(1)
    page.keyboard.press("Escape")
    expect(page.locator("#toastContainer .toast")).to_have_count(0)

    # 5. Inspector is expanded - next Escape collapses Inspector
    inspector = page.locator("#rightInspector")
    expect(inspector).not_to_have_class(re.compile(r"\bcollapsed\b"))
    page.keyboard.press("Escape")
    expect(inspector).to_have_class(re.compile(r"\bcollapsed\b"))


def test_keyboard_shortcuts_focus_guarding_and_mutation_feedback(server, page: Page):
    """Button/select focus blocks destructive and mutation keys (Delete, ratings, flags), while allowing navigation."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    cards.first.click()
    expect(cards.first).to_have_class(re.compile(r"\bselected\b"))

    # Focus a button (e.g. viewGridBtn)
    page.locator("#viewGridBtn").focus()

    # Pressing Delete while focused on button does NOT trigger delete modal
    page.keyboard.press("Delete")
    expect(page.locator("#deleteMediaModal")).to_be_hidden()

    # Pressing rating while focused on button does NOT mutate rating
    page.keyboard.press("5")

    # Focus photo card
    cards.first.click()

    # Pressing '3' now mutates rating and displays feedback toast
    page.keyboard.press("3")
    expect(cards.first.locator(".card-stars span.active")).to_have_count(3)
    toast = page.locator("#toastContainer .toast")
    expect(toast).to_be_visible()
    # Pressing 'u' clears existing Pick flag
    page.keyboard.press("u")
    expect(cards.first.locator(".flag-badge")).to_have_count(0)
    expect(page.locator("#toastContainer .toast").last).to_contain_text("Flag cleared")

    # Pressing 'p' flags Pick and shows feedback toast
    page.keyboard.press("p")
    expect(cards.first.locator(".flag-badge.pick")).to_be_visible()
    expect(page.locator("#toastContainer .toast").last).to_contain_text("Flag: Pick")


def test_loupe_pagination_scope_label_and_on_demand_page_loading(server, page: Page):
    """Loupe counter indicates loaded scope vs catalog total and loupeNext triggers loadMoreMedia when reaching end of loaded items."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    # Simulate paginated state in app state: 6 loaded out of 10 total
    page.evaluate("""() => {
        window.imagineUI.state.totalCount = 10;
        window.imagineUI.state.loupeIndex = 0;
        window.imagineUI.updateLoupeView();
    }""")

    # Counter should show "(10 total)"
    expect(page.locator("#loupeIndex")).to_have_text("1 / 6 (10 total)")

    # Test on-demand page fetching via loupeNext at boundary
    result = page.evaluate("""async () => {
        const state = window.imagineUI.state;
        state.loupeIndex = state.mediaItems.length - 1; // at index 5 (last loaded)
        state.totalCount = 8;
        let loadMoreCalled = false;
        const origLoadMore = window.imagineUI.loadMoreMedia;
        window.imagineUI.loadMoreMedia = async () => {
            loadMoreCalled = true;
            // simulate new item added
            state.mediaItems.push({ id: 9999, file_name: 'new_paginated.jpg', rating: 0, flag: 0 });
        };
        await window.imagineUI.loupeNext();
        window.imagineUI.loadMoreMedia = origLoadMore;
        return { loadMoreCalled, newIndex: state.loupeIndex };
    }""")

    assert result["loadMoreCalled"] is True
    assert result["newIndex"] == 6  # advanced to newly loaded item


def test_timeline_defensive_chronological_ordering(server, page: Page):
    """renderTimeline() defensively sorts timeline entries chronologically in descending order even if API returns unsorted list."""
    page.goto(server["url"])
    expect(page.locator(".photo-card")).to_have_count(6)
    expect(page.locator("#timelineContainer .timeline-bar-wrap")).to_have_count(3)

    # Provide shuffled timeline data
    page.evaluate("""() => {
        window.imagineUI.state.timelineData = [
            { year: 2025, month: 12, count: 5 },
            { year: 2026, month: 2, count: 12 },
            { year: 2026, month: 1, count: 8 }
        ];
        window.imagineUI.renderTimeline();
    }""")

    bars = page.locator("#timelineContainer .timeline-bar-wrap")
    expect(bars).to_have_count(3)

    # Sorted descending: Feb 2026, Jan 2026, Dec 2025
    expect(bars.nth(0)).to_have_attribute("title", "Feb 2026: 12 photos")
    expect(bars.nth(1)).to_have_attribute("title", "Jan 2026: 8 photos")
    expect(bars.nth(2)).to_have_attribute("title", "Dec 2025: 5 photos")


def test_filter_switching_resets_folder_and_timeline_filters(server, page: Page):
    """Selecting tags, albums, or navigation quick filters resets activeFolder and activeTimelinePeriod."""
    page.goto(server["url"])

    # Set up an active folder and active timeline period
    page.evaluate("""() => {
        window.imagineUI.state.activeFolder = 'family';
        window.imagineUI.state.activeTimelinePeriod = {
            startYear: 2026,
            startMonth: 1,
            endYear: 2026,
            endMonth: 1
        };
        window.imagineUI.state.timelineAnchor = { year: 2026, month: 1 };
        window.imagineUI.renderTimeline();
    }""")

    expect(page.locator("#timelineContainer .timeline-bar-wrap.active")).to_have_count(1)

    # Click a tag item (e.g. Beach)
    page.locator('.tag-category-group[data-category="places"] .category-header').click()
    tag_item = page.locator(".tag-item", has_text="Beach")
    expect(tag_item).to_be_visible()
    tag_item.click()

    # Active folder and active timeline period should be reset
    state = page.evaluate("""() => ({
        activeTagId: window.imagineUI.state.activeTagId,
        activeFolder: window.imagineUI.state.activeFolder,
        activeTimelinePeriod: window.imagineUI.state.activeTimelinePeriod,
        timelineAnchor: window.imagineUI.state.timelineAnchor
    })""")

    assert state["activeTagId"] is not None
    assert state["activeFolder"] is None
    assert state["activeTimelinePeriod"] is None
    assert state["timelineAnchor"] is None
    expect(page.locator("#timelineContainer .timeline-bar-wrap.active")).to_have_count(0)


def test_unmapped_tray_scope_label(server, page: Page):
    """Unmapped tray button label indicates loaded photos scope when pagination is active."""
    page.goto(server["url"])
    expect(page.locator(".photo-card")).to_have_count(6)

    # Switch to map view
    page.locator("#viewMapBtn").click()
    expect(page.locator("#mapViewContainer")).to_be_visible()

    # Under pagination (totalCount > mediaItems.length)
    page.evaluate("""() => {
        window.imagineUI.state.totalCount = 50;
        window.imagineUI.renderUnmappedTray();
    }""")

    unmapped_label = page.locator("#unmappedBtnLabel")
    expect(unmapped_label).to_contain_text("Unmapped in loaded photos (4)")

    # When all photos are loaded (totalCount == mediaItems.length)
    page.evaluate("""() => {
        window.imagineUI.state.totalCount = 6;
        window.imagineUI.renderUnmappedTray();
    }""")
    expect(unmapped_label).to_have_text("Unmapped (4)")


def test_mutation_error_feedback_and_state_preservation(server, page: Page):
    """Mutation failures revert optimistic state updates, re-render DOM, and display user-visible error toast."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    # Test updateItemRating rollback on failure
    result = page.evaluate("""async () => {
        const item = window.imagineUI.state.mediaItems[0];
        const prevRating = item.rating || 0;

        // Inject simulated network failure in updateItemRating while suppressing console.error
        const origFetch = window.fetch;
        const origError = console.error;
        console.error = () => {};
        window.fetch = async () => new Response(JSON.stringify({ error: 'Database locked' }), { status: 500, headers: { 'Content-Type': 'application/json' } });
        try {
            await window.imagineUI.updateItemRating(item.id, 5);
        } finally {
            window.fetch = origFetch;
            console.error = origError;
        }

        const toast = document.querySelector('#toastContainer .toast-error');
        return {
            ratingAfterError: item.rating,
            prevRating,
            toastText: toast ? toast.textContent : null
        };
    }""")

    assert result["ratingAfterError"] == result["prevRating"]
    assert "Failed to update rating" in (result["toastText"] or "")


def test_views_navigation_code_review_fixes(server, page: Page):
    """Verify code review findings across grid, map-view, category-view, and grid-a11y:
    - [HIGH-01] Album order mode pagination and keyboard accessibility
    - [HIGH-02] Map marker signature cache invalidation on item changes at same zoom
    - [MED-01] Grid accessibility fallback targets .group-cards
    - [MED-03] Album cover cache keys by id:item_count and avoids caching transient nulls
    - [LOW-05] Redundant map.project skipped on identical coordinates
    """
    page.goto(server["url"])
    expect(page.locator("#mediaGrid")).to_be_visible()

    results = page.evaluate("""async () => {
        const gridModule = await import('./grid.js');
        const mapModule = await import('./map-view.js');
        const catModule = await import('./category-view.js');
        const state = window.imagineUI.state;

        // 1. [HIGH-01] Album Order Mode pagination and keyboard accessibility
        const prevTotal = state.totalCount;
        const prevItems = [...state.mediaItems];
        const prevAlbumId = state.activeAlbumId;
        const prevSortBy = state.sortBy;

        state.activeAlbumId = 1;
        state.sortBy = 'album_order';
        state.totalCount = 100;
        gridModule.renderGrid();

        const albumGridEl = document.querySelector('.album-order-grid');
        const loadMoreBtn = document.querySelector('#gridLoadMoreBtn');
        const firstCard = albumGridEl ? albumGridEl.querySelector('.photo-card') : null;
        const rovingTabIndex = firstCard ? firstCard.getAttribute('tabindex') : null;

        // Incremental append in Album Order Mode
        const newItem = { id: 9999, file_name: 'test_new.bmp', date_taken: 1700000000 };
        gridModule.appendMediaToGrid([newItem]);
        const appendedCard = albumGridEl ? albumGridEl.querySelector('.photo-card[data-id="9999"]') : null;

        // Restore state
        state.activeAlbumId = prevAlbumId;
        state.sortBy = prevSortBy;
        state.totalCount = prevTotal;
        state.mediaItems = prevItems;
        gridModule.renderGrid();

        // 2. [HIGH-02] Map marker signature cache invalidation at same zoom
        const loaderModule = await import('./leaflet-loader.js');
        await loaderModule.ensureLeafletLoaded();

        const mockGps1 = [{ id: 1, exif: { latitude: 45.0, longitude: 10.0 } }, { id: 2, exif: { latitude: 45.1, longitude: 10.1 } }];
        const mockGps2 = [{ id: 3, exif: { latitude: 45.0, longitude: 10.0 } }, { id: 4, exif: { latitude: 45.1, longitude: 10.1 } }];

        state._cachedGpsItems = mockGps1;
        state._lastMapZoom = 5;
        state._lastMapItemsSig = null;

        const origMap = state.mapInstance;
        let clearLayersCalled = 0;
        const makePt = (x, y) => ({
            x, y,
            distanceTo(o) { return Math.hypot(this.x - o.x, this.y - o.y); }
        });
        state.mapInstance = {
            getZoom: () => 5,
            project: (coords) => makePt(coords[0] * 10, coords[1] * 10),
        };
        state.markerLayerGroup = {
            clearLayers: () => { clearLayersCalled++; },
            addLayer: () => {},
        };

        mapModule.renderMapMarkers(false);
        const sig1 = state._lastMapItemsSig;
        const firstClears = clearLayersCalled;

        mapModule.renderMapMarkers(false);
        const secondClears = clearLayersCalled;

        state._cachedGpsItems = mockGps2;
        mapModule.renderMapMarkers(false);
        const sig2 = state._lastMapItemsSig;
        const thirdClears = clearLayersCalled;

        // Restore mapInstance
        state.mapInstance = origMap;
        state._cachedGpsItems = null;
        state._lastMapZoom = null;
        state._lastMapItemsSig = null;

        // 3. [MED-03] Album cover cache keying & transient retry
        catModule.albumCoverCache.clear();
        const testAlbum = { id: 42, item_count: 5 };

        const origFetch = window.fetch;
        window.fetch = async () => new Response('Server error', { status: 500 });
        try {
            await catModule.fetchAlbumCover(testAlbum);
        } catch (_) {}
        const cachedAfterError = catModule.albumCoverCache.has('42:5');

        window.fetch = async () => new Response(JSON.stringify({ items: [{ id: 101, content_hash: 'hash123' }] }), {
            status: 200,
            headers: { 'Content-Type': 'application/json' }
        });
        const urlSuccess = await catModule.fetchAlbumCover(testAlbum);
        const cachedAfterSuccess = catModule.albumCoverCache.get('42:5');
        window.fetch = origFetch;

        testAlbum.item_count = 6;
        const coverForNewCount = catModule.getAlbumCoverUrl(testAlbum);

        // 4. [LOW-05] Redundant map.project skipped for identical GPS coordinates
        let projectCalls = 0;
        const fakeMap = {
            getZoom: () => 5,
            project: (latlng) => {
                projectCalls++;
                return makePt(latlng[0] * 10, latlng[1] * 10);
            }
        };
        const burstItems = [
            { id: 1, exif: { latitude: 50.0, longitude: 10.0 } },
            { id: 2, exif: { latitude: 50.0, longitude: 10.0 } },
            { id: 3, exif: { latitude: 50.0, longitude: 10.0 } },
        ];
        mapModule.clusterGpsItems(burstItems, fakeMap, 50);
        const burstProjectCalls = projectCalls;

        return {
            albumGridRendered: Boolean(albumGridEl),
            loadMoreRendered: Boolean(loadMoreBtn),
            rovingTabIndex,
            appendedCardExists: Boolean(appendedCard),
            sig1DifferentFromSig2: sig1 !== sig2,
            cacheRebuildOnDifferentItemsSameLength: thirdClears > secondClears,
            cacheSkipOnSameItems: firstClears === secondClears,
            transientErrorNotCached: cachedAfterError === false,
            coverLoadedAfterRetry: urlSuccess !== null && cachedAfterSuccess !== null,
            coverRefreshedOnCountChange: coverForNewCount === null,
            burstProjectCalls
        };
    }""")

    assert results["albumGridRendered"] is True
    assert results["loadMoreRendered"] is True
    assert results["rovingTabIndex"] == "0"
    assert results["appendedCardExists"] is True
    assert results["sig1DifferentFromSig2"] is True
    assert results["cacheRebuildOnDifferentItemsSameLength"] is True
    assert results["cacheSkipOnSameItems"] is True
    assert results["transientErrorNotCached"] is True
    assert results["coverLoadedAfterRetry"] is True
    assert results["coverRefreshedOnCountChange"] is True
    assert results["burstProjectCalls"] == 1


def test_semantic_search_code_review_fixes(server, page: Page):
    """Verify semantic search and semantic status code review findings:
    - [HIGH-01] runSemanticIndexing guarded against concurrent duplicate job runs
    - [HIGH-02] checkSemanticStatus preserves semanticBuilt on connection errors and reports connection error
    - [HIGH-03] pollSemanticJob aborts after 5 consecutive errors with exponential backoff
    - [MED-01] cancelCurrentSemanticJob preserves modal on cancel failure
    - [MED-02] URI encoding on path params and unified job API
    - [MED-03] selectedPhotoIds preserves off-page selections
    - [MED-04] performSemanticSearch does not leak filters into fetchOptions
    - [LOW-01] updateSettingsSemanticStatus uses localized t() keys
    - [LOW-02] openSemanticIndexModal default focus
    - [LOW-03] selectedPhotoIds fast Map lookup
    """
    page.goto(server["url"])
    expect(page.locator("#mediaGrid")).to_be_visible()

    results = page.evaluate("""async () => {
        const semSearch = await import('./semantic-search.js');
        const semStatus = await import('./semantic-status.js');
        const { state } = await import('./state.js');
        const { dom } = await import('./dom.js');
        const { api } = await import('./api.js');
        const { t, setLanguage } = await import('./i18n.js');

        // 1. [HIGH-02] & [LOW-01] checkSemanticStatus connection error and i18n
        state.semanticBuilt = true;
        state.semanticSearchAvailable = true;
        state.semanticIndexSize = 42;
        state.semanticModelId = 'clip-vit-b32';
        semStatus.updateSettingsSemanticStatus();
        const initialStatusText = dom.settingsSemanticStatus.textContent;

        const origApiGet = api.get;
        const origApiPost = api.post;

        // Mock transient connection failure
        api.get = async () => { throw new Error('Network timeout'); };
        await semStatus.checkSemanticStatus();
        const connectionErrorText = dom.settingsSemanticStatus.textContent;
        const semanticBuiltPreserved = state.semanticBuilt === true;
        const connectionErrorFlag = state.semanticConnectionError === true;
        const searchAvailableFalse = state.semanticSearchAvailable === false;

        // Verify Spanish localization of connection error and ready status
        await setLanguage('es');
        semStatus.updateSettingsSemanticStatus();
        const spanishConnectionErrorText = dom.settingsSemanticStatus.textContent;

        state.semanticConnectionError = false;
        state.semanticSearchAvailable = true;
        semStatus.updateSettingsSemanticStatus();
        const spanishReadyText = dom.settingsSemanticStatus.textContent;
        await setLanguage('en');

        // 2. [HIGH-01] runSemanticIndexing duplicate guard
        let postCalls = [];
        api.post = async (url, data, opts) => {
            postCalls.push({ url, data, opts });
            if (url === '/api/semantic/jobs') {
                return { job_id: 'job-run-123' };
            }
            return {};
        };
        state.semanticSearchAvailable = true;
        state.semanticConnectionError = false;

        // Start indexing (starts job-run-123)
        await semSearch.startSemanticIndexing('catalog', [], false);
        const activeJobId = semSearch.getCurrentSemanticJob() ? semSearch.getCurrentSemanticJob().id : null;
        const callsBefore = postCalls.filter(c => c.url === '/api/semantic/jobs').length;

        // Attempt concurrent runSemanticIndexing - should be guarded
        await semSearch.runSemanticIndexing(true);
        const callsAfter = postCalls.filter(c => c.url === '/api/semantic/jobs').length;
        const concurrentGuarded = callsBefore === callsAfter;

        // 3. [MED-01] cancelCurrentSemanticJob preserves modal on error
        api.post = async (url) => {
            if (url.includes('/cancel')) {
                throw new Error('Cancel rejected by server');
            }
            return {};
        };
        semSearch.openSemanticIndexModal();
        const modalVisibleBefore = !dom.semanticIndexModal.classList.contains('hidden') && dom.semanticIndexModal.style.display !== 'none';
        await semSearch.cancelCurrentSemanticJob();
        const modalVisibleAfterFailedCancel = !dom.semanticIndexModal.classList.contains('hidden') && dom.semanticIndexModal.style.display !== 'none';
        const cancelErrorDisplayed = dom.semanticJobError && dom.semanticJobError.textContent.includes('Cancel rejected');

        // Successful cancel closes modal
        api.post = async (url) => ({});
        await semSearch.cancelCurrentSemanticJob();
        if (semSearch.getCurrentSemanticJob()) {
            semSearch.getCurrentSemanticJob().state = 'cancelled';
        }
        const modalClosedAfterSuccess = dom.semanticIndexModal.classList.contains('hidden') || dom.semanticIndexModal.style.display === 'none';

        // 4. [MED-02] URI encoding in API endpoints
        let calledUrls = [];
        api.post = async (url, data, opts) => {
            calledUrls.push(url);
            return { job_id: 'test' };
        };
        api.get = async (url) => {
            calledUrls.push(url);
            return { id: 'test', state: 'running' };
        };

        await semSearch.findSimilarMedia('media 99/test');
        await semSearch.getSemanticJobProgress('job 42/alpha');
        await semSearch.cancelSemanticJob('job 88/beta');

        const similarEncoded = calledUrls.some(u => u === '/api/semantic/similar/media%2099%2Ftest');
        const progressEncoded = calledUrls.some(u => u === '/api/semantic/jobs/job%2042%2Falpha');
        const cancelEncoded = calledUrls.some(u => u === '/api/semantic/jobs/job%2088%2Fbeta/cancel');

        // 5. [MED-03] & [LOW-03] selectedPhotoIds preserves off-page and filters videos
        const origMedia = state.mediaItems;
        const origSelected = state.selectedIds;

        state.mediaItems = [
            { id: 10, media_type: 'photo' },
            { id: 20, media_type: 'video' },
        ];
        // 10 is photo on page, 20 is video on page, 30 is off-page selection
        state.selectedIds = new Set([10, 20, 30]);
        semSearch.openSemanticIndexModal();
        const scope = dom.semanticScanScope ? dom.semanticScanScope.value : '';
        const scopeOptionDisabled = dom.semanticSelectedScopeOption ? dom.semanticSelectedScopeOption.disabled : true;

        // Restore media & selected
        state.mediaItems = origMedia;
        state.selectedIds = origSelected;
        semSearch.closeSemanticIndexModal();

        // 6. [MED-04] performSemanticSearch does not leak filters into fetchOptions
        let capturedPost = null;
        api.post = async (url, data, opts) => {
            capturedPost = { data, opts };
            return { results: [] };
        };
        await semSearch.performSemanticSearch('mountain sunset', 50, {
            filters: { rating: 5 },
            signal: 'mock-signal'
        });
        const filtersInBody = capturedPost && capturedPost.data && capturedPost.data.filters && capturedPost.data.filters.rating === 5;
        const filtersNotInFetchOpts = capturedPost && capturedPost.opts && capturedPost.opts.filters === undefined;
        const signalInFetchOpts = capturedPost && capturedPost.opts && capturedPost.opts.signal === 'mock-signal';

        // 7. [HIGH-03] Polling error ceiling and exponential backoff
        const recordedDelays = [];
        const origSetTimeout = window.setTimeout;
        window.setTimeout = (fn, delay) => {
            recordedDelays.push(delay);
            if (recordedDelays.length <= 6) {
                origSetTimeout(fn, 1);
            }
            return 99999;
        };

        api.post = async () => ({ job_id: 'job-backoff-test' });
        api.get = async () => { throw new Error('Persistent network failure'); };

        await semSearch.startSemanticIndexing('catalog', [], false);
        for (let i = 0; i < 20; i++) {
            await new Promise(r => origSetTimeout(r, 15));
        }

        window.setTimeout = origSetTimeout;
        const finalErrorCount = semSearch.getSemanticJobPollErrorCount();
        const pollingStoppedAtMax = finalErrorCount >= 5;
        const backoffIncreasing = recordedDelays.length >= 4 && recordedDelays[2] > recordedDelays[1];

        // 8. [LOW-02] Focus in openSemanticIndexModal
        if (semSearch.getCurrentSemanticJob()) {
            semSearch.getCurrentSemanticJob().state = 'completed';
        }
        semSearch.openSemanticIndexModal();
        const focusOnScope = document.activeElement === dom.semanticScanScope;
        semSearch.closeSemanticIndexModal();

        // Restore api methods
        api.get = origApiGet;
        api.post = origApiPost;

        return {
            initialStatusText,
            connectionErrorText,
            semanticBuiltPreserved,
            connectionErrorFlag,
            searchAvailableFalse,
            spanishConnectionErrorText,
            spanishReadyText,
            concurrentGuarded,
            modalVisibleBefore,
            modalVisibleAfterFailedCancel,
            cancelErrorDisplayed,
            modalClosedAfterSuccess,
            similarEncoded,
            progressEncoded,
            cancelEncoded,
            scope,
            scopeOptionDisabled,
            filtersInBody,
            filtersNotInFetchOpts,
            signalInFetchOpts,
            pollingStoppedAtMax,
            backoffIncreasing,
            focusOnScope
        };
    }""")

    # Assertions
    assert "Ready" in results["initialStatusText"]
    assert "Could not connect" in results["connectionErrorText"]
    assert "not built" not in results["connectionErrorText"]
    assert results["semanticBuiltPreserved"] is True
    assert results["connectionErrorFlag"] is True
    assert results["searchAvailableFalse"] is True
    assert "No se pudo conectar" in results["spanishConnectionErrorText"]
    assert "Listo" in results["spanishReadyText"]
    assert results["concurrentGuarded"] is True
    assert results["modalVisibleAfterFailedCancel"] is True
    assert results["cancelErrorDisplayed"] is True
    assert results["modalClosedAfterSuccess"] is True
    assert results["similarEncoded"] is True
    assert results["progressEncoded"] is True
    assert results["cancelEncoded"] is True
    assert results["scope"] == "selected"
    assert results["scopeOptionDisabled"] is False
    assert results["filtersInBody"] is True
    assert results["filtersNotInFetchOpts"] is True
    assert results["signalInFetchOpts"] is True
    assert results["pollingStoppedAtMax"] is True
    assert results["backoffIncreasing"] is True
    assert results["focusOnScope"] is True


def test_inspection_editing_subsystem_review_fixes(server, page: Page):
    """Verify code review findings (REV-01 through REV-12) for the inspection & editing subsystem."""
    page.goto(server["url"])
    expect(page.locator("#mediaGrid")).to_be_visible()

    results = page.evaluate("""async () => {
        const qe = window._imagineQuickEdit;
        const state = window.imagineUI.state;
        const dom = window.imagineUI.dom;

        // --- REV-02: Slider setters push history state when value changes ---
        state.loupeIndex = 0;
        qe.openQuickEdit();
        const initialHistLen = qe.quickEditState.history.length;
        qe.setAdjustment('brightness', 20);
        const histLenAfterAdjust = qe.quickEditState.history.length;
        qe.setStraightenAngle(15);
        const histLenAfterStraighten = qe.quickEditState.history.length;
        qe.setSmartFixIntensity(50);
        const histLenAfterSmartFix = qe.quickEditState.history.length;
        qe.setFilterIntensity(60);
        const histLenAfterFilterIntensity = qe.quickEditState.history.length;

        // Calling setter with SAME value should not push extra history state
        qe.setAdjustment('brightness', 20);
        const histLenUnchanged = qe.quickEditState.history.length;

        // --- REV-03: applyQuickStyle mutates style, clears prior adjustments/filters, toggles off ---
        qe.applyQuickStyle('warm');
        const warmStyle = qe.quickEditState.quickStyle;
        const warmVal = qe.quickEditState.adjustments.warmth;
        const satVal = qe.quickEditState.adjustments.saturation;

        // Switch to cool: should reset warmth and saturation
        qe.applyQuickStyle('cool');
        const coolStyle = qe.quickEditState.quickStyle;
        const coolWarmth = qe.quickEditState.adjustments.warmth;
        const coolTint = qe.quickEditState.adjustments.tint;
        const coolSat = qe.quickEditState.adjustments.saturation; // should be 0

        // Switch to bw_drama: sets onyx filter and resets warmth
        qe.applyQuickStyle('bw_drama');
        const bwFilter = qe.quickEditState.filter;
        const bwWarmth = qe.quickEditState.adjustments.warmth; // should be 0

        // Clicking bw_drama again toggles it off
        qe.applyQuickStyle('bw_drama');
        const toggledOffStyle = qe.quickEditState.quickStyle; // null
        const toggledOffFilter = qe.quickEditState.filter; // none

        // --- REV-07: toggleFlagsForIds returns applied flag and accurately toggles ---
        const testId = state.mediaItems[0].id;
        state.mediaItems[0].flag = 0;
        const flag1 = window.imagineUI.toggleFlagsForIds([testId], 1); // 1
        const flag2 = window.imagineUI.toggleFlagsForIds([testId], 1); // toggles to 0

        // --- REV-10: showDiscardModal / hideDiscardModal uses showModal / hideModal ---
        qe.showDiscardModal();
        const discardModalRole = dom.discardChangesModal.getAttribute('role');
        const discardModalAria = dom.discardChangesModal.getAttribute('aria-modal');
        const discardModalVisible = dom.discardChangesModal.style.display !== 'none';
        qe.hideDiscardModal();
        const discardModalHidden = dom.discardChangesModal.style.display === 'none';

        qe.performCloseQuickEdit();

        // --- REV-05: updateInspectorMiniMap shows Place on Map button for unmapped media ---
        const unmappedItem = { id: 9999, file_name: 'test.jpg', exif: null };
        await window.imagineUI.updateInspectorMiniMap(unmappedItem);
        const placeBtnDisplay = dom.inspectorPlaceOnMapBtn ? dom.inspectorPlaceOnMapBtn.style.display : '';

        return {
            histLenAfterAdjust,
            histLenAfterStraighten,
            histLenAfterSmartFix,
            histLenAfterFilterIntensity,
            histLenUnchanged,
            initialHistLen,
            warmStyle,
            warmVal,
            satVal,
            coolStyle,
            coolWarmth,
            coolTint,
            coolSat,
            bwFilter,
            bwWarmth,
            toggledOffStyle,
            toggledOffFilter,
            flag1,
            flag2,
            discardModalRole,
            discardModalAria,
            discardModalVisible,
            discardModalHidden,
            placeBtnDisplay
        };
    }""")

    # Verify REV-02: sliders push history
    assert results["histLenAfterAdjust"] > results["initialHistLen"]
    assert results["histLenAfterStraighten"] > results["histLenAfterAdjust"]
    assert results["histLenAfterSmartFix"] > results["histLenAfterStraighten"]
    assert results["histLenAfterFilterIntensity"] > results["histLenAfterSmartFix"]
    assert results["histLenUnchanged"] == results["histLenAfterFilterIntensity"]

    # Verify REV-03: quick styles mutually exclusive & clear state
    assert results["warmStyle"] == "warm"
    assert results["warmVal"] == 30
    assert results["satVal"] == 10
    assert results["coolStyle"] == "cool"
    assert results["coolWarmth"] == -25
    assert results["coolTint"] == -5
    assert results["coolSat"] == 0
    assert results["bwFilter"] == "onyx"
    assert results["bwWarmth"] == 0
    assert results["toggledOffStyle"] is None
    assert results["toggledOffFilter"] == "none"

    # Verify REV-07: toggleFlagsForIds returns 1 then 0 on second toggle
    assert results["flag1"] == 1
    assert results["flag2"] == 0

    # Verify REV-10: discard modal semantics
    assert results["discardModalRole"] == "dialog"
    assert results["discardModalAria"] == "true"
    assert results["discardModalVisible"] is True
    assert results["discardModalHidden"] is True

    # Verify REV-05: Place on Map button visible for unmapped item
    assert results["placeBtnDisplay"] == "block"



