#!/usr/bin/env python3
"""
Turnkey runner script for IMAGINE deterministic UI tests.
Cross-platform implementation supporting Windows, Linux, and macOS.
"""

import os
import sys
import shutil
import subprocess
from pathlib import Path


def main():
    repo_root = Path(__file__).resolve().parent.parent.parent
    print("=== IMAGINE Deterministic UI Test Runner ===")
    print(f"Repository Root: {repo_root}")

    # 1. Ensure C++ binary is built and up-to-date
    bin_name = "imagine.exe" if sys.platform == "win32" else "imagine"
    potential_bin_paths = [
        repo_root / "build" / bin_name,
        repo_root / "build" / "Release" / bin_name,
        repo_root / "build" / "Debug" / bin_name,
    ]
    imagine_bin = next((p for p in potential_bin_paths if p.is_file()), None)

    if not imagine_bin:
        print("imagine binary not found, building...")
        build_dir = repo_root / "build"
        if not (build_dir / "CMakeCache.txt").is_file():
            subprocess.run(["cmake", "-B", str(build_dir), "-S", str(repo_root)], check=True)
        subprocess.run(["cmake", "--build", str(build_dir), "--target", "imagine"], check=True)
        imagine_bin = next((p for p in potential_bin_paths if p.is_file()), None)

    if not imagine_bin or not imagine_bin.is_file():
        print(f"Error: imagine binary not found in {repo_root / 'build'}", file=sys.stderr)
        sys.exit(1)

    print(f"Using binary: {imagine_bin}")

    # 2. Locate virtual environment python / pytest
    pytest_cmd = None
    try:
        check = subprocess.run(
            [sys.executable, "-m", "pytest", "--version"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if check.returncode == 0:
            pytest_cmd = [sys.executable, "-m", "pytest"]
    except Exception:
        pass

    if not pytest_cmd:
        venv_candidates = [
            repo_root / ".venv",
            repo_root / "build" / "test_venv",
        ]
        for venv in venv_candidates:
            if sys.platform == "win32":
                pt = venv / "Scripts" / "pytest.exe"
            else:
                pt = venv / "bin" / "pytest"
            if pt.is_file():
                pytest_cmd = [str(pt)]
                break

    if not pytest_cmd:
        sys_pytest = shutil.which("pytest")
        if sys_pytest:
            pytest_cmd = [sys_pytest]

    if not pytest_cmd:
        print("Error: pytest could not be found in current Python environment, .venv, or PATH.", file=sys.stderr)
        sys.exit(1)

    print(f"Using pytest: {' '.join(pytest_cmd)}")
    print("Running UI test suite...")

    # 3. Check if pytest-xdist is available and auto-parallelize unless already requested
    args = sys.argv[1:]
    has_n = any(arg.startswith("-n") for arg in args)
    xdist_args = []

    if not has_n:
        try:
            help_check = subprocess.run(
                pytest_cmd + ["-h"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            if "-n NUM" in help_check.stdout or "-n auto" in help_check.stdout or "--numprocesses" in help_check.stdout:
                xdist_args = ["-n", "auto", "--dist", "loadfile"]
        except Exception:
            pass

    # 4. Build command arguments
    if args:
        has_test_target = any(not a.startswith("-") for a in args)
        if not has_test_target:
            cmd = pytest_cmd + xdist_args + ["tests/ui", "-v"] + args
        else:
            cmd = pytest_cmd + xdist_args + ["-v"] + args
    else:
        cmd = pytest_cmd + xdist_args + ["tests/ui", "-v"]

    res = subprocess.run(cmd, cwd=str(repo_root))
    sys.exit(res.returncode)


if __name__ == "__main__":
    main()
