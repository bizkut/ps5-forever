#!/usr/bin/env bash
# Build this checkout's DXVK Windows DLLs for PS5 Wine, without changing dxvk/.
# Usage: bash ps5/build-dxvk.sh
# LLVM_MINGW (default ~/llvm-mingw) selects the toolchain; JOBS defaults to 8.
# Meson trees, generated cross files and installed DLLs stay in ps5/build/dxvk-32
# and ps5/build/dxvk-64. The DLLs are in each tree's dlls/ directory.
# Missing build tools are reported with a Homebrew command, never installed here.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# DXVK_SRC and DXVK_BUILD build another checkout (a second git worktree) into
# DXVK_BUILD-32 and DXVK_BUILD-64, e.g. to compare branches on the console.
src=${DXVK_SRC:-$here/../dxvk}
build_root=${DXVK_BUILD:-$here/build/dxvk}
mingw=${LLVM_MINGW:-$HOME/llvm-mingw}
jobs=${JOBS:-8}

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
for triplet in i686-w64-mingw32 x86_64-w64-mingw32; do
    for tool in clang clang++ ar strip windres; do
        if [[ ! -x $mingw/bin/$triplet-$tool ]]; then
            printf 'missing LLVM MinGW tool: %s\nset LLVM_MINGW to its installation root\n' \
                "$mingw/bin/$triplet-$tool" >&2
            exit 1
        fi
    done
done
if [[ ! -x $mingw/bin/llvm-objdump ]]; then
    printf 'missing LLVM MinGW tool: %s\n' "$mingw/bin/llvm-objdump" >&2
    exit 1
fi

# These are used by the Windows build (dxvk/meson.build:39-56,177-178).
# include/native/directx is only used for DXVK Native and is left untouched.
required_submodules=()
[[ -f $src/include/vulkan/include/vulkan/vulkan.h ]] || required_submodules+=(include/vulkan)
[[ -f $src/include/spirv/include/spirv/unified1/spirv.hpp ]] || required_submodules+=(include/spirv)
[[ -f $src/subprojects/libdisplay-info/meson.build ]] || required_submodules+=(subprojects/libdisplay-info)
[[ -f $src/subprojects/dxbc-spirv/meson.build ]] || required_submodules+=(subprojects/dxbc-spirv)
if (( ${#required_submodules[@]} )); then
    printf 'initializing required DXVK submodules: %s\n' "${required_submodules[*]}"
    git -C "$src" submodule update --init -- "${required_submodules[@]}"
fi
# dxbc-spirv/spirv/spirv_types.h includes this nested copy directly.
if [[ ! -f $src/subprojects/dxbc-spirv/submodules/spirv_headers/include/spirv/unified1/spirv.hpp ]]; then
    printf 'initializing dxbc-spirv submodules/spirv_headers\n'
    git -C "$src/subprojects/dxbc-spirv" submodule update --init -- submodules/spirv_headers
fi

# Keep Homebrew's host include/library flags out of this cross build, as in
# build-wine.sh. All compiler paths are explicit in the generated cross files.
unset CPPFLAGS LDFLAGS CFLAGS CXXFLAGS
export PATH=$mingw/bin:$PATH

# Proton enables these DLLs through DXVK's defaults (Makefile.in:773-780;
# dxvk/meson_options.txt:1-5). Its generic Meson rule uses --buildtype=plain
# with global -O2/debug flags (make/rules-meson.mk:49-58; Makefile.in:59-61).
# Use DXVK's standalone release recipe here instead: release, strip and
# b_ndebug=if-release (dxvk/package-release.sh:64-77), with all DLLs explicit.
meson_options=(--buildtype=release --strip -Db_ndebug=if-release
    -Denable_dxgi=true -Denable_d3d9=true -Denable_d3d10=true -Denable_d3d11=true
    --force-fallback-for=libdisplay-info)
dlls=(d3d9 d3d10core d3d11 dxgi)
if [[ -f $src/src/d3d8/meson.build ]]; then
    meson_options+=(-Denable_d3d8=true)
    dlls+=(d3d8)
fi
if [[ -n ${PS5_PE_LDFLAGS:-} ]]; then
    meson_options+=("-Dc_link_args=$PS5_PE_LDFLAGS" "-Dcpp_link_args=$PS5_PE_LDFLAGS")
fi

build_arch() {
    local bits=$1 triplet=$2 cpu=$3 build cross text dll
    local stack_args='[]'
    local cpp_args="['-include', 'new', '-include', 'algorithm']"
    build=$build_root-$bits
    cross=$build/cross.txt
    mkdir -p "$build"
    if [[ $bits == 32 ]]; then
        # LLVM does not support DXVK's GCC -mpreferred-stack-boundary=2.
        # Use Proton's i386 stack realignment (Makefile.in:54) for Wine callers.
        stack_args="['-mstackrealign']"
        cpp_args="['-mstackrealign', '-include', 'new', '-include', 'algorithm']"
    fi
    # Retained sources rely on old libc++ transitive includes for std::launder
    # and std::transform; provide their standard <new>/<algorithm> explicitly.
    # Mirror dxvk/build-win32.txt and build-win64.txt, using LLVM compiler names.
    text=$(cat <<EOF
[binaries]
c = '$mingw/bin/$triplet-clang'
cpp = '$mingw/bin/$triplet-clang++'
ar = '$mingw/bin/$triplet-ar'
strip = '$mingw/bin/$triplet-strip'
windres = '$mingw/bin/$triplet-windres'

[properties]
needs_exe_wrapper = true

[built-in options]
c_args = $stack_args
cpp_args = $cpp_args

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
    # Apply the same architecture-specific flags to fresh and existing trees.
    meson setup "${setup_options[@]}" "$build" "$src" --cross-file="$cross" \
        --prefix="$build" --bindir=dlls --libdir=lib -Dcpp_args="$cpp_args" "${meson_options[@]}"
    ninja -C "$build" -j"$jobs" install
    printf '\nDXVK %s-bit DLLs:\n' "$bits"
    for dll in "${dlls[@]}"; do
        dll=$build/dlls/$dll.dll
        printf '%s (%s bytes)\n' "$dll" "$(wc -c < "$dll" | tr -d '[:space:]')"
        "$mingw/bin/llvm-objdump" -f "$dll"
    done
}

build_arch 32 i686-w64-mingw32 x86
build_arch 64 x86_64-w64-mingw32 x86_64
