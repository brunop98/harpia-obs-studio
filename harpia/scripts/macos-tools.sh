#!/bin/bash
# Sourced by build-macos.sh. Compatible with macOS's bundled Bash 3.2.
# Discovery only: never installs software or changes global xcode-select state.

harpia_macos_version_at_least() {
  local actual=$1 minimum=$2 index have need
  local version_pattern='^[0-9]+([.][0-9]+)*$'
  [[ $actual =~ $version_pattern && $minimum =~ $version_pattern ]] || return 1
  local -a actual_parts=(0) minimum_parts=(0)
  local IFS=.
  read -r -a actual_parts <<< "$actual"
  read -r -a minimum_parts <<< "$minimum"
  for ((index=0; index<3; index++)); do
    have=$((10#${actual_parts[index]:-0}))
    need=$((10#${minimum_parts[index]:-0}))
    ((have > need)) && return 0
    ((have < need)) && return 1
  done
  return 0
}

harpia_macos_search_dirs() {
  local directory
  local -a path_dirs=(.)
  local IFS=:
  read -r -a path_dirs <<< "${PATH:-}"
  for directory in "${path_dirs[@]}"; do
    [[ -n $directory ]] && printf '%s\n' "$directory"
  done
  for directory in /opt/homebrew/bin /usr/local/bin /opt/local/bin \
      /Applications/CMake.app/Contents/bin "${HOME}/Applications/CMake.app/Contents/bin" \
      /Library/Frameworks/Python.framework/Versions/Current/bin \
      /Library/Frameworks/Python.framework/Versions/3.*/bin \
      /opt/homebrew/opt/python@*/libexec/bin /usr/local/opt/python@*/libexec/bin \
      /usr/bin /bin /usr/sbin /sbin; do
    [[ -d $directory ]] && printf '%s\n' "$directory"
  done
  return 0
}

# Prints one compatible executable. A too-old PATH entry does not hide a newer
# supported installation, and paths containing spaces remain single arguments.
harpia_macos_find_tool() {
  local name=$1 minimum=${2:-} directory candidate version
  while IFS= read -r directory; do
	[[ -d $directory ]] || continue
	directory=$(cd "$directory" && pwd) || continue
    candidate="$directory/$name"
    [[ -x $candidate && ! -d $candidate ]] || continue
    case "$name" in
      cmake|ctest)
        version=$("$candidate" --version 2>/dev/null | sed -n '1s/.*version \([0-9][0-9.]*\).*/\1/p')
        harpia_macos_version_at_least "$version" "$minimum" || continue ;;
      python3)
        version=$("$candidate" -c 'import sys; print(".".join(map(str, sys.version_info[:3])))' 2>/dev/null) || continue
        harpia_macos_version_at_least "$version" "$minimum" || continue ;;
      git) "$candidate" --version >/dev/null 2>&1 || continue ;;
      ffmpeg) "$candidate" -version >/dev/null 2>&1 || continue ;;
    esac
    printf '%s\n' "$candidate"
    return 0
  done < <(harpia_macos_search_dirs)
  return 1
}

harpia_macos_problem() {
  printf '\nMissing or unusable: %s\nDownload: %s\n' "$1" "$2" >&2
}

harpia_macos_try_xcode() {
  local developer=$1 minimum_xcode=$2 minimum_sdk=$3 version sdk_version sdk_path
  [[ $developer == *.app ]] && developer="$developer/Contents/Developer"
  [[ -d $developer && -x "$developer/usr/bin/xcodebuild" ]] || return 1
  developer=$(cd "$developer" && pwd) || return 1
  version=$(DEVELOPER_DIR="$developer" "$developer/usr/bin/xcodebuild" -version 2>/dev/null |
    sed -n 's/^Xcode \([0-9][0-9.]*\).*/\1/p')
  harpia_macos_version_at_least "$version" "$minimum_xcode" || return 1
  sdk_version=$(DEVELOPER_DIR="$developer" xcrun --sdk macosx --show-sdk-platform-version 2>/dev/null) || return 1
  harpia_macos_version_at_least "$sdk_version" "$minimum_sdk" || return 1
  sdk_path=$(DEVELOPER_DIR="$developer" xcrun --sdk macosx --show-sdk-path 2>/dev/null) || return 1
  [[ -d $sdk_path ]] || return 1
  export DEVELOPER_DIR="$developer"
  export HARPIA_MAC_SDK="$sdk_path"
  printf 'Found Xcode %s and macOS SDK %s: %s\n' "$version" "$sdk_version" "$developer"
}

harpia_macos_check_tools() {
  local repo=$1 run_tests=${2:-false} directory active developer clang_path tool
  local minimum_xcode minimum_sdk minimum_cmake problems=0 found_xcode=false
  local apple_url=https://developer.apple.com/xcode/
  [[ $(uname -s) == Darwin ]] || { echo 'This build requires macOS and full Xcode.' >&2; return 1; }

  # Finder launches with a minimal PATH. Add known installations for helper
  # processes, then select version-compatible CMake/Python explicitly below.
  while IFS= read -r directory; do
    case ":${PATH:-}:" in
      *":$directory:"*) ;;
      *) PATH="${PATH:+$PATH:}$directory" ;;
    esac
  done < <(harpia_macos_search_dirs)
  export PATH

  minimum_xcode=$(sed -n 's/^[[:space:]]*set(obs_macos_minimum_xcode \([0-9][0-9.]*\)).*/\1/p' "$repo/cmake/macos/compilerconfig.cmake")
  minimum_sdk=$(sed -n 's/^[[:space:]]*set(obs_macos_minimum_sdk \([0-9][0-9.]*\)).*/\1/p' "$repo/cmake/macos/compilerconfig.cmake")
  minimum_cmake=$(sed -nE 's/^cmake_minimum_required\(VERSION ([0-9]+\.[0-9]+(\.[0-9]+)?).*/\1/p' "$repo/CMakeLists.txt")
  if [[ -z $minimum_xcode || -z $minimum_sdk || -z $minimum_cmake ]]; then
    echo "Could not read this checkout's compiler requirements. Restore its CMake configuration files." >&2
    return 1
  fi

  if [[ -n ${DEVELOPER_DIR:-} ]]; then
    if harpia_macos_try_xcode "$DEVELOPER_DIR" "$minimum_xcode" "$minimum_sdk"; then found_xcode=true; fi
  else
    active=$(xcode-select -p 2>/dev/null || true)
    if harpia_macos_try_xcode "$active" "$minimum_xcode" "$minimum_sdk"; then
      found_xcode=true
    else
      for developer in /Applications/Xcode*.app "${HOME}"/Applications/Xcode*.app; do
        if harpia_macos_try_xcode "$developer" "$minimum_xcode" "$minimum_sdk"; then
          found_xcode=true
          break
        fi
      done
    fi
  fi
  if ! $found_xcode; then
    harpia_macos_problem "Full Xcode $minimum_xcode+ with macOS SDK $minimum_sdk+ (Command Line Tools alone are insufficient)" "$apple_url"
    printf 'Additional Xcode downloads: https://developer.apple.com/download/all/?q=Xcode\n' >&2
    [[ -z ${DEVELOPER_DIR:-} ]] || printf 'Check your DEVELOPER_DIR setting: %s\n' "$DEVELOPER_DIR" >&2
    problems=$((problems + 1))
  else
    clang_path=$(xcrun --find clang 2>/dev/null || true)
    if [[ ! -x $clang_path ]] || ! "$clang_path" --version >/dev/null 2>&1; then
      harpia_macos_problem 'the selected Xcode C/C++ compiler (clang)' "$apple_url"
      problems=$((problems + 1))
    fi
    if ! "$DEVELOPER_DIR/usr/bin/xcodebuild" -checkFirstLaunchStatus >/dev/null 2>&1; then
      printf '\nXcode setup is incomplete. Open Xcode once, accept its license, and finish installing its components, then run this script again.\n' >&2
      printf 'Xcode setup help: https://developer.apple.com/documentation/xcode/configuring-command-line-tools-settings\n' >&2
      problems=$((problems + 1))
    fi
  fi

  if HARPIA_MAC_CMAKE=$(harpia_macos_find_tool cmake "$minimum_cmake"); then
    export HARPIA_MAC_CMAKE
    printf 'Found CMake: %s\n' "$HARPIA_MAC_CMAKE"
  else
    harpia_macos_problem "CMake $minimum_cmake+" https://cmake.org/download/
    problems=$((problems + 1))
  fi
  if HARPIA_MAC_PYTHON=$(harpia_macos_find_tool python3 3.10); then
    export HARPIA_MAC_PYTHON
    # Both CMake's interpreter and shell-based packaging use this exact Python.
    PATH="$(dirname "$HARPIA_MAC_PYTHON"):$PATH"
    export PATH
    printf 'Found Python: %s\n' "$HARPIA_MAC_PYTHON"
  else
    harpia_macos_problem 'Python 3.10+' https://www.python.org/downloads/macos/
    problems=$((problems + 1))
  fi
  if HARPIA_MAC_GIT=$(harpia_macos_find_tool git); then
    export HARPIA_MAC_GIT
    printf 'Found Git: %s\n' "$HARPIA_MAC_GIT"
  else
    harpia_macos_problem 'Git (or Xcode first-run setup is incomplete)' https://git-scm.com/download/mac
    problems=$((problems + 1))
  fi
  for tool in codesign sips iconutil otool install_name_tool lipo open; do
    if ! command -v "$tool" >/dev/null 2>&1; then
      harpia_macos_problem "Apple's $tool utility (included with macOS/Xcode developer tools)" "$apple_url"
      problems=$((problems + 1))
    fi
  done
  if $run_tests; then
    HARPIA_MAC_CTEST="$(dirname "${HARPIA_MAC_CMAKE:-cmake}")/ctest"
    local ctest_version
    ctest_version=$("$HARPIA_MAC_CTEST" --version 2>/dev/null | sed -n '1s/.*version \([0-9][0-9.]*\).*/\1/p') || true
    if [[ -z ${HARPIA_MAC_CMAKE:-} || ! -x $HARPIA_MAC_CTEST ]] ||
        ! harpia_macos_version_at_least "$ctest_version" "$minimum_cmake"; then
      harpia_macos_problem 'CTest beside the selected CMake installation' https://cmake.org/download/
      problems=$((problems + 1))
    else
      export HARPIA_MAC_CTEST
    fi
    if HARPIA_MAC_FFMPEG=$(harpia_macos_find_tool ffmpeg); then
      export HARPIA_MAC_FFMPEG
      PATH="$(dirname "$HARPIA_MAC_FFMPEG"):$PATH"
      export PATH
      printf 'Found FFmpeg CLI: %s\n' "$HARPIA_MAC_FFMPEG"
    else
      harpia_macos_problem 'FFmpeg command-line tool for media tests (--test)' https://ffmpeg.org/download.html#build-mac
      printf 'Homebrew formula: https://formulae.brew.sh/formula/ffmpeg\n' >&2
      problems=$((problems + 1))
    fi
  fi
  if ((problems > 0)); then
    printf '\nBuild not started: fix the %s issue(s) above and run this script again.\n' "$problems" >&2
    return 1
  fi
  printf 'All required tools are ready. Qt and OBS/FFmpeg build libraries are downloaded by CMake.\n'
}
