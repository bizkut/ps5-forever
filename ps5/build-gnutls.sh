#!/usr/bin/env bash
# Build the pinned pacbrew GnuTLS, Nettle (with mini-gmp) and libtasn1 packages
# for Wine's PS5 Unix side, then link them into one shared library. Headers and
# libgnutls.so install into ps5/build/gnutls-ps5; stage-wine.sh copies the library
# to $WINE_ROOT/lib/libgnutls.so on the console. That absolute path is also its
# ELF soname: PS5's loader does not resolve bare dependency library names.
#
# From the repository root:
#   ps5/build-freetype.sh
#   ps5/build-gnutls.sh
#   ps5/build-wine.sh configure
#   ps5/build-wine.sh all
#   ps5/stage-wine.sh
# JOBS (default 8) sets the build's job count. PACBREW_REPO selects the repository
# containing nettle/, libtasn1/ and gnutls/PKGBUILD; the default is the sibling
# homebrew checkout below. No makepkg/pacman or system-wide install is needed.
# All sources, package staging and outputs stay in ps5/build. Each run verifies
# cached downloads and rebuilds cleanly; changing ps5/paths.sh requires running
# this script and reconfiguring Wine before staging again.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$here/build-common.sh"
source "$here/paths.sh"
sdk=${PS5_PAYLOAD_SDK:-$HOME/ps5-payload-sdk-platform}
pacbrew=${PACBREW_REPO:-/Users/bizkut/Downloads/PS5/homebrew/pacbrew-repo}
sources=$here/build/gnutls-src
stage=$here/build/gnutls-stage
prefix=$here/build/gnutls-ps5
soname=$WINE_ROOT/lib/libgnutls.so
jobs=${JOBS:-8}

# Do not inherit Homebrew's host include/library flags in a cross build.
unset CPPFLAGS LDFLAGS CFLAGS CXXFLAGS
for tool in prospero-clang prospero-ar prospero-ranlib prospero-nm; do
    if [[ ! -x $sdk/bin/$tool ]]; then
        printf 'missing SDK tool: %s\n' "$sdk/bin/$tool" >&2
        exit 1
    fi
done
for pkg in nettle libtasn1 gnutls; do
    if [[ ! -f $pacbrew/$pkg/PKGBUILD ]]; then
        printf 'missing pacbrew recipe: %s\nset PACBREW_REPO to its checkout\n' \
            "$pacbrew/$pkg/PKGBUILD" >&2
        exit 1
    fi
done
source "$sdk/toolchain/prospero.sh"
# The recipes use the same DESTDIR layout as makepkg. pkgdir redirects that
# entire SDK-rooted layout into staging; the real SDK is never installed into.
deps=$stage$PS5_SYSROOT$PREFIX
export MAKEFLAGS=-j$jobs
export CPPFLAGS="-I$deps/include"
export LDFLAGS="-L$deps/lib"
# Bypass prospero-pkg-config's fixed SDK search root. --define-prefix relocates
# the packages' /user/homebrew .pc prefix to the isolated staging directory.
export PKG_CONFIG='pkg-config --static --define-prefix'
export PKG_CONFIG_PATH="$deps/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$deps/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR=
mkdir -p "$here/build"
rm -rf "$sources" "$stage" "$prefix"
mkdir -p "$sources" "$stage" "$prefix/include" "$prefix/lib"

build_package() (
    local pkg=$1 entry archive filename checksum index
    source "$pacbrew/$pkg/PKGBUILD"
    srcdir=$sources/$pkg
    pkgdir=$stage
    export srcdir pkgdir
    mkdir -p "$srcdir"
    for index in "${!source[@]}"; do
        entry=${source[$index]}
        filename=${entry##*/}
        archive=$here/build/$filename
        checksum=${sha256sums[$index]}
        if [[ ! -f $archive ]]; then
            curl --fail --location --output "$archive.part" "$entry"
            printf '%s  %s\n' "$checksum" "$archive.part" | ps5_sha256 -c -
            mv "$archive.part" "$archive"
        else
            printf '%s  %s\n' "$checksum" "$archive" | ps5_sha256 -c -
        fi
        tar -xf "$archive" -C "$srcdir"
    done
    # makepkg runs every phase in srcdir, not in the preceding phase's cwd.
    cd "$srcdir"
    prepare
    cd "$srcdir"
    build
    cd "$srcdir"
    package
)
for pkg in nettle libtasn1 gnutls; do
    build_package "$pkg"
done

# Keep libtool's versioned/bare sonames out of the console runtime. The whole
# GnuTLS archive retains every public API for Wine's dlsym callers; its internal
# gnulib/unistring archives are already incorporated by upstream libtool.
"$sdk/bin/prospero-clang" -shared -Wl,--no-undefined \
    "-Wl,-soname,$soname" \
    -Wl,--whole-archive "$deps/lib/libgnutls.a" -Wl,--no-whole-archive \
    -Wl,--start-group "$deps/lib/libhogweed.a" "$deps/lib/libnettle.a" \
    "$deps/lib/libtasn1.a" -Wl,--end-group \
    -o "$prefix/lib/libgnutls.so"
cp -R "$deps/include/gnutls" "$prefix/include/"
echo "installed: $prefix (GnuTLS + static Nettle/mini-gmp + libtasn1, soname $soname)"
