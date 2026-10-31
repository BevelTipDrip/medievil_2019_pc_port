#!/usr/bin/env bash
# Lab build with PDB symbols for WinDbg (the normal build's DWARF is unreadable to it), in its own
# CMake tree out/gpu-pdb so the normal out/ binaries (possibly running) are never touched.
# Emits DWARF and CodeView both (llvm-symbolizer / scripts keep working). Log: out/gpu-pdb/build.log.
#   bash research/tools/build-pdb.sh            build into out/gpu-pdb
#   bash research/tools/build-pdb.sh --install  then copy exe + pdb over out/ (the game must be closed)
# From PowerShell: $env:MSYSTEM='CLANG64'; & C:\msys64\usr\bin\bash.exe -l /c/Users/Ty/MediEvil/medievil_2019_pc_port/research/tools/build-pdb.sh
cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 1
case $(uname -s) in MINGW*|MSYS*) ;; *) echo 'Windows (MSYS2 CLANG64) only.' >&2; exit 1 ;; esac
source ./msys2-env.sh
tree=out/gpu-pdb
mkdir -p "$tree"
(
    echo "start: $(date)  commit=$(git rev-parse --short HEAD)"
    # Same configuration as build.sh's Windows branch, plus the debug-info flags.
    cmake -S gpu -B "$tree" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGOW3_PGO=off \
        -DGOW3_LTO="${GOW3_LTO:-ON}" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_C_FLAGS="-gdwarf -gcodeview" \
        -DCMAKE_CXX_FLAGS="-include cstdlib -include cstdio -include cwchar -include cmath -include new -gdwarf -gcodeview" \
        -DCMAKE_EXE_LINKER_FLAGS="-Wl,--pdb=" >/dev/null &&
    ninja -C "$tree" medievil-runtime medievil-gpu-check medievil-play
    status=$?
    echo "ninja exit: $status  end: $(date)"
    exit $status
) > "$tree/build.log" 2>&1 || { tail -20 "$tree/build.log" >&2; exit 1; }
ls -l "$tree"/medievil-runtime.exe "$tree"/medievil-runtime.pdb
if [[ ${1:-} == --install ]]; then
    if tasklist //FI "IMAGENAME eq medievil-runtime.exe" | grep -qi medievil-runtime; then
        echo 'medievil-runtime.exe is running: close the game first.' >&2; exit 1
    fi
    cp "$tree"/medievil-runtime.exe "$tree"/medievil-runtime.pdb out/
    echo "Installed PDB build into out/"
fi
