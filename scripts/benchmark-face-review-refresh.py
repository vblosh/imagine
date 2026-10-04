#!/usr/bin/env python3
"""Measure exact suggestion refresh after review edits on an isolated catalog."""

import argparse
import importlib.util
import json
import os
import sqlite3
import subprocess
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import urlencode

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("lazy_grid_benchmark", ROOT / "scripts/benchmark-face-lazy-grid.py")
lazy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lazy)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/face-grid-preview/imagine.exe")
    parser.add_argument("--photos", type=int, default=25000)
    parser.add_argument("--candidates", type=int, default=2500)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 4 < args.candidates < args.photos - 1:
        parser.error("require 4 < candidates < photos - 1")
    output = args.output.resolve()
    if output.exists():
        parser.error("output already exists")
    work = output.with_suffix("")
    work.mkdir(parents=True, exist_ok=False)
    catalog = work / "catalog.db"
    lazy.helpers.init_catalog(catalog)
    thumbs = work / "thumbs"
    thumbs.mkdir()
    env = os.environ.copy()
    for key in ("IMAGINE_FACE_MODELS", "IMAGINE_FACE_DEVICE", "IMAGINE_FACE_MATCH_THRESHOLD", "IMAGINE_API_TOKEN"):
        env.pop(key, None)
    port = lazy.helpers.available_port()
    samples = []
    with (work / "server.log").open("wb") as log:
        process = subprocess.Popen([str(args.binary.resolve()), "serve", "--catalog", str(catalog),
                                    "--thumbs", str(thumbs), "--host", "127.0.0.1", "--port", str(port),
                                    "--web-dir", str(ROOT / "web")], cwd=ROOT, env=env, stdout=log, stderr=log)
        try:
            lazy.helpers.wait_for_server(f"http://127.0.0.1:{port}", process)
            tag_id = lazy.helpers.seed_catalog(catalog, args.candidates, args.photos - args.candidates)
            base = f"http://127.0.0.1:{port}/api/faces"

            def refresh(label, expected_candidates, expected_total):
                timing, groups = lazy.request(base + "/groups")
                assert groups["total"] == expected_total, groups
                person = next(g for g in groups["groups"] if g["key"] == f"person:{tag_id}")
                assert person["suggestion_faces_count"] == expected_candidates, person
                query = dict(key=person["key"], snapshot=groups["snapshot"], offset=0, limit=1)
                page_timing, page = lazy.request(base + "/group?" + urlencode(query))
                assert len(page["items"]) == 1
                sample = dict(change=label, groups=timing, first_card=page_timing,
                              suggestion_faces=expected_candidates)
                samples.append(sample)
                print(json.dumps(sample), flush=True)

            refresh("cold", args.candidates, args.photos)
            with sqlite3.connect(catalog) as connection:
                connection.execute("UPDATE media_items SET rating=1 WHERE id=1")
            refresh("unrelated_external_rating", args.candidates, args.photos)
            lazy.request(base + "/1/dismiss", dict(revision=1, dismissed=True))
            refresh("dismiss_unnamed", args.candidates - 1, args.photos - 1)
            example_id = args.candidates + 1
            lazy.request(base + f"/{example_id}/dismiss", dict(revision=1, dismissed=True))
            refresh("dismiss_best_confirmed", args.candidates - 1, args.photos - 2)
            lazy.request(base + f"/{example_id}/dismiss", dict(revision=2, dismissed=False))
            refresh("restore_best_confirmed", args.candidates - 1, args.photos - 1)
            lazy.request(base + "/2/identity", dict(revision=1, tag_id=tag_id))
            refresh("confirm_one", args.candidates - 2, args.photos - 1)
            lazy.request(base + "/2/identity", dict(revision=2, tag_id=None))
            refresh("clear_confirmed", args.candidates - 1, args.photos - 1)
            lazy.request(base + "/3/reject", dict(revision=1, tag_id=tag_id))
            refresh("reject_one", args.candidates - 2, args.photos - 1)
            lazy.request(base + "/4/identity", dict(revision=1, name="ZZZ New Person"))
            refresh("name_new_person", args.candidates - 3, args.photos - 1)
            lazy.request(base + "/4/identity", dict(revision=2, tag_id=None))
            refresh("clear_new_person", args.candidates - 2, args.photos - 1)
            with sqlite3.connect(catalog) as connection:
                rows = connection.execute("SELECT id,person_tag_id,dismissed,revision FROM faces WHERE id<=3 ORDER BY id").fetchall()
                assert rows == [(1, None, 1, 2), (2, None, 0, 3), (3, None, 0, 2)], rows
                assert connection.execute("SELECT dismissed,revision FROM faces WHERE id=?", (example_id,)).fetchone() == (0, 3)
                assert connection.execute("SELECT person_tag_id,revision FROM faces WHERE id=4").fetchone() == (None, 3)
            report = dict(created_utc=datetime.now(timezone.utc).isoformat(), binary=str(args.binary.resolve()),
                          photos=args.photos, unnamed_candidates=args.candidates, samples=samples,
                          limits="Identical normalized 512D vectors, one person, synthetic catalog. Individual samples; no inference or browser/crop cost. Current suggestions and persisted revisions checked.")
            output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=10)


if __name__ == "__main__":
    main()
