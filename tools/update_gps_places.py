#!/usr/bin/env python3
"""
Imagine Photo Organizer — Offline Reverse Geocoding Tool
Updates and backfills GPS-to-places geotagging for an existing Imagine catalog database.

Usage:
  python tools/update_gps_places.py [options]

Options:
  --catalog <path>      Path to SQLite catalog database (default: catalog.db or IMAGINE_CATALOG)
  --geonames-csv <path> Path to geonames_cities15000.csv
  --missing-only        Only tag media with zero place tags (default: True)
  --all                 Tag all media with GPS coordinates, complementing missing levels
  --dry-run             Simulate without modifying the database
  --no-backup           Skip automated database backup before writing
  --cutoff-km <N>       Maximum distance cutoff in km (default: 100.0)
  --batch-size <N>      Items per transaction commit (default: 1000)
  -h, --help            Show this help message
"""

import sys
import os
import csv
import math
import time
import shutil
import sqlite3
import argparse
from datetime import datetime
from collections import defaultdict

if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass

# WGS84 Constants
EARTH_RADIUS_KM = 6371.0088


def haversine_km(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    phi1 = math.radians(lat1)
    phi2 = math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlam = math.radians(lon2 - lon1)
    h = math.sin(dphi / 2.0) ** 2 + math.cos(phi1) * math.cos(phi2) * math.sin(dlam / 2.0) ** 2
    h = min(1.0, max(0.0, h))
    return 2.0 * EARTH_RADIUS_KM * math.asin(math.sqrt(h))


class SpatialCityIndex:
    """Fast spatial 1x1 degree grid index for reverse geocoding lookups."""

    def __init__(self, cell_size_deg: float = 1.0):
        self.cell_size = cell_size_deg
        self.grid = defaultdict(list)
        self.cities = []

    def load_csv(self, csv_path: str):
        with open(csv_path, mode="r", encoding="utf-8-sig") as f:
            reader = csv.DictReader(f)
            for row in reader:
                try:
                    lat = float(row["latitude"])
                    lon = float(row["longitude"])
                except (ValueError, KeyError):
                    continue

                name = row.get("name", "").strip()
                admin1 = row.get("admin1", "").strip()
                country = row.get("country_name", "").strip()
                country_code = row.get("country_code", "").strip()

                if not name or not (-90.0 <= lat <= 90.0) or not (-180.0 <= lon <= 180.0):
                    continue

                city_entry = {
                    "city": name,
                    "admin1": admin1,
                    "country": country,
                    "country_code": country_code,
                    "latitude": lat,
                    "longitude": lon,
                }
                self.cities.append(city_entry)

                c_lat = int(math.floor(lat / self.cell_size))
                c_lon = int(math.floor(lon / self.cell_size))
                self.grid[(c_lat, c_lon)].append(city_entry)

    def lookup(self, lat: float, lon: float, max_dist_km: float = 100.0):
        if not (-90.0 <= lat <= 90.0) or not (-180.0 <= lon <= 180.0):
            return None

        # 1 deg latitude is approx 111.2 km.
        lat_span = int(math.ceil(max_dist_km / 111.0 / self.cell_size))
        
        # Longitude degree span depends on latitude
        cos_lat = math.cos(math.radians(lat))
        if abs(cos_lat) > 1e-4:
            lon_span = int(math.ceil(max_dist_km / (111.0 * abs(cos_lat)) / self.cell_size))
        else:
            lon_span = int(math.ceil(180.0 / self.cell_size))

        c_lat = int(math.floor(lat / self.cell_size))
        c_lon = int(math.floor(lon / self.cell_size))

        best_city = None
        min_dist = max_dist_km

        for d_lat in range(-lat_span, lat_span + 1):
            cell_lat = c_lat + d_lat
            for d_lon in range(-lon_span, lon_span + 1):
                cell_lon = c_lon + d_lon
                # Wrap longitude around [-180, 180]
                norm_lon = cell_lon
                while norm_lon * self.cell_size < -180.0:
                    norm_lon += int(360.0 / self.cell_size)
                while norm_lon * self.cell_size >= 180.0:
                    norm_lon -= int(360.0 / self.cell_size)

                for candidate in self.grid.get((cell_lat, norm_lon), []):
                    dist = haversine_km(lat, lon, candidate["latitude"], candidate["longitude"])
                    if dist <= min_dist:
                        min_dist = dist
                        best_city = candidate

        if best_city:
            return {
                "city": best_city["city"],
                "admin1": best_city["admin1"],
                "country": best_city["country"],
                "country_code": best_city["country_code"],
                "distance_km": min_dist,
            }
        return None


def find_geonames_csv(catalog_path: str, user_csv: str = None) -> str:
    if user_csv and os.path.isfile(user_csv):
        return user_csv

    env_path = os.getenv("IMAGINE_GEONAMES_CSV")
    if env_path and os.path.isfile(env_path):
        return env_path

    # Check beside catalog DB
    cat_dir = os.path.dirname(os.path.abspath(catalog_path))
    candidates = [
        os.path.join(cat_dir, "geonames_cities15000.csv"),
        os.path.join(cat_dir, "data", "geonames_cities15000.csv"),
        os.path.join("data", "geonames_cities15000.csv"),
        "geonames_cities15000.csv",
    ]
    for c in candidates:
        if os.path.isfile(c):
            return os.path.abspath(c)
    return ""


def resolve_candidate_tags(place_info: dict) -> list:
    """Returns unique, non-empty tag names in order: City, Admin1, Country."""
    names = []
    for key in ("city", "admin1", "country"):
        val = (place_info.get(key) or "").strip()
        if val and val not in names:
            names.append(val)
    return names


def update_places(
    catalog_path: str,
    geonames_csv: str,
    missing_only: bool = True,
    dry_run: bool = False,
    no_backup: bool = False,
    cutoff_km: float = 100.0,
    batch_size: int = 1000,
):
    print("=" * 75)
    print(" IMAGINE MEDIA ORGANIZER — GPS TO PLACES GEOTAGGING TOOL")
    print("=" * 75)
    print(f" Catalog Database: {catalog_path}")
    print(f" GeoNames Dataset: {geonames_csv}")
    print(f" Mode:             {'--missing-only (untagged photos only)' if missing_only else '--all (all photos with GPS)'}")
    print(f" Dry Run:          {dry_run}")
    print(f" Cutoff Distance:  {cutoff_km} km")
    print(f" Batch Size:       {batch_size} items")
    print("=" * 75)

    if not os.path.isfile(catalog_path):
        print(f"Error: Catalog file not found: {catalog_path}", file=sys.stderr)
        return 1

    if not os.path.isfile(geonames_csv):
        print(f"Error: GeoNames CSV not found: {geonames_csv}", file=sys.stderr)
        return 1

    # Automated backup before any writes
    if not dry_run and not no_backup:
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        backup_path = f"{catalog_path}.backup_{timestamp}"
        print(f"\n[1/5] Creating database backup...")
        try:
            shutil.copy2(catalog_path, backup_path)
            backup_size = os.path.getsize(backup_path) / (1024 * 1024)
            print(f"  Backup created successfully: {backup_path} ({backup_size:.1f} MB)")
        except Exception as e:
            print(f"Error: Failed to create database backup: {e}", file=sys.stderr)
            return 1
    elif dry_run:
        print("\n[1/5] Skipping database backup (dry-run mode).")
    else:
        print("\n[1/5] Skipping database backup (--no-backup specified).")

    # Load Spatial Index
    print("\n[2/5] Loading GeoNames dataset into spatial index...")
    t0 = time.time()
    spatial_index = SpatialCityIndex(cell_size_deg=1.0)
    spatial_index.load_csv(geonames_csv)
    t_load = time.time() - t0
    print(f"  Loaded {len(spatial_index.cities):,} populated places in {t_load:.2f}s.")

    # Connect to database
    print("\n[3/5] Querying candidate media items...")
    conn = sqlite3.connect(catalog_path)
    conn.row_factory = sqlite3.Row
    cur = conn.cursor()

    # Database totals for audit
    total_media = cur.execute("SELECT count(*) FROM media_items").fetchone()[0]
    total_gps = cur.execute(
        "SELECT count(*) FROM media_items WHERE has_gps = 1 AND (latitude != 0.0 OR longitude != 0.0)"
    ).fetchone()[0]
    total_tagged_places = cur.execute("""
        SELECT count(DISTINCT mt.media_id)
        FROM media_tags mt
        JOIN tags t ON mt.tag_id = t.id
        WHERE t.category = 'places'
    """).fetchone()[0]
    gps_with_no_places = cur.execute("""
        SELECT count(DISTINCT m.id)
        FROM media_items m
        WHERE m.has_gps = 1 AND (m.latitude != 0.0 OR m.longitude != 0.0)
        AND NOT EXISTS (
            SELECT 1 FROM media_tags mt
            JOIN tags t ON mt.tag_id = t.id
            WHERE mt.media_id = m.id AND t.category = 'places'
        )
    """).fetchone()[0]

    print(f"  Total media items in catalog:         {total_media:,}")
    print(f"  Total media with valid GPS:           {total_gps:,}")
    print(f"  Total media with existing place tags: {total_tagged_places:,}")
    print(f"  Media with GPS and NO place tags:     {gps_with_no_places:,}")

    # Build candidates query
    if missing_only:
        query = """
            SELECT m.id, m.file_path, m.latitude, m.longitude
            FROM media_items m
            WHERE m.has_gps = 1 AND (m.latitude != 0.0 OR m.longitude != 0.0)
            AND NOT EXISTS (
                SELECT 1 FROM media_tags mt
                JOIN tags t ON mt.tag_id = t.id
                WHERE mt.media_id = m.id AND t.category = 'places'
            )
            ORDER BY m.id
        """
    else:
        query = """
            SELECT m.id, m.file_path, m.latitude, m.longitude
            FROM media_items m
            WHERE m.has_gps = 1 AND (m.latitude != 0.0 OR m.longitude != 0.0)
            ORDER BY m.id
        """

    candidates = cur.execute(query).fetchall()
    total_candidates = len(candidates)
    print(f"  Candidates selected for processing:   {total_candidates:,}")

    if total_candidates == 0:
        print("\nNo candidates to process. All photos already tagged!")
        conn.close()
        return 0

    # Cache existing tags in memory to avoid repeated SELECTs
    print("\n[4/5] Preloading existing place tags...")
    tag_cache = {}  # name -> tag_id
    for r in cur.execute("SELECT id, name FROM tags WHERE category = 'places'").fetchall():
        tag_cache[r["name"]] = r["id"]
    print(f"  Cached {len(tag_cache):,} existing place tags.")

    # Processing loop
    print(f"\n[5/5] Processing {total_candidates:,} photos...")
    coord_cache = {}  # (rounded_lat, rounded_lon) -> place_info
    top_cities = defaultdict(int)
    top_countries = defaultdict(int)

    stats_matched = 0
    stats_no_match = 0
    stats_tags_assigned = 0
    stats_manual_preserved = 0

    t_start = time.time()
    last_print = t_start

    # Prepared statements helpers
    insert_tag_stmt = "INSERT INTO tags (name, category) VALUES (?, 'places')"
    insert_link_stmt = "INSERT OR IGNORE INTO media_tags (media_id, tag_id) VALUES (?, ?)"
    insert_prov_stmt = """
        INSERT INTO media_tag_provenance (media_id, tag_id, manual, source)
        VALUES (?, ?, 0, 'gps')
        ON CONFLICT(media_id, tag_id) DO UPDATE SET manual = 0, source = 'gps'
        WHERE media_tag_provenance.source = 'gps'
    """
    check_prov_stmt = "SELECT manual, source FROM media_tag_provenance WHERE media_id = ? AND tag_id = ?"

    # Begin transaction if writing
    if not dry_run:
        cur.execute("BEGIN TRANSACTION")

    try:
        for idx, item in enumerate(candidates, start=1):
            mid = item["id"]
            lat = float(item["latitude"])
            lon = float(item["longitude"])

            coord_key = (round(lat, 6), round(lon, 6))
            if coord_key in coord_cache:
                place = coord_cache[coord_key]
            else:
                place = spatial_index.lookup(lat, lon, max_dist_km=cutoff_km)
                coord_cache[coord_key] = place

            if place:
                stats_matched += 1
                top_cities[f"{place['city']}, {place['country']}"] += 1
                top_countries[place["country"]] += 1

                names = resolve_candidate_tags(place)

                if not dry_run:
                    tag_ids = []
                    for name in names:
                        if name in tag_cache:
                            tid = tag_cache[name]
                        else:
                            cur.execute(insert_tag_stmt, (name,))
                            tid = cur.lastrowid
                            tag_cache[name] = tid
                        tag_ids.append(tid)

                    for tid in tag_ids:
                        # Check existing provenance
                        prov_row = cur.execute(check_prov_stmt, (mid, tid)).fetchone()
                        if prov_row and prov_row["manual"] == 1:
                            stats_manual_preserved += 1
                            continue

                        cur.execute(insert_link_stmt, (mid, tid))
                        cur.execute(insert_prov_stmt, (mid, tid))
                        stats_tags_assigned += 1
                else:
                    stats_tags_assigned += len(names)
            else:
                stats_no_match += 1

            # Periodic batch commit and progress reporting
            now = time.time()
            if not dry_run and (idx % batch_size == 0):
                conn.commit()
                cur.execute("BEGIN TRANSACTION")

            if (now - last_print >= 0.5) or (idx == total_candidates):
                last_print = now
                pct = (idx / total_candidates) * 100
                elapsed = now - t_start
                rate = idx / elapsed if elapsed > 0 else 0
                eta = (total_candidates - idx) / rate if rate > 0 else 0
                sys.stdout.write(
                    f"\r  [{idx:,}/{total_candidates:,}] {pct:5.1f}% | "
                    f"Matched: {stats_matched:,} | No Match: {stats_no_match:,} | "
                    f"{rate:.0f} items/s | ETA: {eta:.0f}s    "
                )
                sys.stdout.flush()

        if not dry_run:
            conn.commit()
            print("\n  Database transaction committed successfully.")
        else:
            print("\n  Dry run completed. No changes written to database.")

    except Exception as e:
        if not dry_run:
            conn.rollback()
            print(f"\nError encountered during processing: {e}. Rolled back transaction.", file=sys.stderr)
        raise e
    finally:
        conn.close()

    total_time = time.time() - t_start
    print("\n" + "=" * 75)
    print(" SUMMARY OF EXECUTION")
    print("=" * 75)
    print(f" Total candidates processed:      {total_candidates:,}")
    print(f" Successfully matched to a place: {stats_matched:,} ({(stats_matched/total_candidates)*100:.1f}%)")
    print(f" No city within {cutoff_km} km cutoff:    {stats_no_match:,} ({(stats_no_match/total_candidates)*100:.1f}%)")
    print(f" Place tags assigned / linked:    {stats_tags_assigned:,}")
    if stats_manual_preserved > 0:
        print(f" Manual tags preserved intact:    {stats_manual_preserved:,}")
    print(f" Total execution time:            {total_time:.2f}s ({total_candidates/total_time:.0f} items/s)")

    print("\n Top 10 Countries Identified:")
    for country, count in sorted(top_countries.items(), key=lambda x: x[1], reverse=True)[:10]:
        print(f"   - {country:<30s}: {count:,} photos")

    print("\n Top 10 Cities Identified:")
    for city, count in sorted(top_cities.items(), key=lambda x: x[1], reverse=True)[:10]:
        print(f"   - {city:<30s}: {count:,} photos")

    print("=" * 75)
    return 0


def main():
    default_catalog = os.getenv("IMAGINE_CATALOG") or os.getenv("IMAGINE_CATALOG_DB") or "catalog.db"

    parser = argparse.ArgumentParser(
        description="Backfill and update GPS to places geotagging for an Imagine catalog."
    )
    parser.add_argument(
        "--catalog",
        default=default_catalog,
        help=f"Path to SQLite catalog database (default: {default_catalog})",
    )
    parser.add_argument(
        "--geonames-csv",
        default=None,
        help="Path to geonames_cities15000.csv (auto-detected if omitted)",
    )
    parser.add_argument(
        "--missing-only",
        dest="missing_only",
        action="store_true",
        default=True,
        help="Only tag media with zero place tags (default: True)",
    )
    parser.add_argument(
        "--all",
        dest="missing_only",
        action="store_false",
        help="Tag all media with GPS coordinates, complementing missing levels",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Simulate lookups without modifying the database",
    )
    parser.add_argument(
        "--no-backup",
        action="store_true",
        help="Skip automated database backup before writing",
    )
    parser.add_argument(
        "--cutoff-km",
        type=float,
        default=100.0,
        help="Maximum distance cutoff in km (default: 100.0)",
    )
    parser.add_argument(
        "--batch-size",
        type=int,
        default=1000,
        help="Number of items per transaction commit (default: 1000)",
    )

    args = parser.parse_args()

    geonames_path = find_geonames_csv(args.catalog, args.geonames_csv)
    if not geonames_path:
        print(
            "Error: Could not find geonames_cities15000.csv. Please specify --geonames-csv <path> or set IMAGINE_GEONAMES_CSV.",
            file=sys.stderr,
        )
        return 1

    return update_places(
        catalog_path=args.catalog,
        geonames_csv=geonames_path,
        missing_only=args.missing_only,
        dry_run=args.dry_run,
        no_backup=args.no_backup,
        cutoff_km=args.cutoff_km,
        batch_size=args.batch_size,
    )


if __name__ == "__main__":
    sys.exit(main())
