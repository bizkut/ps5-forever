#!/usr/bin/env bash
# Build pinned upstream FreeType for Wine's PS5 Unix side, without external
# zlib, bzip2, PNG, HarfBuzz or Brotli dependencies. Headers and the shared
# library install into ps5/build/freetype-ps5; stage-wine.sh copies the library
# to $WINE_ROOT/lib/libfreetype.so on the console. That absolute path is also
# its ELF soname: PS5's loader does not resolve bare library names.
#
# From the repository root:
#   ps5/build-freetype.sh
#   ps5/build-wine.sh configure
#   ps5/build-wine.sh all
#   ps5/stage-wine.sh
# JOBS (default 8) sets the build's job count. No files are installed outside
# ps5/build on the host; changing ps5/paths.sh requires rebuilding this library
# and reconfiguring Wine before staging again.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$here/build-common.sh"
sdk=${PS5_PAYLOAD_SDK:-$HOME/ps5-payload-sdk-platform}/bin
source "$here/paths.sh"

version=2.14.3
sha256=36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f
url=https://download.savannah.gnu.org/releases/freetype/freetype-$version.tar.xz
archive=$here/build/freetype-$version.tar.xz
src=$here/build/freetype-$version
project=$here/build/freetype-ps5-cmake
build=$here/build/freetype-ps5-build
prefix=$here/build/freetype-ps5
soname=$WINE_ROOT/lib/libfreetype.so
jobs=${JOBS:-8}

# Do not inherit Homebrew's host include/library flags in a cross build.
unset CPPFLAGS LDFLAGS CFLAGS CXXFLAGS
for tool in prospero-clang prospero-cmake; do
    if [[ ! -x $sdk/$tool ]]; then
        printf 'missing SDK tool: %s\n' "$sdk/$tool" >&2
        exit 1
    fi
done
mkdir -p "$here/build" "$project"
if [[ ! -f $archive ]]; then
    curl --fail --location --output "$archive.part" "$url"
    printf '%s  %s\n' "$sha256" "$archive.part" | ps5_sha256 -c -
    mv "$archive.part" "$archive"
else
    printf '%s  %s\n' "$sha256" "$archive" | ps5_sha256 -c -
fi
[[ -f $src/CMakeLists.txt ]] || tar -xf "$archive" -C "$here/build"

# Leave the pinned upstream sources untouched. The SDK toolchain forces
# BUILD_SHARED_LIBS off, so override it after project() loads the toolchain.
# Disable CMake's automatic versioned soname and supply the console path;
# VERSION/SOVERSION are cleared so the install is a regular libfreetype.so.
cat > "$project/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.16)
project(ps5_freetype C)
set(BUILD_SHARED_LIBS ON CACHE BOOL "Build FreeType as a shared library" FORCE)
add_subdirectory("$src" freetype)
set_target_properties(freetype PROPERTIES NO_SONAME TRUE VERSION "" SOVERSION "")
target_link_options(freetype PRIVATE "-Wl,-soname,$soname")
EOF

"$sdk/prospero-cmake" -S "$project" -B "$build" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_SKIP_RPATH=ON \
    -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON \
    -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON
cmake --build "$build" --parallel "$jobs"
cmake --install "$build"
echo "installed: $prefix (FreeType $version, soname $soname)"
