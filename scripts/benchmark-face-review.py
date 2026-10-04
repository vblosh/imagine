#!/usr/bin/env python3
"""Benchmark face-review matching and batch acceptance on synthetic data.

Uses no user photos or models. Run from the repository root with the already
built preview executable, for example:
  python scripts/benchmark-face-review.py --binary build/face-grid-preview/imagine.exe
Each run creates a timestamped catalog and JSON report under build/face-deps
unless --output is supplied. All vectors and catalog rows are synthetic.
"""

import argparse
import hashlib
import json
import os
import socket
import sqlite3
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from statistics import median

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BINARY = ROOT / "build" / "face-grid-preview" / "imagine.exe"
OUTPUT_ROOT = ROOT / "build" / "face-deps"
DEFAULT_CANDIDATES = 200
DEFAULT_EXAMPLES = 1000
DIMENSIONS = 512
RECOGNIZER_CHECKSUM = hashlib.sha256(b"imagine-face-grid-benchmark-recognizer-v1").hexdigest()
DETECTOR_CHECKSUM = hashlib.sha256(b"imagine-face-grid-benchmark-detector-v1").hexdigest()
PIPELINE_VERSION = "synthetic-scrfd10g-arcface-w600k-r50-bench-v1"


def init_catalog(path: Path) -> None:
    test_fixtures = ROOT / "tests" / "ui"
    sys.path.insert(0, str(test_fixtures))
    from fixtures import init_schema

    conn = sqlite3.connect(path)
    try:
        init_schema(conn)
        conn.commit()
    finally:
        conn.close()


def available_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_for_server(url: str, process: subprocess.Popen, timeout: float = 30.0) -> None:
    deadline = time.monotonic() + timeout
    last_error = "server did not answer"
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"benchmark server exited early with code {process.returncode}")
        try:
            with urllib.request.urlopen(url + "/api/stats", timeout=1.0) as response:
                if response.status == 200:
                    return
        except Exception as exc:
            last_error = str(exc)
        time.sleep(0.1)
    raise TimeoutError(f"server readiness timeout: {last_error}")


def request_json(url: str, method: str = "GET", body=None):
    data = None if body is None else json.dumps(body, separators=(",", ":")).encode("utf-8")
    headers = {} if data is None else {"Content-Type": "application/json"}
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    started = time.perf_counter()
    try:
        with urllib.request.urlopen(req, timeout=300) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")
        raise RuntimeError(f"{method} {url} returned HTTP {exc.code}: {detail}") from exc
    return (time.perf_counter() - started) * 1000.0, payload


def seed_catalog(path: Path, n_candidates: int, n_exemplars: int):
    conn = sqlite3.connect(path)
    now = int(time.time())
    try:
        cur = conn.execute("INSERT INTO tags(name,category) VALUES(?, 'people')", ("Synthetic Benchmark Person",))
        tag_id = int(cur.lastrowid)
        conn.execute("PRAGMA foreign_keys=ON")

        vector = [1.0 / (DIMENSIONS ** 0.5)] * DIMENSIONS
        embedding = struct.pack("<512f", *vector)
        assert abs(sum(value * value for value in struct.unpack("<512f", embedding)) - 1.0) < 1e-6

        analysis_rows = []
        for index in range(n_candidates + n_exemplars):
            role = "candidate" if index < n_candidates else "exemplar"
            relative = f"synthetic/{role}-{index:04d}.jpg"
            media_path = str((path.parent / relative).resolve())
            content_hash = hashlib.sha256(relative.encode("utf-8")).hexdigest()
            media_cur = conn.execute("""INSERT INTO media_items
                (file_path,file_name,file_size,file_modified_time,content_hash,width,height,date_taken,created_at,updated_at)
                VALUES(?,?,1,?,?,?,?,?,?,?)""",
                                     (media_path, Path(relative).name, now, content_hash, 640, 480,
                                      now - index, now, now))
            media_id = int(media_cur.lastrowid)
            analysis_rows.append((media_id, "complete", "", content_hash, 640, 480,
                                  DETECTOR_CHECKSUM, RECOGNIZER_CHECKSUM, PIPELINE_VERSION, now))

        conn.executemany("""INSERT INTO face_media_analysis
            (media_id,state,error,source_hash,width,height,detector_checksum,recognizer_checksum,pipeline_version,analyzed_at)
            VALUES(?,?,?,?,?,?,?,?,?,?)""", analysis_rows)

        # Insert candidates first so the 200-item first page is entirely unnamed.
        for index in range(n_candidates + n_exemplars):
            media_id = index + 1
            tag_value = None if index < n_candidates else tag_id
            conn.execute("""INSERT INTO faces
                (media_id,x,y,width,height,score,landmarks,embedding,embedding_size,embedding_error,
                 person_tag_id,dismissed,revision,created_at,updated_at)
                VALUES(?,100,80,240,260,0.99,'[]',?,512,'',?,0,1,?,?)""",
                         (media_id, sqlite3.Binary(embedding), tag_value, now, now))

        conn.commit()
        return tag_id
    finally:
        conn.close()


def summarize(samples):
    return {
        "runs_ms": [round(value, 3) for value in samples],
        "median_ms": round(median(samples), 3),
        "min_ms": round(min(samples), 3),
        "max_ms": round(max(samples), 3),
    }


def measure_full_grid_once(url: str, tag_id: int, n_candidates: int, n_exemplars: int):
    expected_total = n_candidates + n_exemplars
    elapsed_total = 0.0
    ids = []
    for offset in range(0, expected_total, 1000):
        limit = min(1000, expected_total - offset)
        elapsed, payload = request_json(f"{url}/api/faces/grid?offset={offset}&limit={limit}")
        elapsed_total += elapsed
        items = payload.get("items", [])
        if payload.get("total") != expected_total or len(items) != limit:
            raise AssertionError(f"full grid page mismatch at offset {offset}")
        for item in items:
            face_id = item.get("id")
            ids.append(face_id)
            suggestions = item.get("suggestions", [])
            if face_id <= n_candidates:
                if not suggestions or suggestions[0].get("tag_id") != tag_id or float(suggestions[0].get("score", 0)) < 0.999:
                    raise AssertionError(f"candidate suggestion mismatch for face {face_id}")
            elif suggestions and suggestions[0].get("tag_id") != tag_id:
                raise AssertionError(f"confirmed-face suggestion mismatch for face {face_id}")
    if ids != list(range(1, expected_total + 1)):
        raise AssertionError("full grid IDs do not cover all seeded faces in order")
    return elapsed_total


def validate_grid(payload, start_id: int, expected_count: int, total_count: int, tag_id: int):
    items = payload.get("items")
    if payload.get("total") != total_count:
        raise AssertionError(f"grid total mismatch: {payload.get('total')}")
    if not isinstance(items, list) or len(items) != expected_count:
        raise AssertionError(f"grid item count mismatch: {len(items) if isinstance(items, list) else type(items)}")
    if [item.get("id") for item in items] != list(range(start_id, start_id + expected_count)):
        raise AssertionError("grid page ordering/IDs differ from seeded candidates and exemplars")
    for item in items:
        if item.get("embedding_error") or not item.get("suggestions"):
            raise AssertionError(f"face {item.get('id')} did not produce a suggestion")
        top = item["suggestions"][0]
        if top.get("tag_id") != tag_id or float(top.get("score", 0)) < 0.999:
            raise AssertionError(f"face {item.get('id')} suggestion mismatch: {top}")


def measure_grid(url: str, tag_id: int, n_candidates: int, n_exemplars: int, runs: int,
                 skip_full_grid: bool = False):
    total_count = n_candidates + n_exemplars
    candidate_page_size = min(200, n_candidates)
    page_url = f"{url}/api/faces/grid?offset=0&limit={candidate_page_size}"
    _, warm_payload = request_json(page_url)
    validate_grid(warm_payload, 1, candidate_page_size, total_count, tag_id)
    samples = []
    for _ in range(runs):
        elapsed, payload = request_json(page_url)
        validate_grid(payload, 1, candidate_page_size, total_count, tag_id)
        samples.append(elapsed)

    # Sum all API pages (up to 1000 items each) as one complete grid load.
    full_expected = total_count
    if skip_full_grid:
        full_result = {"skipped": True, "reason": "--skip-full-grid"}
    else:
        full_samples = [
            measure_full_grid_once(url, tag_id, n_candidates, n_exemplars)
            for _ in range(runs)
        ]
        full_result = summarize(full_samples)
    return summarize(samples), full_result


def measure_accept(url: str, tag_id: int, n_candidates: int, accept_count: int):
    body = {
        "action": "accept",
        "tag_id": tag_id,
        "faces": [{"id": face_id, "revision": 1} for face_id in range(1, accept_count + 1)],
    }
    elapsed, payload = request_json(url + "/api/faces/batch", method="POST", body=body)
    expected = len(body["faces"])
    if len(payload.get("updated", [])) != expected or payload.get("failed"):
        raise AssertionError(f"batch accept result mismatch: updated={len(payload.get('updated', []))}, failed={payload.get('failed')}")
    return elapsed, {"requested": expected, "updated": len(payload["updated"]), "failed": len(payload.get("failed", []))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--output", type=Path, help="report JSON path; default writes under ignored build/face-deps")
    parser.add_argument("--runs", type=int, default=3, help="warm samples per grid workload (default: 3)")
    parser.add_argument("--skip-full-grid", action="store_true", help="skip paged full-catalog timing")
    parser.add_argument("--full-grid-only", action="store_true", help="time and validate one full paged grid only; skip candidate page and acceptance")
    parser.add_argument("--candidates", type=int, default=DEFAULT_CANDIDATES)
    parser.add_argument("--examples", type=int, default=DEFAULT_EXAMPLES)
    parser.add_argument("--accept", type=int, default=100, help="candidate faces accepted in the timed batch (default: 100)")
    args = parser.parse_args()
    if args.full_grid_only:
        if args.runs != 1 or args.skip_full_grid:
            parser.error("--full-grid-only requires --runs 1 and cannot be combined with --skip-full-grid")
    elif args.runs < 3 or args.runs > 5:
        parser.error("--runs must be between 3 and 5 unless --full-grid-only is used")
    if args.candidates < 1 or args.examples < 1 or not 1 <= args.accept <= min(args.candidates, 1000):
        parser.error("candidates/examples must be positive; accept must be between 1 and min(candidates, 1000)")
    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error(f"preview executable not found: {binary}")

    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    report_path = args.output.resolve() if args.output else OUTPUT_ROOT / f"face-review-benchmark-{run_id}.json"
    report_path.parent.mkdir(parents=True, exist_ok=True)
    if report_path.exists():
        parser.error(f"report already exists; choose another --output: {report_path}")
    work = report_path.with_suffix("") if args.output else OUTPUT_ROOT / f"face-review-benchmark-{run_id}-data"
    work.mkdir(parents=True, exist_ok=False)
    catalog = work / "catalog.db"
    thumbs = work / "thumbs"
    thumbs.mkdir()
    stdout_path, stderr_path = work / "server.stdout.log", work / "server.stderr.log"
    init_catalog(catalog)
    port = available_port()
    env = os.environ.copy()
    for key in ("IMAGINE_FACE_MODELS", "IMAGINE_FACE_DEVICE", "IMAGINE_FACE_MATCH_THRESHOLD",
                "IMAGINE_API_TOKEN", "IMAGINE_API_KEY", "API_TOKEN"):
        env.pop(key, None)
    env["IMAGINE_FACE_DEVICE"] = "cpu"

    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        process = subprocess.Popen(
            [str(binary), "serve", "--catalog", str(catalog), "--thumbs", str(thumbs),
             "--host", "127.0.0.1", "--port", str(port), "--web-dir", str(ROOT / "web")],
            cwd=ROOT, env=env, stdout=stdout, stderr=stderr,
        )
        try:
            url = f"http://127.0.0.1:{port}"
            wait_for_server(url, process)
            tag_id = seed_catalog(catalog, args.candidates, args.examples)
            if args.full_grid_only:
                grid_page = {"skipped": True, "reason": "--full-grid-only"}
                grid_full = summarize([measure_full_grid_once(url, tag_id, args.candidates, args.examples)])
                grid_full["sample_count"] = 1
                accept_ms, accept_result = None, {"skipped": True, "sample_count": 0}
            else:
                grid_page, grid_full = measure_grid(url, tag_id, args.candidates, args.examples, args.runs,
                                                    args.skip_full_grid)
                accept_ms, accept_result = measure_accept(url, tag_id, args.candidates, args.accept)

            assigned = untouched = None
            if not args.full_grid_only:
                # Independently verify persisted acceptance without relying on response counts alone.
                conn = sqlite3.connect(catalog)
                try:
                    rows = conn.execute("SELECT person_tag_id,revision FROM faces WHERE id<=? ORDER BY id",
                                        (args.candidates,)).fetchall()
                finally:
                    conn.close()
                assigned = sum(1 for tag, revision in rows[:args.accept] if tag == tag_id and revision == 2)
                untouched = sum(1 for tag, revision in rows[args.accept:] if tag is None and revision == 1)
                if assigned != accept_result["updated"] or untouched != args.candidates - accept_result["updated"]:
                    raise AssertionError(f"persisted acceptance mismatch: assigned={assigned}, untouched={untouched}")

            report = {
                "created_utc": datetime.now(timezone.utc).isoformat(),
                "binary": str(binary),
                "runtime": "models unset; synthetic CPU-only face-service workload",
                "dataset": {
                    "candidates": args.candidates,
                    "confirmed_examples": args.examples,
                    "total_faces": args.candidates + args.examples,
                    "embedding_dimensions": DIMENSIONS,
                    "embeddings": "identical normalized float32 vectors (synthetic; cosine=1.0)",
                    "recognizer_checksum": RECOGNIZER_CHECKSUM,
                    "pipeline_version": PIPELINE_VERSION,
                },
                "measurements": {
                    "grid_first_candidate_page_limit_200": grid_page,
                    "grid_full_catalog_pages_limit_1000": grid_full,
                    f"batch_accept_{args.accept}": (
                        {**accept_result} if args.full_grid_only else {
                            **summarize([accept_ms]),
                            **accept_result,
                            "sample_count": 1,
                            "persisted_assigned": assigned,
                            "remaining_unnamed_candidates": untouched,
                        }
                    ),
                },
                "validation": ("full grid IDs and candidate suggestions verified" if args.full_grid_only else
                               "all grid IDs/suggestions matched; batch response and persisted identity/revision counts verified"),
                "catalog": str(catalog),
                "server_pid": process.pid,
            }
            report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            print(json.dumps(report, indent=2))
            print(f"REPORT={report_path}")
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)


if __name__ == "__main__":
    main()
