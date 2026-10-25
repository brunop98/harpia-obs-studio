#!/bin/bash
# Build a native or universal Mac app, deploy its runtime and optionally test/package.
set -euo pipefail
script_dir=$(cd "$(dirname "$0")" && pwd)
repo_dir=$(cd "$script_dir/../.." && pwd)
configuration=RelWithDebInfo
architecture=$(uname -m)
build_dir="$repo_dir/build_macos"
preset=macos
identity=${CODESIGN_IDENT:--}
run_app=false
package_app=false
run_tests=false
check_runtime=false
check_tools=false
usage() {
  echo "Usage: build-macos.sh [--arch arm64|x86_64|universal] [--configuration Release|RelWithDebInfo|Debug|MinSizeRel]"
  echo "                      [--build-dir path] [--preset macos|macos-ci] [--identity codesign-id] [--test] [--package] [--check-runtime] [--check-tools] [--run]"
}
while [[ $# -gt 0 ]]; do
  case "$1" in
    --arch) architecture=${2:?Missing architecture}; shift 2 ;;
    --configuration) configuration=${2:?Missing configuration}; shift 2 ;;
    --build-dir) build_dir=${2:?Missing build directory}; shift 2 ;;
    --preset) preset=${2:?Missing preset}; shift 2 ;;
    --identity) identity=${2:?Missing signing identity}; shift 2 ;;
    --test) run_tests=true; shift ;;
    --check-runtime) check_runtime=true; package_app=true; shift ;;
    --check-tools) check_tools=true; shift ;;
    --package) package_app=true; shift ;;
    --run) run_app=true; shift ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done
[[ $(uname -s) == Darwin ]] || { echo "This build requires macOS and full Xcode." >&2; exit 1; }
case "$configuration" in Debug|Release|RelWithDebInfo|MinSizeRel) ;; *) echo "Invalid configuration" >&2; exit 2 ;; esac
case "$architecture" in
  arm64|x86_64) cmake_architectures=$architecture ;;
  universal) cmake_architectures='arm64;x86_64' ;;
  *) echo "Choose arm64, x86_64 or universal." >&2; exit 2 ;;
esac
case "$preset" in macos|macos-ci) ;; *) echo "Choose the macos or macos-ci preset." >&2; exit 2 ;; esac
source "$script_dir/macos-tools.sh"
harpia_macos_check_tools "$repo_dir" "$run_tests" || exit 1
if $check_tools; then exit 0; fi
test_args=()
if $run_tests; then test_args=("-DHARPIA_TEST_FFMPEG=$HARPIA_MAC_FFMPEG"); fi
mkdir -p "$build_dir"
build_dir=$(cd "$build_dir" && pwd)
cd "$repo_dir"
"$HARPIA_MAC_CMAKE" --preset "$preset" -B "$build_dir" \
  "-DCMAKE_OSX_ARCHITECTURES=$cmake_architectures" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
  "-DCMAKE_OSX_SYSROOT=$HARPIA_MAC_SDK" \
  "-DPython3_EXECUTABLE=$HARPIA_MAC_PYTHON" \
  "-DGIT_EXECUTABLE=$HARPIA_MAC_GIT" \
  "-DHARPIA_MAC_CODESIGN_IDENTITY=$identity" \
  "-DHARPIA_BUILD_TESTS=$($run_tests && echo ON || echo OFF)" \
  ${test_args[@]+"${test_args[@]}"}
"$HARPIA_MAC_CMAKE" --build "$build_dir" --config "$configuration" --target harpia-deploy \
  --parallel "${HARPIA_BUILD_JOBS:-4}"
app="$build_dir/harpia/$configuration/Harpia.app"
[[ -d "$app" ]] || { echo "The deployed app was not produced at: $app" >&2; exit 1; }
if $run_tests; then
  "$HARPIA_MAC_CMAKE" --build "$build_dir" --config "$configuration" --target harpia-tests \
    --parallel "${HARPIA_BUILD_JOBS:-4}"
  "$HARPIA_MAC_CTEST" --test-dir "$build_dir" -C "$configuration" --output-on-failure --no-tests=error
fi
if $package_app; then
  package_args=(--app "$app" --output "$build_dir/dist")
  if $check_runtime; then package_args+=(--check-runtime); fi
  "$script_dir/package-macos.sh" "${package_args[@]}"
fi
echo "Deployed application: $app"
if $run_app; then open "$app"; fi
