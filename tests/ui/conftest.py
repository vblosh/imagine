"""
pytest configuration and shared fixtures for IMAGINE UI tests.
"""

import os
import sys
import time
import socket
import shutil
import tempfile
import subprocess
import urllib.request
import sqlite3
import pytest
from typing import Generator, Dict, Any, List

from playwright.sync_api import sync_playwright, Browser, BrowserContext, Page

from fixtures import init_schema, seed_default_catalog, create_import_photos, seed_media_catalog

# 1x1 transparent PNG bytes for mocking external map tile responses
TRANSPARENT_1X1_PNG = (
    b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01"
    b"\x08\x06\x00\x00\x00\x1f\x15c4\x00\x00\x00\nIDATx\x9cc\x00\x01\x00\x00\x05\x00\x01\r\n-\xb4"
    b"\x00\x00\x00\x00IEND\xaeB`\x82"
)


@pytest.fixture(scope="session")
def imagine_bin() -> str:
    """Locate and validate the compiled C++ imagine binary."""
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    bin_name = "imagine.exe" if sys.platform == "win32" else "imagine"
    bin_path = os.path.join(repo_root, "build", bin_name)
    if not os.path.isfile(bin_path) or not os.access(bin_path, os.X_OK):
        # Trigger build if needed
        res = subprocess.run(["cmake", "--build", "build", "--target", "imagine", "-j"], cwd=repo_root)
        if res.returncode != 0 or not os.path.isfile(bin_path):
            pytest.fail(f"imagine executable not found at {bin_path} and build failed.")
    return bin_path


@pytest.fixture(scope="session")
def web_dir() -> str:
    """Return absolute path to web assets."""
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    path = os.path.join(repo_root, "web")
    assert os.path.isdir(path), f"web directory not found at {path}"
    return path


def get_free_port() -> int:
    """Allocate an available local port."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        s.listen(1)
        return s.getsockname()[1]


def wait_for_server(url: str, port: int = 0, timeout_sec: float = 6.0) -> bool:
    """Fast poll using non-blocking TCP socket connect loop before verifying HTTP 200."""
    if not port:
        try:
            port = int(url.split(":")[-1].split("/")[0])
        except Exception:
            port = 80
    
    deadline = time.time() + timeout_sec
    connected = False
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.01):
                connected = True
                break
        except OSError:
            time.sleep(0.005)

    if not connected:
        return False

    stats_url = f"{url}/api/stats"
    req = urllib.request.Request(stats_url, headers={"Connection": "close"})
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(req, timeout=0.2) as resp:
                if resp.status == 200:
                    return True
        except Exception:
            time.sleep(0.005)
    return False


def _link_tree(src_dir: str, dst_dir: str):
    """Recursively hardlink all files from src_dir to dst_dir, falling back to copy."""
    if not os.path.isdir(src_dir):
        return
    for root, _, files in os.walk(src_dir):
        rel = os.path.relpath(root, src_dir)
        dest_subdir = os.path.join(dst_dir, rel) if rel != "." else dst_dir
        os.makedirs(dest_subdir, exist_ok=True)
        for f in files:
            src_file = os.path.join(root, f)
            dst_file = os.path.join(dest_subdir, f)
            try:
                os.link(src_file, dst_file)
            except OSError:
                shutil.copy2(src_file, dst_file)


@pytest.fixture(scope="session")
def golden_empty_template() -> Generator[Dict[str, Any], None, None]:
    """Generate golden template for empty environment once per session/worker."""
    tmpdir = tempfile.mkdtemp(prefix="imagine_golden_empty_")
    db_path = os.path.join(tmpdir, "catalog.db")
    thumbs_dir = os.path.join(tmpdir, "thumbs")
    photos_dir = os.path.join(tmpdir, "photos")
    import_dir = os.path.join(tmpdir, "to_import")

    os.makedirs(thumbs_dir, exist_ok=True)
    os.makedirs(photos_dir, exist_ok=True)
    os.makedirs(import_dir, exist_ok=True)

    import_files = create_import_photos(import_dir)

    conn = sqlite3.connect(db_path)
    init_schema(conn)
    conn.commit()
    conn.close()

    yield {
        "tmpdir": tmpdir,
        "db_path": db_path,
        "thumbs_dir": thumbs_dir,
        "photos_dir": photos_dir,
        "import_dir": import_dir,
        "import_files": import_files,
        "items": []
    }
    shutil.rmtree(tmpdir, ignore_errors=True)


@pytest.fixture(scope="session")
def golden_seeded_template(imagine_bin: str, web_dir: str) -> Generator[Dict[str, Any], None, None]:
    """Generate golden template for seeded environment with pre-cached thumbnails once per session/worker."""
    tmpdir = tempfile.mkdtemp(prefix="imagine_golden_seeded_")
    db_path = os.path.join(tmpdir, "catalog.db")
    thumbs_dir = os.path.join(tmpdir, "thumbs")
    photos_dir = os.path.join(tmpdir, "photos")
    import_dir = os.path.join(tmpdir, "to_import")

    os.makedirs(thumbs_dir, exist_ok=True)
    os.makedirs(photos_dir, exist_ok=True)
    os.makedirs(import_dir, exist_ok=True)

    items = seed_default_catalog(db_path, photos_dir)
    import_files = create_import_photos(import_dir)

    # Warm thumbnail cache by starting server briefly and querying thumbnails
    port = get_free_port()
    cmd = [
        imagine_bin, "serve",
        "--catalog", db_path,
        "--thumbs", thumbs_dir,
        "--port", str(port),
        "--web-dir", web_dir
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    url = f"http://127.0.0.1:{port}"
    if wait_for_server(url, port, timeout_sec=5.0):
        for item in items:
            chash = item.get("content_hash")
            if chash:
                try:
                    with urllib.request.urlopen(f"{url}/api/thumbnails/{chash}/256", timeout=1.0):
                        pass
                except Exception:
                    pass
    proc.terminate()
    try:
        proc.wait(timeout=2)
    except Exception:
        proc.kill()

    yield {
        "tmpdir": tmpdir,
        "db_path": db_path,
        "thumbs_dir": thumbs_dir,
        "photos_dir": photos_dir,
        "import_dir": import_dir,
        "import_files": import_files,
        "items": items
    }
    shutil.rmtree(tmpdir, ignore_errors=True)


@pytest.fixture(scope="session")
def golden_media_template(imagine_bin: str, web_dir: str) -> Generator[Dict[str, Any], None, None]:
    """Generate golden template for media environment once per session/worker."""
    tmpdir = tempfile.mkdtemp(prefix="imagine_golden_media_")
    db_path = os.path.join(tmpdir, "catalog.db")
    thumbs_dir = os.path.join(tmpdir, "thumbs")
    photos_dir = os.path.join(tmpdir, "photos")
    import_dir = os.path.join(tmpdir, "to_import")

    os.makedirs(thumbs_dir, exist_ok=True)
    os.makedirs(photos_dir, exist_ok=True)
    os.makedirs(import_dir, exist_ok=True)

    items = seed_media_catalog(db_path, photos_dir)

    port = get_free_port()
    cmd = [
        imagine_bin, "serve",
        "--catalog", db_path,
        "--thumbs", thumbs_dir,
        "--port", str(port),
        "--web-dir", web_dir
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    url = f"http://127.0.0.1:{port}"
    if wait_for_server(url, port, timeout_sec=5.0):
        for item in items:
            chash = item.get("content_hash")
            if chash:
                try:
                    with urllib.request.urlopen(f"{url}/api/thumbnails/{chash}/256", timeout=1.0):
                        pass
                except Exception:
                    pass
    proc.terminate()
    try:
        proc.wait(timeout=2)
    except Exception:
        proc.kill()

    yield {
        "tmpdir": tmpdir,
        "db_path": db_path,
        "thumbs_dir": thumbs_dir,
        "photos_dir": photos_dir,
        "import_dir": import_dir,
        "import_files": [],
        "items": items
    }
    shutil.rmtree(tmpdir, ignore_errors=True)


def _fast_clone_env(golden: Dict[str, Any], prefix: str) -> Generator[Dict[str, Any], None, None]:
    """Fast clone golden template using file hardlinks and fast SQLite DB copy."""
    tmpdir = tempfile.mkdtemp(prefix=prefix)
    db_path = os.path.join(tmpdir, "catalog.db")
    thumbs_dir = os.path.join(tmpdir, "thumbs")
    photos_dir = os.path.join(tmpdir, "photos")
    import_dir = os.path.join(tmpdir, "to_import")

    os.makedirs(thumbs_dir, exist_ok=True)
    os.makedirs(photos_dir, exist_ok=True)
    os.makedirs(import_dir, exist_ok=True)

    # Fast copy SQLite DB (~1ms)
    shutil.copy2(golden["db_path"], db_path)

    # Fast hardlink photos, import files, and thumbnails (~1ms)
    _link_tree(golden["photos_dir"], photos_dir)
    _link_tree(golden["import_dir"], import_dir)
    _link_tree(golden["thumbs_dir"], thumbs_dir)

    # Update database file paths to point to test photos_dir (<1ms)
    golden_photos = golden["photos_dir"]
    if os.path.exists(golden_photos) and os.listdir(golden_photos):
        conn = sqlite3.connect(db_path)
        conn.execute("UPDATE media_items SET file_path = REPLACE(file_path, ?, ?)", (golden_photos, photos_dir))
        conn.commit()
        conn.close()

    # Create cloned items metadata with adjusted file paths
    cloned_items = []
    for it in golden["items"]:
        c_it = dict(it)
        if "file_path" in c_it:
            c_it["file_path"] = c_it["file_path"].replace(golden_photos, photos_dir)
        cloned_items.append(c_it)

    cloned_import = [
        os.path.join(import_dir, os.path.basename(f))
        for f in golden["import_files"]
    ]

    port = get_free_port()
    env = {
        "tmpdir": tmpdir,
        "db_path": db_path,
        "thumbs_dir": thumbs_dir,
        "photos_dir": photos_dir,
        "import_dir": import_dir,
        "import_files": cloned_import,
        "port": port,
        "items": cloned_items
    }

    yield env
    shutil.rmtree(tmpdir, ignore_errors=True)


@pytest.fixture
def empty_env(golden_empty_template: Dict[str, Any]) -> Generator[Dict[str, Any], None, None]:
    """Provide an isolated temporary environment with an empty catalog."""
    yield from _fast_clone_env(golden_empty_template, prefix="imagine_ui_empty_")


@pytest.fixture
def seeded_env(golden_seeded_template: Dict[str, Any]) -> Generator[Dict[str, Any], None, None]:
    """Provide an isolated temporary environment with a seeded catalog."""
    yield from _fast_clone_env(golden_seeded_template, prefix="imagine_ui_test_")


@pytest.fixture
def media_env(golden_media_template: Dict[str, Any]) -> Generator[Dict[str, Any], None, None]:
    """Provide an isolated environment seeded with photos, video, and audio."""
    yield from _fast_clone_env(golden_media_template, prefix="imagine_ui_media_")


def start_backend(imagine_bin: str, web_dir: str, env: Dict[str, Any]) -> Generator[Dict[str, Any], None, None]:
    """Helper to launch and teardown the imagine serve backend process."""
    cmd = [
        imagine_bin,
        "serve",
        "--catalog", env["db_path"],
        "--thumbs", env["thumbs_dir"],
        "--port", str(env["port"]),
        "--web-dir", web_dir
    ]
    proc_env = os.environ.copy()
    proc_env.pop("IMAGINE_PHOTOS_DIR", None)
    proc_env.pop("IMAGINE_THUMBS_DIR", None)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=proc_env)
    url = f"http://127.0.0.1:{env['port']}"

    ready = wait_for_server(url, port=env["port"], timeout_sec=6.0)
    if not ready:
        proc.terminate()
        out, err = proc.communicate(timeout=2)
        pytest.fail(f"Backend failed to start on {url}.\nStdout: {out.decode()}\nStderr: {err.decode()}")

    server_info = {
        "url": url,
        "proc": proc,
        "env": env
    }

    yield server_info

    try:
        proc.terminate()
        proc.wait(timeout=3)
    except Exception:
        proc.kill()


@pytest.fixture
def empty_server(imagine_bin: str, web_dir: str, empty_env: Dict[str, Any]) -> Generator[Dict[str, Any], None, None]:
    """Launch imagine serve with empty catalog."""
    yield from start_backend(imagine_bin, web_dir, empty_env)


@pytest.fixture
def server(imagine_bin: str, web_dir: str, seeded_env: Dict[str, Any]) -> Generator[Dict[str, Any], None, None]:
    """Launch imagine serve with seeded catalog."""
    yield from start_backend(imagine_bin, web_dir, seeded_env)


@pytest.fixture
def media_server(imagine_bin: str, web_dir: str, media_env: Dict[str, Any]) -> Generator[Dict[str, Any], None, None]:
    """Launch imagine serve with media catalog (photos + video + audio)."""
    yield from start_backend(imagine_bin, web_dir, media_env)


@pytest.fixture(scope="session")
def browser() -> Generator[Browser, None, None]:
    """Launch headless Chromium browser instance."""
    with sync_playwright() as p:
        browser = p.chromium.launch(
            headless=True,
            args=[
                "--no-sandbox",
                "--disable-setuid-sandbox",
                "--disable-dev-shm-usage",
                "--disable-extensions",
                "--disable-background-networking",
                "--disable-sync",
                "--disable-default-apps",
                "--no-first-run",
            ]
        )
        yield browser
        browser.close()


@pytest.fixture
def context(browser: Browser) -> Generator[BrowserContext, None, None]:
    """Provide an isolated browser context with external map tile requests mocked."""
    ctx = browser.new_context(viewport={"width": 1280, "height": 800})
    # Intercept external OpenStreetMap tiles so tests are 100% hermetic and instant
    ctx.route("**/*.tile.openstreetmap.org/**", lambda r: r.fulfill(status=200, content_type="image/png", body=TRANSPARENT_1X1_PNG))
    ctx.route("**/tile.openstreetmap.org/**", lambda r: r.fulfill(status=200, content_type="image/png", body=TRANSPARENT_1X1_PNG))
    yield ctx
    ctx.close()


@pytest.fixture
def page(context: BrowserContext) -> Generator[Page, None, None]:
    """Provide a new Page with error and console listeners to guard against JS exceptions."""
    p = context.new_page()
    page_errors = []
    console_errors = []

    # Inject test mode flag for accelerated client-side debouncing/throttling
    p.add_init_script("window.__TEST_MODE__ = true;")

    def on_page_error(exc):
        page_errors.append(str(exc))

    def on_console(msg):
        if msg.type == "error":
            # Ignore network 404 logs from browser image loader if any
            text = msg.text
            if "Failed to load resource" not in text and "net::ERR_" not in text:
                console_errors.append(text)

    p.on("pageerror", on_page_error)
    p.on("console", on_console)

    yield p

    assert len(page_errors) == 0, f"Uncaught page JavaScript errors encountered: {page_errors}"
    assert len(console_errors) == 0, f"Console errors encountered: {console_errors}"
