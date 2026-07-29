#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST_DIR="$ROOT_DIR/dist"
VERSION="${VERSION:-$(git -C "$ROOT_DIR" describe --tags --always)}"

# 检测当前平台，用于判断是否进行 native 构建（无需工具链文件）
case "$(uname -s)" in
  Linux) current_os="linux";;
  Darwin) current_os="darwin";;
  MINGW*|MSYS*|CYGWIN*) current_os="windows";;
  *) current_os="$(uname -s)";;
esac
case "$(uname -m)" in
  x86_64|amd64) current_arch="amd64";;
  aarch64|arm64) current_arch="arm64";;
  *) current_arch="$(uname -m)";;
esac

mkdir -p "$DIST_DIR"

build_target() {
  local os="$1"
  local arch="$2"
  local suffix=""
  if [[ "$os" == "windows" ]]; then
    suffix=".exe"
  fi

  local build_dir="$ROOT_DIR/build-${os}-${arch}"
  local binary_name="co_${VERSION}_${os}_${arch}${suffix}"
  local binary_path="${build_dir}/co${suffix}"
  local toolchain_arg=()

  if [[ "$os" == "$current_os" && "$arch" == "$current_arch" ]]; then
    # 当前平台：native 构建，不需要工具链文件
    :
  else
    local toolchain_file="$ROOT_DIR/scripts/toolchains/${os}-${arch}.cmake"
    if [[ ! -f "$toolchain_file" ]]; then
      echo "warning: toolchain file not found for ${os}/${arch} (${toolchain_file}), skipping" >&2
      return 0
    fi
    toolchain_arg=(-DCMAKE_TOOLCHAIN_FILE="$toolchain_file")
  fi

  echo "==> Building ${os}/${arch}"
  cmake -B "$build_dir" -S "$ROOT_DIR" -DVERSION="${VERSION}" ${toolchain_arg[@]+"${toolchain_arg[@]}"}
  cmake --build "$build_dir"

  cp "$binary_path" "$DIST_DIR/$binary_name"
  if [[ "$os" == "windows" ]]; then
    (cd "$DIST_DIR" && zip -q "${binary_name}.zip" "$binary_name" && rm "$binary_name")
  else
    (cd "$DIST_DIR" && tar -czf "${binary_name}.tar.gz" "$binary_name" && rm "$binary_name")
  fi

  rm -rf "$build_dir"
}

build_target linux amd64
build_target linux arm64
build_target darwin amd64
build_target darwin arm64
build_target windows amd64

if [[ "${PUBLISH_RELEASE:-}" == "1" ]]; then
  if ! command -v gh >/dev/null 2>&1; then
    echo "gh CLI is required to publish a release" >&2
    exit 1
  fi
  gh release create "$VERSION" "$DIST_DIR"/* --generate-notes
fi
