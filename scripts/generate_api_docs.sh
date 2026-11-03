#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if ! command -v doxygen >/dev/null 2>&1; then
  echo "Error: doxygen is not installed. Install it via: brew install doxygen (macOS) or apt-get install doxygen (Linux)." >&2
  exit 1
fi

OUT_DIR="api-docs/_build/html"
rm -rf "$OUT_DIR" "api-docs/_build/xml"

doxygen Doxyfile | cat

echo "Doxygen HTML docs: $OUT_DIR/index.html"
