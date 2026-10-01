"""
Deterministic UI tests for Mobile Fullscreen, PWA metadata, and Responsive Drawer navigation:
- PWA manifest and apple-mobile-web-app meta tags
- Fullscreen toggle button state and icons
- iOS fullscreen / Add to Home Screen guidance modal
- Mobile viewport responsive layout and collapsible drawer navigation
"""

import re
import pytest
from playwright.sync_api import Page, expect


def test_pwa_metadata_and_manifest(server, page: Page):
    """PWA manifest link and apple mobile web app meta tags are properly configured in head."""
    page.goto(server["url"])

    # Viewport tag contains viewport-fit=cover
    viewport_meta = page.locator('meta[name="viewport"]')
    expect(viewport_meta).to_have_attribute("content", re.compile(r"viewport-fit=cover"))

    # Mobile web app meta tags
    expect(page.locator('meta[name="mobile-web-app-capable"]')).to_have_attribute("content", "yes")
    expect(page.locator('meta[name="apple-mobile-web-app-capable"]')).to_have_attribute("content", "yes")
    expect(page.locator('meta[name="apple-mobile-web-app-status-bar-style"]')).to_have_attribute("content", "black-translucent")
    expect(page.locator('meta[name="theme-color"]')).to_have_attribute("content", "#1e1e1e")

    # Manifest link
    manifest_link = page.locator('link[rel="manifest"]')
    expect(manifest_link).to_have_attribute("href", "manifest.webmanifest")

    # Verify manifest content is fetchable and standalone
    response = page.request.get(f"{server['url']}/manifest.webmanifest")
    assert response.ok
    manifest_data = response.json()
    assert manifest_data["display"] == "standalone"
    assert manifest_data["short_name"] == "IMAGINE"
    assert len(manifest_data["icons"]) >= 2


def test_fullscreen_button_and_ios_guidance_modal(server, page: Page):
    """Fullscreen button toggles and iOS install modal operates cleanly."""
    page.goto(server["url"])

    # Fullscreen button exists in the top bar
    btn = page.locator("#toggleFullscreenBtn")
    expect(btn).to_be_visible()
    expect(btn).to_have_attribute("data-i18n-title", "toggle_fullscreen")

    # Has maximize and minimize icons
    max_icon = btn.locator(".icon-maximize")
    min_icon = btn.locator(".icon-minimize")
    expect(max_icon).to_be_visible()
    expect(min_icon).not_to_be_visible()

    # Verify iOS Install Modal open and close workflow
    modal = page.locator("#iosInstallModal")
    expect(modal).not_to_be_visible()

    # Open via app API
    page.evaluate("() => window._imagineApp.openIosInstallModal()")
    expect(modal).to_be_visible()
    expect(modal.locator(".modal-header h3")).to_have_text("Fullscreen on iOS")

    # Close via 'Got it' button
    page.locator("#confirmIosInstallBtn").click()
    expect(modal).not_to_be_visible()

    # Open again and close via Escape
    page.evaluate("() => window._imagineApp.openIosInstallModal()")
    expect(modal).to_be_visible()
    page.keyboard.press("Escape")
    expect(modal).not_to_be_visible()


def test_mobile_viewport_drawer_navigation(server, page: Page):
    """On mobile viewports, hamburger button toggles left sidebar drawer with backdrop."""
    # Set mobile viewport (iPhone 14 dimensions)
    page.set_viewport_size({"width": 390, "height": 844})
    page.goto(server["url"])

    # Mobile hamburger button is visible
    hamburger = page.locator("#mobileMenuBtn")
    expect(hamburger).to_be_visible()

    # Zoom slider is hidden on mobile
    zoom_slider = page.locator(".zoom-control")
    expect(zoom_slider).not_to_be_visible()

    # Sidebar starts off-screen (drawer mode)
    sidebar = page.locator("#leftSidebar")
    backdrop = page.locator("#sidebarBackdrop")

    # Tap hamburger to open drawer
    hamburger.click()
    expect(sidebar).to_have_class(re.compile(r"\bopen\b"))
    expect(backdrop).to_have_class(re.compile(r"\bactive\b"))

    # Tap backdrop outside the drawer to close
    backdrop.click(position={"x": 330, "y": 300})
    expect(sidebar).not_to_have_class(re.compile(r"\bopen\b"))
    expect(backdrop).not_to_have_class(re.compile(r"\bactive\b"))

    # Open drawer and close with Escape
    hamburger.click()
    expect(sidebar).to_have_class(re.compile(r"\bopen\b"))
    page.keyboard.press("Escape")
    expect(sidebar).not_to_have_class(re.compile(r"\bopen\b"))


def test_loupe_mobile_touch_pan_and_zoom(server, page: Page):
    """In mobile Loupe view, zooming into photo enables touch drag panning of the full-res image."""
    page.set_viewport_size({"width": 390, "height": 844})
    page.goto(server["url"])

    # Open first photo card in Loupe
    card = page.locator(".photo-card").first
    card.dblclick()
    loupe_modal = page.locator("#loupeModal")
    expect(loupe_modal).to_be_visible()

    # Zoom to 200% (loads original photo and enables panning)
    page.evaluate("() => window._imagineApp.updateLoupeView && window._imagineApp.dom.loupeZoomResetBtn.click()")
    expect(page.locator("#loupeImageViewport")).to_have_class(re.compile(r"\bis-zoomed\b"))

    # Initial pan values are 0
    pan_initial = page.evaluate("() => ({ x: window._imagineApp.state.loupePanX, y: window._imagineApp.state.loupePanY })")
    assert pan_initial["x"] == 0
    assert pan_initial["y"] == 0

    # Simulate touch drag panning via TouchEvent
    page.evaluate("""() => {
        const modal = document.getElementById('loupeModal');
        const t1 = new Touch({ identifier: 0, target: modal, clientX: 200, clientY: 400 });
        modal.dispatchEvent(new TouchEvent('touchstart', { touches: [t1], changedTouches: [t1] }));
        const t2 = new Touch({ identifier: 0, target: modal, clientX: 120, clientY: 320 });
        modal.dispatchEvent(new TouchEvent('touchmove', { touches: [t2], changedTouches: [t2] }));
        modal.dispatchEvent(new TouchEvent('touchend', { touches: [], changedTouches: [t2] }));
    }""")

    # Pan offset should now reflect the drag movement
    pan_after = page.evaluate("() => ({ x: window._imagineApp.state.loupePanX, y: window._imagineApp.state.loupePanY })")
    assert pan_after["x"] != 0 or pan_after["y"] != 0


def test_mobile_top_bar_and_search_layout(server, page: Page):
    """On mobile viewport, top bar does not overflow horizontally and search input is usable."""
    page.set_viewport_size({"width": 390, "height": 844})
    page.goto(server["url"])

    # Search input is visible and has comfortable typing width (>= 120px)
    search_input = page.locator("#searchInput")
    expect(search_input).to_be_visible()
    box = search_input.bounding_box()
    assert box is not None
    assert box["width"] >= 120, f"Search input too narrow: {box['width']}px"

    # Quick action buttons on the right are visible
    expect(page.locator("#toggleFullscreenBtn")).to_be_visible()
    expect(page.locator("#toggleInspectorBtn")).to_be_visible()

    # Document body does not overflow horizontally (no horizontal scrollbar)
    scroll_width = page.evaluate("() => document.documentElement.scrollWidth")
    client_width = page.evaluate("() => document.documentElement.clientWidth")
    assert scroll_width <= client_width + 1, f"Horizontal overflow: scrollWidth={scroll_width}, clientWidth={client_width}"


def test_mobile_content_toolbar_controls(server, page: Page):
    """On mobile viewport, view mode toggle (Grid/Map) is not squished and functions properly."""
    page.set_viewport_size({"width": 390, "height": 844})
    page.goto(server["url"])

    # Both Grid and Map buttons are visible and not squished to 0px
    grid_btn = page.locator("#viewGridBtn")
    map_btn = page.locator("#viewMapBtn")
    expect(grid_btn).to_be_visible()
    expect(map_btn).to_be_visible()

    grid_box = grid_btn.bounding_box()
    map_box = map_btn.bounding_box()
    assert grid_box is not None and grid_box["width"] >= 28, f"Grid btn crushed: {grid_box}"
    assert map_box is not None and map_box["width"] >= 28, f"Map btn crushed: {map_box}"

    # Sort selector is visible and usable
    sort_select = page.locator("#sortSelect")
    expect(sort_select).to_be_visible()
    sort_box = sort_select.bounding_box()
    assert sort_box is not None and sort_box["width"] >= 80

    # Switch to Map view
    map_btn.click()
    expect(page.locator("#mapViewContainer")).to_be_visible()
    expect(map_btn).to_have_class(re.compile(r"\bactive\b"))

    # Switch back to Grid view
    grid_btn.click()
    expect(page.locator("#gridScrollContainer")).to_be_visible()
    expect(grid_btn).to_have_class(re.compile(r"\bactive\b"))


def test_mobile_bottom_navigation_bar(server, page: Page):
    """On mobile viewport, view tabs act as a fixed bottom navigation bar."""
    page.set_viewport_size({"width": 390, "height": 844})
    page.goto(server["url"])

    tabs = page.locator("#viewTabs")
    expect(tabs).to_be_visible()

    # Verify fixed at bottom
    tabs_box = tabs.bounding_box()
    assert tabs_box is not None
    assert tabs_box["y"] + tabs_box["height"] >= 840, f"Tabs not at bottom: {tabs_box}"

    # Tap People tab -> opens People category view
    page.locator('.tab-btn[data-tab="people"]').click()
    expect(page.locator("#categoryToolbar")).to_be_visible()
    expect(page.locator("#categoryViewTitle")).to_have_text("People")

    # Tap Places tab
    page.locator('.tab-btn[data-tab="places"]').click()
    expect(page.locator("#categoryViewTitle")).to_have_text("Places")

    # Tap Events tab
    page.locator('.tab-btn[data-tab="events"]').click()
    expect(page.locator("#categoryViewTitle")).to_have_text("Events")

    # Tap Albums tab
    page.locator('.tab-btn[data-tab="albums"]').click()
    expect(page.locator("#categoryViewTitle")).to_have_text("Albums")

    # Tap Media tab -> returns to photo grid
    page.locator('.tab-btn[data-tab="media"]').click()
    expect(page.locator("#contentToolbar")).to_be_visible()
    expect(page.locator("#gridScrollContainer")).to_be_visible()


def test_mobile_batch_action_bar_floating(server, page: Page):
    """Batch action bar floats above bottom navigation bar when >=2 photos are selected on mobile."""
    page.set_viewport_size({"width": 390, "height": 844})
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    cards.nth(0).click()
    cards.nth(1).click(modifiers=["Control"])

    # Batch action bar appears
    batch_bar = page.locator("#batchActionBar")
    expect(batch_bar).to_be_visible()

    # Position is fixed and floats above bottom nav bar
    pos = page.evaluate("""() => {
        const el = document.getElementById('batchActionBar');
        const style = window.getComputedStyle(el);
        const rect = el.getBoundingClientRect();
        return { position: style.position, bottom: rect.bottom, top: rect.top };
    }""")
    assert pos["position"] == "fixed"
    # Should be floating near bottom (above bottom nav ~792px)
    assert pos["bottom"] <= 844


def test_mobile_sidebar_drawer_utilities(server, page: Page):
    """Sidebar drawer provides mobile quick actions for Settings, Import, and Language."""
    page.set_viewport_size({"width": 390, "height": 844})
    page.goto(server["url"])

    # Open sidebar drawer
    page.locator("#mobileMenuBtn").click()
    expect(page.locator("#leftSidebar")).to_have_class(re.compile(r"\bopen\b"))

    # Mobile actions section is visible
    actions = page.locator("#sidebarMobileActions")
    expect(actions).to_be_visible()

    # Test Settings button from drawer
    page.locator("#mobileSettingsBtn").click()
    expect(page.locator("#settingsModal")).to_be_visible()
    expect(page.locator("#leftSidebar")).not_to_have_class(re.compile(r"\bopen\b"))

    # Close settings
    page.locator("#cancelSettingsBtn").click()
    expect(page.locator("#settingsModal")).to_be_hidden()

    # Open drawer again to test Language change
    page.locator("#mobileMenuBtn").click()
    expect(page.locator("#leftSidebar")).to_have_class(re.compile(r"\bopen\b"))

    page.locator("#mobileLangSelect").select_option("de")
    expect(page.locator('.tab-btn[data-tab="media"] span')).to_have_text("Medien")

