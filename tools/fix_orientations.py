#!/usr/bin/env python3
"""
Fix photo orientations in Imagine by decoding photos and inspecting their EXIF
orientation. Requires Pillow. Run against a stopped catalog so the server
reloads derived data when it starts.
"""

import os
import sys
import time
import sqlite3
import argparse
import re
from concurrent.futures import ThreadPoolExecutor
from contextlib import closing

try:
    from PIL import Image
except ImportError:
    Image = None

def get_exif_orientation(filepath):
    """Return the EXIF orientation of a readable photo or None if unavailable."""
    try:
        with Image.open(filepath) as image:
            orientation = image.getexif().get(0x0112, 1)
            image.load()
    except (OSError, SyntaxError, ValueError, Image.DecompressionBombError):
        return None
    return orientation if isinstance(orientation, int) and 1 <= orientation <= 8 else None

def main(argv=None):
    parser = argparse.ArgumentParser(description="Fix orientations in Imagine catalog")
    parser.add_argument("--catalog", default=r"D:\Foto\imagine\catalog.db",
                        help="Path to catalog.db")
    parser.add_argument("--photos-dir", default=r"D:\Foto",
                        help="Root photos directory")
    parser.add_argument("--cache-dir", default=r"D:\Foto\imagine\cache",
                        help="Thumbnail cache directory")
    parser.add_argument("--workers", type=int, default=16,
                        help="Thread pool workers")
    args = parser.parse_args(argv)
    if Image is None:
        print("Orientation repair requires Pillow (python -m pip install Pillow).", file=sys.stderr)
        return 1

    print("=" * 70)
    print(" IMAGINE PHOTO ORGANIZER — EXIF ORIENTATION REPAIR TOOL")
    print("=" * 70)
    print(f" Catalog:    {args.catalog}")
    print(f" Photos Dir: {args.photos_dir}")
    print(f" Cache Dir:  {args.cache_dir}")
    print(f" Workers:    {args.workers}")
    print("=" * 70)

    if not os.path.isfile(args.catalog):
        print(f"Error: Catalog not found at {args.catalog}")
        return 1

    with closing(sqlite3.connect(args.catalog)) as conn:
        columns = {row[1] for row in conn.execute("PRAGMA table_info(media_items)")}
        query = "SELECT id, file_path, content_hash, orientation FROM media_items"
        if "media_type" in columns:
            query += " WHERE media_type = 'photo'"
        rows = conn.execute(query).fetchall()
    total_files = len(rows)
    print(f"\n[1/3] Reading EXIF orientation for {total_files:,} media items...")

    t0 = time.time()
    def process_item(row):
        mid, rel_path, chash, current_orient = row
        full_p = os.path.join(args.photos_dir, rel_path)
        exif_o = get_exif_orientation(full_p)
        return mid, rel_path, chash, current_orient, exif_o

    results = []
    with ThreadPoolExecutor(max_workers=args.workers) as ex:
        results = list(ex.map(process_item, rows, chunksize=250))

    elapsed = time.time() - t0
    print(f"  Scanned {total_files:,} files in {elapsed:.2f}s ({total_files/max(elapsed, 0.001):.0f} files/sec).")

    # Filter mismatches
    to_update = []
    counts = {}
    for mid, rel_path, chash, curr_o, exif_o in results:
        if exif_o is None:
            continue
        counts[exif_o] = counts.get(exif_o, 0) + 1
        if curr_o != exif_o:
            to_update.append((exif_o, mid, rel_path, chash, curr_o))
    print(f"  Skipped {total_files - sum(counts.values()):,} unavailable or unreadable photos.")

    print("\n[2/3] Orientation distribution across library:")
    orientation_labels = {
        1: "1 (Normal 0°)",
        2: "2 (Mirrored Horizontal)",
        3: "3 (Rotated 180°)",
        4: "4 (Mirrored Vertical)",
        5: "5 (Mirrored Horizontal + Rotated 270° CW)",
        6: "6 (Rotated 90° CW)",
        7: "7 (Mirrored Horizontal + Rotated 90° CW)",
        8: "8 (Rotated 270° CW / 90° CCW)"
    }
    for o, count in sorted(counts.items()):
        print(f"  Orientation {orientation_labels.get(o, str(o))}: {count:,} photos")

    print(f"\n  Found {len(to_update):,} photos requiring orientation database updates.")

    hashes_to_invalidate = set()
    updated_count = 0
    if to_update:
        with closing(sqlite3.connect(args.catalog)) as conn, conn:
            conn.execute("PRAGMA foreign_keys = ON")
            conn.execute("BEGIN IMMEDIATE")
            tables = {row[0] for row in conn.execute("SELECT name FROM sqlite_master WHERE type = 'table'")}
            provenance_columns = {row[1] for row in conn.execute("PRAGMA table_info(media_tag_provenance)")}
            derived = "media_id = ? AND manual = 0"
            if "source" in provenance_columns:
                derived += " AND source IN ('face', 'clip')"
            now = int(time.time())
            for exif_o, mid, path, chash, curr_o in to_update:
                updated = conn.execute(
                    "UPDATE media_items SET orientation = ?, updated_at = ? "
                    "WHERE id = ? AND file_path = ? AND content_hash IS ? AND orientation IS ?",
                    (exif_o, now, mid, path, chash, curr_o),
                )
                if not updated.rowcount:
                    continue
                if provenance_columns:
                    conn.execute(
                        f"DELETE FROM media_tags WHERE media_id = ? AND tag_id IN ("
                        f"SELECT tag_id FROM media_tag_provenance WHERE {derived})", (mid, mid)
                    )
                    conn.execute(f"DELETE FROM media_tag_provenance WHERE {derived}", (mid,))
                for table in ("faces", "face_media_analysis", "semantic_index_state", "smart_tag_state"):
                    if table in tables:
                        conn.execute(f"DELETE FROM {table} WHERE media_id = ?", (mid,))
                if chash:
                    hashes_to_invalidate.add(chash)
                updated_count += 1
        print(f"  Updated {updated_count:,} photo orientations and invalidated their analysis.")

    # Step 3: Invalidate stale cached thumbnails
    print(f"\n[3/3] Checking thumbnail cache for {len(hashes_to_invalidate):,} rotated photos...")
    deleted_thumbs = 0
    if os.path.isdir(args.cache_dir):
        for chash in hashes_to_invalidate:
            if not re.fullmatch(r"[0-9a-fA-F]{64}", chash):
                continue
            for directory in (args.cache_dir, os.path.join(args.cache_dir, chash[:2]),
                              os.path.join(args.cache_dir, chash[:2], chash[2:4])):
                if not os.path.isdir(directory):
                    continue
                for filename in os.listdir(directory):
                    if re.fullmatch(chash + r"_\d+\.jpg", filename):
                        try:
                            os.remove(os.path.join(directory, filename))
                            deleted_thumbs += 1
                        except OSError as error:
                            print(f"  Could not remove {filename}: {error}", file=sys.stderr)

    print(f"  Purged {deleted_thumbs:,} stale unrotated thumbnails from cache.")
    print("\n" + "=" * 70)
    print(" Orientation repair finished.")
    print("=" * 70)
    return 0

if __name__ == "__main__":
    if sys.platform == "win32":
        sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main())
