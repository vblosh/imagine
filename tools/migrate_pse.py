#!/usr/bin/env python3
"""
Migrate Adobe Photoshop Elements Organizer Catalog (catalog.pse20db) to Imagine (catalog.db).

Usage:
  python tools/migrate_pse.py [options]

Options:
  --pse-db <path>       Path to Photoshop Elements catalog.pse20db
                        Default: D:\\Foto\\Adobe\\Catalogs\\My Catalog\\catalog.pse20db
  --imagine-db <path>   Path to output Imagine SQLite database
                        Default: catalog.db
  --photos-dir <path>   Photos base directory for checking/storing relative paths
                        Default: D:\\Foto
  --workers <N>         Number of worker threads for parallel file hashing (default: 8)
  --quick-hash          Use fast content hash (path + size + mtime) instead of full SHA-256
  --limit <N>           Limit number of media items migrated (useful for testing)
  --dry-run             Audit and preview migration statistics without writing database
  --include-autotags    Include Adobe AI auto-detected tags (default: false, per user preference)
  -h, --help            Show this help message
"""

import os
import sys
import time
import struct
import argparse
import sqlite3
import hashlib
from datetime import datetime
from concurrent.futures import ThreadPoolExecutor

sys.stdout.reconfigure(encoding='utf-8')

CAMERA_DUMMY_CAPTIONS = {
    'DCF 1.0',
    'CAMERA',
    'Exif_JPEG_PICTURE',
    'OLYMPUS DIGITAL CAMERA',
    'MINOLTA DIGITAL CAMERA',
    'SONY DSC',
    'SAMSUNG DIGITAL CAMERA',
    'DIGITAL CAMERA',
    'NIKON DIGITAL CAMERA',
    '--------',
    '+',
    ','
}

IMAGINE_SCHEMA_SQL = """
CREATE TABLE IF NOT EXISTS schema_version (version INTEGER PRIMARY KEY);
INSERT OR IGNORE INTO schema_version (version) VALUES (1);
INSERT OR IGNORE INTO schema_version (version) VALUES (2);

CREATE TABLE IF NOT EXISTS media_items (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    file_path TEXT UNIQUE NOT NULL,
    file_name TEXT NOT NULL,
    file_size INTEGER NOT NULL,
    file_modified_time INTEGER NOT NULL,
    content_hash TEXT NOT NULL,
    width INTEGER DEFAULT 0,
    height INTEGER DEFAULT 0,
    date_taken INTEGER NOT NULL,
    date_taken_str TEXT DEFAULT '',
    rating INTEGER DEFAULT 0,
    flag INTEGER DEFAULT 0,
    camera_make TEXT DEFAULT '',
    camera_model TEXT DEFAULT '',
    lens TEXT DEFAULT '',
    exposure_time REAL DEFAULT 0.0,
    f_number REAL DEFAULT 0.0,
    iso INTEGER DEFAULT 0,
    focal_length REAL DEFAULT 0.0,
    orientation INTEGER DEFAULT 1,
    has_gps INTEGER DEFAULT 0,
    latitude REAL DEFAULT 0.0,
    longitude REAL DEFAULT 0.0,
    altitude REAL DEFAULT 0.0,
    thumb_small TEXT DEFAULT '',
    thumb_large TEXT DEFAULT '',
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    caption TEXT DEFAULT ''
);

CREATE INDEX IF NOT EXISTS idx_media_hash ON media_items(content_hash);
CREATE INDEX IF NOT EXISTS idx_media_date ON media_items(date_taken DESC);
CREATE INDEX IF NOT EXISTS idx_media_rating ON media_items(rating);
CREATE INDEX IF NOT EXISTS idx_media_flag ON media_items(flag);
CREATE INDEX IF NOT EXISTS idx_media_camera ON media_items(camera_make, camera_model);
CREATE INDEX IF NOT EXISTS idx_media_caption ON media_items(caption);

CREATE TABLE IF NOT EXISTS tags (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    category TEXT NOT NULL DEFAULT 'keyword',
    parent_id INTEGER REFERENCES tags(id) ON DELETE SET NULL,
    UNIQUE(name, category)
);

CREATE INDEX IF NOT EXISTS idx_tags_category ON tags(category);

CREATE TABLE IF NOT EXISTS media_tags (
    media_id INTEGER NOT NULL REFERENCES media_items(id) ON DELETE CASCADE,
    tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
    PRIMARY KEY(media_id, tag_id)
);

CREATE INDEX IF NOT EXISTS idx_media_tags_tag ON media_tags(tag_id);

CREATE TABLE IF NOT EXISTS albums (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL UNIQUE,
    description TEXT DEFAULT '',
    is_smart INTEGER DEFAULT 0,
    query_json TEXT DEFAULT '',
    cover_media_id INTEGER REFERENCES media_items(id) ON DELETE SET NULL,
    created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS album_media (
    album_id INTEGER NOT NULL REFERENCES albums(id) ON DELETE CASCADE,
    media_id INTEGER NOT NULL REFERENCES media_items(id) ON DELETE CASCADE,
    position INTEGER DEFAULT 0,
    PRIMARY KEY(album_id, media_id)
);

CREATE INDEX IF NOT EXISTS idx_album_media_media ON album_media(media_id);
"""

def get_exif_orientation(filepath):
    try:
        with open(filepath, 'rb') as f:
            data = f.read(65536)
            if not data or len(data) < 16:
                return 1

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

def compute_file_sha256(filepath):
    try:
        h = hashlib.sha256()
        with open(filepath, 'rb') as f:
            while chunk := f.read(131072): # 128KB buffer
                h.update(chunk)
        return h.hexdigest()
    except Exception:
        return hashlib.sha256(filepath.encode()).hexdigest()

def compute_quick_hash(filepath, size, mtime):
    return hashlib.sha256(f"{filepath}_{size}_{mtime}".encode()).hexdigest()

def parse_pse_date(date_str):
    if not date_str or len(date_str) < 15:
        return 0, ""
    try:
        dt = datetime.strptime(date_str[:15], "%Y%m%dT%H%M%S")
        epoch = int(dt.timestamp())
        formatted = dt.strftime("%Y-%m-%d %H:%M:%S")
        return epoch, formatted
    except Exception:
        return 0, ""

def is_valid_caption(caption):
    if not caption or not isinstance(caption, str):
        return False
    c = caption.strip()
    if not c or len(c) < 2:
        return False
    if c in CAMERA_DUMMY_CAPTIONS:
        return False
    if all(ch in '-_+=,.;: ' for ch in c):
        return False
    return True

def run_migration():
    parser = argparse.ArgumentParser(description="Migrate PSE Catalog to Imagine")
    parser.add_argument("--pse-db", default=r"D:\Foto\Adobe\Catalogs\My Catalog\catalog.pse20db",
                        help="Path to PSE catalog.pse20db")
    parser.add_argument("--imagine-db", default=r"D:\Foto\imagine\catalog.db",
                        help="Path to output Imagine database (default: D:\\Foto\\imagine\\catalog.db)")
    parser.add_argument("--photos-dir", default=r"D:\Foto",
                        help="Root photos directory (default: D:\\Foto)")
    parser.add_argument("--make-relative", action="store_true", default=True,
                        help="Store photo paths relative to photos-dir (default: True)")
    parser.add_argument("--no-relative", dest="make_relative", action="store_false",
                        help="Store photo paths as absolute paths instead of relative")
    parser.add_argument("--workers", type=int, default=12,
                        help="Thread pool workers for SHA-256 calculation (default: 12)")
    parser.add_argument("--quick-hash", action="store_true",
                        help="Use fast hash (filepath + size + mtime) instead of reading full files")
    parser.add_argument("--limit", type=int, default=0,
                        help="Limit number of photos to migrate")
    parser.add_argument("--dry-run", action="store_true",
                        help="Perform a dry run without modifying the database")
    parser.add_argument("--include-autotags", action="store_true",
                        help="Include Adobe AI auto-detected tags")

    args = parser.parse_args()

    start_time = time.time()
    print("=" * 70)
    print(" IMAGINE PHOTO ORGANIZER — PSE CATALOG MIGRATION ENGINE")
    print("=" * 70)
    print(f" Source PSE DB:     {args.pse_db}")
    print(f" Target Imagine DB: {args.imagine_db}")
    print(f" Photos Directory:  {args.photos_dir}")
    print(f" Workers:           {args.workers}")
    print(f" Hashing Mode:      {'Quick Hash' if args.quick_hash else 'Full SHA-256'}")
    print(f" Dry Run Mode:      {args.dry_run}")
    print(f" Include AutoTags:  {args.include_autotags}")
    print("=" * 70)

    if not os.path.isfile(args.pse_db):
        print(f"Error: PSE database not found: {args.pse_db}")
        sys.exit(1)

    pse_conn = sqlite3.connect(f"file:{args.pse_db}?mode=ro", uri=True)
    pse_conn.row_factory = sqlite3.Row
    pcur = pse_conn.cursor()

    # Step 1: Load Volumes
    print("\n[1/6] Loading drive volumes...")
    pcur.execute("SELECT id, description, drive_path_if_builtin, type FROM volume_table")
    vol_rows = pcur.fetchall()
    vol_map = {}
    for vr in vol_rows:
        vol_map[vr['id']] = vr['drive_path_if_builtin'] or ''
        print(f"  Volume {vr['id']}: drive='{vr['drive_path_if_builtin']}', type='{vr['type']}', desc='{vr['description']}'")

    # Step 2: Load and Categorize Tags
    print("\n[2/6] Reading and categorizing PSE tag taxonomy...")
    pcur.execute("SELECT id, name, parent_id, type_name FROM tag_table")
    all_tags = pcur.fetchall()

    hidden_tag_ids = set()
    for t in all_tags:
        if t['type_name'] == 'hidden' or t['name'] == 'Hidden':
            hidden_tag_ids.add(t['id'])
    print(f"  Hidden tag IDs identified: {hidden_tag_ids}")

    tag_info = {}
    skipped_zz = 0
    skipped_autotag = 0
    skipped_system = 0

    for t in all_tags:
        tid = t['id']
        name = (t['name'] or '').strip()
        t_type = t['type_name']
        parent = t['parent_id']

        if t_type == 'user_person':
            if not name or name.startswith('zz'):
                skipped_zz += 1
                continue
            tag_info[tid] = (name, 'people', parent)
        elif t_type == 'user_place':
            if not name:
                skipped_system += 1
                continue
            tag_info[tid] = (name, 'places', parent)
        elif t_type == 'user_event':
            if not name:
                skipped_system += 1
                continue
            tag_info[tid] = (name, 'events', parent)
        elif t_type in ('user_misc', 'user_custom'):
            if not name:
                skipped_system += 1
                continue
            tag_info[tid] = (name, 'keyword', parent)
        elif t_type == 'autotag':
            if args.include_autotags and name:
                tag_info[tid] = (name, 'autotag', parent)
            else:
                skipped_autotag += 1
        elif t_type == 'collection':
            pass
        else:
            skipped_system += 1

    counts_by_cat = {}
    for _, cat, _ in tag_info.values():
        counts_by_cat[cat] = counts_by_cat.get(cat, 0) + 1

    print(f"  Retained tags to migrate: {len(tag_info)}")
    for cat, cnt in sorted(counts_by_cat.items()):
        print(f"    - {cat.capitalize()}: {cnt} tags")
    print(f"  Filtered out: {skipped_zz} synthetic zz* face clusters, {skipped_autotag} auto-tags, {skipped_system} system/import tags.")

    # Step 3: Load Metadata Pre-caches (Ratings, Captions, GPS)
    print("\n[3/6] Pre-indexing ratings, captions, and GPS coordinates...")
    pcur.execute("""
        SELECT mm.media_id, i.value
        FROM media_to_metadata_table mm
        JOIN metadata_integer_table i ON i.id = mm.metadata_id
        WHERE i.description_id = 5 AND i.value > 0
    """)
    rating_map = {row[0]: row[1] for row in pcur.fetchall()}
    print(f"  Indexed {len(rating_map)} non-zero ratings.")

    pcur.execute("""
        SELECT mm.media_id, s.value
        FROM media_to_metadata_table mm
        JOIN metadata_string_table s ON s.id = mm.metadata_id
        WHERE s.description_id = 3
    """)
    caption_map = {}
    for row in pcur.fetchall():
        mid, val = row[0], row[1]
        if is_valid_caption(val):
            caption_map[mid] = val.strip()
    print(f"  Indexed {len(caption_map)} meaningful captions (camera placeholders filtered out).")

    pcur.execute("""
        SELECT mmlat.media_id, lat.value, lon.value
        FROM media_to_metadata_table mmlat
        JOIN metadata_decimal_table lat ON lat.id = mmlat.metadata_id AND lat.description_id = 24
        JOIN media_to_metadata_table mmlon ON mmlon.media_id = mmlat.media_id
        JOIN metadata_decimal_table lon ON lon.id = mmlon.metadata_id AND lon.description_id = 23
        WHERE lat.value >= -90 AND lat.value <= 90
          AND NOT (lat.value = -91 AND lon.value = -181)
          AND (lat.value != 0 OR lon.value != 0)
    """)
    gps_map = {row[0]: (float(row[1]), float(row[2])) for row in pcur.fetchall()}
    print(f"  Indexed {len(gps_map)} valid GPS coordinates.")

    print("  Indexing EXIF camera & lens properties...")
    pcur.execute("""
        SELECT mm.media_id, d.identifier, s.value
        FROM media_to_metadata_table mm
        JOIN metadata_string_table s ON s.id = mm.metadata_id
        JOIN metadata_description_table d ON d.id = s.description_id
        WHERE d.identifier IN ('exif:Make', 'exif:Model')
    """)
    exif_strings = {}
    for mid, ident, val in pcur.fetchall():
        if mid not in exif_strings: exif_strings[mid] = {}
        exif_strings[mid][ident] = val

    hidden_media_set = set()
    if hidden_tag_ids:
        placeholders = ','.join('?' for _ in hidden_tag_ids)
        pcur.execute(f"SELECT media_id FROM tag_to_media_table WHERE tag_id IN ({placeholders})", tuple(hidden_tag_ids))
        hidden_media_set = {r[0] for r in pcur.fetchall()}
    print(f"  Identified {len(hidden_media_set)} photos flagged as Hidden in PSE.")

    # Step 4: Scan and Resolve Media Records
    print("\n[4/6] Scanning PSE media records and validating files on disk...")
    query = """
        SELECT m.id, m.volume_id, m.full_filepath, m.search_date_begin
        FROM media_table m
        WHERE m.volume_id NOT IN (184, 2)
        ORDER BY m.id
    """
    if args.limit > 0:
        query += f" LIMIT {args.limit}"

    pcur.execute(query)
    media_records = pcur.fetchall()
    print(f"  Read {len(media_records)} media candidates from catalog.")

    path_to_item = {}
    pse_id_to_primary_path = {}
    missing_count = 0
    duplicate_count = 0

    for m in media_records:
        mid = m['id']
        vol_id = m['volume_id']
        rel_path = m['full_filepath']
        s_date = m['search_date_begin']

        drive = vol_map.get(vol_id, '')
        if not drive:
            if rel_path.startswith('/Foto') or rel_path.startswith('\\Foto'):
                drive = 'D:'
            else:
                drive = 'D:'

        full_path = os.path.normpath(drive + rel_path).replace('\\', '/')

        ext = os.path.splitext(full_path)[1].lower()
        if ext in ('.theme', '.mp3'):
            continue

        if not os.path.isfile(full_path):
            missing_count += 1
            continue

        fsize = os.path.getsize(full_path)
        fmtime = int(os.path.getmtime(full_path))
        fname = os.path.basename(full_path)
        date_taken, date_taken_str = parse_pse_date(s_date)
        if date_taken <= 0:
            date_taken = fmtime
            date_taken_str = datetime.fromtimestamp(fmtime).strftime("%Y-%m-%d %H:%M:%S")

        rating = rating_map.get(mid, 0)
        flag = -1 if mid in hidden_media_set else 0
        caption = caption_map.get(mid, '')

        has_gps = 0
        lat_val, lon_val = 0.0, 0.0
        if mid in gps_map:
            has_gps = 1
            lat_val, lon_val = gps_map[mid]

        c_make = exif_strings.get(mid, {}).get('exif:Make', '')
        c_model = exif_strings.get(mid, {}).get('exif:Model', '')

        stored_path = full_path
        if args.make_relative and args.photos_dir:
            try:
                rel = os.path.relpath(full_path, args.photos_dir).replace('\\', '/')
                if not rel.startswith('..'):
                    stored_path = rel
            except ValueError:
                stored_path = full_path

        pse_id_to_primary_path[mid] = stored_path

        if stored_path in path_to_item:
            duplicate_count += 1
            existing = path_to_item[stored_path]
            if rating > existing['rating']:
                existing['rating'] = rating
            if len(caption) > len(existing['caption']):
                existing['caption'] = caption
            if flag < existing['flag']:
                existing['flag'] = flag
            if not existing['has_gps'] and has_gps:
                existing['has_gps'] = has_gps
                existing['latitude'] = lat_val
                existing['longitude'] = lon_val
            if not existing['camera_make'] and c_make:
                existing['camera_make'] = c_make
                existing['camera_model'] = c_model
        else:
            path_to_item[stored_path] = {
                'pse_id': mid,
                'full_path': full_path,
                'stored_path': stored_path,
                'file_name': fname,
                'file_size': fsize,
                'mtime': fmtime,
                'date_taken': date_taken,
                'date_taken_str': date_taken_str,
                'rating': rating,
                'flag': flag,
                'caption': caption,
                'has_gps': has_gps,
                'latitude': lat_val,
                'longitude': lon_val,
                'camera_make': c_make,
                'camera_model': c_model
            }

    items_to_process = list(path_to_item.values())
    print(f"  Valid items verified on disk: {len(items_to_process)} unique files (Deduplicated segments: {duplicate_count}, Missing files skipped: {missing_count})")

    # Step 5: Compute Content Hashes
    print("\n[5/6] Computing cryptographic content hashes...")
    hash_t0 = time.time()

    def process_hash(item):
        exif_o = get_exif_orientation(item['full_path'])
        if args.quick_hash:
            chash = compute_quick_hash(item['full_path'], item['file_size'], item['mtime'])
        else:
            chash = compute_file_sha256(item['full_path'])
        return item['stored_path'], chash, exif_o

    hash_map = {}
    total_to_hash = len(items_to_process)
    completed_hashes = 0
    last_print = time.time()

    with ThreadPoolExecutor(max_workers=args.workers) as executor:
        for stored_p, chash, exif_o in executor.map(process_hash, items_to_process, chunksize=100):
            hash_map[stored_p] = (chash, exif_o)
            completed_hashes += 1
            if time.time() - last_print > 3.0 or completed_hashes == total_to_hash:
                pct = (completed_hashes / max(total_to_hash, 1)) * 100
                elapsed = time.time() - hash_t0
                rate = completed_hashes / max(elapsed, 0.001)
                print(f"  Progress: {completed_hashes}/{total_to_hash} ({pct:.1f}%) — {rate:.1f} files/sec", end='\r')
                last_print = time.time()

    print(f"\n  Hashing completed in {time.time() - hash_t0:.2f} seconds.")

    for item in items_to_process:
        chash, exif_o = hash_map[item['stored_path']]
        item['content_hash'] = chash
        item['orientation'] = exif_o

    # Step 6: Write to Imagine Database
    if args.dry_run:
        print("\n" + "=" * 70)
        print(" [DRY RUN] SUMMARY OF PLANNED WRITES")
        print("=" * 70)
        print(f"  Media Items to Insert:   {len(items_to_process):,}")
        print(f"  Photos with Ratings:     {sum(1 for i in items_to_process if i['rating'] > 0):,}")
        print(f"  Photos with GPS:         {sum(1 for i in items_to_process if i['has_gps'] == 1):,}")
        print(f"  Photos with Captions:    {sum(1 for i in items_to_process if i['caption']):,}")
        print(f"  Photos marked Rejected:  {sum(1 for i in items_to_process if i['flag'] == -1):,}")
        print(f"  Tags to Migrate:         {len(tag_info):,}")
        print("  Dry run complete. No database changes were made.")
        return

    print("\n[6/6] Writing data into Imagine SQLite database...")
    db_dir = os.path.dirname(os.path.abspath(args.imagine_db))
    if db_dir and not os.path.exists(db_dir):
        os.makedirs(db_dir, exist_ok=True)

    im_conn = sqlite3.connect(args.imagine_db)
    im_cur = im_conn.cursor()

    im_cur.execute("PRAGMA journal_mode = WAL;")
    im_cur.execute("PRAGMA synchronous = NORMAL;")
    im_cur.execute("PRAGMA cache_size = -64000;")
    im_cur.execute("PRAGMA temp_store = MEMORY;")

    im_cur.executescript(IMAGINE_SCHEMA_SQL)
    im_conn.commit()

    now = int(time.time())

    # 6.1 Insert Media Items
    print("  Inserting media items...")
    media_batch = []
    for item in items_to_process:
        media_batch.append((
            item['stored_path'],
            item['file_name'],
            item['file_size'],
            item['mtime'],
            item['content_hash'],
            item['date_taken'],
            item['date_taken_str'],
            item['rating'],
            item['flag'],
            item['camera_make'],
            item['camera_model'],
            item['orientation'],
            item['has_gps'],
            item['latitude'],
            item['longitude'],
            now,
            now,
            item['caption']
        ))

    insert_media_sql = """
        INSERT INTO media_items (
            file_path, file_name, file_size, file_modified_time, content_hash,
            date_taken, date_taken_str, rating, flag, camera_make, camera_model,
            orientation, has_gps, latitude, longitude, created_at, updated_at, caption
        ) VALUES (
            ?, ?, ?, ?, ?,
            ?, ?, ?, ?, ?, ?,
            ?, ?, ?, ?, ?, ?, ?
        )
    """
    im_cur.executemany(insert_media_sql, media_batch)
    im_conn.commit()
    print(f"  Inserted {len(media_batch)} media items successfully.")

    im_cur.execute("SELECT id, file_path FROM media_items")
    path_to_imag_id = {row[1]: row[0] for row in im_cur.fetchall()}
    pse_id_to_imag_id = {}
    for pse_mid, full_p in pse_id_to_primary_path.items():
        imag_id = path_to_imag_id.get(full_p)
        if imag_id:
            pse_id_to_imag_id[pse_mid] = imag_id

    # 6.2 Insert Tags
    print("  Inserting tag hierarchy...")
    pse_tag_to_imag_id = {}
    unresolved_tags = dict(tag_info)
    iterations = 0
    while unresolved_tags and iterations < 10:
        iterations += 1
        inserted_this_round = 0
        current_tags = list(unresolved_tags.items())

        for pse_tid, (name, cat, pse_parent) in current_tags:
            imag_parent_id = None
            if pse_parent and pse_parent != 0:
                if pse_parent in pse_tag_to_imag_id:
                    imag_parent_id = pse_tag_to_imag_id[pse_parent]
                elif pse_parent in tag_info:
                    continue

            im_cur.execute("SELECT id FROM tags WHERE name = ? AND category = ?", (name, cat))
            row = im_cur.fetchone()
            if row:
                imag_id = row[0]
            else:
                im_cur.execute(
                    "INSERT INTO tags (name, category, parent_id) VALUES (?, ?, ?)",
                    (name, cat, imag_parent_id)
                )
                imag_id = im_cur.lastrowid

            pse_tag_to_imag_id[pse_tid] = imag_id
            del unresolved_tags[pse_tid]
            inserted_this_round += 1

        if inserted_this_round == 0:
            for pse_tid, (name, cat, _) in unresolved_tags.items():
                im_cur.execute("SELECT id FROM tags WHERE name = ? AND category = ?", (name, cat))
                row = im_cur.fetchone()
                if row:
                    imag_id = row[0]
                else:
                    im_cur.execute("INSERT INTO tags (name, category, parent_id) VALUES (?, ?, NULL)", (name, cat))
                    imag_id = im_cur.lastrowid
                pse_tag_to_imag_id[pse_tid] = imag_id
            break

    im_conn.commit()
    print(f"  Inserted {len(pse_tag_to_imag_id)} unique tags into Imagine.")

    # 6.3 Insert Tag Associations
    print("  Linking media to tags...")
    pcur.execute("SELECT media_id, tag_id FROM tag_to_media_table")
    all_links = pcur.fetchall()

    media_tag_batch = []
    for mid, tid in all_links:
        imag_mid = pse_id_to_imag_id.get(mid)
        imag_tid = pse_tag_to_imag_id.get(tid)
        if imag_mid and imag_tid:
            media_tag_batch.append((imag_mid, imag_tid))

    media_tag_batch = list(set(media_tag_batch))
    print(f"  Matched {len(media_tag_batch)} tag-photo associations to write.")

    im_cur.executemany(
        "INSERT OR IGNORE INTO media_tags (media_id, tag_id) VALUES (?, ?)",
        media_tag_batch
    )
    im_conn.commit()
    print("  Media tags linked successfully.")

    # 6.4 Insert Albums
    print("  Migrating albums and collections...")
    pcur.execute("SELECT id, name FROM tag_table WHERE type_name = 'collection'")
    albums = pcur.fetchall()
    album_count = 0
    album_media_count = 0

    for alb in albums:
        alb_id, alb_name = alb['id'], alb['name']
        if not alb_name:
            continue
        im_cur.execute("SELECT id FROM albums WHERE name = ?", (alb_name,))
        row = im_cur.fetchone()
        if row:
            imag_alb_id = row[0]
        else:
            im_cur.execute("INSERT INTO albums (name, created_at) VALUES (?, ?)", (alb_name, now))
            imag_alb_id = im_cur.lastrowid
            album_count += 1

        pcur.execute("SELECT media_id, media_index FROM tag_to_media_table WHERE tag_id = ?", (alb_id,))
        members = pcur.fetchall()
        for mid, pos in members:
            imag_mid = pse_id_to_imag_id.get(mid)
            if imag_mid:
                im_cur.execute(
                    "INSERT OR IGNORE INTO album_media (album_id, media_id, position) VALUES (?, ?, ?)",
                    (imag_alb_id, imag_mid, pos)
                )
                album_media_count += 1

    im_conn.commit()
    print(f"  Migrated {album_count} albums containing {album_media_count} media items.")

    im_conn.close()
    pse_conn.close()

    total_time = time.time() - start_time
    print("\n" + "=" * 70)
    print(f" MIGRATION COMPLETED SUCCESSFULLY IN {total_time:.1f} SECONDS!")
    print(f" Output Database: {os.path.abspath(args.imagine_db)}")
    print("=" * 70)

if __name__ == "__main__":
    run_migration()
