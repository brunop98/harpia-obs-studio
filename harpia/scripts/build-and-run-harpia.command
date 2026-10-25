#!/bin/bash
# Double-click in Finder, or pass the same arguments as the .sh launcher.
script_dir=$(cd "$(dirname "$0")" && pwd) || exit 1
status=0
"$script_dir/build-and-run-harpia.sh" "$@" || status=$?
if [[ $status != 0 && -t 0 && -t 1 ]]; then
  printf '\nPress Return to close this window...'
  read -r answer || true
fi
exit "$status"
