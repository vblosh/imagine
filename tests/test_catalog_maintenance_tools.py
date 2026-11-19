import sqlite3
import sys
from contextlib import closing

import pytest

from tools import fix_orientations, migrate_pse


def test_pse_migration_includes_readable_volumes_and_pre_epoch_dates(tmp_path, monkeypatch):
    source_path = tmp_path / "source.pse20db"
    target_path = tmp_path / "target.db"
    with closing(sqlite3.connect(source_path)) as source:
        source.executescript("""
            CREATE TABLE volume_table(id, description, drive_path_if_builtin, type);
            CREATE TABLE tag_table(id, name, parent_id, type_name);
            CREATE TABLE media_table(id, volume_id, full_filepath, search_date_begin);
            CREATE TABLE media_to_metadata_table(media_id, metadata_id);
            CREATE TABLE metadata_integer_table(id, description_id, value);
            CREATE TABLE metadata_string_table(id, description_id, value);
            CREATE TABLE metadata_decimal_table(id, description_id, value);
            CREATE TABLE metadata_description_table(id, identifier);
            CREATE TABLE tag_to_media_table(media_id, tag_id, media_index);
        """)
        for volume in (2, 184):
            name = f"photo-{volume}.jpg"
            (tmp_path / name).write_bytes(b"photo data")
            source.execute("INSERT INTO volume_table VALUES (?, 'Photos', ?, 'builtin')", (volume, str(tmp_path)))
            source.execute(
                "INSERT INTO media_table VALUES (?, ?, ?, '19650102T120000')",
                (volume, volume, "/" + name),
            )
        source.commit()

    monkeypatch.setattr(sys, "argv", [
        "migrate_pse", "--pse-db", str(source_path), "--imagine-db", str(target_path),
        "--photos-dir", str(tmp_path), "--quick-hash",
    ])
    migrate_pse.run_migration()
    with closing(sqlite3.connect(target_path)) as target:
        rows = target.execute("SELECT file_name, date_taken, date_taken_str FROM media_items ORDER BY file_name").fetchall()
    assert [row[0] for row in rows] == ["photo-184.jpg", "photo-2.jpg"]
    assert all(epoch < 0 and text == "1965-01-02 12:00:00" for _, epoch, text in rows)


def test_orientation_repair_skips_unreadable_files_and_invalidates_analysis(tmp_path):
    image_module = pytest.importorskip("PIL.Image")
    db_path = tmp_path / "catalog.db"
    cache_path = tmp_path / "cache"
    content_hash = "abcd" + "0" * 60
    photo = image_module.new("RGB", (12, 8))
    exif = image_module.Exif()
    exif[274] = 6
    photo.save(tmp_path / "readable.jpg", exif=exif)
    photo.save(tmp_path / "video.jpg")
    (tmp_path / "corrupt.jpg").write_text("not an image", encoding="utf-8")
    thumbnail = cache_path / "ab" / "cd" / (content_hash + "_768.jpg")
    thumbnail.parent.mkdir(parents=True)
    thumbnail.touch()
    with closing(sqlite3.connect(db_path)) as catalog:
        catalog.executescript("""
            CREATE TABLE media_items(id, file_path, content_hash, orientation, updated_at, media_type);
            CREATE TABLE faces(media_id);
            CREATE TABLE face_media_analysis(media_id);
            CREATE TABLE semantic_index_state(media_id);
            CREATE TABLE smart_tag_state(media_id);
            CREATE TABLE media_tags(media_id, tag_id);
            CREATE TABLE media_tag_provenance(media_id, tag_id, manual, source);
            INSERT INTO media_items VALUES(2, 'video.jpg', 'video', 8, 0, 'video');
            INSERT INTO media_items VALUES(3, 'offline.jpg', 'offline', 8, 0, 'photo');
            INSERT INTO media_items VALUES(4, 'corrupt.jpg', 'corrupt', 8, 0, 'photo');
        """)
        catalog.execute(
            "INSERT INTO media_items VALUES(1, 'readable.jpg', ?, 1, 0, 'photo')",
            (content_hash,),
        )
        for table in ("faces", "face_media_analysis", "semantic_index_state", "smart_tag_state"):
            catalog.executemany(f"INSERT INTO {table} VALUES (?)", [(1,), (2,)])
        for tag_id, manual, source in ((1, 0, "clip"), (2, 0, "gps"), (3, 1, "manual")):
            catalog.execute("INSERT INTO media_tags VALUES(1, ?)", (tag_id,))
            catalog.execute("INSERT INTO media_tag_provenance VALUES(1, ?, ?, ?)", (tag_id, manual, source))
        catalog.commit()

        assert fix_orientations.main([
            "--catalog", str(db_path), "--photos-dir", str(tmp_path), "--cache-dir", str(cache_path),
        ]) == 0
        assert catalog.execute("SELECT orientation FROM media_items ORDER BY id").fetchall() == [(6,), (8,), (8,), (8,)]
        for table in ("faces", "face_media_analysis", "semantic_index_state", "smart_tag_state"):
            assert catalog.execute(f"SELECT media_id FROM {table}").fetchall() == [(2,)]
        assert catalog.execute("SELECT tag_id FROM media_tags ORDER BY tag_id").fetchall() == [(2,), (3,)]
    assert not thumbnail.exists()
