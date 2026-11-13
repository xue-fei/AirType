#!/usr/bin/env bash
# 下载 sherpa-onnx 预编译静态库（sherpa-onnx-sys 在编译期需要）。
#
# 约 118MB，不在 git 里。下载后 .cargo/config.toml 会自动指向
# vendor/sherpa-libs/，无需再设环境变量。
#
# 用法：  tools/fetch_sherpa_libs.sh
set -euo pipefail

VERSION="2.0.1"
ARCHIVE="sherpa-onnx-v${VERSION}-win-x64-static-MT-Release-lib.tar.bz2"
URL="https://github.com/k2-fsa/sherpa-onnx/releases/download/v${VERSION}/${ARCHIVE}"
DEST="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/vendor/sherpa-libs"

mkdir -p "$DEST"
echo "下载 $URL"
echo "  -> $DEST/$ARCHIVE"
curl -L --retry 3 --progress-bar -o "$DEST/$ARCHIVE" "$URL"
ls -lh "$DEST/$ARCHIVE"
echo
echo "完成。现在可以直接 cargo build / cargo test。"
