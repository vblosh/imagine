"""
Deterministic UI tests for Map View, Photo Pins, Leaflet Popups, Inspector Mini-Map, and Geotagging.
"""

import re
import json
import urllib.request
from playwright.sync_api import Page, expect


def test_map_view_toggle(server, page: Page):
    """Test toggling between Grid View and Map View via buttons and keyboard shortcuts (G/M)."""
    page.goto(server["url"])

    grid_btn = page.locator("#viewGridBtn")
    map_btn = page.locator("#viewMapBtn")
    grid_container = page.locator("#gridScrollContainer")
    map_container = page.locator("#mapViewContainer")

    # Initial state: Grid view active
    expect(grid_btn).to_have_class(re.compile(r"\bactive\b"))
    expect(map_btn).not_to_have_class(re.compile(r"\bactive\b"))
    expect(grid_container).to_be_visible()
    expect(map_container).to_be_hidden()

    # 1. Switch to Map View via button
    map_btn.click()
    expect(map_btn).to_have_class(re.compile(r"\bactive\b"))
    expect(grid_btn).not_to_have_class(re.compile(r"\bactive\b"))
    expect(map_container).to_be_visible()
    expect(grid_container).to_be_hidden()

    # 2. Switch back to Grid View via button
    grid_btn.click()
    expect(grid_btn).to_have_class(re.compile(r"\bactive\b"))
    expect(map_btn).not_to_have_class(re.compile(r"\bactive\b"))
    expect(grid_container).to_be_visible()
    expect(map_container).to_be_hidden()

    # 3. Switch to Map View via "m" keyboard shortcut
    page.keyboard.press("m")
    expect(map_btn).to_have_class(re.compile(r"\bactive\b"))
    expect(map_container).to_be_visible()

    # 4. Switch to Grid View via "g" keyboard shortcut
    page.keyboard.press("g")
    expect(grid_btn).to_have_class(re.compile(r"\bactive\b"))
    expect(grid_container).to_be_visible()


def test_map_markers_and_popup(server, page: Page):
    """Photos with GPS coordinates render as custom pins on the map, opening interactive popups."""
    page.goto(server["url"])

    # Switch to Map View
    page.locator("#viewMapBtn").click()
    expect(page.locator("#mapViewContainer")).to_be_visible()

    # In default fixtures: mountain.bmp and beach.bmp have GPS coordinates (2 total)
    expect(page.locator("#mapPhotoCount")).to_have_text("2")

    # Verify custom photo pin markers are present
    pins = page.locator(".custom-photo-pin")
    expect(pins).to_have_count(2)

    # Click the pin for mountain.bmp
    mountain_pin = page.locator(".photo-pin-inner", has=page.locator("img[alt='mountain.bmp']"))
    expect(mountain_pin).to_be_visible()
    mountain_pin.click()

    # Popup card should open
    popup = page.locator(".map-popup-card")
    expect(popup).to_be_visible()
    expect(popup.locator(".map-popup-title")).to_have_text("mountain.bmp")
    expect(popup.locator(".map-popup-coords")).to_contain_text("46.50000, 11.35000")

    # Star rating is interactive in popup
    stars = popup.locator(".map-popup-rating span")
    expect(stars).to_have_count(5)

    # Click Loupe hint in popup to open fullscreen Loupe
    popup.locator(".map-popup-thumb-wrap").click()
    expect(page.locator("#loupeModal")).to_be_visible()
    expect(page.locator("#loupeFileName")).to_have_text("mountain.bmp")

    # Close loupe returns to map view
    page.locator("#loupeCloseBtn").click()
    expect(page.locator("#loupeModal")).to_be_hidden()
    expect(page.locator("#mapViewContainer")).to_be_visible()


def test_inspector_show_on_map(server, page: Page):
    """Selecting a photo with GPS in Inspector displays mini-map and Show on Map centers the map."""
    page.goto(server["url"])

    # Select mountain.bmp in Grid view
    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.click()

    # Inspector shows GPS and mini-map
    expect(page.locator("#infoGps")).to_contain_text("46.50000, 11.35000")
    expect(page.locator("#inspectorShowOnMapBtn")).to_be_visible()
    expect(page.locator("#inspectorMiniMap")).to_be_visible()

    # Click Show on Map
    page.locator("#inspectorShowOnMapBtn").click()

    # Should switch to Map View and open popup
    expect(page.locator("#mapViewContainer")).to_be_visible()
    expect(page.locator(".map-popup-card")).to_be_visible()
    expect(page.locator(".map-popup-title")).to_have_text("mountain.bmp")


def test_geotag_placement_on_map(server, page: Page):
    """Placing an unmapped photo on the map assigns GPS coordinates and renders a pin."""
    page.goto(server["url"])

    # Switch to Map View
    page.locator("#viewMapBtn").click()
    expect(page.locator("#mapViewContainer")).to_be_visible()
    expect(page.locator("#mapPhotoCount")).to_have_text("2")

    # Open unmapped tray
    page.locator("#mapToggleUnmappedBtn").click()
    tray = page.locator("#unmappedTray")
    expect(tray).to_be_visible()

    # 4 photos are unmapped in default fixtures (sunset.bmp, birthday.bmp, portrait.bmp, forest.bmp)
    unmapped_chips = page.locator(".unmapped-chip")
    expect(unmapped_chips).to_have_count(4)

    # Click sunset.bmp chip to enter placement mode
    sunset_chip = page.locator(".unmapped-chip", has_text="sunset.bmp")
    sunset_chip.click()
    expect(sunset_chip).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#mapViewContainer")).to_have_class(re.compile(r"\bplacement-mode\b"))

    # Click on the Leaflet map to place sunset.bmp
    page.locator("#leafletMap").click(position={"x": 200, "y": 200})

    # Placement mode exits and photo count increments to 3
    expect(page.locator("#mapPhotoCount")).to_have_text("3")
    expect(page.locator(".custom-photo-pin")).to_have_count(3)

    # In unmapped tray, count decreased to 3
    expect(page.locator(".unmapped-chip")).to_have_count(3)


def test_map_place_search_coordinates_and_catalog(server, page: Page):
    """Searching coordinates or catalog photos on map shows results dropdown and places a pin."""
    page.goto(server["url"])

    # Switch to Map View
    page.locator("#viewMapBtn").click()
    expect(page.locator("#mapViewContainer")).to_be_visible()

    # Map search overlay should be visible
    search_input = page.locator("#mapSearchInput")
    expect(search_input).to_be_visible()
    search_icon = page.locator("#mapSearchIcon")
    expect(search_icon).to_be_visible()

    # Verify typing hides the lupe (search icon) and clearing shows it
    search_input.fill("test")
    expect(search_icon).to_be_hidden()
    search_input.fill("")
    expect(search_icon).to_be_visible()

    # 1. Search by coordinate
    search_input.fill("48.8584, 2.2945")
    expect(search_icon).to_be_hidden()
    results = page.locator("#mapSearchResults")
    expect(results).to_be_visible()

    first_item = results.locator(".map-search-item").first
    expect(first_item.locator(".item-title")).to_contain_text("Coordinates: 48.8584, 2.2945")

    # Click result to place pin
    first_item.click()
    expect(results).to_be_hidden()
    expect(search_input).to_have_value("Coordinates: 48.8584, 2.2945")
    expect(search_icon).to_be_hidden()

    # Verify search location pin is rendered on map
    search_pin = page.locator(".search-location-pin")
    expect(search_pin).to_be_visible()

    # Popup card should open
    popup = page.locator(".map-popup-card.search-result-popup")
    expect(popup).to_be_visible()
    expect(popup.locator(".map-popup-title")).to_contain_text("Coordinates: 48.8584, 2.2945")

    # 2. Clear search
    clear_btn = page.locator("#clearMapSearchBtn")
    expect(clear_btn).to_be_visible()
    clear_btn.click()
    expect(search_input).to_have_value("")
    expect(search_pin).to_be_hidden()
    expect(search_icon).to_be_visible()

    # 3. Search catalog photo
    search_input.fill("mountain")
    expect(results).to_be_visible()
    catalog_item = results.locator(".map-search-item", has_text="mountain.bmp")
    expect(catalog_item).to_be_visible()
    expect(catalog_item.locator(".item-badge")).to_have_text("Photo")
    catalog_item.click()

    expect(page.locator(".search-location-pin")).to_be_visible()


def test_map_place_search_geotag_place_here(server, page: Page):
    """Searching a place and clicking 'Place Photo Here' geotags the selected unmapped photo."""
    page.goto(server["url"])

    # Switch to Map View
    page.locator("#viewMapBtn").click()

    # Open unmapped tray and select sunset.bmp
    page.locator("#mapToggleUnmappedBtn").click()
    page.locator(".unmapped-chip", has_text="sunset.bmp").click()

    # Search coordinates for a place
    page.locator("#mapSearchInput").fill("37.7749, -122.4194")
    results = page.locator("#mapSearchResults")
    expect(results).to_be_visible()
    results.locator(".map-search-item").first.click()

    # Popup has 'Place "sunset.bmp" Here' button
    popup = page.locator(".map-popup-card.search-result-popup")
    expect(popup).to_be_visible()
    place_btn = popup.locator(".place-here-btn")
    expect(place_btn).to_be_visible()
    expect(place_btn).to_contain_text("sunset.bmp")

    # Click place button
    place_btn.click()

    # Photo count increments from 2 to 3
    expect(page.locator("#mapPhotoCount")).to_have_text("3")
    expect(page.locator(".custom-photo-pin")).to_have_count(3)


def test_unmapped_tray_multi_select_and_batch_placement(server, page: Page):
    """Selecting multiple photos in the unmapped tray and placing them batch-geotags all of them."""
    page.goto(server["url"])

    # Switch to Map View
    page.locator("#viewMapBtn").click()
    expect(page.locator("#mapPhotoCount")).to_have_text("2")

    # Open unmapped tray
    page.locator("#mapToggleUnmappedBtn").click()
    tray = page.locator("#unmappedTray")
    expect(tray).to_be_visible()

    # 4 unmapped photos
    chips = page.locator(".unmapped-chip")
    expect(chips).to_have_count(4)

    # 1. Test Select All
    select_all_btn = page.locator("#unmappedSelectAllBtn")
    expect(select_all_btn).to_be_visible()
    select_all_btn.click()

    # All 4 chips should be active with check badges
    expect(page.locator(".unmapped-chip.active")).to_have_count(4)
    expect(page.locator(".unmapped-chip-check")).to_have_count(4)
    expect(page.locator("#unmappedSelectedCount")).to_have_text("4 selected")
    expect(page.locator("#mapViewContainer")).to_have_class(re.compile(r"\bplacement-mode\b"))

    # 2. Test Deselect
    deselect_btn = page.locator("#unmappedDeselectAllBtn")
    expect(deselect_btn).to_be_visible()
    deselect_btn.click()
    expect(page.locator(".unmapped-chip.active")).to_have_count(0)
    expect(page.locator("#unmappedSelectedCount")).to_be_hidden()
    expect(page.locator("#mapViewContainer")).not_to_have_class(re.compile(r"\bplacement-mode\b"))

    # 3. Multi-select two photos using Ctrl+Click
    sunset_chip = page.locator(".unmapped-chip", has_text="sunset.bmp")
    birthday_chip = page.locator(".unmapped-chip", has_text="birthday.bmp")

    sunset_chip.click()
    expect(sunset_chip).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#unmappedSelectedCount")).to_have_text("1 selected")

    # Ctrl+Click on birthday.bmp
    birthday_chip.click(modifiers=["Control"])
    expect(sunset_chip).to_have_class(re.compile(r"\bactive\b"))
    expect(birthday_chip).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#unmappedSelectedCount")).to_have_text("2 selected")

    # 4. Click on map to place both photos
    page.locator("#leafletMap").click(position={"x": 250, "y": 250})

    # Placement mode exits, total mapped photos increments from 2 to 4
    expect(page.locator("#mapPhotoCount")).to_have_text("4")
    # Remaining unmapped photos should be 2 (portrait.bmp, forest.bmp)
    expect(page.locator(".unmapped-chip")).to_have_count(2)


def test_map_clustering_zoom_combine_and_decombine(server, page: Page):
    """Photos near each other combine into a cluster with < > navigation when zoomed out,
    and decombine into separate pins when zoomed in enough.
    """
    # Geotag sunset.bmp and birthday.bmp near each other in Paris (~3.3km apart)
    sunset = next(i for i in server["env"]["items"] if i["file_name"] == "sunset.bmp")
    birthday = next(i for i in server["env"]["items"] if i["file_name"] == "birthday.bmp")

    for item_id, lat, lon in [(sunset["id"], 48.8584, 2.2945), (birthday["id"], 48.8606, 2.3376)]:
        req = urllib.request.Request(
            f"{server['url']}/api/media/{item_id}/gps",
            data=json.dumps({"has_gps": True, "latitude": lat, "longitude": lon, "altitude": 0.0}).encode(),
            headers={"Content-Type": "application/json"},
            method="POST"
        )
        with urllib.request.urlopen(req) as resp:
            assert resp.status == 200

    page.goto(server["url"])

    # Switch to Map View
    page.locator("#viewMapBtn").click()
    expect(page.locator("#mapViewContainer")).to_be_visible()
    expect(page.locator("#mapPhotoCount")).to_have_text("4")

    # 1. Set zoom to 10 centered over Paris (where 3.3km is ~22px < 50px radius)
    page.evaluate("() => window._imagineState.mapInstance.setView([48.83, 2.316], 10)")
    page.wait_for_timeout(300)

    # When zoomed out, the 2 Paris photos should COMBINE into 1 cluster pin with badge count '2'
    cluster_badge = page.locator(".photo-pin-count", has_text="2")
    expect(cluster_badge).to_be_visible()

    # Click the cluster pin to open popup
    cluster_pin = page.locator(".photo-pin-inner", has=cluster_badge)
    cluster_pin.click()

    popup = page.locator(".map-popup-card")
    expect(popup).to_be_visible()

    # Verify navigation <> bar is present showing "1 of 2"
    nav = popup.locator(".map-popup-nav")
    expect(nav).to_be_visible()
    expect(nav).to_contain_text("1 of 2")

    # Navigate to 2nd photo
    popup.locator("#popNextBtn").click()
    expect(nav).to_contain_text("2 of 2")

    # Navigate back to 1st photo
    popup.locator("#popPrevBtn").click()
    expect(nav).to_contain_text("1 of 2")

    # 2. Zoom in enough (zoom 15, where 3.3km is ~350px > 50px radius)
    # The cluster should DECOMBINE into 2 separate individual pins
    page.evaluate("() => window._imagineState.mapInstance.setView([48.859, 2.316], 15)")
    page.wait_for_timeout(300)

    # In Paris, there should now be no badge '2'
    expect(cluster_badge).to_be_hidden()

    # Both individual photo pins should now be visible on the map
    sunset_pin = page.locator(".photo-pin-inner", has=page.locator("img[alt='sunset.bmp']"))
    birthday_pin = page.locator(".photo-pin-inner", has=page.locator("img[alt='birthday.bmp']"))
    expect(sunset_pin).to_be_visible()
    expect(birthday_pin).to_be_visible()

    # Click sunset pin: popup opens for sunset without < > navigation
    sunset_pin.click()
    expect(popup).to_be_visible()
    expect(popup.locator(".map-popup-title")).to_have_text("sunset.bmp")
    expect(popup.locator(".map-popup-nav")).to_be_hidden()

    # 3. Zoom back out to zoom 10: photos should COMBINE back together
    page.evaluate("() => window._imagineState.mapInstance.setView([48.83, 2.316], 10)")
    page.wait_for_timeout(300)

    # Cluster badge '2' reappears, and navigation <> bar is active again on click
    expect(cluster_badge).to_be_visible()
    cluster_pin.click()
    expect(popup).to_be_visible()
    expect(popup.locator(".map-popup-nav")).to_be_visible()
    expect(popup.locator(".map-popup-nav")).to_contain_text("1 of 2")


def test_map_pagination_controls(server, page: Page):
    """Verify Map toolbar and Unmapped tray pagination controls when photos exceed loaded page."""
    page.goto(server["url"])
    expect(page.locator(".photo-card")).to_have_count(6)

    # Simulate catalog has more photos than loaded page (totalCount = 10, loaded = 6)
    page.evaluate("() => { window._imagineState.totalCount = 10; }")

    # Switch to Map view
    page.locator("#viewMapBtn").click()
    expect(page.locator("#mapViewContainer")).to_be_visible()

    # Open unmapped tray
    page.locator("#mapToggleUnmappedBtn").click()
    expect(page.locator("#unmappedTray")).to_be_visible()

    load_more_btn = page.locator("#mapLoadMoreBtn")
    load_all_btn = page.locator("#mapLoadAllBtn")
    unmapped_btn = page.locator("#unmappedLoadMoreBtn")

    expect(load_more_btn).to_be_visible()
    expect(load_more_btn).to_contain_text("Load More (6/10)")
    expect(load_all_btn).to_be_visible()
    expect(unmapped_btn).to_be_visible()
    expect(unmapped_btn).to_contain_text("Load More (6/10)")

    # Intercept pagination requests to return total=10 so controls remain active
    page.route("**/api/media*offset=*", lambda route: route.fulfill(
        status=200,
        content_type="application/json",
        body=json.dumps({"items": [], "total": 10})
    ))

    # Click Map Load More button and verify pagination request is sent
    with page.expect_request(lambda r: "/api/media" in r.url and "offset=6" in r.url):
        load_more_btn.click()

    # Click Unmapped Load More button and verify pagination request is sent
    with page.expect_request(lambda r: "/api/media" in r.url and "offset=" in r.url):
        unmapped_btn.click()


def test_geocode_backend_proxy_integration(server, page: Page):
    """Verify that searching for places uses the backend /api/geocode proxy endpoint."""
    page.goto(server["url"])
    page.locator("#viewMapBtn").click()
    expect(page.locator("#mapViewContainer")).to_be_visible()

    # Intercept /api/geocode requests to verify parameters and simulate response
    geocode_requests = []

    def handle_geocode(route):
        geocode_requests.append(route.request.url)
        # Mock Nominatim JSON response returned by proxy
        route.fulfill(
            status=200,
            content_type="application/json",
            body=json.dumps([{
                "place_id": 12345,
                "lat": "48.8584",
                "lon": "2.2945",
                "name": "Eiffel Tower",
                "display_name": "Eiffel Tower, Paris, France",
                "type": "monument",
                "addresstype": "tourism"
            }])
        )

    page.route("**/api/geocode*", handle_geocode)

    search_input = page.locator("#mapSearchInput")
    search_input.fill("Eiffel Tower")

    # Verify search result appears from geocode proxy
    results = page.locator("#mapSearchResults")
    expect(results).to_be_visible()
    eiffel_result = results.locator(".map-search-item", has_text="Eiffel Tower")
    expect(eiffel_result).to_be_visible()
    expect(eiffel_result.locator(".item-badge")).to_have_text("tourism")

    # Verify that the backend proxy endpoint /api/geocode was indeed called with ?q=Eiffel%20Tower
    assert len(geocode_requests) > 0
    assert "/api/geocode?q=Eiffel" in geocode_requests[0]

    # Clicking the result places the search pin
    eiffel_result.click()
    expect(page.locator(".search-location-pin")).to_be_visible()
    popup = page.locator(".map-popup-card.search-result-popup")
    expect(popup).to_be_visible()
    expect(popup.locator(".map-popup-title")).to_contain_text("Eiffel Tower")

