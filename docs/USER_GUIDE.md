# Imagine User Guide & Manual

Welcome to **Imagine**, a high-performance photo catalog and organizer inspired by **Adobe Photoshop Elements Organizer**.

Imagine combines a high-speed C++20 core engine with an interactive Command-Line Interface (CLI) and an embedded, responsive Web UI that runs locally on any browser without native GUI dependencies.

---

## Table of Contents
1. [System Requirements & Installation](#1-system-requirements--installation)
2. [Quick Start Tutorial](#2-quick-start-tutorial)
3. [Importing Media](#3-importing-media)
4. [Browsing & Navigation (including Language / i18n)](#4-browsing--navigation)
5. [Organizing & Tagging](#5-organizing--tagging)
6. [Fullscreen Loupe Viewer](#6-fullscreen-loupe-viewer)
7. [Inspector & EXIF Metadata](#7-inspector--exif-metadata)
8. [Map View & Geotagging](#8-map-view--geotagging)
9. [CLI Reference](#9-cli-reference)
10. [REST API Reference](#10-rest-api-reference)
11. [Under the Hood: Database & Cache](#11-under-the-hood-database--cache)
12. [Troubleshooting & FAQ](#12-troubleshooting--faq)

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

Get your photo, video, and audio collection organized in three simple steps:

### Step 1: Import Media
Import your existing folders of photos, videos, and audio into an Imagine catalog:
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

## 3. Importing Media

Imagine includes an asynchronous, multi-threaded importer capable of scanning thousands of media files per second.

### Supported File Formats
File extensions are matched case-insensitively.

**Photos**
- JPEG (`.jpg`, `.jpeg`)
- PNG (`.png`)
- BMP (`.bmp`)
- WebP (`.webp`)
- TIFF (`.tiff`, `.tif`)

**Videos**
- MPEG-4 (`.mp4`, `.m4v`)
- QuickTime (`.mov`)
- WebM (`.webm`)
- Matroska (`.mkv`)
- AVI (`.avi`)
- AVCHD / MPEG transport stream (`.mts`, `.m2ts`, `.m2t`)
- MPEG video (`.mpg`, `.mpeg`)
- Windows Media Video (`.wmv`)
- Flash Video (`.flv`)
- 3GPP (`.3gp`)

**Audio**
- MP3 (`.mp3`)
- WAV (`.wav`)
- FLAC (`.flac`)
- Ogg Vorbis (`.ogg`)
- MPEG-4 audio (`.m4a`)
- AAC (`.aac`)
- Windows Media Audio (`.wma`)

Videos and audio appear alongside photos in the catalog and can be filtered from the **Videos** and **Audio** navigation entries. Imagine extracts video-frame thumbnails when FFmpeg is available, uses embedded cover art when present, and otherwise generates a placeholder thumbnail. Playback uses the browser’s built-in media support.

### Importing via the Web UI
1. Click the blue **"Import Folder"** button in the top navigation bar.
2. Enter the absolute directory path on your computer (e.g., `/home/username/Pictures`).
3. Ensure **"Scan Subdirectories Recursively"** is checked if you have nested folders.
4. Click **"Start Import"**.
5. Watch the live progress bar: Imagine will report scanned files, processed items, skipped duplicates, and current file in real-time.

```
+-------------------------------------------------------------+
| Import Media to Catalog                                 [X] |
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
- **Fast Rescan**: If you run import on a folder that has already been indexed, Imagine inspects file modification times (`mtime`) and skips unchanged media files.
- **SHA-256 Hashing**: Each imported media file receives a cryptographic content hash. If you move a file to a new folder or import duplicates, Imagine recognizes the content hash and prevents duplicate thumbnail generation.

---

## 4. Browsing & Navigation

### Views (Top Tabs)
- **Media**: The master view displaying all photos in your catalog, ordered chronologically.
- **People**: Shows photos tagged under the *People* category.
- **Places**: Shows photos tagged with geographic locations or containing GPS metadata.
- **Events**: Shows photos organized into specific events (e.g., *Birthdays*, *Conferences*, *Holidays*).
- **Albums**: Displays all user-created photo albums as visual collection cards. Click any album to view and manage its photos.

### Thumbnail Zoom Slider
In the top right bar, slide the thumbnail zoom controller:
- **Left (120px)**: Compact contact sheet view displaying dozens of photos per screen.
- **Right (360px)**: Large preview cards showing rich detail, star ratings, flags, and titles.

### The Left Navigation Sidebar
- **Navigation & Quick Filters**:
  - **All Media**: Resets all active filters and displays the entire catalog.
  - **Media Types**: Choose between **Photos**, **Videos**, and **Audio** (each includes live count badges).
  - **Status & Rating Flags**:
    - **Picks**: Displays only media marked with the green Pick flag (includes live item count).
    - **Rejects**: Displays media marked with the red Reject flag (includes live item count, ideal for cleaning up bad shots).
    - **Not Rejects**: Displays all items except rejected ones (`flag != -1`, includes live item count) — ideal for browsing working sets without deleted/rejected items.
    - **Unrated**: Displays media items that have not yet received a star rating (0 stars) (includes live item count badge).
- **Combining Filters (`AND` Logic)**:
  - Navigation filters can be combined cumulatively across multiple dimensions simultaneously:
    - **Media Type** (e.g., Photos) `AND`
    - **Status / Flag** (e.g., Picks or Unrated) `AND`
    - **Album** (e.g., *Best of 2026*) `AND`
    - **Keyword Tag** (e.g., *Vacation*)
  - The filter banner displays a breadcrumb summary of all active criteria (e.g., `Photos • Picks • Album: Best of 2026 • Tag: Vacation`).
  - **Independent Toggling**: Clicking an already active filter toggles it off without clearing the rest of your active criteria. Click **"All Media"** or the **"Clear Filter"** button to reset all filters at once.
- **Albums**: Custom user collections. Click any album to filter media by collection.
- **Keyword Tags**: Categorized tree of tags (People, Places, Events, Keywords). Click any tag to filter.
- **Folders Tree**: Physical directory tree showing the file system layout of your imported photos and media.

### Bottom Timeline Scrubber
At the bottom of the screen is an interactive year and month scrub bar:
- Vertical bars represent the concentration of photos taken in each month.
- Click on any bar (e.g., *Sep 2026*) to jump directly to photos taken during that period.
- Click **"Reset"** to return to the full unconstrained catalog.

### Language & Internationalization (i18n)
Imagine features native multi-language internationalization supporting 5 languages:
- **English** (`en`) — Default
- **Español / Spanish** (`es`)
- **Deutsch / German** (`de`)
- **Русский / Russian** (`ru`)
- **中文 / Chinese (Simplified)** (`zh`)

#### Switching Languages
- **Dropdown Selector**: Click the language selector dropdown (`#langSelect`) in the top navigation bar (accompanied by a globe icon) to choose any language. All UI elements, labels, empty states, and inspector fields update immediately in real-time without reloading the page.
- **Preference Persistence**: Your language selection is automatically stored in browser `localStorage` (`imagine_language`) and restored across restarts.
- **Direct URL Parameter**: Launch the interface in any specific language using the `?lang=<code>` URL query parameter (for example, `http://localhost:8080/?lang=es` or `http://localhost:8080/?lang=zh`).

#### Localization Scope
Full interface localization covers:
- **Navigation & Search**: Top view tabs, search input placeholder, import button, language picker.
- **Sidebar & Filters**: Media types (Photos, Videos, Audio), status flags (Picks, Rejects, Not Rejects, Unrated), album & tag categories.
- **Photo Grid & Inspector**: Date groups, media count badges, EXIF technical parameters, inline edit prompts, and action buttons.
- **Timeline Scrubber**: Localized short month names across all scripts and alphabets (Latin, Cyrillic, Hanzi).
- **Modals & Dialogs**: Tag assignment dialog, album creation, delete confirmation modals, and system notifications/toasts.
- **Map View**: Map action toolbar, geotagging banner, and unmapped tray.

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

### Auto Event Suggestions

Select photos and click **Auto Event** in the batch action bar, or use the Auto Event action in the Inspector for a single photo. Review the suggested groups, edit their names, and uncheck any groups or individual photos you want to exclude. Click **Apply** to attach event tags; cancelling the preview makes no changes.

Suggestions run locally using catalog metadata. Photos are ordered by capture time and separated when the gap exceeds four hours, the group spans more than twelve hours, or the next located photo is more than 25 km from the preceding located photo. Missing GPS and midnight alone do not split an outing. Dates follow the application's UTC convention and may reflect filesystem timestamps when no capture date was available during import.

Names favor an event tag shared by the entire group, then a caption shared by a majority, a shared place tag, or a descriptive folder name. Otherwise, the suggestion uses **Photos** and the date. Common people, places, and keywords provide context in the preview. Videos, audio, missing catalog items, and photos without a valid date are reported as skipped.

Applying suggestions preserves existing tags and reuses an existing event with the same name. If some assignments fail, the dialog retains the failed items for retry. Suggestions do not analyze image contents or contact external services.

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
4. Click an album in the sidebar to filter the grid to that album's photos.
5. To remove photos from the active album, select one or more album photos and click **"Remove from Album"** in the Inspector or, for multiple selected photos, in the Batch Action Bar. This removes the album association without deleting the photos from your catalog.
6. To add an active-album photo to another album, click **"Add to Album"** in the Inspector and choose the destination album.
7. To choose the album's cover, select one photo from the active album. In the Inspector's **Albums** section, click **"Set as cover"**. The button is available only when an album is active and exactly one album photo is selected.
8. To delete an album, hover over its name in the sidebar and click the `×` delete button. Deleting an album does not delete the photos from your catalog.

### Batch Operations
To organize multiple photos simultaneously:
1. Select multiple photos using:
   - `Ctrl` / `Cmd` + Click: Toggle individual photo selection.
   - `Shift` + Click: Select a contiguous range of photos.
2. The **Batch Action Bar** will slide down above the grid:
   - **Rate**: Click any star to apply ratings to all selected photos.
   - **Pick / Reject**: Flag all selected photos with a single click.
   - **Delete**: Remove selected photos from catalog (and optionally disk).
   - **Add Tag**: Attach a tag to all selected photos at once.
   - **Auto Event**: Review suggested event groups and names for the selected photos before applying event tags.
   - **Album**: Add selected photos to an album. When viewing an active album, this becomes **Remove from Album** and removes the selected photos from that album only.
   - **Date**: Open Change Date dialog to shift timestamps by hours, adjust time zones, or set a specified date & time.
   - **Move**: Open Move Media dialog to prompt for a destination folder inside the photos directory, physically move files on disk, and update catalog database records. Moving outside the photos directory is strictly prohibited.
   - **Deselect**: Clear current selection.

---

## 6. Fullscreen Loupe Viewer

Double-click any photo card or click the expand icon in the Inspector to enter **Fullscreen Loupe Mode**.

```
+-------------------------------------------------------------+
| [<]                      [ IMAGE ]                      [>] |
|                                                             |
|-------------------------------------------------------------|
| DSC_0042.JPG (14 / 85)   [***..] [✔][✖]  [-] [100%] [+] [X]|
+-------------------------------------------------------------+
```

### Zoom & Pan
- **Zoom In / Out**: Click the `+` / `−` buttons or use the zoom slider in the loupe toolbar.
- **Mouse Wheel**: Scroll up/down over the photo to zoom smoothly in and out centered at your cursor.
- **Double Click**: Double-click anywhere on the image to zoom in to 200% centered at that point; double-click again to reset to Fit (100%).
- **Pan / Drag**: When zoomed in, click and drag the image to pan around and inspect fine details.
- **Reset Zoom**: Click the zoom percentage button (e.g., `100%`) or press `Z` / `Ctrl+0` / `Cmd+0`.
- **Automatic Reset**: Zoom and pan automatically reset to Fit when navigating to another photo or closing the viewer.

### Keyboard Shortcuts
| Key | Action |
|---|---|
| `Left Arrow` / `Page Up` | Previous photo |
| `Right Arrow` / `Page Down` | Next photo |
| `+` or `=` | Zoom in |
| `-` or `_` | Zoom out |
| `Z` | Toggle zoom between Fit (100%) and 200% |
| `Ctrl+0` / `Cmd+0` | Reset zoom to Fit (100%) |
| `1` – `5` | Assign 1 to 5 star rating |
| `0` | Clear rating (0 stars) |
| `P` | Mark as Pick |
| `X` | Mark as Reject |
| `U` | Clear flag (Unflag) |
| `Delete` / `Backspace` | Delete photo from catalog (with confirmation prompt) |
| `Esc` | Close Loupe viewer |

---

## 7. Inspector & EXIF Metadata

The right-hand **Inspector Panel** provides an exhaustive breakdown of the technical and camera properties for the active photo.

### Information Displayed & Editable Metadata
- **File Properties**:
  - **File Name**: Click the filename or the edit button (✏️) to rename the file. This renames the physical file on disk (if present) and updates the catalog database and UI in real time.
  - **Type**: Media type (Photo, Video, Audio).
  - **Image Dimensions**: Width × Height in pixels.
  - **File Size**: Formatted in KB or MB.
  - **Date & Time Taken**: Click the date taken or the edit button (✏️) to modify the date and time via the inline date picker.
  - **Caption**: Click the caption or the edit button (✏️) to add or modify descriptive text notes for the photo.
  - **Path**: Full filesystem path (automatically updated when renaming or moving). Includes an inline Move button (📁) to move the photo to another folder inside the photos directory.
- **Actions**:
  - **Move...**: Move the active photo to another subfolder inside the photos directory with validation preventing navigation outside the library root.
  - **Delete from Catalog**: Remove the active photo from catalog records (and optionally delete from disk).
- **Camera & Exposure (EXIF)**:
  - Camera Make & Model (e.g., *Sony ILCE-7M4*, *Canon EOS R5*)
  - Lens Model (e.g., *FE 24-70mm F2.8 GM II*)
  - Shutter Speed / Exposure Time (e.g., *1/500 sec*)
  - Aperture Value (e.g., *f/2.8*)
  - ISO Sensitivity (e.g., *ISO 100*)
  - Focal Length (e.g., *50 mm*)
  - GPS Coordinates: Formatted Latitude and Longitude with a direct clickable link to **OpenStreetMap**.

### Resizable Panels
- **Vertical Panel Resizing**: Both the **File Information** and **Camera & Exposure (EXIF)** panels feature draggable splitter handles at their base. Drag up or down to adjust panel height to your preferred size; scrollbars seamlessly engage when space is restricted. Double-click any resize handle (or press `Enter`/`Escape` when focused) to reset to default height.
- **Horizontal Inspector Resizing**: Drag the left border edge of the Inspector panel to dynamically widen or narrow the entire inspector (between 240px and 800px). Double-click the left resizer edge to restore the default width (300px).
- **Layout Persistence**: Your customized panel heights and inspector width are automatically preserved in browser `localStorage` across page reloads and sessions.

---

## 8. Map View & Geotagging

Imagine features an embedded, interactive world map powered by **Leaflet** and **OpenStreetMap**, allowing you to explore your catalog geographically, inspect photo pins, and assign or edit GPS coordinates.

### Overview & Navigation

| Action | Shortcut / Control | Description |
|---|---|---|
| **Switch to Map View** | Press `M` or click **Map** | Switches from Grid View to the interactive Map View via the view switcher. |
| **Switch to Grid View** | Press `G` or click **Grid** | Returns to the standard chronological photo grid view. |
| **Fit All Photos** | Click **Fit All** | Smoothly pans and zooms the map to fit all geotagged photos in your catalog into view. |
| **Toggle Unmapped Tray** | Click **Unmapped (N)** | Toggles the bottom tray showing photos that do not yet have GPS coordinates. |
| **Exit Placement / Deselect** | Press `Esc` or click **Deselect** | Clears active unmapped selection and exits geotagging placement mode. |

### Core Features

#### Custom Photo Pins & Spatial Clustering
- **Thumbnail Pins**: Photos with valid GPS coordinates are rendered as custom map pins featuring visual thumbnail previews.
- **Dynamic Clustering**: Photos within a 50-pixel screen radius automatically combine into clustered pins displaying a count badge.
- **Zoom Decomposition**: Zooming into any area smoothly separates clusters into individual photo pins; zooming out recombines them into clusters.
- **Interactive Popup Cards**: Clicking any pin opens a rich card with:
  - Photo thumbnail, filename, date/time taken, and formatted GPS coordinates.
  - **Cluster Navigation**: When multiple photos share a pin, use `<` and `>` arrow buttons (`X of Y`) to browse photos without zooming.
  - **Quick Ratings & Flags**: Assign 1–5 star ratings or toggle Pick (`✔`) and Reject (`✖`) flags directly inside the map popup.
  - **Loupe Mode**: Click the thumbnail preview to launch into the fullscreen Loupe viewer.

#### Geotagging & The Unmapped Photos Tray
The **Unmapped Tray** slides up from the bottom of the map, displaying thumbnails of all photos lacking GPS data:
1. **Single Photo Placement**:
   - Click an unmapped photo chip in the tray to activate placement mode.
   - Click anywhere on the Leaflet map canvas to assign those coordinates to the photo.
2. **Batch Placement Mode**:
   - Multi-select multiple photos using `Ctrl` / `Cmd` + Click, select ranges with `Shift` + Click, or click **Select All**.
   - Click on the map to assign the exact GPS coordinates to all selected photos simultaneously.
3. **Hover Tooltip Preview**:
   - Hover over any photo chip in the unmapped tray to view an enlarged preview thumbnail alongside pixel dimensions, file size, and timestamp.
4. **Inspector Synchronization**:
   - Selecting a photo chip in the unmapped tray automatically opens and displays that photo in the right-hand Inspector panel.

#### Location & Coordinate Search
The search overlay in the top-left corner of the map provides versatile search capabilities:
- **Direct GPS Coordinates**: Type latitude and longitude (e.g., `48.8584, 2.2945` or `-33.8688 151.2093`) to fly directly to that location and drop a search marker.
- **Catalog Photo Search**: Search by photo filename (e.g., `mountain`) to jump directly to already-geotagged catalog photos.
- **OpenStreetMap Nominatim Geocoding**: Search for cities, addresses, or landmarks (e.g., `Eiffel Tower`, `Tokyo`). Queries are routed through the backend proxy (`/api/geocode`) with 24-hour in-memory caching and rate-limiting.
- **"Place Photo Here" Button**: With photos selected in the unmapped tray, search for a place and click **Place Photo Here** inside the result popup to geotag them to the search result.

#### Inspector Integration & Mini-Map
When selecting a photo with GPS metadata:
- **Inspector Mini-Map**: An embedded mini-map appears under the **Location** section of the Inspector panel, centered on the photo's coordinates.
- **Show on Map**: Click **Show on Map** to switch to Map View, center the map on the photo, and open its popup card.
- **Clear GPS**: Click **Clear** to remove GPS coordinates from the catalog database.
- **Place on Map**: For photos without GPS, click **Place on Map** in the Inspector to jump directly to Map View in placement mode.

---

## 9. CLI Reference

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
- `--search <text>`: Search keyword in filename, camera, lens, or tags.
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
  Total Items:        1,420
    Photos:           1,200
    Videos:           180
    Audio:            40
    Picks:            350
    Rejects:          45
    Not Rejects:      1,375
    Unrated:          215
  Total Media Time:   1h 24m 10s
  Total Disk Size:    8.4 GB
  Total Tags:         28
  Total Albums:       6
  Earliest Item:      2018-04-12 11:20
  Latest Item:        2026-09-03 19:30
==============================================
```

#### `imagine tag`
Attach a keyword tag to a photo by ID.
```bash
imagine tag <media_id> <tag_name> [--category <people|places|events|keyword>] [--catalog <file>]
```

#### `imagine delete`
Deletes one or more photos from the catalog database by ID, or all photos marked as Rejected.
```bash
imagine delete <media_id...> [options]
```
**Options:**
- `--catalog <file>`: Path to SQLite catalog database (default: `catalog.db`).
- `--yes`, `-y`: Skip confirmation prompt.
- `--rejected`: Delete all photos marked with the Reject flag.

---

## 10. REST API Reference

The embedded C++ HTTP server provides a full REST API for developers and external integrations.

| Method | Endpoint | Description |
|---|---|---|
| `GET` | `/api/media` | Query photos with filtering, sorting, and pagination |
| `GET` | `/api/media/:id` | Get single photo details and EXIF |
| `DELETE` | `/api/media/:id` | Delete photo from catalog database (optional query `delete_from_disk=true`) |
| `POST` | `/api/media/batch-delete` | Batch delete photos (`{"ids": [1, 2, ...], "delete_from_disk": false}`) |
| `POST` | `/api/media/:id/rating` | Set star rating (`{"rating": 0-5}`) |
| `POST` | `/api/media/:id/flag` | Set flag (`{"flag": -1\|0\|1}`) |
| `POST` | `/api/media/:id/gps` | Update or clear GPS coordinates (`{"has_gps": true, "latitude": ..., "longitude": ...}`) |
| `POST` | `/api/media/batch-gps` | Batch assign GPS coordinates (`{"ids": [...], "has_gps": true, "latitude": ..., "longitude": ...}`) |
| `GET` | `/api/geocode` | Search locations via OpenStreetMap Nominatim proxy (`?q=<query>&limit=5`) |
| `POST` | `/api/media/:id/tags` | Attach tag (`{"name": "...", "category": "..."}`) |
| `POST` | `/api/media/event-suggestions` | Preview local event groups for selected photos (`{"ids": [1, 2, ...]}`, maximum 1000 IDs); returns `groups` with naming evidence and `skipped` items without modifying tags |
| `POST` | `/api/media/batch-tags` | Apply a tag to selected photos (`{"ids": [...], "name": "...", "category": "events"}`); returns assignment counts and `failed_ids` |
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
| `DELETE` | `/api/albums/:id/media` | Remove one or more photos from an album (`{"media_id": N}` or `{"media_ids": [N, ...]}`) |
| `POST` | `/api/albums/:id/cover` | Set an album member photo as its cover (`{"media_id": N}`) |
| `GET` | `/api/timeline` | Get photo counts grouped by year and month |
| `GET` | `/api/stats` | Get catalog statistics |
| `POST` | `/api/import` | Start asynchronous background folder import |
| `GET` | `/api/import/progress` | Check ongoing import status and metrics |

---

## 11. Under the Hood: Database & Cache

### SQLite Catalog
- **Location**: Default is `catalog.db` in your working directory (or any path specified via `--catalog`).
- **WAL Mode**: Write-Ahead Logging (`PRAGMA journal_mode = WAL`) is enabled, allowing concurrent reads while imports or tag updates are writing to disk.
- **Transactions**: Multi-photo operations are wrapped in atomic transactions for ACID safety and high write throughput.
- **Developer Documentation**: For complete technical specifications of tables, columns, indexes, migrations (v1–v4), and C++ data access APIs, see the [Database Schema Documentation](dev/database_schema.md).

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

## 12. Troubleshooting & FAQ

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
