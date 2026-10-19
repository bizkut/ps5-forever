#!/usr/bin/env bash
# Build the pinned CPU + Vulkan speech engine for the PS5 game parent.
# JOBS controls parallelism; PS5_WHISPER_BUILD/INSTALL override generated paths.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$here/build-common.sh"
sdk=${PS5_PAYLOAD_SDK:-$HOME/ps5-payload-sdk-platform}
export PS5_PAYLOAD_SDK=$sdk
work=${PS5_WHISPER_BUILD:-$here/build/whisper-build}
prefix=${PS5_WHISPER_INSTALL:-$here/build/whisper}
radv=${PS5_RADV_INSTALL:-$here/build/radv-release-patched}
vulkan_headers=$here/../vkd3d-proton/khronos/Vulkan-Headers/include
glslc=$(command -v glslc) || {
    printf 'missing shader compiler: glslc (use the release Docker image)\n' >&2
    exit 1
}
spirv_headers=/usr/include/spirv/unified1
spirv_cmake=/usr/share/cmake/SPIRV-Headers
jobs=${JOBS:-8}

# The same release download manifest pins the archive bytes and upstream commits.
pins=$(python3 - "$here/release/deps.json" <<'PY'
import json, sys
manifest = json.load(open(sys.argv[1]))
for name in ('whisper.cpp-1.9.5', 'ggml-0.26.0'):
    entry = next(item for item in manifest['downloads'] if item['name'] == name)
    print('\t'.join(entry[key] for key in ('name', 'commit', 'sha256', 'url')))
PY
)
for input in "$sdk/.ps5-sdk-revision" "$sdk/target/include/c++/v1/__config" \
    "$radv/lib/libvulkan_radeon.ps5.a" "$vulkan_headers/vulkan/vulkan.hpp" \
    "$vulkan_headers/vulkan/vulkan_core.h" "$spirv_headers/spirv.hpp11" \
    "$spirv_cmake/SPIRV-HeadersConfig.cmake"; do
    [[ -f $input ]] || { printf 'missing whisper build input: %s\n' "$input" >&2; exit 1; }
done
# Never inherit host include, library or compiler flags into a target build.
unset CPPFLAGS LDFLAGS CFLAGS CXXFLAGS CMAKE_PREFIX_PATH
export GIT_CEILING_DIRECTORIES=$work
stamp=$({
    printf '%s\n' "$pins" "$sdk" "$work" "$prefix" "$radv" \
        "${FOREVER_CONTAINER_IMAGE_ID:-}"
    ps5_sha256 "$here/build-whisper.sh" "$here/build-common.sh" "$here/whisper/"* \
        "$sdk/.ps5-sdk-revision" "$sdk/target/include/c++/v1/__config" \
        "$radv/lib/libvulkan_radeon.ps5.a" "$vulkan_headers/vulkan/vulkan.hpp" \
        "$vulkan_headers/vulkan/vulkan_core.h" "$spirv_headers/spirv.hpp11" \
        "$spirv_cmake/SPIRV-HeadersConfig.cmake"
    clang-19 --version
    clang++-19 --version
    "$glslc" --version
    cmake --version
} | ps5_sha256 | cut -d' ' -f1)
complete=true
for output in include/whisper.h include/ggml.h lib/libwhisper.a lib/libggml.a \
    lib/libggml-base.a lib/libggml-cpu.a lib/libggml-vulkan.a; do
    [[ -s $prefix/$output ]] || complete=false
done
if [[ $complete == true && -f $prefix/.build-stamp && $(<"$prefix/.build-stamp") == "$stamp" ]]; then
    printf 'WHISPER_UP_TO_DATE prefix=%s stamp=%s\n' "$prefix" "$stamp"
    exit 0
fi

mkdir -p "$work/downloads"
while IFS=$'\t' read -r name commit sha url; do
    archive=$work/downloads/$name-$commit.tar.gz
    if [[ ! -f $archive ]]; then
        curl --fail --location --output "$archive.part" "$url"
        printf '%s  %s\n' "$sha" "$archive.part" | ps5_sha256 -c -
        mv "$archive.part" "$archive"
    else
        printf '%s  %s\n' "$sha" "$archive" | ps5_sha256 -c -
    fi
done <<< "$pins"
# A changed recipe/toolchain starts with fresh generated sources and objects;
# completed matching installs take the fast path above.
rm -rf -- "$work/src" "$work/obj"
mkdir -p "$work/src" "$work/obj/include-shim/spirv"
while IFS=$'\t' read -r name commit sha url; do
    mkdir -p "$work/src/$name"
    tar -xf "$work/downloads/$name-$commit.tar.gz" --strip-components=1 -C "$work/src/$name"
done <<< "$pins"
patch --batch --fuzz=0 -p1 -d "$work/src/ggml-0.26.0" < "$here/whisper/ps5-buffer-limits.patch"
ln -s "$spirv_headers" "$work/obj/include-shim/spirv/unified1"
cmake -S "$here/whisper" -B "$work/obj" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$here/whisper/toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_INCLUDEDIR=include \
    -DWHISPER_SOURCE_DIR="$work/src/whisper.cpp-1.9.5" \
    -DGGML_SOURCE_DIR="$work/src/ggml-0.26.0" \
    -DVulkan_INCLUDE_DIR="$vulkan_headers" \
    -DVulkan_LIBRARY="$radv/lib/libvulkan_radeon.ps5.a" \
    -DVulkan_GLSLC_EXECUTABLE="$glslc" \
    -DSPIRV-Headers_DIR="$spirv_cmake"
cmake --build "$work/obj" --target whisper --parallel "$jobs"
cmake --install "$work/obj"
printf '%s\n' "$stamp" > "$prefix/.build-stamp"
printf 'WHISPER_INSTALLED prefix=%s stamp=%s\n' "$prefix" "$stamp"
