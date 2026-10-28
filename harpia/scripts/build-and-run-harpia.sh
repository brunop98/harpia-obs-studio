#!/bin/bash
# macOS counterpart to build-and-run-harpia.bat.
set -euo pipefail
script_dir=$(cd "$(dirname "$0")" && pwd)
args=()
case "${1:-}" in
  Debug|Release|RelWithDebInfo|MinSizeRel)
    args=(--configuration "$1")
    shift ;;
esac
if [[ ${1:-} == --help || ${1:-} == -h ]]; then
  echo 'Usage: build-and-run-harpia.sh [Debug|Release|RelWithDebInfo|MinSizeRel] [options]'
  echo 'Builds and opens Harpia.app. Use --check-tools to check tools without building.'
  echo 'Add --install to install into Applications and open the installed copy.'
fi
exec "$script_dir/build-macos.sh" --run ${args[@]+"${args[@]}"} "$@"
