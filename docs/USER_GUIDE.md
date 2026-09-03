# Imagine User Guide & Manual

Welcome to **Imagine**, a high-performance photo catalog and organizer inspired by **Adobe Photoshop Elements Organizer**.

Imagine combines a high-speed C++20 core engine with an interactive Command-Line Interface (CLI) and an embedded, responsive Web UI that runs locally on any browser without native GUI dependencies.

---

## Table of Contents
1. [System Requirements & Installation](#1-system-requirements--installation)
2. [Quick Start Tutorial](#2-quick-start-tutorial)
3. [Importing Photos](#3-importing-photos)
4. [Browsing & Navigation](#4-browsing--navigation)
5. [Organizing & Tagging](#5-organizing--tagging)
6. [Fullscreen Loupe Viewer](#6-fullscreen-loupe-viewer)
7. [Inspector & EXIF Metadata](#7-inspector--exif-metadata)
8. [CLI Reference](#8-cli-reference)
9. [REST API Reference](#9-rest-api-reference)
10. [Under the Hood: Database & Cache](#10-under-the-hood-database--cache)
11. [Troubleshooting & FAQ](#11-troubleshooting--faq)

---

## 1. System Requirements & Installation

### Prerequisites
- **Operating System**: Linux (Ubuntu 22.04+, Debian, Fedora, Arch) or Windows Subsystem for Linux (WSL2).
- **Compiler**: GCC 11+ or Clang 14+ with C++20 support.
- **Build Tools**: CMake 3.20+ and Ninja (or Make).
- **Libraries**: OpenSSL development headers (`libssl-dev`) and pthreads (standard on Linux).

> [!NOTE]
> All other libraries (`sqlite3`, `stb`, `easyexif`, `cpp-httplib`, `nlohmann_json`, `googletest`) are fetched and built automatically via CMake `FetchContent`. No root permissions or external package installations are needed.

### Building from Source

1. Clone or navigate to the project directory:
   ```bash
   cd ~/projects/imagine
   ```

2. Configure and build with CMake:
   ```bash
   cmake -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build -j$(nproc)
   ```

3. (Optional) Run the automated test suite:
   ```bash
   ctest --test-dir build --output-on-failure
   ```
   This runs both the C++ unit test suite (`imagine_unit_tests`) and the Playwright End-to-End UI test suite (`imagine_ui_tests`).
   
   To run UI tests directly:
   ```bash
   ./tests/ui/run_ui_tests.sh
   # Or with headed browser:
   ./tests/ui/run_ui_tests.sh --headed
   ```

---

## 2. Quick Start Tutorial

Get your photo collection organized in three simple steps:

### Step 1: Import Photos
Import your existing folders of photos into an Imagine catalog:
```bash
./build/imagine import /path/to/my/photos --catalog my_catalog.db
```

### Step 2: Launch the Web Organizer
Start the local server:
```bash
./build/imagine serve --catalog my_catalog.db --port 8080
```

### Step 3: Open the Browser
Open [http://localhost:8080](http://localhost:8080) in Chrome, Firefox, Safari, or Edge. You will be greeted with the Photoshop Elements Organizer dark-room workspace!

---

## 3. Importing Photos

Imagine includes an asynchronous, multi-threaded importer capable of scanning thousands of photos per second.

### Supported File Formats
- JPEG (`.jpg`, `.jpeg`)
- PNG (`.png`)
- BMP (`.bmp`)
- WebP (`.webp`)
- TIFF (`.tiff`, `.tif`)

### Importing via the Web UI
1. Click the blue **"Import Folder"** button in the top navigation bar.
2. Enter the absolute directory path on your computer (e.g., `/home/username/Pictures`).
3. Ensure **"Scan Subdirectories Recursively"** is checked if you have nested folders.
4. Click **"Start Import"**.
5. Watch the live progress bar: Imagine will report scanned files, processed items, skipped duplicates, and current file in real-time.

```
+-------------------------------------------------------------+
| Import Photos to Catalog                                [X] |
|-------------------------------------------------------------|
| Directory Path on Server:                                   |
| [/home/slava/Pictures/Vacation2026                        ] |
|                                                             |
| [X] Scan Subdirectories Recursively                         |
|                                                             |
| [========================>               ] 68%              |
| Processed: 340 / 500 | Imported: 320 | Skipped: 20          |
| Current: IMG_20260903_142311.jpg                           |
|-------------------------------------------------------------|
|                             [ Cancel ]  [ Start Import ]    |
+-------------------------------------------------------------+
```

### Importing via CLI
Use the `imagine import` command:
```bash
./build/imagine import ~/Pictures/2026 \
    --catalog my_catalog.db \
    --thumbs ~/.cache/imagine/thumbs \
    --threads 8
```

### Deduplication & Change Detection
- **Fast Rescan**: If you run import on a folder that has already been indexed, Imagine inspects file modification times (`mtime`) and skips unchanged photos in **0 milliseconds**.
- **SHA-256 Hashing**: Each imported photo receives a cryptographic content hash. If you move a file to a new folder or import duplicates, Imagine recognizes the content hash and prevents duplicate thumbnail generation.

---

## 4. Browsing & Navigation

### Views (Top Tabs)
- **Media**: The master view displaying all photos in your catalog, ordered chronologically.
- **People**: Shows photos tagged under the *People* category.
- **Places**: Shows photos tagged with geographic locations or containing GPS metadata.
- **Events**: Shows photos organized into specific events (e.g., *Birthdays*, *Conferences*, *Holidays*).

### Thumbnail Zoom Slider
In the top right bar, slide the thumbnail zoom controller:
- **Left (120px)**: Compact contact sheet view displaying dozens of photos per screen.
- **Right (360px)**: Large preview cards showing rich detail, star ratings, flags, and titles.

### The Left Navigation Sidebar
- **Quick Filters**:
  - **All Media**: Resets all filters and shows the entire catalog.
  - **Picks**: Displays only photos marked with the green Pick flag.
  - **Rejects**: Displays photos marked with the red Reject flag (ideal for cleaning up bad shots).
  - **Unrated**: Displays photos that haven't received star ratings yet.
- **Albums**: Custom user collections. Click any album to view its contents.
- **Keyword Tags**: Categorized tree of tags (People, Places, Events, Keywords). Click any tag to filter.
- **Folders Tree**: Physical directory tree showing the file system layout of your imported photos.

### Bottom Timeline Scrubber
At the bottom of the screen is an interactive year and month scrub bar:
- Vertical bars represent the concentration of photos taken in each month.
- Click on any bar (e.g., *Sep 2026*) to jump directly to photos taken during that period.
- Click **"Reset"** to return to the full unconstrained catalog.

---

## 5. Organizing & Tagging

### Star Ratings (0 to 5 Stars)
Rate photos to distinguish your best shots:
- **On Photo Cards**: Hover or click directly on the 5 stars on any photo card.
- **In Inspector**: Click the star rating widget in the right-hand panel.
- **In Loupe View**: Press keys `0`, `1`, `2`, `3`, `4`, or `5` on your keyboard.

### Pick and Reject Flags
Adopt the professional Lightroom/Elements culling workflow:
- **Pick (`P`)**: Flag your best selects with a green checkmark.
- **Reject (`Del` or `X`)**: Mark photos for deletion or exclusion.
- **Unflag (`U`)**: Clear flag status back to neutral.

### Tagging Photos
1. Select a photo in the grid.
2. In the right **Inspector** panel, locate the **Tags** section.
3. Type a tag name into the *"Add tag..."* input box (or select from suggested tags).
4. Select the tag category from the dropdown (`Keyword`, `Places`, `People`, or `Events`). Existing tags automatically auto-select their registered category.
5. Press **Enter** or click **"Add"**.
6. To detach a tag from an individual photo, click the `×` button on any tag pill badge in the Inspector.
7. To delete a tag entirely from the catalog and all photos, click the `×` button next to the tag in the left sidebar under **Keyword Tags**.

### Creating Categories & Hierarchies
Click the `+` button next to **Keyword Tags** in the left sidebar to create a tag with a designated category:
- `People`: Friends, family members, models.
- `Places`: Cities, countries, landmarks, venues.
- `Events`: Weddings, vacations, parties, sports events.
- `Keywords`: Subjects, themes, colors, techniques (e.g., *Portrait*, *Landscape*, *Night*).

### Albums & Collections
1. Click the `+` button next to **Albums** in the left sidebar.
2. Give the album a name (e.g., *"Best of 2026"*) and optional description.
3. Select one or more photos in the grid and click **"Add to Album"** to populate it.
4. To delete an album, hover over its name in the sidebar and click the `×` delete button. Deleting an album does not delete the photos from your catalog.

### Batch Operations
To organize multiple photos simultaneously:
1. Select multiple photos using:
   - `Ctrl` / `Cmd` + Click: Toggle individual photo selection.
   - `Shift` + Click: Select a contiguous range of photos.
2. The **Batch Action Bar** will slide down above the grid:
   - **Rate**: Click any star to apply ratings to all selected photos.
   - **Pick / Reject**: Flag all selected photos with a single click.
   - **Add Tag**: Attach a tag to all selected photos at once.
   - **Deselect**: Clear current selection.

---

## 6. Fullscreen Loupe Viewer

Double-click any photo card or click the expand icon in the Inspector to enter **Fullscreen Loupe Mode**.

```
+-------------------------------------------------------------+
| [<]                      [ IMAGE ]                      [>] |
|                                                             |
|-------------------------------------------------------------|
| DSC_0042.JPG (14 / 85)     [***..]   [✔ Pick] [✖ Reject] [X]|
+-------------------------------------------------------------+
```

### Keyboard Shortcuts
| Key | Action |
|---|---|
| `Left Arrow` / `Page Up` | Previous photo |
| `Right Arrow` / `Page Down` | Next photo |
| `1` – `5` | Assign 1 to 5 star rating |
| `0` | Clear rating (0 stars) |
| `P` | Mark as Pick |
| `X` or `Delete` | Mark as Reject |
| `U` | Clear flag (Unflag) |
| `Esc` | Close Loupe viewer |

---

## 7. Inspector & EXIF Metadata

The right-hand **Inspector Panel** provides an exhaustive breakdown of the technical and camera properties for the active photo.

### Information Displayed
- **File Properties**:
  - File Name
  - Image Dimensions (Width × Height in pixels)
  - File Size (formatted in KB or MB)
  - Date & Time Taken
  - Full absolute filesystem path
- **Camera & Exposure (EXIF)**:
  - Camera Make & Model (e.g., *Sony ILCE-7M4*, *Canon EOS R5*)
  - Lens Model (e.g., *FE 24-70mm F2.8 GM II*)
  - Shutter Speed / Exposure Time (e.g., *1/500 sec*)
  - Aperture Value (e.g., *f/2.8*)
  - ISO Sensitivity (e.g., *ISO 100*)
  - Focal Length (e.g., *50 mm*)
  - GPS Coordinates: Formatted Latitude and Longitude with a direct clickable link to **OpenStreetMap**.

---

## 8. CLI Reference

Imagine provides a unified command-line tool (`imagine`) for headless servers, scripts, and automation.

### Synopsis
```bash
imagine <command> [options]
```

### Commands

#### `imagine import`
Recursively scans a directory, extracts EXIF, calculates SHA-256 hashes, generates thumbnails, and indexes photos into the catalog.
```bash
imagine import <directory_path> [options]
```
**Options:**
- `--catalog <file>`: Path to SQLite catalog database (default: `catalog.db`).
- `--thumbs <dir>`: Thumbnail cache directory (default: `~/.cache/imagine/thumbs`).
- `--recursive`: Scan subdirectories recursively (default).
- `--no-recursive`: Only scan top-level directory.
- `--threads <N>`: Number of parallel worker threads (default: hardware concurrency).

#### `imagine serve`
Starts the embedded multi-threaded HTTP web server and REST API.
```bash
imagine serve [options]
```
**Options:**
- `--catalog <file>`: Path to SQLite catalog database (default: `catalog.db`).
- `--thumbs <dir>`: Thumbnail cache directory (default: `~/.cache/imagine/thumbs`).
- `--host <ip>`: Bind address (default: `0.0.0.0`).
- `--port <port>`: Port to listen on (default: `8080`).
- `--web-dir <dir>`: Directory containing web UI static assets (default: `web`).

#### `imagine list`
Query and display photos from the catalog in your terminal.
```bash
imagine list [options]
```
**Options:**
- `--catalog <file>`: Path to SQLite catalog database (default: `catalog.db`).
- `--rating <N>`: Filter by minimum star rating (1–5).
- `--search <text>`: Search keyword in filename, camera, or lens.
- `--limit <N>`: Maximum items to display (default: `50`).
- `--offset <N>`: Pagination offset.

#### `imagine stats`
Display database statistics and storage metrics.
```bash
imagine stats [--catalog <file>]
```
**Example Output:**
```
============= Catalog Statistics =============
  Catalog Database:   catalog.db
  Total Photos:       1,420
  Total Disk Size:    8.4 GB
  Total Tags:         28
  Total Albums:       6
  Earliest Photo:     2018-04-12 11:20
  Latest Photo:       2026-09-03 19:30
==============================================
```

#### `imagine tag`
Attach a keyword tag to a photo by ID.
```bash
imagine tag <media_id> <tag_name> [--category <people|places|events|keyword>] [--catalog <file>]
```

---

## 9. REST API Reference

The embedded C++ HTTP server provides a full REST API for developers and external integrations.

| Method | Endpoint | Description |
|---|---|---|
| `GET` | `/api/media` | Query photos with filtering, sorting, and pagination |
| `GET` | `/api/media/:id` | Get single photo details and EXIF |
| `POST` | `/api/media/:id/rating` | Set star rating (`{"rating": 0-5}`) |
| `POST` | `/api/media/:id/flag` | Set flag (`{"flag": -1\|0\|1}`) |
| `POST` | `/api/media/:id/tags` | Attach tag (`{"name": "...", "category": "..."}`) |
| `DELETE` | `/api/media/:id/tags/:tag_id` | Detach tag from photo |
| `GET` | `/api/thumbnails/:hash/:size` | Fetch cached JPEG thumbnail (`size`: 256 or 1024) |
| `GET` | `/api/photos/:id/original` | Fetch original full-resolution image |
| `GET` | `/api/tags` | List all tags grouped by category |
| `POST` | `/api/tags` | Create a new tag |
| `DELETE` | `/api/tags/:id` | Delete a tag completely from catalog and all photos |
| `GET` | `/api/albums` | List all albums |
| `POST` | `/api/albums` | Create a new album |
| `DELETE` | `/api/albums/:id` | Delete an album |
| `POST` | `/api/albums/:id/media` | Add photo to album (`{"media_id": N}`) |
| `GET` | `/api/timeline` | Get photo counts grouped by year and month |
| `GET` | `/api/stats` | Get catalog statistics |
| `POST` | `/api/import` | Start asynchronous background folder import |
| `GET` | `/api/import/progress` | Check ongoing import status and metrics |

---

## 10. Under the Hood: Database & Cache

### SQLite Catalog
- **Location**: Default is `catalog.db` in your working directory (or any path specified via `--catalog`).
- **WAL Mode**: Write-Ahead Logging (`PRAGMA journal_mode = WAL`) is enabled, allowing concurrent reads while imports or tag updates are writing to disk.
- **Transactions**: Multi-photo operations are wrapped in atomic transactions for ACID safety and high write throughput.

### Thumbnail Cache
- **Location**: Default is `~/.cache/imagine/thumbs` (or `$XDG_CACHE_HOME/imagine/thumbs`).
- **Structure**: Hierarchical directory based on the SHA-256 content hash:
  ```
  ~/.cache/imagine/thumbs/
  └── 34/
      └── 2a/
          ├── 342a0f0dc50f8e515d027..._256.jpg   # Small grid thumbnail
          └── 342a0f0dc50f8e515d027..._1024.jpg  # Large inspector preview
  ```
- This structure prevents directories from becoming overwhelmed with tens of thousands of files and enables instant file lookup.

---

## 11. Troubleshooting & FAQ

### Q: Why do my photos rotate sideways when imported?
**A**: Imagine automatically reads EXIF orientation tags (1 through 8) and losslessly rotates thumbnails to the correct upright orientation during thumbnail generation.

### Q: Can I run Imagine on a remote Linux server or NAS and access it from my laptop?
**A**: Yes! Start the server binding to `0.0.0.0`:
```bash
./build/imagine serve --catalog /media/nas/catalog.db --host 0.0.0.0 --port 8080
```
Then navigate to `http://<your-server-ip>:8080` from any computer or tablet on your local network.

### Q: Does Imagine modify my original photo files?
**A**: No. Imagine is strictly non-destructive. All ratings, tags, albums, and metadata edits are stored exclusively in the SQLite database (`catalog.db`) and thumbnail cache. Your original media files on disk are never altered.

### Q: How do I back up my catalog?
**A**: Simply back up your `catalog.db` file. Since thumbnails can always be re-generated from the original photos, `catalog.db` contains all your ratings, tags, album structures, and metadata history in a single portable file.
