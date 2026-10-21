"""
UI tests for Smart Tags auto-labeling feature:
- Settings section status and buttons
- Smart Tags modal dialog styling, structure, and controls
- Scope selection, force option, and start button disabled states
- Modal dismissal via Close button, footer Cancel button, backdrop, and Escape key
- Multilingual i18n support across English, German, and Spanish
- AI marker badge rendering in sidebar and inspector
"""

import json
from playwright.sync_api import Page, expect


def test_smart_tags_settings_section_and_status(server, page: Page):
    """The Settings dialog displays the Smart Tags section with status and action buttons."""
    page.goto(server["url"])

    page.locator("#settingsBtn").click()
    settings_modal = page.locator("#settingsModal")
    expect(settings_modal).to_be_visible()

    # Section container exists
    section = page.locator("#settingsSmartTagsSection")
    expect(section).to_be_visible()

    # Title label
    expect(section.locator('label[data-i18n="smart_tags_title"]')).to_have_text("Smart Tags")

    # Status node
    status_el = page.locator("#settingsSmartTagsStatus")
    expect(status_el).to_be_visible()
    status_text = status_el.text_content()
    assert len(status_text) > 0, "Expected status text to be non-empty"

    # Action buttons
    generate_btn = page.locator("#settingsSmartTagsBtn")
    expect(generate_btn).to_be_visible()
    expect(generate_btn).to_have_text("Generate Smart Tags")

    clear_btn = page.locator("#settingsClearSmartTagsBtn")
    expect(clear_btn).to_be_visible()
    expect(clear_btn).to_have_text("Remove Smart Tags")

    # By default without CLIP models or indexed photos, buttons are appropriately disabled
    expect(generate_btn).to_be_disabled()

    page.locator("#closeSettingsModalBtn").click()
    expect(settings_modal).to_be_hidden()


def test_smart_tags_modal_structure_and_controls(server, page: Page):
    """The Smart Tags modal has face-job-dialog styling, header, body controls, and footer."""
    page.goto(server["url"])

    page.evaluate("() => window.imagineUI.openSmartTagsModal()")
    modal = page.locator("#smartTagsModal")
    expect(modal).to_be_visible()
    expect(modal).to_have_class("modal face-job-modal")

    # Backdrop
    backdrop = modal.locator(".modal-backdrop")
    expect(backdrop).to_be_visible()

    # Dialog
    dialog = modal.locator(".modal-dialog")
    expect(dialog).to_be_visible()
    expect(dialog).to_have_class("modal-dialog face-job-dialog")

    # Header with title and close button
    header = modal.locator(".modal-header")
    expect(header).to_be_visible()
    expect(header.locator("h3")).to_have_text("Generate Smart Tags")
    close_btn = page.locator("#closeSmartTagsJobBtn")
    expect(close_btn).to_be_visible()

    # Body
    body = modal.locator(".modal-body")
    expect(body).to_be_visible()
    expect(body).to_have_class("modal-body face-job-body")

    # Scope selection dropdown
    expect(page.locator("#smartTagsScanScopeWrap")).to_be_visible()
    expect(page.locator("#smartTagsScanScope")).to_be_visible()
    expect(page.locator("#smartTagsScanScope")).to_have_value("catalog")

    # State prompt
    expect(page.locator("#smartTagsJobState")).to_be_visible()

    # Force relabel checkbox
    expect(page.locator("#smartTagsForceOption")).to_be_visible()
    expect(page.locator("#smartTagsForce")).not_to_be_checked()

    # Progress bar and counts wrap (hidden initially)
    expect(page.locator("#smartTagsJobProgressWrap")).to_be_hidden()
    expect(page.locator("#smartTagsJobProgress")).to_be_attached()
    expect(page.locator("#smartTagsJobCounts")).to_be_attached()

    # Footer buttons
    footer = modal.locator(".modal-footer")
    expect(footer).to_be_visible()
    expect(footer).to_have_class("modal-footer face-job-footer")

    start_btn = page.locator("#smartTagsJobStartBtn")
    expect(start_btn).to_be_visible()
    expect(start_btn).to_have_text("Start Tagging")
    expect(start_btn).to_be_disabled()  # disabled when index empty / models missing

    stop_btn = page.locator("#smartTagsJobStopBtn")
    expect(stop_btn).to_be_hidden()
    expect(stop_btn).to_have_text("Cancel")

    cancel_btn = page.locator("#cancelSmartTagsJobBtn")
    expect(cancel_btn).to_be_visible()

    page.locator("#closeSmartTagsJobBtn").click()
    expect(modal).to_be_hidden()


def test_smart_tags_modal_dismissal_methods(server, page: Page):
    """The Smart Tags modal can be dismissed via close icon, footer Close button, backdrop, and Escape."""
    page.goto(server["url"])
    modal = page.locator("#smartTagsModal")

    # 1. Dismiss via header close icon
    page.evaluate("() => window.imagineUI.openSmartTagsModal()")
    expect(modal).to_be_visible()
    page.locator("#closeSmartTagsJobBtn").click()
    expect(modal).to_be_hidden()

    # 2. Dismiss via footer cancel/close button
    page.evaluate("() => window.imagineUI.openSmartTagsModal()")
    expect(modal).to_be_visible()
    page.locator("#cancelSmartTagsJobBtn").click()
    expect(modal).to_be_hidden()

    # 3. Dismiss via backdrop click
    page.evaluate("() => window.imagineUI.openSmartTagsModal()")
    expect(modal).to_be_visible()
    page.locator("#smartTagsBackdrop").click(position={"x": 10, "y": 10})
    expect(modal).to_be_hidden()

    # 4. Dismiss via Escape key
    page.evaluate("() => window.imagineUI.openSmartTagsModal()")
    expect(modal).to_be_visible()
    page.keyboard.press("Escape")
    expect(modal).to_be_hidden()


def test_smart_tags_settings_open_modal_when_ready(server, page: Page):
    """When smart tags service is ready with indexed photos, clicking Generate opens the modal with start button enabled."""
    def handle_status(route):
        route.fulfill(
            status=200,
            content_type="application/json",
            body=json.dumps({
                "built": True,
                "ready": True,
                "taxonomyReady": True,
                "taxonomyError": "",
                "taxonomyVersion": "v1-hash",
                "labelCount": 56,
                "indexedPhotos": 42,
                "labeledPhotos": 10,
                "tagAssignments": 25,
                "modelId": "clip-vit-b32"
            })
        )

    page.route("**/api/smart-tags/status", handle_status)
    page.goto(server["url"])

    page.locator("#settingsBtn").click()
    settings_modal = page.locator("#settingsModal")
    expect(settings_modal).to_be_visible()

    # Status displays formatted ready counts
    status_el = page.locator("#settingsSmartTagsStatus")
    expect(status_el).to_contain_text("10 photos labeled · 56 labels (index: 42 photos)")

    generate_btn = page.locator("#settingsSmartTagsBtn")
    expect(generate_btn).to_be_enabled()

    # Clicking Generate Smart Tags closes settings and opens Smart Tags modal
    generate_btn.click()
    expect(settings_modal).to_be_hidden()

    smart_modal = page.locator("#smartTagsModal")
    expect(smart_modal).to_be_visible()

    start_btn = page.locator("#smartTagsJobStartBtn")
    expect(start_btn).to_be_visible()
    expect(start_btn).to_be_enabled()

    page.locator("#closeSmartTagsJobBtn").click()
    expect(smart_modal).to_be_hidden()


def test_smart_tags_i18n_multilingual(server, page: Page):
    """Smart Tags labels and modal content render correctly in English, German, and Spanish."""
    def handle_status(route):
        route.fulfill(
            status=200,
            content_type="application/json",
            body=json.dumps({
                "built": True,
                "ready": True,
                "taxonomyReady": True,
                "taxonomyError": "",
                "taxonomyVersion": "v1",
                "labelCount": 50,
                "indexedPhotos": 100,
                "labeledPhotos": 5,
                "tagAssignments": 12,
                "modelId": "clip-vit-b32"
            })
        )

    page.route("**/api/smart-tags/status", handle_status)
    page.goto(server["url"])

    # 1. English verification
    page.locator("#settingsBtn").click()
    expect(page.locator('label[data-i18n="smart_tags_title"]')).to_have_text("Smart Tags")
    expect(page.locator("#settingsSmartTagsBtn")).to_have_text("Generate Smart Tags")
    expect(page.locator("#settingsClearSmartTagsBtn")).to_have_text("Remove Smart Tags")
    page.locator("#closeSettingsModalBtn").click()

    page.evaluate("() => window.imagineUI.openSmartTagsModal()")
    expect(page.locator("#smartTagsTitle")).to_have_text("Generate Smart Tags")
    expect(page.locator('label[for="smartTagsScanScope"]')).to_have_text("Tag")
    expect(page.locator('label#smartTagsForceOption span')).to_have_text("Re-label all photos (overwrite existing smart tags)")
    page.locator("#closeSmartTagsJobBtn").click()

    # 2. German (de) verification
    page.locator("#langSelect").select_option("de")
    expect(page.locator("#langSelect")).to_have_value("de")

    page.locator("#settingsBtn").click()
    expect(page.locator('label[data-i18n="smart_tags_title"]')).to_have_text("Smart-Tags")
    expect(page.locator("#settingsSmartTagsBtn")).to_have_text("Smart-Tags generieren")
    expect(page.locator("#settingsClearSmartTagsBtn")).to_have_text("Smart-Tags entfernen")
    page.locator("#closeSettingsModalBtn").click()

    page.evaluate("() => window.imagineUI.openSmartTagsModal()")
    expect(page.locator("#smartTagsTitle")).to_have_text("Smart-Tags generieren")
    expect(page.locator('label[for="smartTagsScanScope"]')).to_have_text("Taggen")
    expect(page.locator('label#smartTagsForceOption span')).to_have_text("Alle Fotos neu taggen (bestehende Smart-Tags überschreiben)")
    page.locator("#closeSmartTagsJobBtn").click()

    # 3. Spanish (es) verification
    page.locator("#langSelect").select_option("es")
    expect(page.locator("#langSelect")).to_have_value("es")

    page.locator("#settingsBtn").click()
    expect(page.locator('label[data-i18n="smart_tags_title"]')).to_have_text("Etiquetas inteligentes")
    expect(page.locator("#settingsSmartTagsBtn")).to_have_text("Generar etiquetas inteligentes")
    expect(page.locator("#settingsClearSmartTagsBtn")).to_have_text("Eliminar etiquetas inteligentes")
    page.locator("#closeSettingsModalBtn").click()

    page.evaluate("() => window.imagineUI.openSmartTagsModal()")
    expect(page.locator("#smartTagsTitle")).to_have_text("Generar etiquetas inteligentes")
    expect(page.locator('label[for="smartTagsScanScope"]')).to_have_text("Etiquetar")
    expect(page.locator('label#smartTagsForceOption span')).to_have_text("Re-etiquetar todas las fotos (sobrescribir etiquetas inteligentes existentes)")
    page.locator("#closeSmartTagsJobBtn").click()


def test_smart_tags_ai_marker_in_sidebar_and_inspector(server, page: Page):
    """Tags with AI provenance render the .tag-ai-marker badge with tooltip."""
    def handle_tags(route):
        route.fulfill(
            status=200,
            content_type="application/json",
            body=json.dumps([
                {"id": 101, "name": "Sunset", "category": "keyword", "media_count": 3, "ai_count": 3},
                {"id": 102, "name": "Family", "category": "keyword", "media_count": 5, "ai_count": 0}
            ])
        )

    page.route("**/api/tags", handle_tags)
    page.goto(server["url"])

    # Expand keyword tags category
    page.locator('.tag-category-group[data-category="keyword"] .category-header').click()

    # Sunset tag has .tag-ai-marker badge
    sunset_item = page.locator('.tag-item[data-tag-id="101"]')
    expect(sunset_item).to_be_visible()
    ai_marker = sunset_item.locator(".tag-ai-marker")
    expect(ai_marker).to_be_visible()
    expect(ai_marker).to_have_text("AI")
    expect(ai_marker).to_have_attribute("title", "AI-generated smart tag")

    # Family tag has no AI marker
    family_item = page.locator('.tag-item[data-tag-id="102"]')
    expect(family_item).to_be_visible()
    expect(family_item.locator(".tag-ai-marker")).to_have_count(0)

    # Inspector chip test with tag.source === 'clip'
    page.evaluate("""() => {
        const item = {
            id: 1,
            title: "Test photo",
            tags: [
                { id: 101, name: "Sunset", category: "keyword", source: "clip" },
                { id: 102, name: "Family", category: "keyword", source: "manual" }
            ]
        };
        window.imagineUI.renderInspectorTags(item);
        const sel = document.getElementById('inspectorSelection');
        if (sel) sel.style.display = 'block';
        const noSel = document.getElementById('inspectorNoSelection');
        if (noSel) noSel.style.display = 'none';
    }""")

    inspector_tags = page.locator("#inspectorTags")
    expect(inspector_tags).to_be_visible()

    clip_badge = inspector_tags.locator(".tag-badge").nth(0)
    clip_marker = clip_badge.locator(".tag-ai-marker")
    expect(clip_marker).to_be_visible()
    expect(clip_marker).to_have_text("AI")
    expect(clip_marker).to_have_attribute("title", "Generated by CLIP visual model")

    manual_badge = inspector_tags.locator(".tag-badge").nth(1)
    expect(manual_badge.locator(".tag-ai-marker")).to_have_count(0)


def test_smart_tags_job_start_poll_cancel_and_clear(server, page: Page):
    """Exercises starting a job, polling progress, cancelling, and clearing smart tags."""
    job_status = {
        "id": 88,
        "kind": "tags",
        "state": "running",
        "scope": "catalog",
        "total": 100,
        "processed": 45,
        "skipped": 5,
        "failed": 0,
        "remaining": 50,
        "error": "",
        "created_at": 1000,
        "updated_at": 1010,
    }

    def handle_status(route):
        route.fulfill(
            status=200,
            content_type="application/json",
            body=json.dumps({
                "built": True,
                "ready": True,
                "taxonomyReady": True,
                "taxonomyError": "",
                "taxonomyVersion": "b47c8f2a1e0d4a9f",
                "labelCount": 58,
                "indexedPhotos": 100,
                "labeledPhotos": 10,
                "tagAssignments": 25,
                "modelId": "clip-vit-base-patch32",
            }),
        )

    def handle_jobs(route):
        if route.request.method == "POST":
            route.fulfill(status=200, content_type="application/json", body=json.dumps({"job_id": 88, "status": "ok"}))
        else:
            route.fulfill(status=200, content_type="application/json", body=json.dumps(job_status))

    def handle_cancel(route):
        job_status["state"] = "cancelling"
        route.fulfill(status=200, content_type="application/json", body=json.dumps({"status": "ok"}))

    def handle_clear(route):
        route.fulfill(status=200, content_type="application/json", body=json.dumps({"status": "ok"}))

    page.route("**/api/smart-tags/status", handle_status)
    page.route("**/api/smart-tags/jobs", handle_jobs)
    page.route("**/api/smart-tags/jobs/88", handle_jobs)
    page.route("**/api/smart-tags/jobs/88/cancel", handle_cancel)
    page.route("**/api/smart-tags/clear", handle_clear)

    page.goto(server["url"])

    # 1. Open modal with ready status
    page.evaluate("() => window.imagineUI.openSmartTagsModal()")
    modal = page.locator("#smartTagsModal")
    expect(modal).to_be_visible()

    start_btn = page.locator("#smartTagsJobStartBtn")
    expect(start_btn).to_be_enabled()
    expect(start_btn).to_have_text("Start Tagging")

    # 2. Click Start Tagging -> triggers POST /api/smart-tags/jobs and polling starts
    start_btn.click()

    # Progress bar and counts become visible
    progress_wrap = page.locator("#smartTagsJobProgressWrap")
    expect(progress_wrap).to_be_visible()
    stop_btn = page.locator("#smartTagsJobStopBtn")
    expect(stop_btn).to_be_visible()
    expect(stop_btn).to_have_text("Cancel")

    # Counts text reflects polling state
    counts_el = page.locator("#smartTagsJobCounts")
    expect(counts_el).to_contain_text("45 labeled")

    # 3. Click Cancel -> triggers POST /api/smart-tags/jobs/88/cancel
    stop_btn.click()
    state_msg = page.locator("#smartTagsJobState")
    expect(state_msg).to_contain_text("Cancelling")

    # Close modal
    page.locator("#closeSmartTagsJobBtn").click()
    expect(modal).to_be_hidden()

    # 4. Clear Smart Tags from Settings
    page.locator("#settingsBtn").click()
    settings_modal = page.locator("#settingsModal")
    expect(settings_modal).to_be_visible()

    clear_btn = page.locator("#settingsClearSmartTagsBtn")
    expect(clear_btn).to_be_visible()
    expect(clear_btn).to_be_enabled()

    # Handle confirmation dialog
    page.once("dialog", lambda dialog: dialog.accept())
    clear_btn.click()

    # Toast confirms removal
    toast = page.locator(".toast")
    expect(toast).to_be_visible()
    expect(toast).to_contain_text("Smart tags removed")
