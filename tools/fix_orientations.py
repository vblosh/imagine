#!/usr/bin/env python3
"""
Fix photo orientations in Imagine catalog by reading EXIF orientation headers
from disk, updating media_items.orientation in SQLite, and clearing any
stale unrotated thumbnails from the cache directory.
"""

import os
import sys
import time
import struct
import sqlite3
import argparse
from concurrent.futures import ThreadPoolExecutor

sys.stdout.reconfigure(encoding='utf-8')

def get_exif_orientation(filepath):
    """
    Read EXIF orientation tag (0x0112) directly from JPEG/TIFF header.
    Returns 1..8 if found, otherwise 1.
    """
    try:
        with open(filepath, 'rb') as f:
            data = f.read(65536)
            if not data or len(data) < 16:
                return 1

            # Check JPEG
            idx = data.find(b'Exif\x00\x00')
            if idx != -1:
                tiff_start = idx + 6
                byte_order = data[tiff_start:tiff_start+2]
                endian = '<' if byte_order == b'II' else '>'
                magic = struct.unpack(endian + 'H', data[tiff_start+2:tiff_start+4])[0]
                if magic == 42:
                    offset = struct.unpack(endian + 'I', data[tiff_start+4:tiff_start+8])[0]
                    ifd_start = tiff_start + offset
                    if ifd_start + 2 <= len(data):
                        num_entries = struct.unpack(endian + 'H', data[ifd_start:ifd_start+2])[0]
                        for i in range(num_entries):
                            entry_offset = ifd_start + 2 + i * 12
                            if entry_offset + 12 > len(data):
                                break
                            tag = struct.unpack(endian + 'H', data[entry_offset:entry_offset+2])[0]
                            if tag == 0x0112:
                                val = struct.unpack(endian + 'H', data[entry_offset+8:entry_offset+10])[0]
                                if 1 <= val <= 8:
                                    return val
                                return 1

            # Check TIFF
            if data.startswith(b'II*\x00') or data.startswith(b'MM\x00*'):
                endian = '<' if data[:2] == b'II' else '>'
                offset = struct.unpack(endian + 'I', data[4:8])[0]
                if offset + 2 <= len(data):
                    num_entries = struct.unpack(endian + 'H', data[offset:offset+2])[0]
                    for i in range(num_entries):
                        entry_offset = offset + 2 + i * 12
                        if entry_offset + 12 > len(data):
                            break
                        tag = struct.unpack(endian + 'H', data[entry_offset:entry_offset+2])[0]
                        if tag == 0x0112:
                            val = struct.unpack(endian + 'H', data[entry_offset+8:entry_offset+10])[0]
                            if 1 <= val <= 8:
                                return val
                            return 1
    except Exception:
        pass
    return 1

def main():
    parser = argparse.ArgumentParser(description="Fix orientations in Imagine catalog")
    parser.add_argument("--catalog", default=r"D:\Foto\imagine\catalog.db",
                        help="Path to catalog.db")
    parser.add_argument("--photos-dir", default=r"D:\Foto",
                        help="Root photos directory")
    parser.add_argument("--cache-dir", default=r"D:\Foto\imagine\cache",
                        help="Thumbnail cache directory")
    parser.add_argument("--workers", type=int, default=16,
                        help="Thread pool workers")
    args = parser.parse_args()

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

    conn = sqlite3.connect(args.catalog)
    cur = conn.cursor()
    cur.execute("SELECT id, file_path, content_hash, orientation FROM media_items")
    rows = cur.fetchall()
    total_files = len(rows)
    print(f"\n[1/3] Reading EXIF orientation for {total_files:,} media items...")

    t0 = time.time()
    def process_item(row):
        mid, rel_path, chash, current_orient = row
        full_p = os.path.join(args.photos_dir, rel_path)
        exif_o = get_exif_orientation(full_p)
        return mid, chash, current_orient, exif_o

    results = []
    with ThreadPoolExecutor(max_workers=args.workers) as ex:
        results = list(ex.map(process_item, rows, chunksize=250))

    elapsed = time.time() - t0
    print(f"  Scanned {total_files:,} files in {elapsed:.2f}s ({total_files/max(elapsed, 0.001):.0f} files/sec).")

    # Filter mismatches
    to_update = []
    hashes_to_invalidate = set()
    counts = {}
    for mid, chash, curr_o, exif_o in results:
        counts[exif_o] = counts.get(exif_o, 0) + 1
        if curr_o != exif_o:
            to_update.append((exif_o, mid))
            if curr_o != 1 or exif_o != 1:
                hashes_to_invalidate.add(chash)

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

    if to_update:
        cur.executemany("UPDATE media_items SET orientation = ? WHERE id = ?", to_update)
        conn.commit()
        print("  Database updated successfully.")
    conn.close()

    # Step 3: Invalidate stale cached thumbnails
    print(f"\n[3/3] Checking thumbnail cache for {len(hashes_to_invalidate):,} rotated photos...")
    deleted_thumbs = 0
    if os.path.isdir(args.cache_dir):
        for chash in hashes_to_invalidate:
            if not chash or len(chash) < 4:
                continue
            p1 = chash[:2]
            p2 = chash[2:4]
            candidates = [
                os.path.join(args.cache_dir, f"{chash}_256.jpg"),
                os.path.join(args.cache_dir, f"{chash}_1024.jpg"),
                os.path.join(args.cache_dir, p1, f"{chash}_256.jpg"),
                os.path.join(args.cache_dir, p1, f"{chash}_1024.jpg"),
                os.path.join(args.cache_dir, p1, p2, f"{chash}_256.jpg"),
                os.path.join(args.cache_dir, p1, p2, f"{chash}_1024.jpg")
            ]
            for c in candidates:
                if os.path.isfile(c):
                    try:
                        os.remove(c)
                        deleted_thumbs += 1
                    except Exception:
                        pass

    print(f"  Purged {deleted_thumbs:,} stale unrotated thumbnails from cache.")
    print("\n" + "=" * 70)
    print(" ALL PHOTO ORIENTATIONS SYNCHRONIZED SUCCESSFULLY!")
    print(" When viewing photos in the Web UI, thumbnails will now generate")
    print(" with the correct rotation and aspect ratio.")
    print("=" * 70)
    return 0

if __name__ == "__main__":
    sys.exit(main())
