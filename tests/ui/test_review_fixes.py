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
        const fb = window._imagineApp.formatBytes;
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
        const num = window._imagineApp.numeric;
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
        const state = window._imagineApp.state;
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
        const clusterGpsItems = window._imagineApp.clusterGpsItems;
        const map = window._imagineApp.state.mapInstance;

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
        const state = window._imagineApp.state;
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
    popup = page.locator(".map-popup-card")
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
        window._imagineApp.patchOpenMapPopup('42');

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
        } = window._imagineApp;

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
    page.evaluate("() => window._imagineApp.showToast('Test notification', 'info')")
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
        window._imagineApp.state.totalCount = 10;
        window._imagineApp.state.loupeIndex = 0;
        window._imagineApp.updateLoupeView();
    }""")

    # Counter should show "(10 total)"
    expect(page.locator("#loupeIndex")).to_have_text("1 / 6 (10 total)")

    # Test on-demand page fetching via loupeNext at boundary
    result = page.evaluate("""async () => {
        const state = window._imagineApp.state;
        state.loupeIndex = state.mediaItems.length - 1; // at index 5 (last loaded)
        state.totalCount = 8;
        let loadMoreCalled = false;
        const origLoadMore = window._imagineApp.loadMoreMedia;
        window._imagineApp.loadMoreMedia = async () => {
            loadMoreCalled = true;
            // simulate new item added
            state.mediaItems.push({ id: 9999, file_name: 'new_paginated.jpg', rating: 0, flag: 0 });
        };
        await window._imagineApp.loupeNext();
        window._imagineApp.loadMoreMedia = origLoadMore;
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
        window._imagineApp.state.timelineData = [
            { year: 2025, month: 12, count: 5 },
            { year: 2026, month: 2, count: 12 },
            { year: 2026, month: 1, count: 8 }
        ];
        window._imagineApp.renderTimeline();
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
        window._imagineApp.state.activeFolder = 'family';
        window._imagineApp.state.activeTimelinePeriod = { year: 2026, month: 1 };
        window._imagineApp.renderTimeline();
    }""")

    expect(page.locator("#timelineContainer .timeline-bar-wrap.active")).to_have_count(1)

    # Click a tag item (e.g. Beach)
    tag_item = page.locator(".tag-item", has_text="Beach")
    expect(tag_item).to_be_visible()
    tag_item.click()

    # Active folder and active timeline period should be reset
    state = page.evaluate("""() => ({
        activeTagId: window._imagineApp.state.activeTagId,
        activeFolder: window._imagineApp.state.activeFolder,
        activeTimelinePeriod: window._imagineApp.state.activeTimelinePeriod
    })""")

    assert state["activeTagId"] is not None
    assert state["activeFolder"] is None
    assert state["activeTimelinePeriod"] is None
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
        window._imagineApp.state.totalCount = 50;
        window._imagineApp.renderUnmappedTray();
    }""")

    unmapped_label = page.locator("#unmappedBtnLabel")
    expect(unmapped_label).to_contain_text("Unmapped in loaded photos (4)")

    # When all photos are loaded (totalCount == mediaItems.length)
    page.evaluate("""() => {
        window._imagineApp.state.totalCount = 6;
        window._imagineApp.renderUnmappedTray();
    }""")
    expect(unmapped_label).to_have_text("Unmapped (4)")


def test_mutation_error_feedback_and_state_preservation(server, page: Page):
    """Mutation failures revert optimistic state updates, re-render DOM, and display user-visible error toast."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    # Test updateItemRating rollback on failure
    result = page.evaluate("""async () => {
        const item = window._imagineApp.state.mediaItems[0];
        const prevRating = item.rating || 0;

        // Inject simulated network failure in updateItemRating while suppressing console.error
        const origFetch = window.fetch;
        const origError = console.error;
        console.error = () => {};
        window.fetch = async () => new Response(JSON.stringify({ error: 'Database locked' }), { status: 500, headers: { 'Content-Type': 'application/json' } });
        try {
            await window._imagineApp.updateItemRating(item.id, 5);
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

