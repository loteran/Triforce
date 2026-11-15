#!/bin/bash
# Builds libamd_fidelityfx_vk.so, the Linux counterpart of AMD's amd_fidelityfx_vk.dll that the Vulkan back end loads from
# beside the game for FSR 3.1 upscaling and frame generation. AMD ships that library for Windows only; this builds it from
# the FidelityFX SDK v1.1.4 release zip (MIT, pinned by SHA-256) with the patches in patches/: the FSR 3.1 upscaler, FSR 3.1
# frame generation and its frame interpolation swapchain, and their Vulkan backend. See README.md.
#
#   scripts/linux/ffx_linux/build.sh <output folder> [download cache]
#
# Needs curl, unzip, patch, cmake, ninja, a C++17 compiler, glslangValidator and the Vulkan headers and loader
# (Arch: cmake ninja glslang vulkan-headers vulkan-icd-loader; Ubuntu: cmake ninja-build glslang-tools libvulkan-dev).
# Environment (all optional):
#   FFX_LLVM=<folder with bin/clang, bin/clang++, bin/ld.lld>   build with that clang and lld instead of the system's compiler
#                                                               (deps.sh: the pinned LLVM; the C++ runtime is linked statically)
#   FFX_GLSLANG=<glslangValidator or a glslang that takes its options>   the GLSL compiler the shader compiler runs
#   FFX_WORK=<folder>   scratch folder (default: a new one under $TMPDIR); removed at the end
# The library needs only glibc and libvulkan.so.1: its C++ runtime is inside. Its glibc floor is that of the system it is
# built on (scripts/linux/deps.sh runs on the CI's Ubuntu 22.04: 2.35) and the script refuses to finish when a symbol
# newer than $FFX_MAX_GLIBC (default 2.35) is needed or a libstdc++ / libgcc_s is a dynamic dependency.
set -euo pipefail
version=1.1.4
sha256=0216556bfb0e243cec30004a2a98d38f4e3f7406cb7938e3c1b85c758e95d952
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
cache=${2:-}
work=${FFX_WORK:-$(mktemp -d)}
mkdir -p "$work"
work=$(cd "$work" && pwd)
trap 'rm -rf "$work"' EXIT
[ -n "$cache" ] || cache=$work
mkdir -p "$cache"
zip="$cache/FidelityFX-SDK-v$version.zip"
if ! { [ -f "$zip" ] && echo "$sha256  $zip" | sha256sum -c --status; }; then
    curl -fsSL --retry 5 --retry-delay 10 -o "$zip.part" "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/v$version/FidelityFX-SDK-v$version.zip"
    echo "$sha256  $zip.part" | sha256sum -c --status || { rm -f "$zip.part"; echo "FidelityFX SDK zip: wrong SHA-256" >&2; exit 1; }
    mv "$zip.part" "$zip"
fi
mkdir "$work/sdk"
unzip -q "$zip" common.cmake docs/license.md 'ffx-api/*' 'sdk/*' -x 'ffx-api/bin/*' -d "$work/sdk"
# the zip's text files have CRLF line ends; the patches are LF
find "$work/sdk" -type f \( -name '*.h' -o -name '*.cpp' -o -name '*.c' -o -name '*.glsl' -o -name '*.hpp' -o -name '*.txt' \) \
    -exec sed -i 's/\r$//' {} +
for p in fidelityfx-sdk-v1.1.4-linux.patch fsr-3.1.5-backport.patch nx2navi-ffx-api-linux.patch \
         fidelityfx-sdk-v1.1.4-framegeneration-linux.patch; do
    patch -s -p1 -d "$work/sdk" < "$here/patches/$p"
done
tc=()
if [ -n "${FFX_LLVM:-}" ]; then
    t=$(cd "$FFX_LLVM" && pwd)
    tc=(-DCMAKE_C_COMPILER="$t/bin/clang" -DCMAKE_CXX_COMPILER="$t/bin/clang++" "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld"
        "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld")
fi
# (gen_shaders.sh and the shader compiler call "glslangValidator": a folder first in PATH names the one wanted)
if [ -n "${FFX_GLSLANG:-}" ]; then
    mkdir -p "$work/bin"
    ln -sf "$(readlink -f "$FFX_GLSLANG")" "$work/bin/glslangValidator"
    export PATH="$work/bin:$PATH"
fi
# (the same sources under another folder give the same library: no build path in the debug info or __FILE__)
export CXXFLAGS="${CXXFLAGS:-} -ffile-prefix-map=$work=. -ffile-prefix-map=$here=ffx_linux"
export CFLAGS="${CFLAGS:-} -ffile-prefix-map=$work=. -ffile-prefix-map=$here=ffx_linux"
cmake -S "$here" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DFFX_ROOT="$work/sdk" "${tc[@]+"${tc[@]}"}" >/dev/null
cmake --build "$work/build"
so=$work/build/libamd_fidelityfx_vk.so
strip --strip-unneeded "$so" 2>/dev/null || true
# ---- check: exports, glibc floor, no C++ runtime dependency ----
max=${FFX_MAX_GLIBC:-2.35}
syms=$(objdump -T "$so" | awk '$4 == ".text"' | grep -cE ' ffx(CreateContext|DestroyContext|Configure|Query|Dispatch)$' || true)
[ "$syms" = 5 ] || { echo "libamd_fidelityfx_vk.so exports $syms of the 5 ffx entry points"; exit 1; }
need=$(objdump -T "$so" | grep -oE 'GLIBC_[0-9]+(\.[0-9]+)+' | sed 's/GLIBC_//' | sort -uV | tail -1)
if [ -n "$need" ] && [ "$(printf '%s\n%s\n' "$need" "$max" | sort -V | tail -1)" != "$max" ]; then
    echo "libamd_fidelityfx_vk.so needs GLIBC_$need, newer than the GLIBC_$max floor (build on the oldest system)"; exit 1
fi
if objdump -p "$so" | grep NEEDED | grep -E 'libstdc\+\+|libgcc_s|libc\+\+'; then
    echo "libamd_fidelityfx_vk.so depends on a dynamic C++ runtime"; exit 1
fi
cp "$so" "$out/"
{ cat "$work/sdk/docs/license.md"; echo; echo 'Built with code from xplane-fsr3 (https://github.com/JonathanOrr/xplane-fsr3):'; echo
  cat "$here/LICENSE.xplane-fsr3"; } > "$out/libamd_fidelityfx_vk-LICENSE.txt"
echo "built $out/libamd_fidelityfx_vk.so (AMD FidelityFX SDK $version, FSR 3.1 upscaler and frame generation; glibc ${need:-none})"
