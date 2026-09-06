"""
Fixtures and database seeding utilities for IMAGINE deterministic UI tests.
"""

import os
import struct
import hashlib
import sqlite3
from typing import Dict, Any, List


def create_bmp(filepath: str, width: int = 120, height: int = 80, color: tuple = (100, 150, 200)) -> str:
    """Create a minimal valid 24-bit uncompressed BMP image."""
    os.makedirs(os.path.dirname(filepath), exist_ok=True)
    row_padding = (4 - (width * 3) % 4) % 4
    row_size = width * 3 + row_padding
    pixel_data_size = row_size * height
    file_size = 54 + pixel_data_size

    # 14-byte BITMAPFILEHEADER
    bmp_header = struct.pack('<2sIHHI', b'BM', file_size, 0, 0, 54)
    # 40-byte BITMAPINFOHEADER
    dib_header = struct.pack('<IIIHHIIIIII', 40, width, height, 1, 24, 0, pixel_data_size, 2835, 2835, 0, 0)

    b, g, r = color
    pixel_row = bytes([b, g, r] * width) + b'\x00' * row_padding
    pixel_data = pixel_row * height

    with open(filepath, 'wb') as f:
        f.write(bmp_header + dib_header + pixel_data)

    return filepath


def init_schema(conn: sqlite3.Connection) -> None:
    """Initialize SQLite database with IMAGINE v1 schema."""
    conn.executescript("""
        CREATE TABLE IF NOT EXISTS schema_version (version INTEGER PRIMARY KEY);
        INSERT OR IGNORE INTO schema_version (version) VALUES (1);

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
            updated_at INTEGER NOT NULL
        );

        CREATE INDEX IF NOT EXISTS idx_media_hash ON media_items(content_hash);
        CREATE INDEX IF NOT EXISTS idx_media_date ON media_items(date_taken DESC);
        CREATE INDEX IF NOT EXISTS idx_media_rating ON media_items(rating);
        CREATE INDEX IF NOT EXISTS idx_media_flag ON media_items(flag);
        CREATE INDEX IF NOT EXISTS idx_media_camera ON media_items(camera_make, camera_model);

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
    """)


def seed_default_catalog(db_path: str, photos_dir: str) -> List[Dict[str, Any]]:
    """Seed the database with a deterministic dataset of 6 diverse photos."""
    conn = sqlite3.connect(db_path)
    init_schema(conn)

    items_meta = [
        {
            "rel_path": "nature/mountain.bmp",
            "file_name": "mountain.bmp",
            "width": 1920,
            "height": 1080,
            "color": (220, 100, 40),
            "date_taken": 1771165800,  # 2026-02-15 14:30:00 UTC
            "rating": 5,
            "flag": 1,  # Pick
            "camera_make": "Sony",
            "camera_model": "ILCE-7M4",
            "lens": "FE 24-70mm F2.8 GM II",
            "exposure_time": 0.002,  # 1/500s
            "f_number": 2.8,
            "iso": 100,
            "focal_length": 35.0,
            "has_gps": 1,
            "latitude": 46.50000,
            "longitude": 11.35000,
            "altitude": 1500.0,
            "tags": [("Alps", "places"), ("Sunset", "keyword")],
            "albums": ["Best of 2026"]
        },
        {
            "rel_path": "nature/sunset.bmp",
            "file_name": "sunset.bmp",
            "width": 1280,
            "height": 720,
            "color": (50, 80, 240),
            "date_taken": 1770749100,  # 2026-02-10 18:45:00 UTC
            "rating": 4,
            "flag": 0,  # Unflagged
            "camera_make": "Canon",
            "camera_model": "EOS R5",
            "lens": "RF 50mm F1.2L USM",
            "exposure_time": 0.008,  # 1/125s
            "f_number": 1.4,
            "iso": 200,
            "focal_length": 50.0,
            "has_gps": 0,
            "latitude": 0.0,
            "longitude": 0.0,
            "altitude": 0.0,
            "tags": [("Sunset", "keyword")],
            "albums": ["Best of 2026"]
        },
        {
            "rel_path": "nature/beach.bmp",
            "file_name": "beach.bmp",
            "width": 1600,
            "height": 900,
            "color": (210, 200, 60),
            "date_taken": 1768907700,  # 2026-01-20 11:15:00 UTC
            "rating": 3,
            "flag": -1,  # Reject
            "camera_make": "Nikon",
            "camera_model": "Z 8",
            "lens": "NIKKOR Z 24-120mm F4 S",
            "exposure_time": 0.001,  # 1/1000s
            "f_number": 4.0,
            "iso": 64,
            "focal_length": 24.0,
            "has_gps": 1,
            "latitude": 21.30694,
            "longitude": -157.85833,
            "altitude": 5.0,
            "tags": [("Beach", "places")],
            "albums": ["Vacation"]
        },
        {
            "rel_path": "family/birthday.bmp",
            "file_name": "birthday.bmp",
            "width": 1000,
            "height": 750,
            "color": (180, 50, 180),
            "date_taken": 1767628800,  # 2026-01-05 16:00:00 UTC
            "rating": 0,
            "flag": 0,  # Unflagged
            "camera_make": "Sony",
            "camera_model": "ILCE-7M4",
            "lens": "FE 35mm F1.4 GM",
            "exposure_time": 0.0167,  # 1/60s
            "f_number": 1.8,
            "iso": 800,
            "focal_length": 35.0,
            "has_gps": 0,
            "latitude": 0.0,
            "longitude": 0.0,
            "altitude": 0.0,
            "tags": [("Alice", "people"), ("Birthday 2026", "events")],
            "albums": ["Vacation"]
        },
        {
            "rel_path": "family/portrait.bmp",
            "file_name": "portrait.bmp",
            "width": 800,
            "height": 1000,
            "color": (70, 160, 90),
            "date_taken": 1766664000,  # 2025-12-25 12:00:00 UTC
            "rating": 2,
            "flag": 1,  # Pick
            "camera_make": "Canon",
            "camera_model": "EOS R5",
            "lens": "RF 85mm F1.2L USM",
            "exposure_time": 0.005,  # 1/200s
            "f_number": 1.2,
            "iso": 100,
            "focal_length": 85.0,
            "has_gps": 0,
            "latitude": 0.0,
            "longitude": 0.0,
            "altitude": 0.0,
            "tags": [("Bob", "people")],
            "albums": []
        },
        {
            "rel_path": "nature/forest.bmp",
            "file_name": "forest.bmp",
            "width": 1200,
            "height": 800,
            "color": (40, 110, 50),
            "date_taken": 1765359000,  # 2025-12-10 09:30:00 UTC
            "rating": 0,
            "flag": -1,  # Reject
            "camera_make": "Nikon",
            "camera_model": "Z 8",
            "lens": "NIKKOR Z 14-30mm F4 S",
            "exposure_time": 0.0333,  # 1/30s
            "f_number": 8.0,
            "iso": 400,
            "focal_length": 16.0,
            "has_gps": 0,
            "latitude": 0.0,
            "longitude": 0.0,
            "altitude": 0.0,
            "tags": [],
            "albums": []
        }
    ]

    # Pre-create albums
    album_ids = {}
    albums_def = [
        ("Best of 2026", "Top picks of 2026"),
        ("Vacation", "Holiday trip memories")
    ]
    for alb_name, alb_desc in albums_def:
        cur = conn.cursor()
        cur.execute(
            "INSERT INTO albums (name, description, is_smart, query_json, created_at) VALUES (?, ?, 0, '', ?)",
            (alb_name, alb_desc, 1770000000)
        )
        album_ids[alb_name] = cur.lastrowid

    # Pre-create tags
    tag_ids = {}
    all_tags = [
        ("Alice", "people"),
        ("Bob", "people"),
        ("Alps", "places"),
        ("Beach", "places"),
        ("Birthday 2026", "events"),
        ("Sunset", "keyword")
    ]
    for tag_name, tag_cat in all_tags:
        cur = conn.cursor()
        cur.execute("INSERT OR IGNORE INTO tags (name, category) VALUES (?, ?)", (tag_name, tag_cat))
        cur.execute("SELECT id FROM tags WHERE name = ? AND category = ?", (tag_name, tag_cat))
        tag_ids[(tag_name, tag_cat)] = cur.fetchone()[0]

    seeded_records = []

    for meta in items_meta:
        abs_path = os.path.join(photos_dir, meta["rel_path"])
        create_bmp(abs_path, meta["width"], meta["height"], meta["color"])
        file_size = os.path.getsize(abs_path)
        with open(abs_path, "rb") as f:
            chash = hashlib.sha256(f.read()).hexdigest()

        cur = conn.cursor()
        cur.execute("""
            INSERT INTO media_items (
                file_path, file_name, file_size, file_modified_time, content_hash,
                width, height, date_taken, rating, flag,
                camera_make, camera_model, lens, exposure_time, f_number, iso,
                focal_length, has_gps, latitude, longitude, altitude,
                created_at, updated_at
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, (
            abs_path, meta["file_name"], file_size, meta["date_taken"], chash,
            meta["width"], meta["height"], meta["date_taken"], meta["rating"], meta["flag"],
            meta["camera_make"], meta["camera_model"], meta["lens"], meta["exposure_time"],
            meta["f_number"], meta["iso"], meta["focal_length"], meta["has_gps"],
            meta["latitude"], meta["longitude"], meta["altitude"],
            meta["date_taken"], meta["date_taken"]
        ))
        media_id = cur.lastrowid
        meta["id"] = media_id
        meta["file_path"] = abs_path
        meta["content_hash"] = chash
        seeded_records.append(meta)

        # Attach tags
        for t_name, t_cat in meta["tags"]:
            t_id = tag_ids[(t_name, t_cat)]
            cur.execute("INSERT INTO media_tags (media_id, tag_id) VALUES (?, ?)", (media_id, t_id))

        # Attach albums
        for alb_name in meta["albums"]:
            a_id = album_ids[alb_name]
            cur.execute("INSERT INTO album_media (album_id, media_id, position) VALUES (?, ?, 0)", (a_id, media_id))

    conn.commit()
    conn.close()
    return seeded_records


def create_import_photos(import_dir: str) -> List[str]:
    """Create test photos in a target folder to be imported during import tests."""
    os.makedirs(import_dir, exist_ok=True)
    f1 = os.path.join(import_dir, "imported_photo_1.bmp")
    f2 = os.path.join(import_dir, "imported_photo_2.bmp")
    create_bmp(f1, 300, 200, (120, 220, 100))
    create_bmp(f2, 400, 300, (220, 120, 100))
    return [f1, f2]


def create_wav(filepath: str, duration_sec: float = 2.0, sample_rate: int = 44100) -> str:
    """Create a minimal valid 16-bit PCM WAV audio file."""
    os.makedirs(os.path.dirname(filepath), exist_ok=True)
    num_samples = int(duration_sec * sample_rate)
    data_size = num_samples * 2  # 16-bit mono = 2 bytes per sample
    byte_rate = sample_rate * 2
    riff_header = struct.pack('<4sI4s4sIHHIIHH4sI',
        b'RIFF', 36 + data_size, b'WAVE',
        b'fmt ', 16, 1, 1, sample_rate, byte_rate, 2, 16,
        b'data', data_size
    )
    with open(filepath, 'wb') as f:
        f.write(riff_header)
        f.write(b'\x00' * data_size)
    return filepath


def create_mp4(filepath: str, duration_sec: float = 15.0, width: int = 1280, height: int = 720) -> str:
    """Create a minimal valid ISO Base Media MP4 video file with ftyp, mvhd, and tkhd."""
    os.makedirs(os.path.dirname(filepath), exist_ok=True)
    ftyp_data = b'isom\x00\x00\x02\x00isomiso2mp41'
    ftyp = struct.pack('>I4s', 8 + len(ftyp_data), b'ftyp') + ftyp_data

    timescale = 1000
    duration_units = int(duration_sec * timescale)
    mvhd_payload = struct.pack('>BBBBIIII', 0, 0, 0, 0, 0, 0, timescale, duration_units) + (b'\x00' * 80)
    mvhd = struct.pack('>I4s', 8 + len(mvhd_payload), b'mvhd') + mvhd_payload

    tkhd_payload = (
        struct.pack('>BBBBIIIII', 0, 0, 0, 0, 0, 0, 1, 0, duration_units) +
        (b'\x00' * 8) +
        struct.pack('>hhhh', 0, 0, 0, 0) +
        (b'\x00' * 36) +
        struct.pack('>II', width << 16, height << 16)
    )
    tkhd = struct.pack('>I4s', 8 + len(tkhd_payload), b'tkhd') + tkhd_payload
    trak = struct.pack('>I4s', 8 + len(tkhd), b'trak') + tkhd
    moov_payload = mvhd + trak
    moov = struct.pack('>I4s', 8 + len(moov_payload), b'moov') + moov_payload

    with open(filepath, 'wb') as f:
        f.write(ftyp + moov)
    return filepath


def init_v3_schema(conn: sqlite3.Connection) -> None:
    """Initialize or upgrade SQLite database schema to IMAGINE v3."""
    init_schema(conn)
    cols = [
        ("caption", "TEXT DEFAULT ''"),
        ("media_type", "TEXT DEFAULT 'photo'"),
        ("duration", "REAL DEFAULT 0.0"),
        ("audio_artist", "TEXT DEFAULT ''"),
        ("audio_title", "TEXT DEFAULT ''"),
        ("audio_album", "TEXT DEFAULT ''"),
        ("audio_genre", "TEXT DEFAULT ''"),
        ("codec", "TEXT DEFAULT ''"),
        ("bitrate", "INTEGER DEFAULT 0"),
        ("channels", "INTEGER DEFAULT 0"),
        ("sample_rate", "INTEGER DEFAULT 0")
    ]
    for col_name, col_def in cols:
        try:
            conn.execute(f"ALTER TABLE media_items ADD COLUMN {col_name} {col_def};")
        except Exception:
            pass
    conn.execute("INSERT OR REPLACE INTO schema_version (version) VALUES (3);")
    conn.commit()


init_v2_schema = init_v3_schema


def seed_media_catalog(db_path: str, photos_dir: str) -> List[Dict[str, Any]]:
    """Seed catalog with 6 photos plus 1 video and 1 audio file."""
    records = seed_default_catalog(db_path, photos_dir)
    conn = sqlite3.connect(db_path)
    init_v3_schema(conn)

    # 1. Video item
    video_rel = "media/drone_flight.mp4"
    video_abs = os.path.join(photos_dir, video_rel)
    create_mp4(video_abs, duration_sec=15.0, width=1920, height=1080)
    v_size = os.path.getsize(video_abs)
    with open(video_abs, "rb") as f:
        v_hash = hashlib.sha256(f.read()).hexdigest()

    cur = conn.cursor()
    cur.execute("""
        INSERT INTO media_items (
            file_path, file_name, file_size, file_modified_time, content_hash,
            width, height, date_taken, rating, flag, media_type, duration, codec,
            created_at, updated_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, (
        video_abs, "drone_flight.mp4", v_size, 1771200000, v_hash,
        1920, 1080, 1771200000, 4, 1, "video", 15.0, "h264",
        1771200000, 1771200000
    ))
    v_id = cur.lastrowid
    records.append({
        "id": v_id,
        "rel_path": video_rel,
        "file_name": "drone_flight.mp4",
        "file_path": video_abs,
        "content_hash": v_hash,
        "media_type": "video",
        "duration": 15.0,
        "width": 1920,
        "height": 1080,
        "codec": "h264"
    })

    # 2. Audio item
    audio_rel = "media/ambient_track.wav"
    audio_abs = os.path.join(photos_dir, audio_rel)
    create_wav(audio_abs, duration_sec=45.0, sample_rate=44100)
    a_size = os.path.getsize(audio_abs)
    with open(audio_abs, "rb") as f:
        a_hash = hashlib.sha256(f.read()).hexdigest()

    cur.execute("""
        INSERT INTO media_items (
            file_path, file_name, file_size, file_modified_time, content_hash,
            width, height, date_taken, rating, flag, media_type, duration,
            audio_artist, audio_title, audio_album, audio_genre, codec, channels, sample_rate,
            created_at, updated_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, (
        audio_abs, "ambient_track.wav", a_size, 1771250000, a_hash,
        0, 0, 1771250000, 5, 0, "audio", 45.0,
        "SynthWave Artist", "Night Drive", "Neon City", "Electronic", "pcm", 1, 44100,
        1771250000, 1771250000
    ))
    a_id = cur.lastrowid
    records.append({
        "id": a_id,
        "rel_path": audio_rel,
        "file_name": "ambient_track.wav",
        "file_path": audio_abs,
        "content_hash": a_hash,
        "media_type": "audio",
        "duration": 45.0,
        "audio_artist": "SynthWave Artist",
        "audio_title": "Night Drive",
        "audio_album": "Neon City",
        "codec": "pcm"
    })

    conn.commit()
    conn.close()
    return records
