#!/usr/bin/env bash
# Double-click launcher for the RecLight Link board-flasher GUI.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"
exec /usr/bin/python3 flash_gui.py
