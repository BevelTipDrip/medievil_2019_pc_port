#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds dist/MediEvil-x86_64.AppImage from the Linux build (run build.sh first).
#
# What is inside: medievil-runtime and libgow3gpu.so, the libraries a desktop may not have
# (SDL3, FFmpeg, xxHash, the Vulkan loader), a standalone Python 3.13 with Tk, Pillow and
# cryptography for the launcher and the preparation scripts, and the port's scripts and launcher.
# The host provides glibc and the GPU's Vulkan driver (Mesa, NVIDIA), as for a native game.
# libstdc++ is bundled as a fallback, used only when the host's is older than the build's.
#
# Usage: bash packaging/linux/appimage.sh [--python DIR] [--appimagetool FILE] [--runtime FILE]
#   --python DIR       an unpacked python-build-standalone "install_only" tree (python/ inside)
#                      with Pillow and cryptography installed; downloaded and set up when missing
#   --appimagetool     appimagetool-x86_64.AppImage (downloaded when missing)
#   --runtime          the type2 runtime (downloaded when missing)
# Libraries are looked up with ldd, so build.sh's PKG_CONFIG_PATH/LD_LIBRARY_PATH must also be
# set here when the dependencies live in a private prefix (MEDIEVIL_DEPS=/opt/mdeps does that).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
root=$PWD
python_dir='' tool='' runtime=''
while (( $# )); do
    case $1 in
        --python) python_dir=$2; shift 2 ;;
        --appimagetool) tool=$2; shift 2 ;;
        --runtime) runtime=$2; shift 2 ;;
        *) echo "Unknown option $1" >&2; exit 1 ;;
    esac
done
[[ -f out/medievil-runtime && -f out/gpu/libgow3gpu.so ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }
if [[ -n ${MEDIEVIL_DEPS:-} ]]; then
    export LD_LIBRARY_PATH=$MEDIEVIL_DEPS/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
fi
tools=out/appimage-tools
mkdir -p "$tools"
fetch() { [[ -s $2 ]] || curl -fsSL -o "$2" "$1"; }
if [[ -z $tool ]]; then
    tool=$tools/appimagetool
    fetch https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage "$tool"
    chmod +x "$tool"
fi
if [[ -z $runtime ]]; then
    runtime=$tools/runtime-x86_64
    fetch https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64 "$runtime"
fi
if [[ -z $python_dir ]]; then
    python_dir=$tools/python
    if [[ ! -x $python_dir/python/bin/python3 ]]; then
        tag=$(git ls-remote --tags --refs https://github.com/astral-sh/python-build-standalone | awk -F/ '{print $3}' | sort | tail -1)
        for v in 3.13.{20..10}; do
            url="https://github.com/astral-sh/python-build-standalone/releases/download/$tag/cpython-$v+$tag-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz"
            if curl -fsSL -o "$tools/python.tar.gz" "$url" 2>/dev/null; then break; fi
        done
        rm -rf "$python_dir" && mkdir -p "$python_dir" && tar -C "$python_dir" -xzf "$tools/python.tar.gz"
        "$python_dir/python/bin/python3" -m pip install --quiet pillow cryptography
    fi
fi
py=$python_dir/python
"$py/bin/python3" -I -c 'import tkinter, PIL.ImageTk, cryptography' \
    || { echo "$py: needs tkinter, Pillow and cryptography" >&2; exit 1; }

app=out/MediEvil.AppDir
rm -rf "$app"
share=$app/usr/share/medievil   # the port tree (PORT_DIR / PORT in the launcher and run.py)
lib=$app/usr/lib
mkdir -p "$share/bin/gpu" "$lib/compat" "$app/usr/share/fonts" "$app/usr/share/applications" \
    "$app/usr/share/icons/hicolor/256x256/apps" "$app/usr/share/metainfo"

# The port: scripts, launcher, patches, docs (no build tree, no Windows packaging).
cp -r run.py scripts launcher patches LICENSE README.md "$share/"
[[ -d docs ]] && cp -r docs "$share/"
mkdir -p "$share/gpu/third_party" && cp -r gpu/third_party/fonts "$share/gpu/third_party/"
mkdir -p "$share/packaging/linux" && cp packaging/linux/README-Linux.md "$share/packaging/linux/" 2>/dev/null || true
find "$share" -name '__pycache__' -prune -exec rm -rf {} +
# Native programs: run.py finds bin/medievil-runtime; its RUNPATH has $ORIGIN/gpu.
install -m755 out/medievil-runtime "$share/bin/"
install -m755 out/medievil-gpu-check "$share/bin/" 2>/dev/null || true
install -m755 out/gpu/libgow3gpu.so "$share/bin/gpu/"
patchelf=${PATCHELF:-$(command -v patchelf || echo "${MEDIEVIL_DEPS:-/opt/mdeps}/bin/patchelf")}
"$patchelf" --set-rpath '$ORIGIN/gpu:$ORIGIN/../../../lib' "$share/bin/medievil-runtime"
"$patchelf" --set-rpath '$ORIGIN:$ORIGIN/../../../../lib' "$share/bin/gpu/libgow3gpu.so"
[[ -f $share/bin/medievil-gpu-check ]] && "$patchelf" --set-rpath '$ORIGIN/../../../lib' "$share/bin/medievil-gpu-check"

# Bundled libraries: everything the programs need that is not part of a normal desktop.
# Host-provided: glibc, libstdc++/libgcc (fallback copy below), X11/xcb, zlib; GPU drivers.
host_lib() {
    case $1 in
        ld-linux*|libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|libresolv.so*|libutil.so*) return 0 ;;
        libstdc++.so*|libgcc_s.so*) return 0 ;;
        libX11.so*|libX11-xcb.so*|libxcb*.so*|libXau.so*|libXdmcp.so*|libXext.so*|libz.so*) return 0 ;;
        libbsd.so*|libmd.so*) return 0 ;;   # X11's own dependencies
        libgow3gpu.so) return 0 ;;          # installed beside the runtime (bin/gpu)
        libGL*|libEGL*|libdrm*|libnvidia*|libvulkan*) return 0 ;;  # GPU stack: the host's (one Vulkan loader)
    esac
    return 1
}
collect() {
    local elf=$1 name path
    while read -r name _ path _; do
        [[ $path == /* ]] || continue
        host_lib "$name" && continue
        if [[ ! -e $lib/$name ]]; then
            cp -L "$path" "$lib/$name"
            chmod 644 "$lib/$name"
            "$patchelf" --set-rpath '$ORIGIN' "$lib/$name" 2>/dev/null || true
            echo "  bundled $name"
        fi
    done < <(ldd "$elf" | grep '=>')
}
echo 'Libraries:'
for elf in "$share/bin/medievil-runtime" "$share/bin/gpu/libgow3gpu.so"; do collect "$elf"; done
# The Vulkan loader is the host's: a second, bundled loader next to it crashed (interposed symbols).
for compat in libstdc++.so.6 libgcc_s.so.1; do
    path=$("${CXX:-g++}" -print-file-name="$compat")
    [[ -f $path ]] && cp -L "$path" "$lib/compat/$compat"
done
# The newest GLIBCXX symbol version the bundle needs: AppRun uses the bundled libstdc++ when the
# host's lacks it.
newest() { # PREFIX FILE...: the newest symbol version PREFIX_x.y the files import
    local prefix=$1 f; shift
    for f in "$@"; do
        [[ -f $f ]] && file -b "$f" | grep -q ELF && objdump -T "$f" 2>/dev/null || true
    done | grep -o "${prefix}_[0-9.]*" | sort -V | tail -1 || true
}
native=("$share/bin/medievil-runtime" "$share/bin/gpu/libgow3gpu.so" "$lib"/*.so*)
need=$(newest GLIBCXX "${native[@]}")
echo "${need:-GLIBCXX_3.4}" > "$lib/compat/needs"

# Python (launcher, run.py and the preparation scripts).
cp -a "$py" "$app/usr/python"
rm -rf "$app/usr/python/lib/python3"*/test "$app/usr/python/lib/python3"*/idlelib/idle_test \
    "$app/usr/python/lib/python3"*/ensurepip "$app/usr/python/lib/python3"*/lib2to3 2>/dev/null || true
# Tk with Xft (anti-aliased fonts) instead of the standalone build's X11 core fonts.
if ! ldd "$app"/usr/python/lib/libtcl*tk*.so 2>/dev/null | grep -q libXft; then
    PATCHELF=$patchelf bash packaging/linux/build-tk-xft.sh "$app/usr/python" "$root/$tools/tk-xft"
fi
rm -f "$app"/usr/python/lib/libtcl*tk*.so.orig
# Bytecode inside the image: a read-only AppImage cannot write it at run time.
"$app/usr/python/bin/python3" -I -m compileall -q -j0 "$app/usr/python/lib" "$share/scripts" "$share/launcher" "$share/run.py" >/dev/null 2>&1 || true
mapfile -t python_elves < <(find "$app/usr/python" -type f \( -name '*.so*' -o -path '*/bin/python3*' \))
glibc=$(newest GLIBC "${native[@]}" "${python_elves[@]}")
echo "Needs glibc ${glibc#GLIBC_}+ from the host; libstdc++ ${need} (bundled fallback)"
echo "${glibc#GLIBC_}" > "$lib/compat/glibc"

# Fonts for the launcher without the game's own (DejaVu: the GPU library's overlay font too).
cp gpu/third_party/fonts/*.ttf gpu/third_party/fonts/LICENSE-DejaVu.txt "$app/usr/share/fonts/"

# Desktop integration.
install -m755 packaging/linux/AppRun "$app/AppRun"
cp launcher/medievil.png "$app/usr/share/icons/hicolor/256x256/apps/medievil-pc.png"
cp launcher/medievil.png "$app/medievil-pc.png"
cat > "$app/usr/share/applications/medievil-pc.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=MediEvil (PC)
Comment=MediEvil (PS4, 2019) running natively on Linux from your own copy of the game
Exec=AppRun
Icon=medievil-pc
Terminal=false
Categories=Game;ActionGame;
Keywords=MediEvil;PS4;Sir Dan;
EOF
cp "$app/usr/share/applications/medievil-pc.desktop" "$app/"
cat > "$app/usr/share/metainfo/medievil-pc.appdata.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<component type="desktop-application">
  <id>medievil-pc</id>
  <metadata_license>CC0-1.0</metadata_license>
  <project_license>GPL-2.0-or-later</project_license>
  <name>MediEvil (PC)</name>
  <summary>Native PC port runtime for MediEvil (PS4); bring your own game files</summary>
  <description><p>Runs the PlayStation 4 MediEvil executable from your own copy of the game. No game files included.</p></description>
  <launchable type="desktop-id">medievil-pc.desktop</launchable>
  <url type="homepage">https://github.com/bc160201983/medievil_2019_pc_port</url>
  <content_rating type="oars-1.1"/>
</component>
EOF

mkdir -p dist
out=dist/MediEvil-x86_64.AppImage
rm -f "$out"
# No FUSE needed to build: the tool runs from its extracted copy.
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$tool" --no-appstream --runtime-file "$runtime" "$app" "$out" >out/appimagetool.log 2>&1 \
    || { tail -20 out/appimagetool.log >&2; exit 1; }
ls -lh "$out"
