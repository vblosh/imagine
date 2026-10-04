"""Face scans and grouped bulk review through a deterministic API fixture."""

import json
import re
import sqlite3
import struct
import time
import zlib
from urllib.parse import parse_qs, urlparse
import pytest
from playwright.sync_api import Page, expect
from conftest import TRANSPARENT_1X1_PNG


def _face(face_id, media_id, suggestions=None):
    return dict(id=face_id, media_id=media_id, revision=1, x=24, y=12,
                width=42, height=48, score=.94, landmarks=[], person_tag_id=None,
                person_name=None, dismissed=False, embedding_error="",
                suggestions=suggestions or [], crop_url=f"/api/faces/{face_id}/crop?revision=1")


def _solid_png(width, height, color=(180, 50, 180)):
    def chunk(kind, data):
        payload = kind + data
        return struct.pack(">I", len(data)) + payload + struct.pack(">I", zlib.crc32(payload) & 0xffffffff)

    row = b"\x00" + bytes(color) * width
    raw = row * height
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw))
            + chunk(b"IEND", b""))


def _assert_overlay_matches_image(page, image_id, overlay_id):
    page.wait_for_function("""([imageId, overlayId]) => {
      const image = document.getElementById(imageId);
      const overlay = document.getElementById(overlayId);
      if (!image || !overlay || !image.complete || image.naturalWidth !== 200 || overlay.hidden) return false;
      if (image.getAnimations().some(animation => animation.playState === 'running')) return false;
      const a = image.getBoundingClientRect();
      const b = overlay.getBoundingClientRect();
      return Math.abs(a.left - b.left) <= 1 && Math.abs(a.top - b.top) <= 1
        && Math.abs(a.width - b.width) <= 1 && Math.abs(a.height - b.height) <= 1;
    }""", arg=[image_id, overlay_id])
    size = page.locator(f"#{image_id}").evaluate("img => [img.naturalWidth, img.naturalHeight]")
    assert size == [200, 100]
    image_bounds = page.locator(f"#{image_id}").bounding_box()
    overlay_bounds = page.locator(f"#{overlay_id}").bounding_box()
    for key in ("x", "y", "width", "height"):
        assert abs(image_bounds[key] - overlay_bounds[key]) <= 1, (key, image_bounds, overlay_bounds)
    # Compare the source-coordinate rectangle, excluding the non-scaling border stroke.
    face_bounds = page.locator(f"#{overlay_id} .face-box").first.evaluate("""rect => {
      const box = rect.getBBox(), matrix = rect.getScreenCTM();
      const start = new DOMPoint(box.x, box.y).matrixTransform(matrix);
      const end = new DOMPoint(box.x + box.width, box.y + box.height).matrixTransform(matrix);
      return {x: start.x, y: start.y, width: end.x - start.x, height: end.y - start.y};
    }""")
    expected = {
        "x": image_bounds["x"] + image_bounds["width"] * 24 / 200,
        "y": image_bounds["y"] + image_bounds["height"] * 12 / 100,
        "width": image_bounds["width"] * 42 / 200,
        "height": image_bounds["height"] * 48 / 100,
    }
    for key, value in expected.items():
        assert abs(face_bounds[key] - value) <= 2, (key, face_bounds, expected)


def _seed_review_face(server, file_name="mountain.bmp"):
    conn = sqlite3.connect(server["env"]["db_path"])
    try:
        media = conn.execute(
            "SELECT id, content_hash, width, height FROM media_items WHERE file_name=?", (file_name,)
        ).fetchone()
        tag = conn.execute("SELECT id FROM tags WHERE name='Alice' AND category='people'").fetchone()
        assert media and tag
        media_id, content_hash, width, height = media
        now = int(time.time())
        conn.execute("""INSERT INTO face_media_analysis
            (media_id,state,error,source_hash,width,height,analyzed_at)
            VALUES(?, 'complete', '', ?, ?, ?, ?)""",
                     (media_id, content_hash, width, height, now))
        cursor = conn.execute("""INSERT INTO faces
            (media_id,x,y,width,height,score,landmarks,embedding,embedding_size,embedding_error,
             person_tag_id,dismissed,revision,created_at,updated_at)
            VALUES(?, ?, ?, ?, ?, ?, ?, NULL, 0, '', NULL, 0, 1, ?, ?)""",
                             (media_id, 120, 110, 240, 260, .94, "[]", now, now))
        conn.commit()
        return media_id, int(tag[0]), int(cursor.lastrowid)
    finally:
        conn.close()


class FaceApi:
    def __init__(self, page):
        self.faces, self.names, self.jobs = {}, {}, {}
        self.posts, self.batches, self.grid_reads = [], [], []
        self.face_actions = []
        self.fail_ids = set()
        self.start_failures, self.job_overrides = [], []
        self.initial_job, self.hold_first_job = None, False
        self.ready = True
        page.route("**/api/faces/**", self.route)

    def route(self, route):
        parsed = urlparse(route.request.url)
        path, method = parsed.path, route.request.method
        body = json.loads(route.request.post_data or "{}")
        if path == "/api/faces/status":
            route.fulfill(json=dict(built=True, ready=self.ready, error="" if self.ready else "Models not configured", config={}, runtime={}, job=self.initial_job))
        elif path == "/api/faces/jobs" and method == "POST":
            self.posts.append((body, route.request.headers.get("authorization")))
            if self.start_failures:
                failure = self.start_failures.pop(0)
                if failure.get("network"):
                    route.abort("failed")
                else:
                    route.fulfill(status=failure["status"], json={"error": failure["error"]})
                return
            job_id = len(self.posts)
            total = len(body.get("media_ids", [])) if body["scope"] == "selected" else 6
            state = "running" if self.hold_first_job and job_id == 1 else "completed"
            job = dict(id=job_id, state=state, total=total,
                       processed=total if state == "completed" else 0,
                       skipped=0, failed=0, remaining=0 if state == "completed" else total, error="")
            if self.job_overrides:
                job.update(self.job_overrides.pop(0))
            self.jobs[job_id] = job
            route.fulfill(status=202, json=self.jobs[job_id])
        elif re.fullmatch(r"/api/faces/jobs/\d+/cancel", path):
            job = self.jobs[int(path.split("/")[-2])]
            job["state"] = "cancelled"
            route.fulfill(json=job)
        elif re.fullmatch(r"/api/faces/jobs/\d+", path):
            route.fulfill(json=self.jobs[int(path.rsplit("/", 1)[1])])
        elif path in ("/api/faces/grid", "/api/faces/review"):
            query = parse_qs(parsed.query)
            self.grid_reads.append(query)
            items = [face for face in self.faces.values()
                     if not face["dismissed"] or query.get("include_dismissed") == ["true"]]
            if path.endswith("review"):
                items = [face for face in items if face["person_tag_id"] is None]
            offset, limit = int(query.get("offset", [0])[0]), int(query.get("limit", [50])[0])
            route.fulfill(json=dict(items=items[offset:offset + limit], total=len(items)))
        elif re.fullmatch(r"/api/faces/media/\d+", path):
            media_id = int(path.rsplit("/", 1)[1])
            route.fulfill(json=dict(media_id=media_id, state="complete", error="", width=200, height=100,
                                    faces=[face for face in self.faces.values() if face["media_id"] == media_id]))
        elif re.fullmatch(r"/api/faces/\d+/(identity|dismiss|reject)", path):
            face_id, action = int(path.split("/")[-2]), path.rsplit("/", 1)[1]
            self.face_actions.append((face_id, action, body, route.request.headers.get("authorization")))
            face = self.faces[face_id]
            if face_id in self.fail_ids or body["revision"] != face["revision"]:
                route.fulfill(status=409, json={"error": "Face review is stale"})
                return
            if action == "identity":
                face["person_tag_id"] = body.get("tag_id")
                face["person_name"] = self.names.get(body.get("tag_id"))
            elif action == "dismiss":
                face["dismissed"] = body["dismissed"]
            else:
                face["suggestions"] = [s for s in face["suggestions"] if s["tag_id"] != body["tag_id"]]
            face["revision"] += 1
            face["crop_url"] = f"/api/faces/{face_id}/crop?revision={face['revision']}"
            route.fulfill(json=face)
        elif path == "/api/faces/batch":
            self.batches.append((body, route.request.headers.get("authorization")))
            updated, failed = [], []
            for item in body["faces"]:
                face = self.faces[item["id"]]
                if face["id"] in self.fail_ids or item["revision"] != face["revision"]:
                    failed.append(dict(id=face["id"], status=409, error="Face review is stale"))
                    continue
                if body["action"] in ("identity", "accept"):
                    face["person_tag_id"] = body.get("tag_id") if not body.get("name") else 999
                    face["person_name"] = body.get("name") or self.names.get(body.get("tag_id"))
                    face["suggestions"] = []
                elif body["action"] == "dismiss":
                    face["dismissed"] = body["dismissed"]
                face["revision"] += 1
                face["crop_url"] = f"/api/faces/{face['id']}/crop?revision={face['revision']}"
                updated.append(face)
            route.fulfill(json=dict(updated=updated, failed=failed))
        elif re.fullmatch(r"/api/faces/\d+/crop", path):
            route.fulfill(content_type="image/png", body=TRANSPARENT_1X1_PNG)
        else:
            route.continue_()


def _open_existing_grid(page):
    page.locator("#faceToolbarBtn").click()
    expect(page.locator("#faceJobModal")).to_be_visible()
    page.locator("#faceViewFacesBtn").click()
    expect(page.locator("#faceGridModal")).to_be_visible()


def test_inspector_faces_last_and_clear_identity_with_visible_conflict(server, page: Page):
    api = FaceApi(page)
    page.add_init_script("localStorage.setItem('imagine_api_token', 'face-inspector-test-token')")
    page.goto(server["url"])
    photo = page.locator(".photo-card").first
    media_id = int(photo.get_attribute("data-id"))
    api.names[98] = "Alice"
    api.faces = {81: _face(81, media_id), 82: _face(82, media_id)}
    for face in api.faces.values():
        face.update(person_tag_id=98, person_name="Alice")
    photo.click()
    expect(page.locator("#inspectorFaceList .face-card")).to_have_count(2)
    assert page.locator("#inspectorFacesSection").evaluate("section => section.parentElement.lastElementChild === section")
    remove = page.locator('.face-card[data-face-id="81"] [data-face-remove]')
    expect(remove).to_have_attribute("title", "Clear identity")
    remove.click()
    expect(page.locator('.face-card[data-face-id="81"]')).to_contain_text("Unnamed")
    assert api.face_actions[0] == (81, "identity", {"revision": 1, "tag_id": None}, "Bearer face-inspector-test-token")
    expect(photo).to_have_class(re.compile(r"\bselected\b"))
    api.fail_ids.add(82)
    conflict = page.locator('.face-card[data-face-id="82"] [data-face-remove]')
    conflict.click()
    expect(page.locator(".toast-error").filter(has_text="Face review is stale")).to_be_visible()
    expect(conflict).to_be_enabled()
    assert api.faces[82]["person_tag_id"] == 98


def test_inspector_unnamed_x_dismisses_detection_without_rejecting_person(server, page: Page):
    api = FaceApi(page)
    page.goto(server["url"])
    photo = page.locator(".photo-card").first
    media_id = int(photo.get_attribute("data-id"))
    api.faces = {81: _face(81, media_id, [dict(tag_id=98, name="Alice", score=.9)])}
    photo.click()
    page.locator("#inspectorFaceOverlayToggle").check()
    expect(page.locator("#inspectorFaceOverlay .face-box")).to_have_count(1)
    remove = page.locator('.face-card[data-face-id="81"] [data-face-remove]')
    expect(remove).to_have_attribute("title", "Dismiss detection")
    remove.click()
    expect(page.locator('.face-card[data-face-id="81"]')).to_contain_text("Detection dismissed")
    expect(page.locator("#inspectorFaceOverlay .face-box")).to_have_count(0)
    expect(page.locator('.face-card[data-face-id="81"] [data-face-remove]')).to_have_count(0)
    assert api.face_actions[0][:3] == (81, "dismiss", {"revision": 1, "dismissed": True})
    assert api.faces[81]["suggestions"][0]["tag_id"] == 98


@pytest.mark.parametrize("failure", [
    {"status": 401, "error": "Authentication required"},
    {"status": 409, "error": "A face scan is already running"},
    {"network": True},
], ids=["unauthorized", "conflict", "network"])
def test_scan_submission_failures_are_visible(server, page: Page, failure):
    api = FaceApi(page)
    api.start_failures = [failure]
    page.goto(server["url"])
    page.locator("#faceToolbarBtn").click()
    page.locator("#faceJobStartBtn").click()
    error = page.locator("#faceJobError")
    expect(error).to_be_visible()
    expect(error).to_contain_text("Could not start face analysis")
    if not failure.get("network"):
        expect(error).to_contain_text(failure["error"])
    expect(page.locator("#faceJobStartBtn")).to_be_enabled()


def test_scan_submission_error_survives_language_rerender_and_clears_on_retry(server, page: Page):
    api = FaceApi(page)
    api.start_failures = [{"status": 409, "error": "A face scan is already running"}]
    page.goto(server["url"])
    page.locator("#faceToolbarBtn").click()
    page.locator("#faceJobStartBtn").click()
    error = page.locator("#faceJobError")
    expect(error).to_be_visible()
    page.evaluate("""async () => {
      const i18n = await import('/i18n.js');
      i18n.setLanguage('es');
    }""")
    expect(error).to_be_visible()
    expect(error).to_contain_text("A face scan is already running")
    page.locator("#faceJobStartBtn").click()
    expect(page.locator("#faceGridModal")).to_be_visible()
    expect(error).to_be_hidden()
    assert len(api.posts) == 2


def test_completed_scan_with_photo_failures_keeps_retry_entry_in_face_grid(server, page: Page):
    api = FaceApi(page)
    api.job_overrides = [dict(processed=3, failed=2, remaining=1, error="2 photos failed")]
    page.goto(server["url"])
    page.locator("#faceToolbarBtn").click()
    page.locator("#faceJobStartBtn").click()
    expect(page.locator("#faceGridModal")).to_be_visible()
    expect(page.locator("#faceGridJobIssue")).to_be_visible()
    expect(page.locator("#faceGridJobIssueText")).to_contain_text("2")
    expect(page.locator("#faceGridJobIssueText")).to_contain_text("1")
    retry_from_grid = page.locator("#faceGridJobRetryBtn")
    expect(retry_from_grid).to_be_visible()
    retry_from_grid.click()
    expect(page.locator("#faceJobModal")).to_be_visible()
    expect(page.locator("#faceJobRetryBtn")).to_be_visible()
    page.locator("#faceJobCloseBtn").click()
    page.locator("#faceToolbarBtn").click()
    expect(page.locator("#faceJobError")).to_contain_text("2 photos failed")
    expect(page.locator("#faceJobRetryBtn")).to_be_visible()
    api.initial_job = api.jobs[1]
    page.reload()
    expect(page.locator("#faceJobModal")).to_be_visible()
    expect(page.locator("#faceJobError")).to_contain_text("2 photos failed")
    expect(page.locator("#faceJobRetryBtn")).to_be_visible()
    page.locator("#faceJobRetryBtn").click()
    expect(page.locator("#faceGridModal")).to_be_visible()
    expect(page.locator("#faceGridJobIssue")).to_be_hidden()
    assert len(api.posts) == 2


def test_toolbar_scan_warning_cancel_retry_and_automatic_grid(server, page: Page):
    api = FaceApi(page)
    api.hold_first_job = True
    page.add_init_script("localStorage.setItem('imagine_api_token', 'face-ui-test-token')")
    native_dialogs = []
    page.on("dialog", lambda dialog: (native_dialogs.append(dialog.message), dialog.dismiss()))
    page.goto(server["url"])
    photo = page.locator(".photo-card").first
    media_id = int(photo.get_attribute("data-id"))
    api.faces[81] = _face(81, media_id)
    photo.click()
    expect(page.locator("#inspectorAnalyzeFacesBtn")).to_have_count(0)
    page.locator("#faceToolbarBtn").click()
    expect(page.locator("#faceScanScope")).to_have_value("selected")
    page.locator("#faceForceReanalysis").check()
    page.locator("#faceJobStartBtn").click()
    expect(page.locator("#faceForceWarningModal .modal-dialog")).to_be_visible()
    page.keyboard.press("Escape")
    expect(page.locator("#faceForceWarningModal")).to_be_hidden()
    expect(page.locator("#faceJobModal")).to_be_visible()
    assert api.posts == []
    page.locator("#faceJobStartBtn").click()
    page.locator("#faceForceCancelBtn").click()
    assert api.posts == []
    page.locator("#faceJobStartBtn").click()
    page.locator("#faceForceConfirmBtn").click()
    expect(page.locator("#faceJobState")).to_contain_text("running")
    page.locator("#faceJobCloseBtn").click()
    expect(page.locator("#faceJobModal")).to_be_hidden()
    page.locator("#faceToolbarBtn").click()
    page.locator("#faceJobStopBtn").click()
    expect(page.locator("#faceJobRetryBtn")).to_be_visible()
    page.locator("#faceJobRetryBtn").click()
    expect(page.locator("#faceGridModal")).to_be_visible()
    expect(page.locator("#faceGridList .face-grid-card")).to_have_count(1)
    assert [body for body, _ in api.posts] == [dict(scope="selected", media_ids=[media_id], force=True),
                                            dict(scope="selected", media_ids=[media_id], force=False)]
    assert all(header == "Bearer face-ui-test-token" for _, header in api.posts)
    assert any(query.get("job_id") == ["2"] for query in api.grid_reads)
    assert native_dialogs == []
    expect(photo).to_have_class(re.compile(r"\bselected\b"))


def test_group_acceptance_and_shared_selected_face_controls(server, page: Page):
    api = FaceApi(page)
    page.add_init_script("localStorage.setItem('imagine_api_token', 'face-ui-test-token')")
    small_image = _solid_png(200, 100)
    page.route("**/api/thumbnails/**", lambda route: route.fulfill(content_type="image/png", body=small_image))
    page.route("**/api/photos/*/original*", lambda route: route.fulfill(content_type="image/png", body=small_image))
    page.set_viewport_size(dict(width=1600, height=1100))
    page.goto(server["url"])
    photo = page.locator(".photo-card").first
    media_id = int(photo.get_attribute("data-id"))
    photo.click()
    tags = page.evaluate("window._imagineState.tags.filter(t => t.category === 'people')")
    alice = next(tag for tag in tags if tag["name"] == "Alice")
    api.names[alice["id"]] = "Alice"
    suggestion = [dict(tag_id=alice["id"], name="Alice", score=.85)]
    api.faces = {81: _face(81, media_id, suggestion), 82: _face(82, media_id, suggestion),
                 83: _face(83, media_id), 84: _face(84, media_id)}
    page.locator("#inspectorFaceOverlayToggle").check()
    expect(page.locator("#inspectorFaceOverlay")).to_be_visible()
    expect(page.locator("#inspectorFaceOverlay")).to_have_attribute("viewBox", "0 0 200 100")
    expect(page.locator("#inspectorFaceOverlay .face-box")).to_have_count(4)
    _assert_overlay_matches_image(page, "inspectorImg", "inspectorFaceOverlay")
    expect(page.locator("#inspectorFaceList .face-name-input")).to_have_count(0)
    page.locator("#openLoupeFromInspector").click()
    page.locator("#loupeFaceOverlayToggle").check()
    expect(page.locator("#loupeFaceOverlay")).to_be_visible()
    _assert_overlay_matches_image(page, "loupeImg", "loupeFaceOverlay")
    page.locator("#loupeZoomInBtn").click()
    _assert_overlay_matches_image(page, "loupeImg", "loupeFaceOverlay")
    page.set_viewport_size(dict(width=1100, height=850))
    _assert_overlay_matches_image(page, "loupeImg", "loupeFaceOverlay")
    page.locator("#loupeCloseBtn").click()
    _open_existing_grid(page)
    expect(page.locator("#faceGridList .face-grid-card")).to_have_count(4)
    expect(page.locator("#faceGridPagination")).to_be_hidden()
    expect(page.locator("#faceGridRetryBtn")).to_be_hidden()
    page.set_viewport_size(dict(width=1600, height=1100))
    bounds = page.locator("#faceGridModal .modal-dialog").bounding_box()
    panel = page.locator("#faceGridSidePanel").bounding_box()
    main = page.locator(".face-grid-main").bounding_box()
    assert bounds["width"] >= 1000
    assert panel["x"] >= main["x"] + main["width"]
    page.mouse.move(bounds["x"] + bounds["width"] - 3, bounds["y"] + bounds["height"] - 3)
    page.mouse.down()
    page.mouse.move(bounds["x"] + bounds["width"] - 143, bounds["y"] + bounds["height"] - 123, steps=8)
    page.mouse.up()
    resized = page.locator("#faceGridModal .modal-dialog").bounding_box()
    assert 760 <= resized["width"] < bounds["width"]
    assert 480 <= resized["height"] < bounds["height"]
    group = page.locator(".face-grid-group").filter(has=page.locator('[data-face-id="81"]'))
    expect(group.locator(".face-grid-card")).to_have_count(2)
    expect(group.locator("[data-face-group-accept]")).to_have_text("Accept 1 suggestions")
    group.locator("[data-face-group-accept]").click()
    page.wait_for_function("() => [...document.querySelectorAll('.face-grid-group')].some(g => g.textContent.includes('Alice') && !g.querySelector('[data-face-group-accept]') && g.querySelectorAll('.face-grid-card').length === 2)")
    assert api.batches[0][0]["action"] == "accept"
    expect(page.locator("#faceGridIncludeDismissed")).to_be_enabled()
    assert {item["id"] for item in api.batches[0][0]["faces"]} == {81, 82}
    page.locator('.face-grid-card[data-face-id="83"] .face-grid-select').check()
    page.locator('.face-grid-card[data-face-id="84"] .face-grid-select').check()
    expect(page.locator("#faceGridSelectedCount")).to_contain_text("2")
    page.set_viewport_size(dict(width=390, height=844))
    bounds = page.locator("#faceGridModal .modal-dialog").bounding_box()
    assert bounds["x"] >= 0 and bounds["x"] + bounds["width"] <= 391
    name = page.locator("#faceGridNameInput")
    name.fill("é" * 51)
    page.locator("#faceGridApplyNameBtn").click()
    expect(page.locator(".toast-error, #faceGridActionError").filter(has_text="100 UTF-8 bytes").first).to_be_visible()
    assert len(api.batches) == 1
    name.fill("Charlie")
    page.locator("#faceGridApplyNameBtn").click()
    page.wait_for_function("() => [...document.querySelectorAll('.face-grid-group')].some(g => g.textContent.includes('Charlie') && g.querySelectorAll('.face-grid-card').length === 2)")
    assert api.batches[-1][0]["name"] == "Charlie"
    assert {item["id"] for item in api.batches[-1][0]["faces"]} == {83, 84}
    page.locator('.face-grid-card[data-face-id="83"] .face-grid-select').check()
    page.locator('.face-grid-card[data-face-id="84"] .face-grid-select').check()
    page.locator("#faceGridDismissBtn").click()
    expect(page.locator("#faceGridList .face-grid-card")).to_have_count(2)
    assert api.batches[-1][0]["action"] == "dismiss"
    assert api.batches[-1][0]["dismissed"] is True
    assert all(header == "Bearer face-ui-test-token" for _, header in api.batches)
    expect(photo).to_have_class(re.compile(r"\bselected\b"))


def test_group_accept_all_spans_pages_and_keeps_failed_faces_selected(server, page: Page):
    api = FaceApi(page)
    page.goto(server["url"])
    media_id = int(page.locator(".photo-card").first.get_attribute("data-id"))
    alice = next(tag for tag in page.evaluate("window._imagineState.tags") if tag["name"] == "Alice")
    api.names[alice["id"]] = "Alice"
    api.faces = {i: _face(i, media_id, [dict(tag_id=alice["id"], name="Alice", score=.85)]) for i in range(100, 1102)}
    api.fail_ids = {100}
    _open_existing_grid(page)
    accept = page.locator("[data-face-group-accept]").first
    expect(accept).to_be_enabled()
    expect(accept).to_have_text("Accept 1 suggestions")
    accept.click()
    expect(page.locator("#faceGridActionError")).to_be_visible()
    expect(page.locator("#faceGridSelectedCount")).to_contain_text("1")
    assert len(api.batches) == 2
    assert all(len(body["faces"]) <= 1000 for body, _ in api.batches)
    assert {item["id"] for body, _ in api.batches for item in body["faces"]} == set(range(100, 1102))
    assert sum(1 for face in api.faces.values() if face["person_name"] == "Alice") == 1001
    expect(page.locator('.face-grid-card[data-face-id="100"] .face-grid-select')).to_be_checked()


def test_interrupted_scan_without_saved_request_can_retry_catalog(server, page: Page):
    api = FaceApi(page)
    api.initial_job = dict(id=12, state="interrupted", total=5, processed=2, skipped=0, failed=1, remaining=2, error="server restarted")
    page.goto(server["url"])
    expect(page.locator("#faceJobRetryBtn")).to_be_visible()
    page.locator("#faceJobRetryBtn").click()
    expect(page.locator("#faceGridModal")).to_be_visible()
    assert api.posts[0][0] == dict(scope="catalog", force=False)


def test_named_unnamed_filters_selection_pagination_and_dismissed(server, page: Page):
    api = FaceApi(page)
    page.goto(server["url"])
    media_id = int(page.locator(".photo-card").first.get_attribute("data-id"))
    api.faces = {i: _face(i, media_id) for i in range(1, 204)}
    api.faces[1].update(person_tag_id=99, person_name="Named person")
    api.faces[2].update(person_tag_id=99, person_name="Named person", dismissed=True)
    api.faces[3]["suggestions"] = [dict(tag_id=99, name="Named person", score=.9)]
    _open_existing_grid(page)
    expect(page.locator("#faceGridShowNamed")).to_be_checked()
    expect(page.locator("#faceGridShowUnnamed")).to_be_checked()
    page.locator("#faceGridSelectAllBtn").click()
    expect(page.locator("#faceGridSelectedCount")).to_contain_text("202")
    page.locator("#faceGridShowNamed").uncheck()
    expect(page.locator("#faceGridSelectedCount")).to_contain_text("201")
    expect(page.locator('.face-grid-card[data-face-id="1"]')).to_have_count(0)
    expect(page.locator('.face-grid-card[data-face-id="3"]')).to_be_visible()
    page.locator("#faceGridNextBtn").click()
    expect(page.locator("#faceGridList .face-grid-card")).to_have_count(1)
    page.locator("#faceGridShowUnnamed").uncheck()
    expect(page.locator("#faceGridEmpty")).to_be_visible()
    expect(page.locator("#faceGridSelectedCount")).to_contain_text("0")
    expect(page.locator("#faceGridSelectAllBtn")).to_be_disabled()
    page.locator("#faceGridShowNamed").check()
    expect(page.locator("#faceGridList .face-grid-card")).to_have_count(1)
    page.locator("#faceGridIncludeDismissed").check()
    expect(page.locator("#faceGridList .face-grid-card")).to_have_count(2)
    page.locator("#faceGridSelectAllBtn").click()
    expect(page.locator("#faceGridSelectedCount")).to_contain_text("2")
    page.locator("#faceGridDismissBtn").click()
    expect(page.locator("#faceGridSelectedCount")).to_contain_text("0")
    assert [f["id"] for f in api.batches[0][0]["faces"]] == [1]


def test_person_group_merges_confirmed_and_suggested_faces_without_page_split(server, page: Page):
    api = FaceApi(page)
    page.goto(server["url"])
    photos = page.locator(".photo-card")
    first_id = int(photos.nth(0).get_attribute("data-id"))
    second_id = int(photos.nth(1).get_attribute("data-id"))
    alice = dict(tag_id=98, name="Alice", score=.9)
    bob = dict(tag_id=99, name="Bob", score=.8)
    api.names = {98: "Alice", 99: "Bob"}
    # Interleave another identity in source order, beyond the old 200-card boundary.
    api.faces = {1: _face(1, first_id), 204: _face(204, first_id, [bob])}
    api.faces[1].update(person_tag_id=98, person_name="Alice")
    api.faces.update({i: _face(i, first_id if i % 2 else second_id, [alice])
                      for i in range(2, 204)})
    _open_existing_grid(page)
    group = page.locator('.face-grid-group[data-group-key="person:98"]')
    expect(group).to_have_count(1)
    expect(group.locator(".face-grid-card")).to_have_count(203)
    expect(group.locator("[data-face-group-accept]")).to_have_text("Accept 2 suggestions")
    expect(page.locator('.face-grid-card[data-face-id="204"]')).to_have_count(0)
    page.locator("#faceGridNextBtn").click()
    expect(page.locator('.face-grid-group[data-group-key="person:99"] .face-grid-card')).to_have_count(1)
    page.locator("#faceGridPrevBtn").click()
    group.locator("[data-face-group-accept]").click()
    expect(group.locator("[data-face-group-accept]")).to_have_count(0)
    expect(group.locator(".face-grid-card")).to_have_count(203)
    assert {f["id"] for f in api.batches[0][0]["faces"]} == set(range(2, 204))
    assert api.faces[1]["revision"] == 1


def test_faces_toolbar_can_review_without_models_and_localizes_grid(server, page: Page):
    api = FaceApi(page)
    api.ready = False
    page.goto(server["url"])
    page.locator("#faceToolbarBtn").click()
    expect(page.locator("#faceScanScope")).to_have_value("catalog")
    expect(page.locator("#faceJobStartBtn")).to_be_disabled()
    expect(page.locator("#faceJobNotice")).to_contain_text("Models not configured")
    page.locator("#faceViewFacesBtn").click()
    expect(page.locator("#faceGridModal")).to_be_visible()
    expect(page.locator("#faceGridList .face-grid-card")).to_have_count(0)
    for language in ("es", "de", "ru", "zh", "en"):
        translated, failure_notice = page.evaluate("""async lang => {
            const i18n = await import('/i18n.js'); i18n.setLanguage(lang);
            return [i18n.t('face_grid_title'), i18n.t('face_job_finished_with_failures', { failed: 2, remaining: 1 })];
        }""", language)
        expect(page.locator("#faceGridHeading")).to_have_text(translated)
        assert translated != "face_grid_title"
        assert "face_job_finished_with_failures" not in failure_notice
        assert "2" in failure_notice and "1" in failure_notice


def test_identity_change_refreshes_active_people_filter_and_prunes_stale_selection(server, page: Page):
    birthday_id = None
    _, alice_id, face_id = _seed_review_face(server)
    page.goto(server["url"])
    page.evaluate("window._imagineState.mediaLimit = 1")
    media_queries = []

    def capture_media_query(request):
        parsed = urlparse(request.url)
        if parsed.path == "/api/media":
            media_queries.append(parse_qs(parsed.query))

    page.on("request", capture_media_query)
    page.locator('.tag-category-group[data-category="people"] .category-header').click()
    alice_tag = page.locator("#tagCategoryPeople .tag-item", has_text="Alice")
    alice_tag.click()
    expect(page.locator(".photo-card")).to_have_count(1)
    birthday = page.locator(".photo-card", has_text="birthday.bmp")
    expect(birthday).to_be_visible()
    birthday_id = int(birthday.get_attribute("data-id"))
    birthday.click()
    expect(birthday).to_have_class(re.compile(r"\bselected\b"))
    assert birthday_id in page.evaluate("Array.from(window._imagineState.selectedIds)")

    media_queries.clear()
    page.locator("#faceToolbarBtn").click()
    page.locator("#faceViewFacesBtn").click()
    expect(page.locator(f'.face-grid-card[data-face-id="{face_id}"]')).to_be_visible()
    page.locator(f'.face-grid-card[data-face-id="{face_id}"] .face-grid-select').check()
    page.locator("#faceGridPersonSelect").select_option(str(alice_id))
    page.locator("#faceGridApplyNameBtn").click()

    mountain = page.locator(".photo-card", has_text="mountain.bmp")
    expect(mountain).to_be_visible()
    expect(page.locator(".photo-card")).to_have_count(1)
    assert page.evaluate("window._imagineState.totalCount") == 2
    assert birthday_id not in page.evaluate("Array.from(window._imagineState.selectedIds)")
    assert alice_id in page.evaluate("Array.from(window._imagineState.activeTagIds)")
    refreshed_queries = [query for query in media_queries if query.get("tag_id") == [str(alice_id)]]
    assert refreshed_queries, media_queries
    assert any(query.get("limit") == ["1"] and query.get("offset") == ["0"] for query in refreshed_queries)

    media_queries.clear()
    page.locator(f'.face-grid-card[data-face-id="{face_id}"] .face-grid-select').check()
    page.locator("#faceGridClearBtn").click()
    birthday = page.locator(".photo-card", has_text="birthday.bmp")
    expect(birthday).to_be_visible()
    expect(page.locator(".photo-card")).to_have_count(1)
    assert page.evaluate("window._imagineState.totalCount") == 1
    assert alice_id in page.evaluate("Array.from(window._imagineState.activeTagIds)")
    cleared_queries = [query for query in media_queries if query.get("tag_id") == [str(alice_id)]]
    assert cleared_queries, media_queries
    assert any(query.get("limit") == ["1"] and query.get("offset") == ["0"] for query in cleared_queries)
