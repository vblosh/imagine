#!/usr/bin/env python3
"""Measure lazy face-group loading using an isolated synthetic catalog."""

import argparse
import importlib.util
import json
import os
import sqlite3
import subprocess
import time
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from statistics import median
from urllib.parse import urlencode

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("face_review_benchmark", ROOT / "scripts/benchmark-face-review.py")
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)


def request(url, body=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, headers={} if data is None else {"Content-Type": "application/json"})
    start = time.perf_counter()
    with urllib.request.urlopen(req, timeout=300) as response:
        raw = response.read()
    return {"ms": round((time.perf_counter() - start) * 1000, 3), "bytes": len(raw)}, json.loads(raw)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/face-grid-preview/imagine.exe")
    parser.add_argument("--photos", type=int, default=25000)
    parser.add_argument("--candidates", type=int, default=200)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not 100 < args.candidates < args.photos:
        parser.error("require 100 < candidates < photos to verify acceptance of unloaded faces")
    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error(f"binary does not exist: {binary}")
    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = args.output.resolve() if args.output else ROOT / f"build/face-deps/lazy-grid-{run_id}.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        parser.error("output already exists; choose a new output path")
    work = output.with_suffix("")
    work.mkdir(exist_ok=False)
    catalog = work / "catalog.db"
    helpers.init_catalog(catalog)
    thumbs = work / "thumbs"
    thumbs.mkdir()
    env = os.environ.copy()
    for key in ("IMAGINE_FACE_MODELS", "IMAGINE_FACE_DEVICE", "IMAGINE_FACE_MATCH_THRESHOLD", "IMAGINE_API_TOKEN"):
        env.pop(key, None)
    port = helpers.available_port()
    with (work / "server.log").open("wb") as log:
        process = subprocess.Popen([str(binary), "serve", "--catalog", str(catalog), "--thumbs", str(thumbs),
                                    "--host", "127.0.0.1", "--port", str(port), "--web-dir", str(ROOT / "web")],
                                   cwd=ROOT, env=env, stdout=log, stderr=log)
        try:
            base = f"http://127.0.0.1:{port}/api/faces"
            helpers.wait_for_server(f"http://127.0.0.1:{port}", process)
            tag_id = helpers.seed_catalog(catalog, args.candidates, args.photos - args.candidates)
            cold, groups = request(base + "/groups")
            assert groups["total"] == args.photos, groups
            group = next(group for group in groups["groups"] if group["key"] == f"person:{tag_id}")
            assert group["faces_count"] == args.photos
            assert group["suggestion_faces_count"] == args.candidates
            assert group["suggestion_photos_count"] == args.candidates
            cached = []
            for _ in range(5):
                sample, fresh = request(base + "/groups")
                assert fresh["snapshot"] == groups["snapshot"]
                cached.append(sample)
            query = {"key": group["key"], "snapshot": groups["snapshot"], "offset": 0, "limit": 100}
            page_time, page = request(base + "/group?" + urlencode(query))
            assert page["total"] == args.photos and len(page["items"]) == 100
            first_ids = {face["id"] for face in page["items"]}
            # Read a distant confirmed page, leaving candidate faces 101..
            # candidates unloaded when accepting the complete group.
            query["offset"] = args.candidates
            next_time, next_page = request(base + "/group?" + urlencode(query))
            assert len(next_page["items"]) == min(100, args.photos - args.candidates)
            assert not first_ids.intersection(face["id"] for face in next_page["items"])
            assert all(face["person_tag_id"] == tag_id for face in next_page["items"])
            accept_time, accepted = request(base + "/groups/accept", {"snapshot": groups["snapshot"], "key": group["key"]})
            assert accepted["updated_count"] == args.candidates and accepted["failed_count"] == 0, accepted
            with sqlite3.connect(catalog) as connection:
                assigned = connection.execute("SELECT count(*) FROM faces WHERE id<=? AND person_tag_id=? AND revision=2",
                                              (args.candidates, tag_id)).fetchone()[0]
            assert assigned == args.candidates
            report = {"created_utc": datetime.now(timezone.utc).isoformat(), "binary": str(binary),
                      "dataset": {"photos": args.photos, "unnamed_candidates": args.candidates,
                                  "confirmed_examples": args.photos - args.candidates},
                      "cold_groups": cold, "cached_groups": cached,
                      "cached_groups_median_ms": median(sample["ms"] for sample in cached),
                      "first_100_cards": page_time, "distant_confirmed_page": next_time,
                      "accept_entire_group": accept_time, "accepted_unloaded_and_loaded": assigned,
                      "accepted_candidates_not_fetched": args.candidates - len(first_ids),
                      "limits": "Synthetic identical 512D vectors, one face per photo and one person; no inference, image decoding, crops or browser rendering. Cold means first index request in this process, not a flushed OS cache. Acceptance is a single timed sample."}
            output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            print(json.dumps(report, indent=2))
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=10)


if __name__ == "__main__":
    main()
