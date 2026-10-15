#!/usr/bin/env bash
# Build this checkout's vkd3d-proton Windows DLLs for PS5 Wine.
# Usage: bash ps5/build-vkd3d-proton.sh
# LLVM_MINGW (default ~/llvm-mingw) selects the toolchain; JOBS defaults to 8.
# WIDL optionally selects a host Wine IDL compiler by absolute path. Otherwise
# use llvm-mingw's architecture-specific widl, or WINE_TOOLS/tools/widl/widl
# (WINE_TOOLS defaults to ps5/build/wine-ps5-tools, as in build-wine.sh).
# Meson trees and generated cross files stay in ps5/build/vkd3d-proton-32 and -64;
# each tree's dlls/ contains d3d12.dll and d3d12core.dll. Nothing is deployed.
# Missing build tools are reported, never installed here.
# PS5_CPU_FLAGS (default empty) adds Meson c_args/cpp_args for target CPUs.
# VKD3D_BUILD_SUFFIX (default empty) isolates output trees; VKD3D_ARCHS
# (default "32 64") selects architectures. Example for a 64-bit Zen 2 DLL:
# PS5_CPU_FLAGS="-march=znver2" VKD3D_BUILD_SUFFIX=-znver2 VKD3D_ARCHS=64 \
#   bash ps5/build-vkd3d-proton.sh
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
src=$here/../vkd3d-proton
mingw=${LLVM_MINGW:-$HOME/llvm-mingw}
jobs=${JOBS:-8}
cpu_flags=${PS5_CPU_FLAGS:-}
build_suffix=${VKD3D_BUILD_SUFFIX:-}

missing=()
for tool in meson ninja; do
    if ! which "$tool"; then
        missing+=("$tool")
    fi
done
if ! which glslang && ! which glslangValidator; then
    missing+=(glslang)
fi
if (( ${#missing[@]} )); then
    printf 'missing build tools: %s\ninstall with: brew install %s\n' \
        "${missing[*]}" "${missing[*]}" >&2
    exit 1
fi
widls=()
for triplet in i686-w64-mingw32 x86_64-w64-mingw32; do
    for tool in clang clang++ ar strip; do
        if [[ ! -x $mingw/bin/$triplet-$tool ]]; then
            printf 'missing LLVM MinGW tool: %s\nset LLVM_MINGW to its installation root\n' \
                "$mingw/bin/$triplet-$tool" >&2
            exit 1
        fi
    done
    if [[ -n ${WIDL:-} ]]; then
        widl=$WIDL
    elif [[ -x $mingw/bin/$triplet-widl ]]; then
        widl=$mingw/bin/$triplet-widl
    else
        widl=${WINE_TOOLS:-$here/build/wine-ps5-tools}/tools/widl/widl
    fi
    if [[ ! -x $widl ]]; then
        printf 'missing Wine IDL compiler: %s\nbuild the host tools with ps5/build-wine.sh, or set WIDL to a host widl executable\n' \
            "$widl" >&2
        exit 1
    fi
    widls+=("$widl")
done
if [[ ! -x $mingw/bin/llvm-objdump ]]; then
    printf 'missing LLVM MinGW tool: %s\n' "$mingw/bin/llvm-objdump" >&2
    exit 1
fi

# These supply the Khronos headers and shader translator used by meson.build.
required_submodules=()
[[ -f $src/khronos/Vulkan-Headers/include/vulkan/vulkan.h ]] || required_submodules+=(khronos/Vulkan-Headers)
[[ -f $src/khronos/SPIRV-Headers/include/spirv/unified1/spirv.hpp ]] || required_submodules+=(khronos/SPIRV-Headers)
[[ -f $src/subprojects/dxil-spirv/meson.build ]] || required_submodules+=(subprojects/dxil-spirv)
if (( ${#required_submodules[@]} )); then
    printf 'initializing required vkd3d-proton submodules: %s\n' "${required_submodules[*]}"
    git -C "$src" submodule update --init -- "${required_submodules[@]}"
fi
# dxil-spirv's Meson build needs these children, but not its CMake-only SPIRV
# tools and SPIRV-Cross. Check them separately when the parent already exists.
dxil=$src/subprojects/dxil-spirv
required_submodules=()
[[ -f $dxil/third_party/spirv-headers/include/spirv/unified1/spirv.hpp ]] || required_submodules+=(third_party/spirv-headers)
[[ -f $dxil/subprojects/dxbc-spirv/meson.build ]] || required_submodules+=(subprojects/dxbc-spirv)
if (( ${#required_submodules[@]} )); then
    printf 'initializing required dxil-spirv submodules: %s\n' "${required_submodules[*]}"
    git -C "$dxil" submodule update --init -- "${required_submodules[@]}"
fi
dxbc=$dxil/subprojects/dxbc-spirv
if [[ ! -f $dxbc/submodules/spirv_headers/include/spirv/unified1/spirv.hpp ]]; then
    printf 'initializing dxbc-spirv submodules/spirv_headers\n'
    git -C "$dxbc" submodule update --init -- submodules/spirv_headers
fi

# Keep Homebrew's host include/library flags out of this cross build, as in
# build-dxvk.sh. All compiler and IDL compiler paths are explicit below.
unset CPPFLAGS LDFLAGS CFLAGS CXXFLAGS
export PATH=$mingw/bin:$PATH

# Match package-release.sh's default Windows recipe: release and strip.
# Tests and extras retain its disabled defaults in meson_options.txt.
meson_options=(--buildtype=release --strip)
if [[ -n ${PS5_PE_LDFLAGS:-} ]]; then
    meson_options+=("-Dc_link_args=$PS5_PE_LDFLAGS" "-Dcpp_link_args=$PS5_PE_LDFLAGS")
fi

build_arch() {
    local bits=$1 triplet=$2 cpu=$3 widl=$4 build cross text dll
    build=$here/build/vkd3d-proton-$bits$build_suffix
    cross=$build/cross.txt
    mkdir -p "$build"
    # Mirror build-win32.txt/build-win64.txt with LLVM compiler names. An explicit
    # widl overrides any unrelated PATH compiler; -m32/-m64 also covers host Wine.
    # Retained shader sources rely on old libc++ transitive includes for
    # std::launder/std::terminate; supply standard <new>/<exception> explicitly.
    text=$(cat <<EOF
[binaries]
c = '$mingw/bin/$triplet-clang'
cpp = '$mingw/bin/$triplet-clang++'
ar = '$mingw/bin/$triplet-ar'
strip = '$mingw/bin/$triplet-strip'
widl = ['$widl', '-m$bits']

[properties]
needs_exe_wrapper = true

[built-in options]
cpp_args = ['-include', 'new', '-include', 'exception']

[host_machine]
system = 'windows'
cpu_family = '$cpu'
cpu = '$cpu'
endian = 'little'
EOF
)
    # Preserve the timestamp when unchanged, so a rerun stays incremental.
    [[ -f $cross && $(<"$cross") == "$text" ]] || printf '%s\n' "$text" > "$cross"
    local setup_options=()
    if [[ -f $build/meson-private/coredata.dat ]]; then
        setup_options+=(--reconfigure)
    fi
    # Reconfigure retains cached machine-file options unless set explicitly.
    meson setup "${setup_options[@]}" "$build" "$src" --cross-file="$cross" \
        --prefix="$build" --bindir=dlls --libdir=lib \
        -Dc_args="$cpu_flags" -Dcpp_args="-include new -include exception $cpu_flags" "${meson_options[@]}"
    ninja -C "$build" -j"$jobs" install
    printf '\nvkd3d-proton %s-bit DLLs:\n' "$bits"
    for dll in d3d12 d3d12core; do
        dll=$build/dlls/$dll.dll
        printf '%s (%s bytes)\n' "$dll" "$(wc -c < "$dll" | tr -d '[:space:]')"
        "$mingw/bin/llvm-objdump" -f "$dll"
    done
}

for bits in ${VKD3D_ARCHS:-32 64}; do
    case $bits in
        32) build_arch 32 i686-w64-mingw32 x86 "${widls[0]}" ;;
        64) build_arch 64 x86_64-w64-mingw32 x86_64 "${widls[1]}" ;;
        *) printf 'unsupported VKD3D_ARCHS entry: %s\n' "$bits" >&2; exit 1 ;;
    esac
done
