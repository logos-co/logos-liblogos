#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/run.sh [options] [-- <logoscore args>]

Options:
  --build-dir <path>      Build directory (default: ./build)
  --build-type <type>     CMake build type (default: RelWithDebInfo)
  --sdk-root <path>       Path to a logos-cpp-sdk checkout (optional)
  --qt-prefix <path>      Qt install prefix forwarded to compile.sh (default: QT_DIR/QT_PREFIX env)
  --modules <path>        Plugins directory to expose via LOGOS_PLUGINS_DIR
  --no-build              Skip the build step
  -h, --help              Show this help text
USAGE
}

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build"}
BUILD_TYPE=${BUILD_TYPE:-RelWithDebInfo}
SDK_ROOT=${LOGOS_CPP_SDK_ROOT:-}
MODULES_DIR=""
RUN_BUILD=1
RUNTIME_ARGS=()
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
    --qt-prefix)
      [[ $# -lt 2 ]] && { echo "Missing value for $1" >&2; exit 1; }
      QT_PREFIX_INPUT=$2
      shift 2
      ;;
    --modules)
      [[ $# -lt 2 ]] && { echo "Missing value for $1" >&2; exit 1; }
      MODULES_DIR=$2
      shift 2
      ;;
    --no-build)
      RUN_BUILD=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      RUNTIME_ARGS=("$@")
      break
      ;;
    *)
      RUNTIME_ARGS+=("$1")
      shift
      ;;
  esac
done

if [[ $RUN_BUILD -eq 1 ]]; then
  compile_cmd=("$ROOT_DIR/scripts/compile.sh" --build-dir "$BUILD_DIR" --build-type "$BUILD_TYPE")
  if [[ -n "$SDK_ROOT" ]]; then
    compile_cmd+=(--sdk-root "$SDK_ROOT")
  fi
  if [[ -n "$QT_PREFIX_INPUT" ]]; then
    compile_cmd+=(--qt-prefix "$QT_PREFIX_INPUT")
  fi
  "${compile_cmd[@]}"
fi

BUILD_DIR=$(cd "$BUILD_DIR" && pwd)
BIN_DIR="$BUILD_DIR/bin"
EXECUTABLE="$BIN_DIR/logoscore"

if [[ ! -x "$EXECUTABLE" ]]; then
  echo "Executable not found: $EXECUTABLE" >&2
  exit 1
fi

if [[ -z "$MODULES_DIR" && -d "$BUILD_DIR/modules" ]]; then
  MODULES_DIR="$BUILD_DIR/modules"
fi

if [[ -n "$MODULES_DIR" ]]; then
  if [[ ! -d "$MODULES_DIR" ]]; then
    echo "Plugins directory not found: $MODULES_DIR" >&2
    exit 1
  fi
  export LOGOS_PLUGINS_DIR=$(cd "$MODULES_DIR" && pwd)
  echo "Using plugins from $LOGOS_PLUGINS_DIR"
fi

echo "Running $EXECUTABLE ${RUNTIME_ARGS[*]}"
"$EXECUTABLE" "${RUNTIME_ARGS[@]}"
