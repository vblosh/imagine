#!/usr/bin/env bash
set -e

# Turnkey runner script for IMAGINE deterministic UI tests.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

echo "=== IMAGINE Deterministic UI Test Runner ==="
echo "Repository Root: ${REPO_ROOT}"

# 1. Ensure C++ binary is built and up-to-date
if [ -f "${REPO_ROOT}/build/imagine.exe" ]; then
    IMAGINE_BIN="${REPO_ROOT}/build/imagine.exe"
else
    IMAGINE_BIN="${REPO_ROOT}/build/imagine"
fi

cmake -B "${REPO_ROOT}/build" -S "${REPO_ROOT}"
cmake --build "${REPO_ROOT}/build" --target imagine -j

if [ ! -f "${IMAGINE_BIN}" ]; then
    echo "Error: ${IMAGINE_BIN} not found." >&2
    exit 1
fi
echo "Using binary: ${IMAGINE_BIN}"

# 2. Locate virtual environment python/pytest
VENV_DIR="${REPO_ROOT}/build/test_venv"
if [ -f "${REPO_ROOT}/.venv/bin/activate" ]; then
    # shellcheck disable=SC1091
    source "${REPO_ROOT}/.venv/bin/activate"
    PYTEST_BIN="${REPO_ROOT}/.venv/bin/pytest"
elif [ -f "${REPO_ROOT}/.venv/Scripts/activate" ]; then
    # shellcheck disable=SC1091
    source "${REPO_ROOT}/.venv/Scripts/activate"
    PYTEST_BIN="${REPO_ROOT}/.venv/Scripts/pytest"
elif [ -f "${VENV_DIR}/bin/activate" ]; then
    # shellcheck disable=SC1091
    source "${VENV_DIR}/bin/activate"
    PYTEST_BIN="${VENV_DIR}/bin/pytest"
elif [ -f "${VENV_DIR}/Scripts/activate" ]; then
    # shellcheck disable=SC1091
    source "${VENV_DIR}/Scripts/activate"
    PYTEST_BIN="${VENV_DIR}/Scripts/pytest"
elif command -v pytest >/dev/null 2>&1; then
    PYTEST_BIN="$(command -v pytest)"
else
    echo "Error: pytest could not be found in ${REPO_ROOT}/.venv, ${VENV_DIR}, or PATH." >&2
    exit 1
fi

echo "Using pytest: ${PYTEST_BIN}"
echo "Running UI test suite..."

# Check if pytest-xdist is available and auto-parallelize unless already requested
XDIST_ARGS=()
if "${PYTEST_BIN}" -h 2>/dev/null | grep -q -- "-n"; then
    HAS_N=false
    for arg in "$@"; do
        if [[ "$arg" == -n* ]]; then
            HAS_N=true
            break
        fi
    done
    if [ "$HAS_N" = false ]; then
        XDIST_ARGS=(-n auto --dist loadfile)
    fi
fi

if [ "$#" -gt 0 ]; then
    "${PYTEST_BIN}" "${XDIST_ARGS[@]}" -v "$@"
else
    "${PYTEST_BIN}" "${XDIST_ARGS[@]}" tests/ui -v
fi
