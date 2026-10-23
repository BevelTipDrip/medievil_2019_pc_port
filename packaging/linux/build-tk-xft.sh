#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Rebuilds the Tk library of a python-build-standalone tree with Xft (anti-aliased TrueType text
# through fontconfig). The standalone builds use Tk's X11 core fonts, which draw the launcher's
# text with bitmap fonts. The same Tk version is built, so _tkinter and the Tk scripts match.
#
# Usage: bash packaging/linux/build-tk-xft.sh PYTHON_DIR [WORK_DIR]
#   PYTHON_DIR: the tree with bin/python3 and lib/libtcl9tk9.0.so (python/ of the archive)
# Needs: a C compiler, make, git, and the development files of X11, Xft (libXft), fontconfig and
# freetype2 (with libXft in a private prefix: CPPFLAGS=-I<prefix>/include LDFLAGS=-L<prefix>/lib).
set -euo pipefail
py=$(cd "$1" && pwd)
work=${2:-$(mktemp -d)}
mkdir -p "$work"
version=$("$py/bin/python3" -I -c 'import tkinter; print(tkinter.Tcl().eval("info patchlevel"))')
major=${version%%.*}
tag=core-${version//./-}
lib=$(ls "$py"/lib/libtcl*tk*.so | head -1)
echo "Tk $version with Xft for $lib"
for repo in tcl tk; do
    [[ -d $work/$repo ]] || git clone -q --depth 1 --branch "$tag" "https://github.com/tcltk/$repo" "$work/$repo"
done
prefix=$work/prefix
if ! ls "$work"/tk/unix/libtcl*tk*.so >/dev/null 2>&1; then  # built once per work directory
(cd "$work/tcl/unix" && sh ./configure --prefix="$prefix" --enable-shared --enable-threads >/dev/null \
    && make -j"$(nproc)" >/dev/null && make install >/dev/null)
(cd "$work/tk/unix" && sh ./configure --prefix="$prefix" --with-tcl="$prefix/lib" --enable-shared \
    --enable-xft --disable-xss CPPFLAGS="${CPPFLAGS:-} $(pkg-config --cflags freetype2 fontconfig)" >/dev/null \
    && grep -q 'HAVE_XFT\|-DHAVE_XFT=1' Makefile && make -j"$(nproc)" >/dev/null)
fi
built=$(ls "$work"/tk/unix/libtcl*tk*.so | head -1)
[[ -f $lib.orig ]] || cp "$lib" "$lib.orig"
cp "$built" "$lib"
# libXft beside it: most desktops have it, the bundle should not depend on that.
xft=$(ldd "$lib" | awk '/libXft.so.2/{print $3}')
[[ -n $xft ]] && cp -L "$xft" "$py/lib/libXft.so.2"
patchelf --set-rpath '$ORIGIN' "$lib" "$py/lib/libXft.so.2" 2>/dev/null \
    || "${PATCHELF:-patchelf}" --set-rpath '$ORIGIN' "$lib" "$py/lib/libXft.so.2"
[[ $major == 9 || $major == 8 ]] || echo "Note: Tk $version untested" >&2
echo "Tk with Xft installed into $lib (original kept as $(basename "$lib").orig)"
