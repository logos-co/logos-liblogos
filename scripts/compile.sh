#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/compile.sh [options]

Options:
  --build-dir <path>      Build directory (default: ./build)
  --build-type <type>     CMake build type (default: RelWithDebInfo)
  --sdk-root <path>       Path to a logos-cpp-sdk checkout (optional)
  --target <name>         Build only the given CMake target (may repeat)
  --qt-prefix <path>      Qt install prefix forwarded to CMake (default: QT_DIR/QT_PREFIX env)
  --clean                 Remove the build directory before configuring
  -h, --help              Show this help text

Environment:
  BUILD_DIR, BUILD_TYPE, LOGOS_CPP_SDK_ROOT, QT_DIR, QT_PREFIX can override defaults.
USAGE
}

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build"}
BUILD_TYPE=${BUILD_TYPE:-RelWithDebInfo}
DEFAULT_SDK_ROOT="$ROOT_DIR/vendor/logos-cpp-sdk"
if [[ -d "$DEFAULT_SDK_ROOT" ]]; then
  DEFAULT_SDK_ROOT=$(cd "$DEFAULT_SDK_ROOT" && pwd)
else
  DEFAULT_SDK_ROOT=""
fi
SDK_ROOT=${LOGOS_CPP_SDK_ROOT:-}
CLEAN=0
TARGETS=()
QT_PREFIX_INPUT=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      [[ $# -lt 2 ]] && { echo "Missing value for $1" >&2; exit 1; }
      BUILD_DIR=$2
      shift 2
      ;;
    --build-type)
      [[ $# -lt 2 ]] && { echo "Missing value for $1" >&2; exit 1; }
      BUILD_TYPE=$2
      shift 2
      ;;
    --sdk-root)
      [[ $# -lt 2 ]] && { echo "Missing value for $1" >&2; exit 1; }
      SDK_ROOT=$2
      shift 2
      ;;
    --target)
      [[ $# -lt 2 ]] && { echo "Missing value for $1" >&2; exit 1; }
      TARGETS+=("$2")
      shift 2
      ;;
    --qt-prefix)
      [[ $# -lt 2 ]] && { echo "Missing value for $1" >&2; exit 1; }
      QT_PREFIX_INPUT=$2
      shift 2
      ;;
    --clean)
      CLEAN=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -z "$SDK_ROOT" && -n "$DEFAULT_SDK_ROOT" ]]; then
  SDK_ROOT="$DEFAULT_SDK_ROOT"
fi

if [[ -n "$SDK_ROOT" ]]; then
  if [[ ! -d "$SDK_ROOT" ]]; then
    echo "logos-cpp-sdk not found at: $SDK_ROOT" >&2
    exit 1
  fi
  SDK_ROOT=$(cd "$SDK_ROOT" && pwd)
fi

if [[ -z "$QT_PREFIX_INPUT" ]]; then
  if [[ -n "${QT_PREFIX:-}" ]]; then
    QT_PREFIX_INPUT=${QT_PREFIX}
  elif [[ -n "${QT_DIR:-}" ]]; then
    QT_PREFIX_INPUT=${QT_DIR}
  fi
fi

QT_PREFIX_PATHS=()
QT_PREFIX_FIRST=""
QT_PREFIX_CMAKE=""
if [[ -n "$QT_PREFIX_INPUT" ]]; then
  IFS=':' read -r -a _qt_paths <<<"$QT_PREFIX_INPUT"
  for _path in "${_qt_paths[@]}"; do
    [[ -z "$_path" ]] && continue
    if [[ ! -d "$_path" ]]; then
      echo "Qt prefix not found at: $_path" >&2
      exit 1
    fi
    _abs_path=$(cd "$_path" && pwd)
    QT_PREFIX_PATHS+=("$_abs_path")
  done
  if (( ${#QT_PREFIX_PATHS[@]} == 0 )); then
    echo "Qt prefix list resolved to empty set" >&2
    exit 1
  fi
  QT_PREFIX_FIRST=${QT_PREFIX_PATHS[0]}
  QT_PREFIX_CMAKE=$(printf "%s;" "${QT_PREFIX_PATHS[@]}")
  QT_PREFIX_CMAKE=${QT_PREFIX_CMAKE%;}
fi

if [[ -d "$BUILD_DIR" ]]; then
  cache_file="$BUILD_DIR/CMakeCache.txt"
  if [[ -f "$cache_file" ]]; then
    cached_home=$(sed -n 's/^CMAKE_HOME_DIRECTORY:.*=\(.*\)$/\1/p' "$cache_file")
    if [[ -n "$cached_home" && "$cached_home" != "$ROOT_DIR" ]]; then
      echo "Removing stale CMake cache at $BUILD_DIR (points to $cached_home)"
      rm -rf "$BUILD_DIR"
    elif [[ -n "$QT_PREFIX_FIRST" ]]; then
      cached_qt=$(sed -n 's/^Qt[56]_DIR:PATH=\(.*\)$/\1/p' "$cache_file" | head -n1)
      if [[ -n "$cached_qt" && "$cached_qt" != "$QT_PREFIX_FIRST" ]]; then
        echo "Removing stale CMake cache at $BUILD_DIR (Qt from $cached_qt)"
        rm -rf "$BUILD_DIR"
      fi
    fi
  fi
fi

if [[ $CLEAN -eq 1 && -d "$BUILD_DIR" ]]; then
  echo "Removing build directory: $BUILD_DIR"
  rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"
BUILD_DIR=$(cd "$BUILD_DIR" && pwd)

cmake_args=(
  -S "$ROOT_DIR"
  -B "$BUILD_DIR"
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
)

if [[ -n "$SDK_ROOT" ]]; then
  cmake_args+=(-DLOGOS_CPP_SDK_ROOT="$SDK_ROOT")
fi

if [[ -n "$QT_PREFIX_CMAKE" ]]; then
  cmake_args+=(-DCMAKE_PREFIX_PATH="$QT_PREFIX_CMAKE")
fi

echo "Configuring with: ${cmake_args[*]}"
cmake "${cmake_args[@]}"

build_cmd=(cmake --build "$BUILD_DIR" --parallel)
if (( ${#TARGETS[@]} )); then
  for target in "${TARGETS[@]}"; do
    build_cmd+=(--target "$target")
  done
fi

echo "Building with: ${build_cmd[*]}"
"${build_cmd[@]}"
