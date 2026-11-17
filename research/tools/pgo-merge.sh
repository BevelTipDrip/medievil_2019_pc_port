#!/usr/bin/env bash
# Clang PGO (card tooling/clang-pgo): merge a training session's raw profiles
# (pgo/<platform>/raw/*.profraw, written by out/medievil-runtime-pgo-train.exe) into
# pgo/<platform>/medievil.profdata, which the release build then uses (build.sh). Run from MSYS2
# CLANG64 (llvm-profdata). Usage: research/tools/pgo-merge.sh [windows]
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
platform=${1:-windows}
dir=pgo/$platform
command -v llvm-profdata >/dev/null || { echo 'Need llvm-profdata (MSYS2 CLANG64: mingw-w64-clang-x86_64-llvm).' >&2; exit 1; }
shopt -s nullglob
raw=("$dir"/raw/*.profraw)
(( ${#raw[@]} )) || { echo "No .profraw files in $dir/raw: run a training session first." >&2; exit 1; }
ls -la "${raw[@]}"
llvm-profdata merge -o "$dir/medievil.profdata.new" "${raw[@]}"
mv -f "$dir/medievil.profdata.new" "$dir/medievil.profdata"
llvm-profdata show "$dir/medievil.profdata" | tail -6
echo "Wrote $PWD/$dir/medievil.profdata: build.sh (release) now uses it."
