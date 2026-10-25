#!/usr/bin/env python3
"""
Unit and integration tests for tools/update_gps_places.py
"""

import os
import sys
import tempfile
import sqlite3
import pytest
from tools.update_gps_places import SpatialCityIndex, haversine_km, update_places, resolve_candidate_tags

TEST_CSV_CONTENT = """name,admin1,country_code,country_name,latitude,longitude,population
Berlin,Berlin,DE,Germany,52.52000,13.40500,3645000
Potsdam,Brandenburg,DE,Germany,52.39886,13.06566,180000
Aachen,North Rhine-Westphalia,DE,Germany,50.77664,6.08342,249000
Pula,Istria,HR,Croatia,44.86833,13.84806,59000
"""


def create_test_db(db_path: str):
    conn = sqlite3.connect(db_path)
    cur = conn.cursor()
    cur.executescript("""
        CREATE TABLE media_items (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            file_path TEXT NOT NULL,
            has_gps INTEGER DEFAULT 0,
            latitude REAL,
            longitude REAL
        );

        CREATE TABLE tags (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            category TEXT NOT NULL,
            UNIQUE(name, category)
        );

        CREATE TABLE media_tags (
            media_id INTEGER NOT NULL REFERENCES media_items(id) ON DELETE CASCADE,
            tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
            PRIMARY KEY(media_id, tag_id)
        );

        CREATE TABLE media_tag_provenance (
            media_id INTEGER NOT NULL,
            tag_id INTEGER NOT NULL,
            manual INTEGER NOT NULL DEFAULT 1,
            source TEXT NOT NULL DEFAULT 'manual',
            PRIMARY KEY (media_id, tag_id)
        );
    """)
    conn.commit()
    conn.close()


def test_haversine_distance():
    # Berlin to Potsdam is approx 27 km
    dist = haversine_km(52.52000, 13.40500, 52.39886, 13.06566)
    assert 25.0 < dist < 30.0


def test_spatial_index_lookup():
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", delete=False, suffix=".csv") as f:
        f.write(TEST_CSV_CONTENT)
        csv_path = f.name

    try:
        index = SpatialCityIndex(cell_size_deg=1.0)
        index.load_csv(csv_path)

        # Lookup near Berlin (e.g. 52.51, 13.40)
        res = index.lookup(52.51, 13.40, max_dist_km=50.0)
        assert res is not None
        assert res["city"] == "Berlin"
        assert res["country"] == "Germany"

        # Lookup point in ocean / faraway (>100 km from anything in test CSV)
        res_ocean = index.lookup(0.0, 0.0, max_dist_km=100.0)
        assert res_ocean is None
    finally:
        os.remove(csv_path)


def test_update_places_missing_only():
    with tempfile.TemporaryDirectory() as tmpdir:
        db_path = os.path.join(tmpdir, "test_catalog.db")
        csv_path = os.path.join(tmpdir, "cities.csv")
        with open(csv_path, "w", encoding="utf-8") as f:
            f.write(TEST_CSV_CONTENT)

        create_test_db(db_path)
        conn = sqlite3.connect(db_path)
        cur = conn.cursor()

        # Item 1: Untagged photo in Berlin
        cur.execute("INSERT INTO media_items (file_path, has_gps, latitude, longitude) VALUES ('berlin.jpg', 1, 52.52, 13.405)")
        # Item 2: Photo in Aachen that already has manual tag "Aachen"
        cur.execute("INSERT INTO media_items (file_path, has_gps, latitude, longitude) VALUES ('aachen.jpg', 1, 50.77, 6.08)")
        cur.execute("INSERT INTO tags (name, category) VALUES ('Aachen', 'places')")
        cur.execute("INSERT INTO media_tags (media_id, tag_id) VALUES (2, 1)")
        cur.execute("INSERT INTO media_tag_provenance (media_id, tag_id, manual, source) VALUES (2, 1, 1, 'manual')")
        conn.commit()
        conn.close()

        # Run update with missing_only=True
        ret = update_places(
            catalog_path=db_path,
            geonames_csv=csv_path,
            missing_only=True,
            dry_run=False,
            no_backup=True,
        )
        assert ret == 0

        # Check results
        conn = sqlite3.connect(db_path)
        cur = conn.cursor()

        # Item 1 should now have Berlin place tags
        item1_tags = cur.execute("""
            SELECT t.name, p.source, p.manual
            FROM media_tags mt
            JOIN tags t ON mt.tag_id = t.id
            JOIN media_tag_provenance p ON mt.media_id = p.media_id AND mt.tag_id = p.tag_id
            WHERE mt.media_id = 1
        """).fetchall()
        assert len(item1_tags) >= 2  # Berlin, Germany
        for t_name, src, man in item1_tags:
            assert src == "gps"
            assert man == 0

        # Item 2 should have ONLY the single manual tag "Aachen" because missing_only=True skipped it
        item2_tags = cur.execute("""
            SELECT t.name, p.source, p.manual
            FROM media_tags mt
            JOIN tags t ON mt.tag_id = t.id
            JOIN media_tag_provenance p ON mt.media_id = p.media_id AND mt.tag_id = p.tag_id
            WHERE mt.media_id = 2
        """).fetchall()
        assert len(item2_tags) == 1
        assert item2_tags[0][0] == "Aachen"
        assert item2_tags[0][1] == "manual"
        assert item2_tags[0][2] == 1

        conn.close()


def test_update_places_dry_run():
    with tempfile.TemporaryDirectory() as tmpdir:
        db_path = os.path.join(tmpdir, "test_catalog.db")
        csv_path = os.path.join(tmpdir, "cities.csv")
        with open(csv_path, "w", encoding="utf-8") as f:
            f.write(TEST_CSV_CONTENT)

        create_test_db(db_path)
        conn = sqlite3.connect(db_path)
        cur = conn.cursor()
        cur.execute("INSERT INTO media_items (file_path, has_gps, latitude, longitude) VALUES ('berlin.jpg', 1, 52.52, 13.405)")
        conn.commit()
        conn.close()

        # Run dry run
        ret = update_places(
            catalog_path=db_path,
            geonames_csv=csv_path,
            missing_only=True,
            dry_run=True,
            no_backup=True,
        )
        assert ret == 0

        # Verify database was NOT modified
        conn = sqlite3.connect(db_path)
        cur = conn.cursor()
        count = cur.execute("SELECT count(*) FROM media_tags").fetchone()[0]
        assert count == 0
        conn.close()
