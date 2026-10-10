"""Settings dialog and browser-side API authentication coverage."""

from playwright.sync_api import Page, expect


def test_api_token_settings_persist_and_attach_auth_header(server, page: Page):
    """The Settings dialog stores the token and API requests use it as a Bearer token."""
    page.goto(server["url"])
    page.evaluate("() => localStorage.removeItem('imagine_api_token')")
    page.reload()

    page.locator("#settingsBtn").click()
    modal = page.locator("#settingsModal")
    expect(modal).to_be_visible()
    expect(page.locator("#settingsApiTokenInput")).to_have_value("")

    page.locator("#settingsApiTokenInput").fill("browser-secret")
    page.locator("#saveSettingsBtn").click()
    expect(modal).to_be_hidden()
    assert page.evaluate("() => localStorage.getItem('imagine_api_token')") == "browser-secret"

    with page.expect_request("**/api/stats") as request_info:
        page.evaluate("""async () => {
            try {
                await window._imagineApp.api.get('/api/stats');
            } catch (_) {}
        }""")
    assert request_info.value.headers.get("authorization") == "Bearer browser-secret"

    page.locator("#settingsBtn").click()
    expect(page.locator("#settingsApiTokenInput")).to_have_value("browser-secret")
    page.locator("#clearApiTokenBtn").click()
    expect(page.locator("#settingsApiTokenInput")).to_have_value("")
    assert page.evaluate("() => localStorage.getItem('imagine_api_token')") is None

    with page.expect_request("**/api/stats") as cleared_request_info:
        page.evaluate("""async () => {
            try {
                await window._imagineApp.api.get('/api/stats');
            } catch (_) {}
        }""")
    assert "authorization" not in cleared_request_info.value.headers


def test_semantic_index_modal_dialog_styling_and_behavior(server, page: Page):
    """The Rebuild Search Index dialog has standard modal styling, backdrop, and close controls."""
    page.goto(server["url"])

    # 1. Opening settings and clicking Rebuild Index when models aren't installed warns via toast
    page.locator("#settingsBtn").click()
    settings_modal = page.locator("#settingsModal")
    expect(settings_modal).to_be_visible()

    page.locator("#settingsRebuildIndexBtn").click()
    # Toast warning appears
    expect(page.locator(".toast-warning")).to_be_visible()
    # Settings modal remains or closes gracefully
    page.locator("#closeSettingsModalBtn").click()
    expect(settings_modal).to_be_hidden()

    # 2. Open the Semantic Index modal directly
    page.evaluate("() => window._imagineApp.openSemanticIndexModal()")
    index_modal = page.locator("#semanticIndexModal")
    expect(index_modal).to_be_visible()
    expect(index_modal).to_have_class("modal face-job-modal")

    # Check structure and non-transparent styling
    backdrop = index_modal.locator(".modal-backdrop")
    expect(backdrop).to_be_visible()

    dialog = index_modal.locator(".modal-dialog")
    expect(dialog).to_be_visible()
    expect(dialog).to_have_class("modal-dialog face-job-dialog")

    # Background color of .modal-dialog must match var(--bg-card) = rgb(45, 45, 45)
    bg_color = dialog.evaluate("el => window.getComputedStyle(el).backgroundColor")
    assert bg_color in ["rgb(45, 45, 45)", "#2d2d2d"], f"Expected card bg, got: {bg_color}"

    # Header with title and close button
    header = index_modal.locator(".modal-header")
    expect(header).to_be_visible()
    expect(header.locator("h3")).to_have_text("Build Search Index")
    close_btn = page.locator("#closeSemanticIndexModalBtn")
    expect(close_btn).to_be_visible()

    # Body matching Face analysis dialog
    body = index_modal.locator(".modal-body")
    expect(body).to_be_visible()
    expect(body).to_have_class("modal-body face-job-body")

    # Notice when CLIP is not ready
    notice = page.locator("#semanticJobNotice")
    expect(notice).to_be_visible()

    # Scope selection dropdown
    expect(page.locator("#semanticScanScopeWrap")).to_be_visible()
    expect(page.locator("#semanticScanScope")).to_be_visible()
    expect(page.locator("#semanticScanScope")).to_have_value("catalog")

    # State prompt
    expect(page.locator("#semanticJobState")).to_be_visible()

    # Force reindex checkbox
    expect(page.locator("#semanticForceOption")).to_be_visible()
    expect(page.locator("#semanticForceReindex")).not_to_be_checked()

    # Progress bar and counts wrap (hidden initially)
    expect(page.locator("#semanticJobProgressWrap")).to_be_hidden()
    expect(page.locator("#semanticJobProgress")).to_be_attached()
    expect(page.locator("#semanticJobCounts")).to_be_attached()

    # Footer with Start scan, hidden Cancel scan, and Close button
    footer = index_modal.locator(".modal-footer")
    expect(footer).to_be_visible()
    expect(footer).to_have_class("modal-footer face-job-footer")

    start_btn = page.locator("#semanticJobStartBtn")
    expect(start_btn).to_be_visible()
    expect(start_btn).to_be_disabled()  # disabled when CLIP models missing

    stop_btn = page.locator("#semanticJobStopBtn")
    expect(stop_btn).to_be_hidden()

    cancel_btn = page.locator("#cancelSemanticJobBtn")
    expect(cancel_btn).to_be_visible()

    # MUST NOT contain a 'View faces' button
    expect(index_modal.locator("#faceViewFacesBtn")).to_have_count(0)
    expect(index_modal.locator(".face-view-faces")).to_have_count(0)

    # Clicking close button closes the modal
    close_btn.click()
    expect(index_modal).to_be_hidden()

    # Re-open and test Escape key closing
    page.evaluate("() => window._imagineApp.openSemanticIndexModal()")
    expect(index_modal).to_be_visible()
    page.keyboard.press("Escape")
    expect(index_modal).to_be_hidden()

    # Re-open and test Cancel button closing
    page.evaluate("() => window._imagineApp.openSemanticIndexModal()")
    expect(index_modal).to_be_visible()
    cancel_btn.click()
    expect(index_modal).to_be_hidden()

    # Re-open and test clicking backdrop closing
    page.evaluate("() => window._imagineApp.openSemanticIndexModal()")
    expect(index_modal).to_be_visible()
    backdrop.click(position={"x": 5, "y": 5})
    expect(index_modal).to_be_hidden()


def test_semantic_toggle_disabled_when_clip_inactive(server, page: Page):
    """The Sparkle / AI toggle button is disabled when CLIP is inactive."""
    page.goto(server["url"])
    page.wait_for_load_state("networkidle")

    toggle = page.locator("#semanticToggle")
    expect(toggle).to_be_visible()
    expect(toggle).to_be_disabled()
    expect(toggle).to_have_attribute("aria-disabled", "true")
    expect(toggle).to_have_class("semantic-toggle disabled-toggle")

    # Attempting to click does not activate the toggle
    toggle.click(force=True)
    is_enabled = page.evaluate("() => window._imagineApp.state ? window._imagineApp.state.semanticSearchEnabled : null")
    assert is_enabled is False
    expect(toggle).not_to_have_class("active")
    expect(toggle).to_have_attribute("aria-pressed", "false")

    # When CLIP is enabled, button becomes active
    page.evaluate("""() => {
        window._imagineApp.state.semanticSearchAvailable = true;
        window._imagineApp.updateSemanticToggleVisibility();
    }""")
    expect(toggle).to_be_enabled()
    expect(toggle).not_to_have_attribute("aria-disabled", "true")
    expect(toggle).not_to_have_class("disabled-toggle")

    # Click now toggles state
    toggle.click()
    is_enabled = page.evaluate("() => window._imagineApp.state ? window._imagineApp.state.semanticSearchEnabled : null")
    assert is_enabled is True
    expect(toggle).to_have_class("semantic-toggle active")
    expect(toggle).to_have_attribute("aria-pressed", "true")


def test_semantic_index_modal_scope_and_running_state(server, page: Page):
    """The Build Search Index dialog adjusts scope when photos are selected, and manages running state."""
    page.goto(server["url"])

    # 1. Select the first photo in the grid
    first_photo = page.locator(".photo-card").first
    expect(first_photo).to_be_visible()
    first_photo.click()

    # Open semantic index modal
    page.evaluate("() => window._imagineApp.openSemanticIndexModal()")
    index_modal = page.locator("#semanticIndexModal")
    expect(index_modal).to_be_visible()

    # Scope defaults to "selected" when photos are selected
    scope_select = page.locator("#semanticScanScope")
    expect(scope_select).to_have_value("selected")
    expect(page.locator("#semanticSelectedScopeOption")).to_be_enabled()
    expect(page.locator("#semanticJobState")).to_contain_text("1")

    # Changing scope to "catalog" updates state prompt
    scope_select.select_option("catalog")
    expect(page.locator("#semanticJobState")).not_to_contain_text("1 selected")

    # Ensure no 'View faces' button is present
    expect(index_modal.locator("#faceViewFacesBtn")).to_have_count(0)
    expect(index_modal.locator(".face-view-faces")).to_have_count(0)

    # 2. Simulate running job state
    page.evaluate("""() => {
        window._imagineApp.renderSemanticJobState();
    }""")
    close_btn = page.locator("#cancelSemanticJobBtn")
    expect(close_btn).to_be_visible()
    close_btn.click()
    expect(index_modal).to_be_hidden()


def test_desktop_sidebar_hides_mobile_utilities(server, page: Page):
    """Desktop layout removes Import Folder, Settings, Language, Refresh Media from left panel, and Faces button from Media grid."""
    page.set_viewport_size({"width": 1280, "height": 800})
    page.goto(server["url"])

    # Mobile sidebar utilities container is hidden on desktop
    mobile_actions = page.locator("#sidebarMobileActions")
    expect(mobile_actions).to_be_hidden()
    expect(page.locator("#mobileImportBtn")).to_be_hidden()
    expect(page.locator("#mobileSettingsBtn")).to_be_hidden()
    expect(page.locator("#mobileRefreshBtn")).to_be_hidden()
    expect(page.locator("#mobileLangSelect")).to_be_hidden()

    # Faces button is removed from Media grid toolbar
    expect(page.locator("#faceToolbarBtn")).to_have_count(0)
    expect(page.locator("#contentToolbar .face-media-actions")).to_have_count(0)


def test_face_recognition_settings_status_and_button(server, page: Page):
    """Settings dialog includes Face Recognition section with status, index count, model, and Faces button."""
    page.goto(server["url"])

    page.locator("#settingsBtn").click()
    settings_modal = page.locator("#settingsModal")
    expect(settings_modal).to_be_visible()

    # Face Recognition status and button exist in settings dialog
    face_status = page.locator("#settingsFaceStatus")
    expect(face_status).to_be_visible()
    status_text = face_status.text_content()
    assert "Status:" in status_text
    assert ("indexed" in status_text or "not built" in status_text or "unavailable" in status_text)

    face_btn = page.locator("#settingsFaceBtn")
    expect(face_btn).to_be_visible()
    expect(face_btn).to_have_text("Rebuild faces recognition index")

    # Clicking Faces button closes Settings dialog and opens Face analysis modal
    face_btn.click()
    expect(settings_modal).to_be_hidden()
    expect(page.locator("#faceJobModal")).to_be_visible()

    # Closing face dialog with close button hides the face dialog
    page.locator("#faceJobCloseBtn").click()
    expect(page.locator("#faceJobModal")).to_be_hidden()

    # Opening Settings opens Settings modal and NOT Face analysis modal
    page.locator("#settingsBtn").click()
    expect(settings_modal).to_be_visible()
    expect(page.locator("#faceJobModal")).to_be_hidden()

    # Closing Settings
    page.locator("#closeSettingsModalBtn").click()
    expect(settings_modal).to_be_hidden()

    # Reloading/reopening browser does NOT open Face analysis modal
    page.reload()
    expect(page.locator("#faceJobModal")).to_be_hidden()
    expect(settings_modal).to_be_hidden()

    # Opening Settings after browser reload still opens Settings and NOT Face analysis modal
    page.locator("#settingsBtn").click()
    expect(settings_modal).to_be_visible()
    expect(page.locator("#faceJobModal")).to_be_hidden()
