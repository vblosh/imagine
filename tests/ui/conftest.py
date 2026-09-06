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
import json
import pytest
from typing import Generator, Dict, Any

from playwright.sync_api import sync_playwright, Browser, BrowserContext, Page

from fixtures import init_schema, seed_default_catalog, create_import_photos


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


def wait_for_server(url: str, timeout_sec: float = 5.0) -> bool:
    """Poll the server /api/stats endpoint until ready."""
    deadline = time.time() + timeout_sec
    stats_url = f"{url}/api/stats"
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(stats_url, timeout=0.5) as resp:
                if resp.status == 200:
                    return True
        except Exception:
            pass
        time.sleep(0.05)
    return False


@pytest.fixture
def empty_env() -> Generator[Dict[str, Any], None, None]:
    """Provide an isolated temporary environment with an empty catalog."""
    tmpdir = tempfile.mkdtemp(prefix="imagine_ui_empty_")
    db_path = os.path.join(tmpdir, "catalog.db")
    thumbs_dir = os.path.join(tmpdir, "thumbs")
    photos_dir = os.path.join(tmpdir, "photos")
    import_dir = os.path.join(tmpdir, "to_import")

    os.makedirs(thumbs_dir, exist_ok=True)
    os.makedirs(photos_dir, exist_ok=True)
    os.makedirs(import_dir, exist_ok=True)

    import_files = create_import_photos(import_dir)

    import sqlite3
    conn = sqlite3.connect(db_path)
    init_schema(conn)
    conn.commit()
    conn.close()

    port = get_free_port()
    env = {
        "tmpdir": tmpdir,
        "db_path": db_path,
        "thumbs_dir": thumbs_dir,
        "photos_dir": photos_dir,
        "import_dir": import_dir,
        "import_files": import_files,
        "port": port,
        "items": []
    }

    yield env
    shutil.rmtree(tmpdir, ignore_errors=True)


@pytest.fixture
def seeded_env() -> Generator[Dict[str, Any], None, None]:
    """Provide an isolated temporary environment with a seeded catalog."""
    tmpdir = tempfile.mkdtemp(prefix="imagine_ui_test_")
    db_path = os.path.join(tmpdir, "catalog.db")
    thumbs_dir = os.path.join(tmpdir, "thumbs")
    photos_dir = os.path.join(tmpdir, "photos")
    import_dir = os.path.join(tmpdir, "to_import")

    os.makedirs(thumbs_dir, exist_ok=True)
    os.makedirs(photos_dir, exist_ok=True)
    os.makedirs(import_dir, exist_ok=True)

    items = seed_default_catalog(db_path, photos_dir)
    import_files = create_import_photos(import_dir)

    port = get_free_port()
    env = {
        "tmpdir": tmpdir,
        "db_path": db_path,
        "thumbs_dir": thumbs_dir,
        "photos_dir": photos_dir,
        "import_dir": import_dir,
        "import_files": import_files,
        "port": port,
        "items": items
    }

    yield env
    shutil.rmtree(tmpdir, ignore_errors=True)


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
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    url = f"http://127.0.0.1:{env['port']}"

    ready = wait_for_server(url, timeout_sec=6.0)
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


@pytest.fixture(scope="session")
def browser() -> Generator[Browser, None, None]:
    """Launch headless Chromium browser instance."""
    with sync_playwright() as p:
        browser = p.chromium.launch(
            headless=True,
            args=["--no-sandbox", "--disable-setuid-sandbox", "--disable-dev-shm-usage"]
        )
        yield browser
        browser.close()


@pytest.fixture
def context(browser: Browser) -> Generator[BrowserContext, None, None]:
    """Provide an isolated browser context."""
    ctx = browser.new_context(viewport={"width": 1280, "height": 800})
    yield ctx
    ctx.close()


@pytest.fixture
def page(context: BrowserContext) -> Generator[Page, None, None]:
    """Provide a new Page with error and console listeners to guard against JS exceptions."""
    p = context.new_page()
    page_errors = []
    console_errors = []

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
