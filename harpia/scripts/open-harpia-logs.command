#!/bin/bash
# Finder launcher: works even while Harpia's UI is busy or not running.
set -euo pipefail
logs_dir="$HOME/Library/Application Support/harpia-recorder/logs"
mkdir -p "$logs_dir"
exec /usr/bin/open "$logs_dir"
