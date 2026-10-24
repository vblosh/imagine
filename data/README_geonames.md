# GeoNames cities15000 extract

`geonames_cities15000.csv` is the **full** offline extract of the
[GeoNames](https://www.geonames.org/) `cities15000` dump. It contains
**34,180 populated places in 244 countries and territories**, with real WGS84
coordinates and the GeoNames population estimate.

## Attribution and licence

This product includes information from **GeoNames**, available at
<https://www.geonames.org/>, and is licensed under the
**Creative Commons Attribution 4.0 International License (CC BY 4.0)**:
<https://creativecommons.org/licenses/by/4.0/>.

GeoNames data is provided "as is" without warranty of any kind. If you
redistribute this file, keep this attribution and licence notice.

## Data snapshot

Retrieved on **2026-10-24** from <https://download.geonames.org/export/dump/>.
Coordinates are in decimal degrees on WGS84 (positive north/east, negative
south/west). Names preserve GeoNames diacritics and spelling (for example
`São Paulo`, `Irákleion`, `Chaniá`).

## Source dump and joins

Source files:

- `cities15000.zip` (`cities15000.txt`) - populated places. GeoNames describes
  this as places with a population of at least 15,000, and it also includes
  national and administrative capitals below that threshold. This dataset
  keeps **every** row from that dump.
- `admin1CodesASCII.txt` - lookup keyed by `CC.admin1` code, providing the
  first-order administrative division name (column 2).
- `countryInfo.txt` - ISO country codes and English country names (column 4).

Processing rules used for this snapshot:

1. Read `cities15000.txt` as UTF-8 tab-separated data without modifying it.
2. For every row, take `name` from field 1, `latitude` from field 4,
   `longitude` from field 5, `country_code` from field 8, `admin1 code` from
   field 10 and `population` from field 14.
3. Join `admin1` with the key `country_code.admin1_code` against
   `admin1CodesASCII.txt`. Leave `admin1` empty when GeoNames has no name for
   that key (for example, some city-states and small territories).
4. Join `country_name` from `countryInfo.txt` by ISO country code.
5. Keep all 34,180 data rows; no per-country selection, population cutoff or
   de-duplication is applied. Population is the value reported by GeoNames,
   as-is.
6. Sort by continent, country name, admin1, name and population for a stable
   diff, then write comma-separated CSV as UTF-8 **without a BOM** with LF
   line endings, quoting fields only when needed.

## Columns

| Column | Description |
| --- | --- |
| `name` | GeoNames preferred place name, with original diacritics (for example `São Paulo`). |
| `admin1` | First-order administrative division name (state, province, region, etc.). Empty when GeoNames has no admin1 name lookup for that place. |
| `country_code` | ISO 3166-1 alpha-2 country code. |
| `country_name` | English country name from GeoNames `countryInfo.txt`. |
| `latitude` | Latitude in decimal degrees, WGS84. |
| `longitude` | Longitude in decimal degrees, WGS84. |
| `population` | GeoNames population estimate for the place, as an integer, unchanged from the dump. |

## Regenerating the file

Requirements: internet access and Python 3 (standard library only).

1. Download the three source files into a temporary directory:

   ```sh
   curl -LO https://download.geonames.org/export/dump/cities15000.zip
   curl -LO https://download.geonames.org/export/dump/admin1CodesASCII.txt
   curl -LO https://download.geonames.org/export/dump/countryInfo.txt
   unzip cities15000.zip cities15000.txt
   ```

2. Parse `cities15000.txt` (UTF-8, tab separated). The columns used here are
   index 0 `geonameid`, 1 `name`, 4 `latitude`, 5 `longitude`, 8 `country code`,
   10 `admin1 code`, and 14 `population`.
3. Build a dictionary from `admin1CodesASCII.txt` keyed by `CC.admin1` (the
   first column), with the second column as the admin1 name. Build a country
   dictionary from `countryInfo.txt` keyed by ISO code (column 0), using
   column 4 for the country name and column 8 for the continent.
4. Emit the seven columns above with Python's `csv.writer`, `lineterminator="\n"`,
   `encoding="utf-8"` and `newline=""` (no BOM). Save as
   `data/geonames_cities15000.csv`.
5. Verify the header, the full data row count, the UTF-8 decoding, the absence
   of a BOM and CRLF, and spot-check known cities and coordinates before
   replacing the checked-in file.

If a newer GeoNames dump is used, the row count may change; update the count in
this README and the snapshot date accordingly.
