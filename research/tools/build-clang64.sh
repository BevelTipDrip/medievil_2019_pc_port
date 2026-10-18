#!/usr/bin/env bash
# Builds the port in MSYS2 CLANG64 and writes out/build.log.
# From PowerShell (no quoting through PowerShell 5.1, which strips inner double quotes):
#   $env:MSYSTEM='CLANG64'; & C:\msys64\usr\bin\bash.exe -l /c/Users/Ty/MediEvil/medievil_2019_pc_port/research/tools/build-clang64.sh
cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 1
mkdir -p out
(
    echo "start: $(date)  MSYSTEM=$MSYSTEM  clang=$(command -v clang)  commit=$(git rev-parse --short HEAD)"
    CXXFLAGS="-include cstdlib -include cstdio -include cwchar -include cmath -include new" bash build.sh "$@"
    status=$?
    echo "build.sh exit: $status  end: $(date)"
    exit $status
) > out/build.log 2>&1
