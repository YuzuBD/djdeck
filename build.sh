#!/usr/bin/env bash
# Build djdeck. The only requirement is a C++ toolchain: the portable LLVM-MinGW
# toolchain is fetched into tools/ on first run (no installation, no system deps).
set -e

ROOT="$(cd "$(dirname "$0")" && { pwd -W 2>/dev/null || pwd; })"
TC_VER="20260908"
TC_NAME="llvm-mingw-$TC_VER-ucrt-x86_64"
TC="$ROOT/tools/$TC_NAME"
CXX="$TC/bin/clang++.exe"
OUT="$ROOT/build"
mkdir -p "$OUT/obj"

if [ ! -x "$CXX" ]; then
  PY=""
  for c in python py python3; do
    if command -v "$c" >/dev/null 2>&1; then PY="$c"; break; fi
  done
  if [ -z "$PY" ]; then
    echo "tools/$TC_NAME is missing and no python was found to unpack it." >&2
    echo "Download https://github.com/mstorsjo/llvm-mingw/releases/download/$TC_VER/$TC_NAME.zip and extract it into tools/." >&2
    exit 1
  fi
  echo "  fetching LLVM-MinGW $TC_VER (~190 MB, one time) ..."
  mkdir -p "$ROOT/tools"
  curl -fL --retry 3 -o "$ROOT/tools/$TC_NAME.zip" \
       "https://github.com/mstorsjo/llvm-mingw/releases/download/$TC_VER/$TC_NAME.zip"
  "$PY" -c "import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" \
        "$ROOT/tools/$TC_NAME.zip" "$ROOT/tools"
  rm -f "$ROOT/tools/$TC_NAME.zip"
fi

CXXFLAGS=(-std=c++20 -O2 -Wall -Wno-unused-function -Wno-missing-field-initializers -static
          -I"$ROOT/src" -I"$ROOT/third_party" -I"$ROOT/third_party/imgui"
          -I"$ROOT/third_party/imgui/backends")
LIBS=(-ld3d11 -ldxgi -ld3dcompiler -luser32 -lgdi32 -ldwmapi -lcomdlg32 -lole32 -luuid
      -lshell32 -lwinmm -limm32)

SRC=(
  "$ROOT/third_party/imgui/imgui.cpp"
  "$ROOT/third_party/imgui/imgui_draw.cpp"
  "$ROOT/third_party/imgui/imgui_tables.cpp"
  "$ROOT/third_party/imgui/imgui_widgets.cpp"
  "$ROOT/third_party/imgui/imgui_demo.cpp"
  "$ROOT/third_party/imgui/backends/imgui_impl_win32.cpp"
  "$ROOT/third_party/imgui/backends/imgui_impl_dx11.cpp"
  "$ROOT/src/dj.cpp"
  "$ROOT/src/main.cpp"
)

OBJS=()
for f in "${SRC[@]}"; do
  o="$OUT/obj/$(basename "${f%.cpp}").o"
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ]; then
    echo "  CXX $(basename "$f")"
    "$CXX" "${CXXFLAGS[@]}" -c "$f" -o "$o"
  fi
  OBJS+=("$o")
done

echo "  LD  djdeck.exe"
"$CXX" "${CXXFLAGS[@]}" "${OBJS[@]}" "${LIBS[@]}" -o "$OUT/djdeck.exe"
echo "built $OUT/djdeck.exe"
