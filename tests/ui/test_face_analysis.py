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
        self.group_queries, self.group_reads, self.group_accepts, self.group_accept_headers = [], [], [], []
        self.face_get_reads, self.face_lookup_reads = [], []
        self.group_page_failures, self.short_group_pages = {}, set()
        self.group_snapshot_number, self.group_snapshot, self.group_members = 0, "", {}
        self.group_snapshots = {}
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
        elif path == "/api/faces/groups" and method == "GET":
            query = parse_qs(parsed.query)
            self.group_queries.append(query)
            include_dismissed = query.get("include_dismissed") == ["true"]
            show_named = query.get("show_named", ["true"])[0] == "true"
            show_unnamed = query.get("show_unnamed", ["true"])[0] == "true"
            confirmed_person_id = query.get("person_tag_id", [""])[0]
            filtered = []
            for face in self.faces.values():
                if confirmed_person_id and str(face["person_tag_id"] or "") != confirmed_person_id:
                    continue
                if face["dismissed"]:
                    if not include_dismissed:
                        continue
                else:
                    confirmed = face["person_tag_id"] is not None
                    if confirmed and not show_named or not confirmed and not show_unnamed:
                        continue
                filtered.append(face)
            members = {}
            for face in filtered:
                if face["dismissed"]:
                    key, group_type = "dismissed", "dismissed"
                elif face["person_tag_id"] is not None:
                    key, group_type = f"person:{face['person_tag_id']}", "person"
                elif face["suggestions"] and int(face["suggestions"][0]["tag_id"]) > 0:
                    key, group_type = f"person:{face['suggestions'][0]['tag_id']}", "person"
                else:
                    key, group_type = "unnamed", "unnamed"
                members.setdefault(key, []).append(face)
            groups = []
            for key, faces in members.items():
                faces.sort(key=lambda face: (face["media_id"], face["id"]))
                if key.startswith("person:"):
                    group_type = "person"
                    tag_id = int(key.split(":", 1)[1])
                    name = self.names.get(tag_id) or next((face["person_name"] for face in faces if face["person_name"]), None)
                    if not name:
                        name = next((face["suggestions"][0]["name"] for face in faces if face["suggestions"]), f"Person {tag_id}")
                else:
                    tag_id = None
                    group_type = "dismissed" if key == "dismissed" else "unnamed"
                    name = "Dismissed" if group_type == "dismissed" else "Unnamed"
                eligible = [face for face in faces if not face["dismissed"] and face["person_tag_id"] is None
                            and face["suggestions"] and key == f"person:{face['suggestions'][0]['tag_id']}"]
                groups.append(dict(key=key, type=group_type, tag_id=tag_id, name=name,
                                   faces_count=len(faces), photos_count=len({face["media_id"] for face in faces}),
                                   suggestion_faces_count=len(eligible),
                                   suggestion_photos_count=len({face["media_id"] for face in eligible})))
            groups.sort(key=lambda group: (0 if group["type"] == "person" else 1 if group["type"] == "unnamed" else 2,
                                           group["name"].casefold(), group["key"]))
            self.group_snapshot_number += 1
            self.group_snapshot = f"face-snapshot-{self.group_snapshot_number}"
            self.group_members = {group["key"]: members[group["key"]] for group in groups}
            self.group_snapshots[self.group_snapshot] = self.group_members
            while len(self.group_snapshots) > 4:
                del self.group_snapshots[next(iter(self.group_snapshots))]
            route.fulfill(json=dict(snapshot=self.group_snapshot, total=len(filtered), groups=groups))
        elif path == "/api/faces/group" and method == "GET":
            query = parse_qs(parsed.query)
            key = query.get("key", [""])[0]
            snapshot = query.get("snapshot", [""])[0]
            offset, limit = int(query.get("offset", [0])[0]), int(query.get("limit", [48])[0])
            self.group_reads.append(dict(key=key, snapshot=snapshot, offset=offset, limit=limit))
            snapshot_members = self.group_snapshots.get(snapshot)
            if snapshot_members is None or key not in snapshot_members:
                route.fulfill(status=409, json={"error": "Face review snapshot is stale"})
                return
            if self.group_page_failures.get(offset, 0):
                self.group_page_failures[offset] -= 1
                route.fulfill(status=503, json={"error": "Temporary crop page failure"})
                return
            members = snapshot_members[key]
            items = members[offset:offset + limit]
            if offset in self.short_group_pages and items:
                items = items[:-1]
            route.fulfill(json=dict(snapshot=snapshot, items=items, total=len(members), offset=offset, limit=limit))
        elif path == "/api/faces/groups/accept" and method == "POST":
            self.group_accepts.append(body)
            self.group_accept_headers.append(route.request.headers.get("authorization"))
            snapshot_members = self.group_snapshots.get(body.get("snapshot"))
            if snapshot_members is None or body.get("key") not in snapshot_members:
                route.fulfill(status=409, json={"error": "Face review snapshot is stale"})
                return
            updated, failed = 0, []
            for face in snapshot_members[body["key"]]:
                if face["dismissed"] or face["person_tag_id"] is not None or not face["suggestions"]:
                    continue
                suggestion = face["suggestions"][0]
                if face["id"] in self.fail_ids:
                    failed.append(dict(id=face["id"], status=409, error="Face review is stale"))
                    continue
                face["person_tag_id"] = suggestion["tag_id"]
                face["person_name"] = suggestion["name"]
                face["suggestions"] = []
                face["revision"] += 1
                face["crop_url"] = f"/api/faces/{face['id']}/crop?revision={face['revision']}"
                updated += 1
            route.fulfill(json=dict(updated_count=updated, failed_count=len(failed), failed=failed))
        elif path == "/api/faces/lookup" and method == "GET":
            ids = [int(value) for value in parse_qs(parsed.query).get("ids", [""])[0].split(",") if value]
            self.face_lookup_reads.append(ids)
            route.fulfill(json=dict(items=[self.faces[face_id] for face_id in ids if face_id in self.faces], failed=[]))
        elif re.fullmatch(r"/api/faces/\d+", path) and method == "GET":
            face_id = int(path.rsplit("/", 1)[1])
            self.face_get_reads.append(face_id)
            if face_id not in self.faces:
                route.fulfill(status=404, json={"error": "Face not found"})
            else:
                route.fulfill(json=self.faces[face_id])
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
                elif body["action"] == "reject":
                    tag_id = item.get("tag_id") or body.get("tag_id")
                    face["suggestions"] = [s for s in face["suggestions"] if s["tag_id"] != tag_id]
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
    expect(page.locator("#faceGridIncludeDismissed")).to_be_enabled()


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
    expect(page.locator("#faceJobRetryBtn")).to_have_count(0)
    expect(page.locator("#faceJobCancelBtn")).to_have_count(0)
    expect(page.locator("#faceViewFacesBtn")).to_be_visible()
    expect(page.locator("#faceJobDoneBtn")).to_be_visible()
    page.locator("#faceJobCloseBtn").click()
    page.locator("#faceToolbarBtn").click()
    expect(page.locator("#faceJobModal")).to_be_visible()
    expect(page.locator("#faceViewFacesBtn")).to_be_visible()
    expect(page.locator("#faceJobStartBtn")).to_be_visible()
    expect(page.locator("#faceJobDoneBtn")).to_be_visible()
    expect(page.locator("#faceJobCancelBtn")).to_have_count(0)
    expect(page.locator("#faceJobRetryBtn")).to_have_count(0)


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
    expect(page.locator("#faceViewFacesBtn")).to_be_visible()
    expect(page.locator("#faceJobStartBtn")).to_be_visible()
    expect(page.locator("#faceJobDoneBtn")).to_be_visible()
    expect(page.locator("#faceJobCancelBtn")).to_have_count(0)
    expect(page.locator("#faceJobRetryBtn")).to_have_count(0)
    expect(page.locator("#faceJobStopBtn")).to_be_hidden()
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
    expect(page.locator("#faceViewFacesBtn")).to_be_visible()
    expect(page.locator("#faceJobStopBtn")).to_be_visible()
    expect(page.locator("#faceJobDoneBtn")).to_be_visible()
    expect(page.locator("#faceJobStartBtn")).to_be_hidden()
    expect(page.locator("#faceJobCancelBtn")).to_have_count(0)
    expect(page.locator("#faceJobRetryBtn")).to_have_count(0)
    page.locator("#faceViewFacesBtn").click()
    expect(page.locator("#faceGridModal")).to_be_hidden()
    expect(page.locator("#faceScanBusyModal")).to_be_visible()
    expect(page.locator("#faceScanBusyHeading")).to_contain_text("Face analysis")
    expect(page.locator("#faceScanBusyMessage")).to_contain_text("Cannot show Review faces during the scan, please cancel or wait to the end")
    page.locator("#faceScanBusyOkBtn").click()
    expect(page.locator("#faceScanBusyModal")).to_be_hidden()
    expect(page.locator("#faceGridModal")).to_be_hidden()
    page.locator("#faceJobDoneBtn").click()
    expect(page.locator("#faceJobModal")).to_be_hidden()
    page.locator("#faceToolbarBtn").click()
    expect(page.locator("#faceJobModal")).to_be_visible()
    page.locator("#faceJobStopBtn").click()
    expect(page.locator("#faceJobRetryBtn")).to_have_count(0)
    expect(page.locator("#faceJobStopBtn")).to_be_hidden()
    expect(page.locator("#faceViewFacesBtn")).to_be_visible()
    expect(page.locator("#faceJobDoneBtn")).to_be_visible()
    page.locator("#faceViewFacesBtn").click()
    expect(page.locator("#faceGridModal")).to_be_visible()
    expect(page.locator("#faceGridList .face-grid-card")).to_have_count(1)
    assert [body for body, _ in api.posts] == [dict(scope="selected", media_ids=[media_id], force=True)]
    assert all(header == "Bearer face-ui-test-token" for _, header in api.posts)
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
    page.locator("#faceGridShowNamed").check()
    expect(page.locator("#faceGridList .face-grid-card")).to_have_count(2)
    expect(page.locator("#faceGridPagination")).to_be_visible()
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
    page.wait_for_function("""() => {
      const image = document.querySelector('.face-grid-card[data-face-id="82"] .face-grid-crop');
      return image && image.complete && image.naturalWidth > 0;
    }""")
    page.evaluate("""() => {
      window.__retainedFaceCrop = document.querySelector('.face-grid-card[data-face-id="82"] .face-grid-crop');
      window.__retainedFaceCropSrc = window.__retainedFaceCrop.getAttribute('src');
    }""")
    group.locator("[data-face-group-accept]").click()
    page.wait_for_function("() => [...document.querySelectorAll('.face-grid-group')].some(g => g.textContent.includes('Alice') && !g.querySelector('[data-face-group-accept]') && g.querySelectorAll('.face-grid-card').length === 2)")
    assert page.evaluate("window.__retainedFaceCrop === document.querySelector('.face-grid-card[data-face-id=\"82\"] .face-grid-crop')")
    assert page.evaluate("window.__retainedFaceCropSrc === document.querySelector('.face-grid-card[data-face-id=\"82\"] .face-grid-crop').getAttribute('src')")
    assert api.group_accepts[0]["key"] == f"person:{alice['id']}"
    assert api.group_accept_headers == ["Bearer face-ui-test-token"]
    expect(page.locator("#faceGridIncludeDismissed")).to_be_enabled()
    assert {api.faces[face_id]["person_tag_id"] for face_id in (81, 82)} == {alice["id"]}
    page.locator("#faceGridNextBtn").click()
    expect(page.locator('.face-grid-group[data-group-key="unnamed"]')).to_be_visible()
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
    assert len(api.group_accepts) == 1
    assert len(api.batches) == 0
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


def test_accept_refreshes_review_grid_while_metadata_is_pending(server, page: Page):
    api = FaceApi(page)
    page.add_init_script("""(() => {
      const originalFetch = window.fetch.bind(window);
      window.__holdFaceMetadata = false;
      window.__faceMetadataWaiting = false;
      window.__releaseFaceMetadata = null;
      window.fetch = async (input, init) => {
        const url = typeof input === 'string' ? input : input.url;
        if (window.__holdFaceMetadata && new URL(url, location.href).pathname === '/api/tags') {
          window.__faceMetadataWaiting = true;
          await new Promise(resolve => { window.__releaseFaceMetadata = resolve; });
        }
        return originalFetch(input, init);
      };
    })();""")
    page.goto(server["url"])
    media_id = int(page.locator(".photo-card").first.get_attribute("data-id"))
    alice = next(tag for tag in page.evaluate("window._imagineState.tags") if tag["name"] == "Alice")
    api.names[alice["id"]] = "Alice"
    suggestion = [dict(tag_id=alice["id"], name="Alice", score=.9)]
    api.faces = {81: _face(81, media_id, suggestion), 82: _face(82, media_id, suggestion)}
    _open_existing_grid(page)
    page.locator("#faceGridShowNamed").check()
    group = page.locator(".face-grid-group").filter(has=page.locator('[data-face-id="81"]'))
    page.evaluate("window.__holdFaceMetadata = true")
    group.locator("[data-face-group-accept]").click()
    try:
        page.wait_for_function("""() => {
          const group = [...document.querySelectorAll('.face-grid-group')]
            .find(node => node.querySelector('[data-face-id="81"]'));
          return window.__faceMetadataWaiting && group && !group.querySelector('[data-face-group-accept]')
            && group.querySelectorAll('.face-grid-card').length === 2;
        }""", timeout=30000)
        assert len(api.group_accepts) == 1
        assert len(api.batches) == 0
    finally:
        page.evaluate("window.__releaseFaceMetadata?.()")
    expect(page.locator("#faceGridIncludeDismissed")).to_be_enabled(timeout=30000)


def test_reject_refreshes_review_grid_while_metadata_is_pending(server, page: Page):
    api = FaceApi(page)
    page.add_init_script("""(() => {
      const originalFetch = window.fetch.bind(window);
      window.__holdFaceMetadata = false;
      window.__faceMetadataWaiting = false;
      window.__releaseFaceMetadata = null;
      window.fetch = async (input, init) => {
        const url = typeof input === 'string' ? input : input.url;
        if (window.__holdFaceMetadata && new URL(url, location.href).pathname === '/api/tags') {
          window.__faceMetadataWaiting = true;
          await new Promise(resolve => { window.__releaseFaceMetadata = resolve; });
        }
        return originalFetch(input, init);
      };
    })();""")
    page.goto(server["url"])
    media_id = int(page.locator(".photo-card").first.get_attribute("data-id"))
    alice = next(tag for tag in page.evaluate("window._imagineState.tags") if tag["name"] == "Alice")
    suggestion = [dict(tag_id=alice["id"], name="Alice", score=.9)]
    api.faces = {81: _face(81, media_id, suggestion)}
    _open_existing_grid(page)
    card_checkbox = page.locator('.face-grid-card[data-face-id="81"] .face-grid-select')
    card_checkbox.check()
    expect(card_checkbox).to_be_checked()
    reject_btn = page.locator("#faceGridRejectBtn")
    expect(reject_btn).to_be_enabled()
    page.evaluate("window.__holdFaceMetadata = true")
    reject_btn.click()
    try:
        page.wait_for_function("""() => {
          const group = document.querySelector('.face-grid-group[data-group-key="unnamed"]');
          return window.__faceMetadataWaiting && group && group.querySelector('[data-face-id="81"]');
        }""", timeout=30000)
        # One summary refresh replaces the old suggestion group while catalog metadata
        # remains pending; the grid must not wait for that unrelated refresh to begin.
        assert len(api.group_queries) == 2
        assert (len(api.batches) > 0 and api.batches[0][0]["action"] == "reject") or (len(api.face_actions) > 0 and api.face_actions[0][1] == "reject")
        assert api.group_accepts == []
    finally:
        page.evaluate("window.__releaseFaceMetadata?.()")
    expect(page.locator("#faceGridIncludeDismissed")).to_be_enabled(timeout=30000)


@pytest.mark.parametrize("face_count", [1002, 2002])
def test_group_accept_all_spans_pages_and_keeps_failed_faces_selected(server, page: Page, face_count: int):
    api = FaceApi(page)
    page.goto(server["url"])
    media_id = int(page.locator(".photo-card").first.get_attribute("data-id"))
    alice = next(tag for tag in page.evaluate("window._imagineState.tags") if tag["name"] == "Alice")
    api.names[alice["id"]] = "Alice"
    api.faces = {
        i: _face(i, media_id, [dict(tag_id=alice["id"], name="Alice", score=.85)])
        for i in range(100, 100 + face_count)
    }
    api.fail_ids = {100}
    _open_existing_grid(page)
    accept = page.locator("[data-face-group-accept]").first
    expect(accept).to_be_enabled()
    expect(accept).to_have_text("Accept 1 suggestions")
    accept.click()
    expect(page.locator("#faceGridActionError")).to_be_visible()
    expect(page.locator("#faceGridSelectedCount")).to_contain_text("1")
    assert len(api.group_accepts) == 1
    assert api.group_accepts[0]["key"] == f"person:{alice['id']}"
    assert len(api.batches) == 0
    assert sum(1 for face in api.faces.values() if face["person_name"] == "Alice") == face_count - 1
    expect(page.locator('.face-grid-card[data-face-id="100"] .face-grid-select')).to_be_checked(timeout=20000)

    api.faces[100].update(x=29, revision=2, crop_url="/api/faces/100/crop?revision=2")
    page.evaluate("""() => {
      window.__failedFaceCrop = document.querySelector('.face-grid-card[data-face-id="100"] .face-grid-crop');
      window.__failedFaceCropSrc = window.__failedFaceCrop.getAttribute('src');
    }""")
    expect(page.locator("#faceGridIncludeDismissed")).to_be_enabled()
    page.locator("#faceGridIncludeDismissed").check()
    expect(page.locator("#faceGridIncludeDismissed")).to_be_checked()
    page.wait_for_function("""() => {
      const image = document.querySelector('.face-grid-card[data-face-id="100"] .face-grid-crop');
      return image && image.getAttribute('src').includes('revision=2');
    }""", timeout=30000)
    assert page.evaluate("window.__failedFaceCropSrc !== document.querySelector('.face-grid-card[data-face-id=\"100\"] .face-grid-crop').getAttribute('src')")
    page.wait_for_function("""() => {
      const image = document.querySelector('.face-grid-card[data-face-id="100"] .face-grid-crop');
      return image && image.complete && image.naturalWidth > 0;
    }""", timeout=30000)
    expect(page.locator('.face-grid-card[data-face-id="100"] .face-grid-select')).to_be_checked(timeout=20000)


def test_interrupted_scan_without_saved_request_can_retry_catalog(server, page: Page):
    api = FaceApi(page)
    api.initial_job = dict(id=12, state="interrupted", total=5, processed=2, skipped=0, failed=1, remaining=2, error="server restarted")
    page.goto(server["url"])
    expect(page.locator("#faceJobModal")).to_be_visible()
    expect(page.locator("#faceJobRetryBtn")).to_have_count(0)
    expect(page.locator("#faceJobCancelBtn")).to_have_count(0)
    expect(page.locator("#faceViewFacesBtn")).to_be_visible()
    expect(page.locator("#faceJobDoneBtn")).to_be_visible()
    page.locator("#faceJobDoneBtn").click()
    expect(page.locator("#faceJobModal")).to_be_hidden()


def test_named_unnamed_filters_selection_pagination_and_dismissed(server, page: Page):
    api = FaceApi(page)
    page.goto(server["url"])
    media_id = int(page.locator(".photo-card").first.get_attribute("data-id"))
    api.faces = {i: _face(i, media_id) for i in range(1, 204)}
    api.faces[1].update(person_tag_id=99, person_name="Named person")
    api.faces[2].update(person_tag_id=99, person_name="Named person", dismissed=True)
    api.faces[3]["suggestions"] = [dict(tag_id=99, name="Named person", score=.9)]
    _open_existing_grid(page)
    expect(page.locator("#faceGridShowNamed")).not_to_be_checked()
    expect(page.locator("#faceGridShowUnnamed")).to_be_checked()
    expect(page.locator('.face-grid-card[data-face-id="1"]')).to_have_count(0)
    expect(page.locator('.face-grid-card[data-face-id="3"]')).to_be_visible()
    page.locator("#faceGridSelectAllBtn").click()
    expect(page.locator("#faceGridSelectedCount")).to_contain_text("1")
    page.locator("#faceGridShowNamed").check()
    expect(page.locator('.face-grid-card[data-face-id="1"]')).to_be_visible()
    page.locator("#faceGridSelectAllBtn").click()
    expect(page.locator("#faceGridSelectedCount")).to_contain_text("2")
    page.locator("#faceGridShowNamed").uncheck()
    expect(page.locator("#faceGridSelectedCount")).to_contain_text("1")
    expect(page.locator('.face-grid-card[data-face-id="1"]')).to_have_count(0)
    expect(page.locator('.face-grid-card[data-face-id="3"]')).to_be_visible()
    page.locator("#faceGridNextBtn").click()
    assert page.locator("#faceGridList .face-grid-card").count() <= 96
    page.locator("#faceGridShowUnnamed").uncheck()
    expect(page.locator("#faceGridEmpty")).to_be_visible()
    expect(page.locator("#faceGridSelectedCount")).to_contain_text("0")
    expect(page.locator("#faceGridSelectAllBtn")).to_be_disabled()
    page.locator("#faceGridShowNamed").check()
    expect(page.locator("#faceGridList .face-grid-card")).to_have_count(1)
    page.locator("#faceGridIncludeDismissed").check()
    expect(page.locator("#faceGridList .face-grid-card")).to_have_count(1)
    page.locator("#faceGridSelectAllBtn").click()
    expect(page.locator("#faceGridSelectedCount")).to_contain_text("1")
    page.locator("#faceGridDismissBtn").click()
    expect(page.locator("#faceGridSelectedCount")).to_contain_text("0")
    assert [f["id"] for f in api.batches[0][0]["faces"]] == [1]


def test_confirmed_people_filter_is_populated_on_open_and_matches_confirmed_identity_only(server, page: Page):
    api = FaceApi(page)
    page.goto(server["url"])
    media_id = int(page.locator(".photo-card").first.get_attribute("data-id"))
    alice = next(tag for tag in page.evaluate("window._imagineState.tags") if tag["name"] == "Alice")
    api.names[alice["id"]] = "Alice"
    api.faces = {
        1: _face(1, media_id),
        2: _face(2, media_id, [dict(tag_id=alice["id"], name="Alice", score=.9)]),
        3: _face(3, media_id),
    }
    api.faces[1].update(person_tag_id=alice["id"], person_name="Alice")
    _open_existing_grid(page)
    page.locator("#faceGridShowNamed").check()

    person_filter = page.locator("#faceGridConfirmedPersonFilter")
    expect(person_filter.locator(f'option[value="{alice["id"]}"]')).to_have_count(1)
    expect(person_filter).to_be_enabled()
    person_filter.select_option(str(alice["id"]))
    expect(page.locator('.face-grid-card[data-face-id="1"]')).to_be_visible()
    expect(page.locator('.face-grid-card[data-face-id="2"]')).to_have_count(0)
    expect(page.locator('.face-grid-card[data-face-id="3"]')).to_have_count(0)
    assert api.group_queries[-1].get("person_tag_id") == [str(alice["id"])]


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
    assert group.locator(".face-grid-card").count() <= 96
    expect(group.locator("[data-face-group-accept]")).to_have_text("Accept 2 suggestions")
    expect(page.locator('.face-grid-card[data-face-id="204"]')).to_have_count(0)
    page.locator("#faceGridNextBtn").click()
    expect(page.locator('.face-grid-group[data-group-key="person:99"] .face-grid-card')).to_have_count(1)
    page.locator("#faceGridPrevBtn").click()
    group.locator("[data-face-group-accept]").click()
    expect(group.locator("[data-face-group-accept]")).to_have_count(0)
    assert group.locator(".face-grid-card").count() <= 96
    assert api.group_accepts[0]["key"] == "person:98"
    assert sum(face["person_tag_id"] == 98 for face in api.faces.values()) == 203
    assert api.faces[1]["revision"] == 1


def test_dismissed_group_restore_is_labeled_and_limited_to_visible_faces(server, page: Page):
    api = FaceApi(page)
    page.goto(server["url"])
    media_id = int(page.locator(".photo-card").first.get_attribute("data-id"))
    api.faces = {1: _face(1, media_id), 2: _face(2, media_id)}
    api.faces[1].update(person_tag_id=98, person_name="Alice", dismissed=True)
    api.faces[2].update(dismissed=True)
    _open_existing_grid(page)
    page.locator("#faceGridShowNamed").check()
    expect(page.locator("#faceGridEmpty")).to_be_visible()

    page.locator("#faceGridIncludeDismissed").check()
    group = page.locator('.face-grid-group[data-group-key="dismissed"]')
    restore = group.locator("[data-face-group-restore]")
    expect(restore).to_have_text("Restore 2 visible faces")
    restore.click()
    expect(page.locator('.face-grid-group[data-group-key="dismissed"]')).to_have_count(0)
    assert api.batches[-1][0]["action"] == "dismiss"
    assert api.batches[-1][0]["dismissed"] is False
    assert {face["id"] for face in api.batches[-1][0]["faces"]} == {1, 2}


def test_lazy_face_grid_virtualizes_random_access_scroll_and_whole_group_accept(server, page: Page):
    api = FaceApi(page)
    page.goto(server["url"])
    media_id = int(page.locator(".photo-card").first.get_attribute("data-id"))
    alice = next(tag for tag in page.evaluate("window._imagineState.tags") if tag["name"] == "Alice")
    api.names[alice["id"]] = "Alice"
    api.faces = {
        i: _face(i, media_id, [dict(tag_id=alice["id"], name="Alice", score=.85)])
        for i in range(100, 25_100)
    }
    failed_id = 25_099
    api.fail_ids = {failed_id}

    _open_existing_grid(page)
    first = page.locator('.face-grid-card[data-face-id="100"]')
    expect(first).to_be_visible()
    assert page.locator(".face-grid-card").count() <= 96
    assert len(api.group_reads) <= 4
    assert {read["offset"] for read in api.group_reads} <= {0, 48, 96, 144}

    page.set_viewport_size(dict(width=1100, height=850))
    expect(first).to_be_visible()
    assert page.locator(".face-grid-card").count() <= 96
    page.evaluate("""() => new Promise(resolve => {
      const list = document.getElementById('faceGridList');
      let previous = '';
      let stableFrames = 0;
      const sample = () => {
        const cards = list.querySelector('.face-grid-group-cards');
        const card = list.querySelector('.face-grid-card:not(.face-grid-card-placeholder)');
        const rect = card?.getBoundingClientRect();
        const signature = [list.clientWidth, list.clientHeight,
          getComputedStyle(cards).gridTemplateColumns,
          rect ? `${rect.width}:${rect.height}` : 'no-card'].join('|');
        if (signature === previous) stableFrames += 1;
        else { previous = signature; stableFrames = 0; }
        if (stableFrames >= 2) resolve();
        else requestAnimationFrame(sample);
      };
      requestAnimationFrame(sample);
    })""")

    grid = page.locator("#faceGridList")
    grid.evaluate("element => { element.scrollTop = element.scrollHeight; element.dispatchEvent(new Event('scroll')); }")
    last_id = 25_099
    last_card = page.locator(f'.face-grid-card[data-face-id="{last_id}"]')
    expect(last_card).to_be_visible(timeout=20000)
    expect(page.locator(".face-grid-group-header [data-face-group-accept]")).to_be_visible()
    assert last_card.evaluate("""card => {
      const viewport = document.getElementById('faceGridList').getBoundingClientRect();
      const rect = card.getBoundingClientRect();
      return rect.bottom > viewport.top && rect.top < viewport.bottom
        && rect.right > viewport.left && rect.left < viewport.right;
    }""")
    assert page.locator(".face-grid-group-header [data-face-group-accept]").evaluate("""button => {
      const viewport = document.getElementById('faceGridList').getBoundingClientRect();
      const rect = button.getBoundingClientRect();
      return rect.bottom > viewport.top && rect.top < viewport.bottom;
    }""")
    assert page.locator(".face-grid-card").count() <= 96
    high_offsets = [read["offset"] for read in api.group_reads if read["offset"] >= 24_000]
    assert high_offsets and min(high_offsets) >= 24_000
    assert len(api.group_reads) <= 10

    grid.evaluate("element => { element.scrollTop = 0; element.dispatchEvent(new Event('scroll')); }")
    expect(page.locator('.face-grid-card[data-face-id="100"]')).to_be_visible(timeout=20000)
    assert page.locator('.face-grid-card[data-face-id="100"]').evaluate("""card => {
      const viewport = document.getElementById('faceGridList').getBoundingClientRect();
      const rect = card.getBoundingClientRect();
      return rect.bottom > viewport.top && rect.top < viewport.bottom
        && rect.right > viewport.left && rect.left < viewport.right;
    }""")
    assert len(api.group_reads) <= 12
    accept = page.locator("[data-face-group-accept]")
    accept.click()
    expect(page.locator("#faceGridSelectedCount")).to_contain_text("1", timeout=20000)
    expect(page.locator("[data-face-group-accept]")).to_be_visible()
    assert len(api.group_accepts) == 1
    assert api.group_accepts[0]["key"] == f"person:{alice['id']}"
    assert api.group_accepts[0]["snapshot"]
    assert api.face_lookup_reads == [[failed_id]]
    assert sum(face["person_tag_id"] == alice["id"] for face in api.faces.values()) == 24_999


def test_virtual_face_grid_keeps_bottom_position_while_far_page_is_pending(server, page: Page):
    api = FaceApi(page)
    page.add_init_script("""(() => {
      const originalFetch = window.fetch.bind(window);
      window.__holdFarFacePages = false;
      window.__farFacePageOffsets = [];
      window.__farFacePageReleases = [];
      window.__releaseFarFacePages = () => {
        window.__holdFarFacePages = false;
        for (const release of window.__farFacePageReleases.splice(0)) release();
      };
      window.fetch = async (input, init) => {
        const url = typeof input === 'string' ? input : input.url;
        const parsed = new URL(url, location.href);
        const offset = Number(parsed.searchParams.get('offset') || 0);
        if (window.__holdFarFacePages && parsed.pathname === '/api/faces/group' && offset >= 4800) {
          window.__farFacePageOffsets.push(offset);
          await new Promise(resolve => window.__farFacePageReleases.push(resolve));
        }
        return originalFetch(input, init);
      };
    })();""")
    page.goto(server["url"])
    media_id = int(page.locator(".photo-card").first.get_attribute("data-id"))
    alice = next(tag for tag in page.evaluate("window._imagineState.tags") if tag["name"] == "Alice")
    api.names[alice["id"]] = "Alice"
    api.faces = {
        face_id: _face(face_id, media_id, [dict(tag_id=alice["id"], name="Alice", score=.85)])
        for face_id in range(100, 5100)
    }
    _open_existing_grid(page)
    expect(page.locator('.face-grid-card[data-face-id="100"]')).to_be_visible()
    page.evaluate("window.__holdFarFacePages = true")
    grid = page.locator("#faceGridList")
    grid.evaluate("element => { element.scrollTop = element.scrollHeight; element.dispatchEvent(new Event('scroll')); }")
    page.wait_for_function("window.__farFacePageOffsets.length > 0", timeout=15000)
    pending = page.evaluate("""() => {
      const list = document.getElementById('faceGridList');
      return {
        scrollTop: list.scrollTop,
        maxScrollTop: list.scrollHeight - list.clientHeight,
        placeholderHeights: [...list.querySelectorAll('.face-grid-card-placeholder')]
          .map(card => card.getBoundingClientRect().height)
      };
    }""")
    assert pending["maxScrollTop"] > 100_000
    assert pending["scrollTop"] >= pending["maxScrollTop"] - 300
    assert pending["placeholderHeights"] and min(pending["placeholderHeights"]) >= 100

    page.evaluate("window.__releaseFarFacePages()")
    last_card = page.locator('.face-grid-card[data-face-id="5099"]')
    expect(last_card).to_be_visible(timeout=20000)
    assert last_card.evaluate("""card => {
      const viewport = document.getElementById('faceGridList').getBoundingClientRect();
      const rect = card.getBoundingClientRect();
      return rect.bottom > viewport.top && rect.top < viewport.bottom;
    }""")
    assert any(read["offset"] >= 4800 for read in api.group_reads)


def test_lazy_face_grid_initial_page_failure_stays_visible_until_explicit_retry(server, page: Page):
    api = FaceApi(page)
    page.goto(server["url"])
    media_id = int(page.locator(".photo-card").first.get_attribute("data-id"))
    api.faces = {1: _face(1, media_id), 2: _face(2, media_id)}
    api.group_page_failures[0] = 1

    _open_existing_grid(page)
    expect(page.locator("#faceGridRetryBtn")).to_be_visible()
    expect(page.locator("#faceGridState")).to_contain_text("Could not load")
    assert [read["offset"] for read in api.group_reads] == [0]

    page.locator("#faceGridRetryBtn").click()
    expect(page.locator('.face-grid-card[data-face-id="1"]')).to_be_visible()
    assert [read["offset"] for read in api.group_reads] == [0, 0]


def test_lazy_face_grid_rejects_short_nonempty_page_and_retries(server, page: Page):
    api = FaceApi(page)
    page.goto(server["url"])
    media_id = int(page.locator(".photo-card").first.get_attribute("data-id"))
    api.faces = {1: _face(1, media_id), 2: _face(2, media_id)}
    api.short_group_pages.add(0)

    _open_existing_grid(page)
    expect(page.locator("#faceGridRetryBtn")).to_be_visible()
    expect(page.locator("#faceGridState")).to_contain_text("Could not load")
    assert [read["offset"] for read in api.group_reads] == [0]

    api.short_group_pages.clear()
    page.locator("#faceGridRetryBtn").click()
    expect(page.locator('.face-grid-card[data-face-id="1"]')).to_be_visible()
    expect(page.locator('.face-grid-card[data-face-id="2"]')).to_be_visible()
    assert [read["offset"] for read in api.group_reads] == [0, 0]


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
    expect(page.locator("#faceGridIncludeDismissed")).to_be_enabled()
    person_filter = page.locator("#faceGridConfirmedPersonFilter")
    expect(person_filter.locator(f"option[value='{alice_id}']")).to_be_attached()
    page.locator("#faceGridShowNamed").check()
    face_card = page.locator(f'.face-grid-card[data-face-id="{face_id}"]')
    expect(face_card).to_be_visible(timeout=30000)
    expect(face_card.locator(".face-grid-select")).to_be_enabled(timeout=30000)
    face_card.locator(".face-grid-select").check()
    expect(page.locator("#faceGridClearBtn")).to_be_enabled(timeout=30000)
    page.locator("#faceGridClearBtn").click()
    birthday = page.locator(".photo-card", has_text="birthday.bmp")
    expect(birthday).to_be_visible()
    expect(page.locator(".photo-card")).to_have_count(1)
    assert page.evaluate("window._imagineState.totalCount") == 1
    assert alice_id in page.evaluate("Array.from(window._imagineState.activeTagIds)")
    cleared_queries = [query for query in media_queries if query.get("tag_id") == [str(alice_id)]]
    assert cleared_queries, media_queries
    assert any(query.get("limit") == ["1"] and query.get("offset") == ["0"] for query in cleared_queries)


def test_dismiss_selection_updates_scroll_position_when_photos_disappear(server, page: Page):
    api = FaceApi(page)
    page.goto(server["url"])
    media_id = int(page.locator(".photo-card").first.get_attribute("data-id"))
    api.faces = {i: _face(i, media_id) for i in range(1, 101)}
    _open_existing_grid(page)
    page.set_viewport_size(dict(width=1100, height=850))

    grid = page.locator("#faceGridList")
    page.wait_for_timeout(300)
    # Scroll down into the group
    grid.evaluate("element => { element.scrollTop = 1500; element.dispatchEvent(new Event('scroll')); }")
    page.wait_for_timeout(500)

    # Select 8 rendered cards around the scroll position
    selected_ids = page.evaluate("""() => {
        const cards = [...document.querySelectorAll('.face-grid-card[data-face-id]')].slice(0, 8);
        for (const card of cards) {
            const cb = card.querySelector('.face-grid-select');
            if (cb) {
                cb.checked = true;
                cb.dispatchEvent(new Event('change', { bubbles: true }));
            }
        }
        return cards.map(c => c.dataset.faceId);
    }""")
    assert len(selected_ids) == 8
    expect(page.locator("#faceGridSelectedCount")).to_contain_text("8")

    before = page.evaluate("""() => {
        const list = document.getElementById('faceGridList');
        return {
            scrollTop: list.scrollTop,
            scrollHeight: list.scrollHeight,
            clientHeight: list.clientHeight
        };
    }""")
    assert before["scrollTop"] == 1500

    page.locator("#faceGridDismissBtn").click()
    expect(page.locator("#faceGridSelectedCount")).to_contain_text("0")
    expect(page.locator("#faceGridIncludeDismissed")).to_be_enabled(timeout=30000)
    expect(page.locator("#faceGridList .face-grid-card:not(.face-grid-card-placeholder)").first).to_be_visible(timeout=30000)

    after = page.evaluate("""() => {
        const list = document.getElementById('faceGridList');
        return {
            scrollTop: list.scrollTop,
            scrollHeight: list.scrollHeight,
            clientHeight: list.clientHeight,
            visibleIds: [...list.querySelectorAll('.face-grid-card[data-face-id]')].map(c => c.dataset.faceId)
        };
    }""")

    # Verify dismissed photos disappeared
    for sid in selected_ids:
        assert sid not in after["visibleIds"]

    # Verify scroll height decreased and scroll position adjusted to reflect the removed photos
    assert after["scrollHeight"] < before["scrollHeight"]
    assert after["scrollTop"] < before["scrollTop"]
    # Verify the remaining items are rendered in view
    assert len(after["visibleIds"]) > 0

def test_shift_select_range_after_scrolling_grid(server, page: Page):
    api = FaceApi(page)
    page.goto(server["url"])
    media_id = int(page.locator(".photo-card").first.get_attribute("data-id"))
    api.faces = {i: _face(i, media_id) for i in range(1, 101)}
    _open_existing_grid(page)
    page.set_viewport_size(dict(width=1100, height=850))

    # Click first card (face 1)
    card1 = page.locator('.face-grid-card[data-face-id="1"]')
    expect(card1).to_be_visible()
    card1.click()
    expect(page.locator("#faceGridSelectedCount")).to_contain_text("1")

    # Scroll down so card 1 is no longer rendered
    grid = page.locator("#faceGridList")
    grid.evaluate("element => { element.scrollTop = 1500; element.dispatchEvent(new Event('scroll')); }")
    page.wait_for_timeout(300)

    # Verify card 1 is scrolled out of DOM
    expect(page.locator('.face-grid-card[data-face-id="1"]')).to_have_count(0)

    # Find a visible card after scroll, e.g. face 25
    card25 = page.locator('.face-grid-card[data-face-id="25"]')
    expect(card25).to_be_visible()
    card25.click(modifiers=["Shift"])

    # Expect cards 1 through 25 to be selected (25 cards selected)
    expect(page.locator("#faceGridSelectedCount")).to_contain_text("25")

    # Scroll back up to the top
    grid.evaluate("element => { element.scrollTop = 0; element.dispatchEvent(new Event('scroll')); }")
    page.wait_for_timeout(300)

    # Verify card 1 is visible and still marked selected and checked
    expect(card1).to_be_visible()
    assert card1.locator(".face-grid-select").is_checked()
    assert "selected" in (card1.get_attribute("class") or "")

    # Scroll back down and extend shift selection to card 30
    grid.evaluate("element => { element.scrollTop = 1800; element.dispatchEvent(new Event('scroll')); }")
    page.wait_for_timeout(300)
    card30 = page.locator('.face-grid-card[data-face-id="30"]')
    expect(card30).to_be_visible()
    card30.click(modifiers=["Shift"])
    expect(page.locator("#faceGridSelectedCount")).to_contain_text("30")


def test_review_faces_dialog_starts_with_only_show_unnamed_selected(server, page: Page):
    api = FaceApi(page)
    page.goto(server["url"])
    media_id = int(page.locator(".photo-card").first.get_attribute("data-id"))
    api.faces = {
        1: _face(1, media_id),
        2: _face(2, media_id),
    }
    api.faces[1].update(person_tag_id=99, person_name="Named person")

    # Open from People tab 'Review faces' button
    page.locator('.tab-btn[data-tab="people"]').click()
    expect(page.locator("#facePeopleActions")).to_be_visible()
    review_btn = page.locator("#faceReviewBtn")
    expect(review_btn).to_be_visible()
    review_btn.click()

    # Dialog starts with only show unnamed selected
    expect(page.locator("#faceGridModal")).to_be_visible()
    expect(page.locator("#faceGridShowNamed")).not_to_be_checked()
    expect(page.locator("#faceGridShowUnnamed")).to_be_checked()
    expect(page.locator("#faceGridIncludeDismissed")).not_to_be_checked()
    expect(page.locator('.face-grid-card[data-face-id="1"]')).to_have_count(0)
    expect(page.locator('.face-grid-card[data-face-id="2"]')).to_be_visible()

    # User toggles Show Named, then closes dialog
    page.locator("#faceGridShowNamed").check()
    expect(page.locator("#faceGridShowNamed")).to_be_checked()
    page.locator("#faceGridCloseBtn").click()
    expect(page.locator("#faceGridModal")).to_be_hidden()

    # Reopening 'Review faces' dialog resets to only show unnamed selected
    review_btn.click()
    expect(page.locator("#faceGridModal")).to_be_visible()
    expect(page.locator("#faceGridShowNamed")).not_to_be_checked()
    expect(page.locator("#faceGridShowUnnamed")).to_be_checked()
    expect(page.locator("#faceGridIncludeDismissed")).not_to_be_checked()
    expect(page.locator('.face-grid-card[data-face-id="1"]')).to_have_count(0)
    expect(page.locator('.face-grid-card[data-face-id="2"]')).to_be_visible()


def test_people_tab_review_faces_shows_busy_modal_while_scan_running(server, page: Page):
    api = FaceApi(page)
    job = dict(id=1, state="running", total=10, processed=2, skipped=0, failed=0, remaining=8, error="")
    api.initial_job = job
    api.jobs[1] = job
    page.goto(server["url"])
    expect(page.locator("#faceJobModal")).to_be_visible()
    page.locator("#faceJobDoneBtn").click()
    expect(page.locator("#faceJobModal")).to_be_hidden()

    page.locator('.tab-btn[data-tab="people"]').click()
    expect(page.locator("#facePeopleActions")).to_be_visible()
    review_btn = page.locator("#faceReviewBtn")
    expect(review_btn).to_be_visible()
    review_btn.click()

    expect(page.locator("#faceGridModal")).to_be_hidden()
    expect(page.locator("#faceScanBusyModal")).to_be_visible()
    expect(page.locator("#faceScanBusyHeading")).to_contain_text("Face analysis")
    expect(page.locator("#faceScanBusyMessage")).to_contain_text(
        "Cannot show Review faces during the scan, please cancel or wait to the end."
    )
    page.locator("#faceScanBusyOkBtn").click()
    expect(page.locator("#faceScanBusyModal")).to_be_hidden()
    expect(page.locator("#faceGridModal")).to_be_hidden()


def test_show_checkboxes_independent_named_unnamed_or_dismissed(server, page: Page):
    api = FaceApi(page)
    page.goto(server["url"])
    media_id = int(page.locator(".photo-card").first.get_attribute("data-id"))
    api.faces = {
        1: _face(1, media_id),
        2: _face(2, media_id),
        3: _face(3, media_id),
        4: _face(4, media_id),
    }
    api.faces[1].update(person_tag_id=98, person_name="Alice")
    api.faces[3].update(dismissed=True)
    api.faces[4].update(person_tag_id=99, person_name="Bob", dismissed=True)

    _open_existing_grid(page)
    # Default: only Unnamed is checked
    expect(page.locator("#faceGridShowNamed")).not_to_be_checked()
    expect(page.locator("#faceGridShowUnnamed")).to_be_checked()
    expect(page.locator("#faceGridIncludeDismissed")).not_to_be_checked()
    expect(page.locator('.face-grid-card[data-face-id="2"]')).to_be_visible()
    expect(page.locator('.face-grid-card[data-face-id="1"]')).to_have_count(0)
    expect(page.locator('.face-grid-card[data-face-id="3"]')).to_have_count(0)
    expect(page.locator('.face-grid-card[data-face-id="4"]')).to_have_count(0)

    # 1. Only Dismissed checked (uncheck Unnamed, check Dismissed)
    page.locator("#faceGridShowUnnamed").uncheck()
    page.locator("#faceGridIncludeDismissed").check()
    expect(page.locator('.face-grid-group[data-group-key="dismissed"]')).to_be_visible()
    expect(page.locator('.face-grid-card[data-face-id="3"]')).to_be_visible()
    expect(page.locator('.face-grid-card[data-face-id="4"]')).to_be_visible()
    expect(page.locator('.face-grid-card[data-face-id="1"]')).to_have_count(0)
    expect(page.locator('.face-grid-card[data-face-id="2"]')).to_have_count(0)

    # 2. Only Named checked (uncheck Dismissed, check Named)
    page.locator("#faceGridIncludeDismissed").uncheck()
    page.locator("#faceGridShowNamed").check()
    expect(page.locator('.face-grid-group[data-group-key="person:98"]')).to_be_visible()
    expect(page.locator('.face-grid-card[data-face-id="1"]')).to_be_visible()
    expect(page.locator('.face-grid-card[data-face-id="2"]')).to_have_count(0)
    expect(page.locator('.face-grid-card[data-face-id="3"]')).to_have_count(0)
    expect(page.locator('.face-grid-card[data-face-id="4"]')).to_have_count(0)

    # 3. None checked (uncheck Named)
    page.locator("#faceGridShowNamed").uncheck()
    expect(page.locator("#faceGridEmpty")).to_be_visible()
    expect(page.locator('.face-grid-card')).to_have_count(0)

    # 4. Named AND Dismissed checked (both show, unnamed does not)
    page.locator("#faceGridShowNamed").check()
    page.locator("#faceGridIncludeDismissed").check()
    expect(page.locator('.face-grid-card[data-face-id="1"]')).to_be_visible()
    expect(page.locator('.face-grid-card[data-face-id="2"]')).to_have_count(0)
    page.locator("#faceGridNextBtn").click()
    expect(page.locator('.face-grid-card[data-face-id="3"]')).to_be_visible()
    expect(page.locator('.face-grid-card[data-face-id="4"]')).to_be_visible()
    expect(page.locator('.face-grid-card[data-face-id="2"]')).to_have_count(0)


