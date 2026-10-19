#!/usr/bin/env bash
# Build the launcher title, PPSA99030 (ps5/paths.sh), from C sources and existing
# Unix .o/.a inputs, with ../../PS5_Vulkan's title tooling as
# tools/build-radv-title.sh uses it: its CRT
# and C++ runtime (tooling/native), its linker script, its ps5-native-tool
# (link, FSELF) and its clean-room libc.prx, against the payload SDK fork it
# pins (.deps/native/ps5-payload-sdk), whose platform layer gives the title
# the libc functions no system module exports.
#
#   ps5/title/build.sh OUT_DIR INPUT.c|INPUT.o|INPUT.a...
#
# OUT_DIR receives eboot.bin, sce_sys/param.json and sce_module/libc.prx: the
# title folder's files, uploaded beside wine/ (ps5/deploy-wine.py
# upload-title). Nothing else in OUT_DIR is touched.
#
# Host compiler: PS5_CLANG (default Homebrew's LLVM 19). Its macOS install has
# no x86-64 Linux compiler-rt builtins; BUILTINS names a
# libclang_rt.builtins-x86_64.a (default: the copy ps5/probes/d4 uses).
# PS5_TITLE_WORK selects an isolated intermediate directory for probe builds.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$here/../paths.sh"
vulkan=${PS5_VULKAN:-$here/../../../../PS5_Vulkan}
sdk=$vulkan/.deps/native/ps5-payload-sdk
native=$vulkan/tooling/native
tool=$vulkan/build/host/ps5-native-tool
wine_src=${WINE_SRC:-$here/../../../ps5-wine}
if [[ -z ${PS5_CLANG:-} ]]; then
    if [[ $(uname -s) == Darwin ]]; then
        PS5_CLANG=/opt/homebrew/opt/llvm@19/bin/clang
    else
        PS5_CLANG=$(command -v clang-19)
    fi
fi
export PS5_CLANG
# compiler-rt builtins: the same archive PS5_Vulkan's host tooling builds.
builtins=${BUILTINS:-$vulkan/build/host/clang-resource/lib/linux/libclang_rt.builtins-x86_64.a}
(( $# >= 2 )) || { echo "usage: $0 OUT_DIR INPUT.c|INPUT.o|INPUT.a..." >&2; exit 2; }
out=$1; shift
for file in "$tool" "$sdk/bin/prospero-lld" "$vulkan/runtime/libc.prx" "$builtins" \
        "$vulkan/tooling/psbc/ps5-pie-unwind.ld" "$here/sce_sys/param.json"; do
    [[ -e $file ]] || { echo "missing $file" >&2; exit 2; }
done
[[ $(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' \
    "$here/sce_sys/param.json") == "$TITLE_ID" ]] || { echo "param.json is not $TITLE_ID" >&2; exit 2; }

work=${PS5_TITLE_WORK:-$here/../build/title}
mkdir -p "$work/obj" "$out/sce_sys" "$out/sce_module"
cc() { PS5_PAYLOAD_SDK="$sdk" sh "$vulkan/tooling/prospero-clang18" "$@"; }
cxxflags=(-std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections)
diag_flags=()
[[ ${FOREVER_MENU_DIAG:-0} == 1 ]] && diag_flags+=(-DFOREVER_MENU_DIAG=1)
cc "${cxxflags[@]}" -c "$native/app_crt.cpp" -o "$work/obj/app_crt.o"
cc "${cxxflags[@]}" -c "$native/app_cpp_runtime.cpp" -o "$work/obj/app_cpp_runtime.o"
objs=()
archives=()
includes=()
[[ -n ${PS5_TITLE_RADV:-} ]] && includes+=(-I "$vulkan/../PS5_Mesa/include")
for input in "$@"; do
    [[ -f $input ]] || { echo "missing $input" >&2; exit 2; }
    case $input in
        *.o) objs+=("$input") ;;
        *.a) archives+=("$input") ;;
        *.c)
            obj=$work/obj/$(basename "${input%.c}").o
            # Wine headers last: only private PS5 host/protocol interfaces.
            cc -std=gnu11 -O2 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
                "${diag_flags[@]}" -I "$here/../include" "${includes[@]}" \
                -idirafter "$wine_src/include" -c "$input" -o "$obj"
            objs+=("$obj")
            ;;
        *) echo "unsupported title input: $input (expected .c, .o or .a)" >&2; exit 2 ;;
    esac
done

# Every allocation goes to the platform's heap in direct memory
# (ps5platform/heap.h), and the libc functions no system module exports are
# the platform's, bound locally (the title converter refuses exports), as
# ../../PS5_Vulkan/tools/radv-link.sh binds them.
flags=(--wrap=pthread_create --wrap=pthread_join --wrap=pthread_detach)
for name in malloc calloc realloc free posix_memalign aligned_alloc memalign malloc_usable_size \
        reallocf reallocarray getline getdelim; do
    flags+=("--wrap=$name")
done
map=$work/platform-local.map
{
    printf '{\n    local:\n'
    for name in arc4random arc4random_buf arc4random_uniform gmtime_r localtime_r opendir \
            fdopendir readdir closedir getaddrinfo freeaddrinfo access; do
        flags+=("--defsym=$name=ps5_$name")
        printf '        %s;\n' "$name"
    done
    printf '};\n'
} > "$map"
# The system modules' stubs: libkernel and libkernel_sys first, so the kernel
# functions bind to them and not to the WebKit process's libkernel_web, which
# a title does not get (the first build's imports went there and the title
# called through null in its first function, ps5_klog_capture_stderr).
stubs=("$sdk/target/lib/libkernel.so" "$sdk/target/lib/libkernel_sys.so")
for stub in "$sdk"/target/lib/*.so; do
    case $(basename "$stub") in
        libkernel.so|libkernel_sys.so|libkernel_web.so|libkernel_stub_weak.so|libc_stub_weak.so) ;;
        *) stubs+=("$stub") ;;
    esac
done
# AGC comes from a system module the SDK ships no stub for: a host-link stub
# names the imports, as ../../PS5_Vulkan/tools/build-radv-title.sh builds it.
cc -std=c11 -O2 -fPIC -c "$vulkan/vendor/ps5/sdk/stubs/agc_canary_link_stub.c" -o "$work/obj/libSceAgc_stub.o"
"$sdk/bin/prospero-lld" --shared -soname libSceAgc.prx -o "$work/libSceAgc.so" "$work/obj/libSceAgc_stub.o"
stubs+=("$work/libSceAgc.so")
# The inline keyboard (libSceIme) has no SDK stub either: stubs/libSceIme_stub.c.
cc -std=c11 -O2 -fPIC -c "$here/stubs/libSceIme_stub.c" -o "$work/obj/libSceIme_stub.o"
"$sdk/bin/prospero-lld" --shared -soname libSceIme.prx -o "$work/libSceIme.so" "$work/obj/libSceIme_stub.o"
stubs+=("$work/libSceIme.so")
extra_inputs=()
extra_stubs=(--stub "$work/libSceIme.so")
# Only the game parent captures audio; no microphone imports in the desktop
# parent, child, or ordinary title probes.
if [[ -n ${PS5_TITLE_RADV:-} ]]; then
    cc -std=c11 -O2 -fPIC -c "$here/stubs/libSceAudioIn_stub.c" -o "$work/obj/libSceAudioIn_stub.o"
    "$sdk/bin/prospero-lld" --shared -soname libSceAudioIn.prx -o "$work/libSceAudioIn.so" "$work/obj/libSceAudioIn_stub.o"
    stubs+=("$work/libSceAudioIn.so")
    extra_stubs+=(--stub "$work/libSceAudioIn.so")
fi
if [[ -n ${PS5_TITLE_RADV:-} ]]; then
    # Reuse RADV's native-title recipe; its imports never use kernel_web.
    source "$vulkan/tools/radv-link.sh"
    radv_link_recipe "$vulkan" "$sdk" "$PS5_TITLE_RADV"
    flags+=("${radv_link_flags[@]}")
    extra_inputs+=("${radv_link_inputs[@]}")
    cc -std=c11 -O2 -fPIC -c "$vulkan/vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c" -o "$work/obj/libSceAgcDriver_stub.o"
    "$sdk/bin/prospero-lld" --shared -soname libSceAgcDriver.prx -o "$work/libSceAgcDriver.so" "$work/obj/libSceAgcDriver_stub.o"
    stubs+=("$work/libSceAgcDriver.so")
    extra_stubs+=(--stub "$work/libSceAgcDriver.so")
fi
if [[ -n ${PS5_TITLE_RADV:-} ]]; then
    voice=${PS5_WHISPER_INSTALL:-$here/../build/whisper}
    [[ -f $voice/lib/libwhisper.a ]] || {
        echo "Whisper archives missing: run bash ps5/build-whisper.sh" >&2; exit 2;
    }
    cc -std=c++17 -O2 -g -Wall -Wextra -Werror -stdlib=libc++ -fexceptions -fcxx-exceptions -frtti \
        -ffunction-sections -fdata-sections -DGGML_MAX_NAME=160 \
        -I "$voice/include" -c "$here/voice.cpp" -o "$work/obj/voice.o"
    objs+=("$work/obj/voice.o")
    for library in whisper ggml ggml-base ggml-cpu ggml-vulkan; do
        archives+=("$voice/lib/lib$library.a")
    done
    # Vulkan-Hpp's dispatcher owns GGML's instance/device; no WoW WSI hooks.
    flags+=(--defsym=vkGetInstanceProcAddr=vk_icdGetInstanceProcAddr
        --defsym=vkGetDeviceProcAddr=vk_common_GetDeviceProcAddr
        --defsym=vkCmdCopyBuffer=vk_common_CmdCopyBuffer
        --defsym=vkGetPhysicalDeviceFeatures2=vk_common_GetPhysicalDeviceFeatures2)
    # GGML's optional crash backtrace may not spawn a debugger in a title.
    flags+=(--defsym=execlp=pw_native_fail_minus_one)
    flags+=(--defsym=fork=forever_voice_fork --defsym=pathconf=forever_voice_pathconf)
    # Keep these reviewed fail-closed adapters local, never system providers.
    printf '{ local: fork; pathconf; execlp; };\n' > "$work/voice-local.map"
    flags+=(--version-script "$work/voice-local.map")
fi
"$sdk/bin/prospero-lld" -T "$vulkan/tooling/psbc/ps5-pie-unwind.ld" -L "$native" --eh-frame-hdr \
    "${flags[@]}" --version-script "$map" --version-script "$native/app-symbols.map" \
    --exclude-libs=ALL -e _start -o "$work/llvm-pie.elf" \
    "$work/obj/app_crt.o" "$work/obj/app_cpp_runtime.o" "${objs[@]}" \
    "${extra_inputs[@]}" \
    -L "$sdk/target/lib" --start-group "${archives[@]}" "$sdk/target/lib/libc++.a" "$sdk/target/lib/libc++abi.a" \
    "$sdk/target/lib/libunwind.a" "$builtins" "$sdk/target/lib/libps5platform.a" --end-group \
    --as-needed "${stubs[@]}"
"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" --stub-dir "$sdk/target/lib" \
    --stub "$work/libSceAgc.so" "${extra_stubs[@]}" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
"$tool" self --sign --in "$work/eboot.elf" --out "$out/eboot.bin" --magic 0x1D3D154F
cp "$here/sce_sys/param.json" "$out/sce_sys/param.json"
if [[ -n ${PS5_TITLE_RADV:-} ]]; then
    python3 - "$out/sce_sys/param.json" <<'EOF'
import json, sys
path = sys.argv[1]
with open(path) as file:
    param = json.load(file)
param["attribute3"] |= 0x80040
with open(path, "w") as file:
    json.dump(param, file, indent=2)
    file.write("\n")
EOF
fi
# The icon: drawn by tools/make-title-art.py. Once Battle.net is installed the
# app shows the installed launcher's icon instead (psmkit-wine/title-icon.h).
cp "$here/sce_sys/icon0.png" "$out/sce_sys/icon0.png"
# The home screen's background: drawn by tools/make-title-background.py (no
# outside artwork), one file installed under both names the console reads.
if [[ -f $here/sce_sys/pic0.dds ]]; then
    cp "$here/sce_sys/pic0.dds" "$out/sce_sys/pic0.dds"
    cp "$here/sce_sys/pic0.dds" "$out/sce_sys/pic1.dds"
fi
cp "$vulkan/runtime/libc.prx" "$out/sce_module/libc.prx"
# The desktop picture behind Wine's windows: the drawn home-screen background
# above (tools/make-title-background.py), decoded and scaled to the display as
# 0xAARRGGBB words. DESKTOP_DDS may name another 3840x2160 DDS for a local
# build; that file is never committed. Without a picture the title draws a
# plain colour.
desktop=${DESKTOP_DDS:-$here/sce_sys/pic0.dds}
if [[ -f $desktop ]]; then
    mkdir -p "$out/assets"
    python3 - "$desktop" "$out/assets/desktop.bgra" <<'EOF'
import sys
from PIL import Image
image = Image.open(sys.argv[1]).convert("RGB").resize((1920, 1080), Image.LANCZOS)
r, g, b = image.split()
with open(sys.argv[2], "wb") as output:
    output.write(Image.merge("RGBA", (b, g, r, Image.new("L", image.size, 255))).tobytes())
EOF
fi
"$tool" self --inspect --file "$out/eboot.bin" > /dev/null
echo "title: $out ($(wc -c < "$out/eboot.bin" | tr -d ' ') bytes)"
