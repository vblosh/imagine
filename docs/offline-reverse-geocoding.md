# Offline Reverse Geocoding

Imagine labels photos with a city, region, and country using a bundled city
dataset. Lookups run entirely on the local machine: no network request is made
and EXIF coordinates never leave the computer.

## Dataset

- File: `data/geonames_cities15000.csv`
- Source: [GeoNames `cities15000`](https://download.geonames.org/export/dump/)
- License: [Creative Commons Attribution 4.0](https://creativecommons.org/licenses/by/4.0/)
- Columns: `name,admin1,country_code,country_name,latitude,longitude,population`

This is the **full GeoNames `cities15000` extract: 34,180 populated places in
244 countries and territories** (snapshot 2026-10-24). Every row of the dump is
kept, so coverage is global down to places of roughly 15,000 inhabitants (plus
capitals and administrative seats below that threshold). A few very remote
locations may still have no city within the 100 km cutoff and therefore
receive no place tags - that is the intended behaviour, not an error. The CSV
ships with the application and is loaded lazily on the first
`applyGpsPlaceTags` call with valid GPS, not at startup. The app remains fully
functional without it (place labelling is skipped, everything else works). Set
`IMAGINE_GEONAMES_CSV` to override the dataset path for testing or custom
data.

## Runtime discovery & manual testing

The dataset is **not** opened at startup. The first call to
`applyGpsPlaceTags` with valid GPS triggers a one-time lazy load
(`CatalogDb::ensureGeocoderLoaded`). Discovery happens at that moment, using
`findGeonamesDataset` in this exact order:

1. `IMAGINE_GEONAMES_CSV`, if set and non-empty. Its value is used verbatim as
   the path (no search, no existence check at discovery time).
2. `geonames_cities15000.csv` beside the server executable.
3. `data/geonames_cities15000.csv` beside the server executable.
4. `geonames_cities15000.csv` in the current working directory.
5. `data/geonames_cities15000.csv` in the current working directory.

The first existing regular file wins. Steps 2–5 are skipped entirely when the
environment variable is set, even if its path is wrong. If the resolved file is
missing or fails to parse, place tagging is disabled for the lifetime of the
process: the load attempt — including a negative outcome — is cached in
`CatalogDb` (`geocoderLoadAttempted_` / `geocoderAvailable_`). Adding, fixing,
or moving the CSV later requires restarting the server.

When the geocoder is unavailable, `applyGpsPlaceTags` skips the tag update and
still returns success. The detailed load warning appears once; the per-photo
"Skipping GPS place tag update" warning is throttled to at most once every
60 seconds. No warning is emitted for each affected photo.

At server startup, the resolved path is logged once:

```
GeoNames dataset: C:\path\to\geonames_cities15000.csv
```

If nothing is found, startup logs a warning instead ("GeoNames dataset not
found ..."). This line is diagnostic only; the file is still loaded lazily on
the first GPS place-tag update.

### Manual test recipe

1. Copy `data/geonames_cities15000.csv` anywhere, for example
   `C:\temp\geonames_cities15000.csv`.
2. Point `IMAGINE_GEONAMES_CSV` at the copy:
   - PowerShell: `$env:IMAGINE_GEONAMES_CSV = "C:\temp\geonames_cities15000.csv"`
   - POSIX shell: `export IMAGINE_GEONAMES_CSV=/tmp/geonames_cities15000.csv`
3. Restart the server (required: discovery and the cached load outcome happen
   once per process).
4. Check the startup log for `GeoNames dataset: <your path>`.
5. Import a photo with GPS, or set GPS on an existing photo, and verify the
   `places` tags appear.

To test the fallback search, unset the variable, place the CSV beside the
executable (or in `data/` there), restart, and confirm the logged path.

## Lookup rule

For a photo's GPS coordinates the nearest city is found using the great-circle
(Haversine) distance. A match is accepted only when it is within
`ReverseGeocoder::kMaxDistanceKm` (**100 km**). Invalid coordinates (outside
-90..90 latitude / -180..180 longitude, or non-finite) and points with no city
inside the cutoff produce no place result and therefore no place tags.

## Triggers

Place tags are refreshed whenever GPS coordinates are established:

- **Import**: a newly imported photo whose EXIF/metadata contains GPS.
- **Set GPS**: the user sets or changes GPS coordinates for a photo
  (`Catalog::setGps`).

Both paths call `CatalogDb::applyGpsPlaceTags(mediaId, hasGps, latitude,
longitude)`. When GPS is cleared (`hasGps == false`) the previously generated
GPS place tags are removed. Updating GPS replaces the previous GPS-derived
place tags.

## Tag hierarchy and provenance

Place tags use the `places` category and describe the match at up to three
levels:

```
Country  (e.g. Germany)
Region   (admin1, e.g. Brandenburg)
City     (e.g. Potsdam)
```

Empty names and duplicate names are skipped, so a match can produce fewer than
three tags. For example, Berlin's city and region are both named Berlin, so it
adds two place tags: City/Region **Berlin** and Country **Germany**.

Every assignment created by reverse geocoding is recorded in
`media_tag_provenance` with `source = 'gps'` and `manual = 0`. Tags are shared
across photos; provenance is per media item and tag.

## Preservation of manual tags

Reverse geocoding only owns rows whose provenance source is `gps`. Refreshing
or clearing place tags never removes a tag the user assigned manually
(`source = 'manual'`), even when it has the same name. Manual tags therefore
survive re-import, GPS edits, and GPS clearing. If a user manually assigns a
tag that also exists as a GPS tag, the manual assignment takes precedence.

## Offline behaviour

All processing is local: dataset loading, distance calculation, and tag writes
happen on-device. There is no geocoding API, no background network call, and no
dependency on a developer key. Tests for the geocoder use a small temporary CSV
and do not access the network.
