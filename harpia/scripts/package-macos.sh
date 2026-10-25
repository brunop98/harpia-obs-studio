#!/bin/bash
# Verify an independently relocated app, then create a distributable ZIP.
# Set HARPIA_NOTARY_PROFILE to an existing notarytool keychain profile to notarize.
set -euo pipefail
script_dir=$(cd "$(dirname "$0")" && pwd)
app=
output=
notary_profile=${HARPIA_NOTARY_PROFILE:-}
check_runtime=false
python=${HARPIA_MAC_PYTHON:-python3}
usage() { echo "Usage: package-macos.sh --app Harpia.app [--output directory] [--notary-profile keychain-profile] [--check-runtime]"; }
while [[ $# -gt 0 ]]; do
  case "$1" in
    --app) app=${2:?Missing app}; shift 2 ;;
    --output) output=${2:?Missing output directory}; shift 2 ;;
    --notary-profile) notary_profile=${2:?Missing profile}; shift 2 ;;
    --check-runtime) check_runtime=true; shift ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done
[[ $(uname -s) == Darwin ]] || { echo "Packaging requires macOS's signing and bundle tools." >&2; exit 1; }
[[ -n "$app" && -d "$app/Contents/MacOS" ]] || { usage >&2; exit 2; }
app=$(cd "$app" && pwd)
output=${output:-"$(dirname "$app")/dist"}
mkdir -p "$output"
output=$(cd "$output" && pwd)
package_work=$(mktemp -d)
trap 'rm -rf "$package_work"' EXIT
# A different directory catches accidental build-tree/CWD dependencies. The
# verifier reads all Mach-O load commands; --check-runtime also launches it.
relocated="$package_work/Harpia.app"
ditto "$app" "$relocated"
executable=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$relocated/Contents/Info.plist")
version=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$relocated/Contents/Info.plist")
architectures=$(lipo -archs "$relocated/Contents/MacOS/$executable")
verify_args=()
for architecture in $architectures; do verify_args+=(--architecture "$architecture"); done
"$python" "$script_dir/deploy-macos.py" --verify-only --app "$relocated" "${verify_args[@]}"
architecture_label=$(echo "$architectures" | tr ' ' '-')
archive="$output/Harpia-$version-macos-$architecture_label.zip"
if $check_runtime; then
  "$python" - "$relocated/Contents/MacOS/$executable" "$archive.runtime.json" <<'PYRUNTIME'
import subprocess, sys
with open(sys.argv[2], "w") as report:
    subprocess.run([sys.argv[1], "--check-runtime"], stdout=report, timeout=90, check=True)
PYRUNTIME
fi
ditto -c -k --sequesterRsrc --keepParent "$relocated" "$archive"
if [[ -n "$notary_profile" ]]; then
  signature=$(codesign -d --verbose=4 "$relocated" 2>&1)
  if ! [[ "$signature" == *'Authority=Developer ID Application:'* ]]; then
    echo "Notarization requires the app to be built with a Developer ID Application signing identity." >&2
    exit 1
  fi
  report="$output/Harpia-$version-notarization.json"
  xcrun notarytool submit "$archive" --keychain-profile "$notary_profile" --wait --output-format json > "$report"
  "$python" - "$report" <<'PY'
import json, sys
with open(sys.argv[1]) as file:
    report = json.load(file)
if report.get("status") != "Accepted":
    raise SystemExit("Apple did not accept notarization; inspect " + sys.argv[1])
PY
  xcrun stapler staple "$relocated"
  xcrun stapler validate "$relocated"
  spctl --assess --type execute --verbose=2 "$relocated"
  ditto -c -k --sequesterRsrc --keepParent "$relocated" "$archive"
else
  echo "Notarization was not requested. Ad-hoc/development builds are for local testing, not a notarized public release."
fi
shasum -a 256 "$archive" > "$archive.sha256"
echo "Verified package: $archive"
