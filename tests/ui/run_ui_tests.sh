#!/usr/bin/env bash
set -e

# Turnkey runner script for IMAGINE deterministic UI tests.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

echo "=== IMAGINE Deterministic UI Test Runner ==="
echo "Repository Root: ${REPO_ROOT}"

# 1. Ensure C++ binary is built and up-to-date
IMAGINE_BIN="${REPO_ROOT}/build/imagine"
cmake -B "${REPO_ROOT}/build" -S "${REPO_ROOT}"
cmake --build "${REPO_ROOT}/build" --target imagine -j

if [ ! -x "${IMAGINE_BIN}" ]; then
    echo "Error: ${IMAGINE_BIN} is not executable." >&2
    exit 1
fi
echo "Using binary: ${IMAGINE_BIN}"

# 2. Locate virtual environment python/pytest
VENV_DIR="${REPO_ROOT}/build/test_venv"
if [ -f "${VENV_DIR}/bin/activate" ]; then
    # shellcheck disable=SC1091
    source "${VENV_DIR}/bin/activate"
    PYTEST_BIN="${VENV_DIR}/bin/pytest"
elif command -v pytest >/dev/null 2>&1; then
    PYTEST_BIN="$(command -v pytest)"
else
    echo "Error: pytest could not be found in ${VENV_DIR} or PATH." >&2
    exit 1
fi

echo "Using pytest: ${PYTEST_BIN}"
echo "Running UI test suite..."

cd "${REPO_ROOT}"
"${PYTEST_BIN}" tests/ui -v "$@"
