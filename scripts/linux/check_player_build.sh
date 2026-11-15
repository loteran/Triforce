#!/bin/bash
# Game-free: a player's build started from what the AppImage carries, not from the repository. CI's AppImage job packages
# the builder but never builds a game with it, so a file the AppImage leaves out was found by every player instead: #1459
# made nx2/build.py write the menu's fonts from third_party/fonts at every build start, the AppImage does not carry that
# folder, and every Linux build stopped at LibertinusSans-Regular.ttf while CI stayed green (2026-11-10, #1567).
#
#   scripts/linux/check_player_build.sh [<Triforce-...-x86_64.AppImage>]   (default: the newest in build-linux/)
#
# The image is extracted, as its runtime does, and run with AppRun's own environment (the AppImage's Python, compiler,
# sysroot, FFmpeg and the prebuilt Vulkan back end, HOOKSHOT_VK_PREBUILT) in an otherwise empty one, so nothing of the
# checkout or of CI's caches stands in for what the image lacks:
#   - AppRun --cli --help: nx2/make_game.py, as the builder window starts it;
#   - nx2/build.py --standalone --dry-run, as make_game.py runs it: its whole start-up up to the compile, which is where
#     the game's lifted code is first needed. Every other file the compile would read must be in the image.
# Neither may write into the image's tree: on a player's PC it is read-only.
set -euo pipefail
cd "$(dirname "$0")/../.."
IMAGE=${1:-$(ls -t build-linux/Triforce-*-x86_64.AppImage 2>/dev/null | sed -n 1p || true)}
[ -n "$IMAGE" ] && [ -f "$IMAGE" ] || { echo "no AppImage to check (scripts/linux/appimage.sh makes one)"; exit 1; }
IMAGE=$(readlink -f "$IMAGE")
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
(cd "$TMP" && "$IMAGE" --appimage-extract >/dev/null)
R=$TMP/squashfs-root
T=$R/usr/share/hookshot
mkdir -p "$TMP/home" "$TMP/work"
# (AppRun's environment, from AppRun itself: its lines before it starts Python, sourced with $0 naming it, so HERE is the image)
sed '/^PY=/,$d' "$R/AppRun" > "$TMP/apprun-env.sh"
in_image() {
    env -i PATH=/usr/bin:/bin HOME="$TMP/home" LANG=C.UTF-8 bash -c '. "$1"; shift; exec "$@"' "$R/AppRun" "$TMP/apprun-env.sh" "$@"
}
touch "$TMP/before"
sleep 1   # (so a file written below is newer than the stamp)

echo "== AppRun --cli --help"
in_image "$R/AppRun" --cli --help > "$TMP/help.txt"
grep -q -- "--nup" "$TMP/help.txt" || { cat "$TMP/help.txt"; echo "nx2/make_game.py did not start"; exit 1; }

echo "== nx2/build.py --standalone --dry-run (as nx2/make_game.py runs it, with no game)"
# (-B: no __pycache__ written into the image's tree. make_game.py's other settings: nx2/make_game.py, the compile's run())
(cd "$T" && in_image env OOT_WORK="$TMP/work" RX_RELEASE=1 A64_HOT_CHUNK=1 PYTHONIOENCODING=utf-8 \
    "$R/usr/python/bin/python3" -I -B -u -X utf8 nx2/build.py --standalone --jobs 1 --dry-run)

echo "== AMD FSR 4's kit (nx2/fsr4_linux.py): its tools start, and the library's compiler links against the sysroot"
K=$R/usr/lib/hookshot/fsr4
for tool in dxil-spirv glslang spirv-as spirv-dis spirv-opt spirv-val; do
    [ -x "$K/tools/$tool" ] || { echo "the FSR 4 kit has no $tool"; exit 1; }
done
# (no `| head`/`grep -q` below: an early-closing reader SIGPIPEs the writer under pipefail)
# (each runs here: the tools need nothing of this system but its C library; dxil-spirv finds its library beside it)
in_image env LD_LIBRARY_PATH="$K/tools" "$K/tools/dxil-spirv" --help > /dev/null 2>&1 ||
    in_image env LD_LIBRARY_PATH="$K/tools" "$K/tools/dxil-spirv" 2>&1 | grep -i "usage" > /dev/null ||
    { echo "dxil-spirv did not start:"; in_image env LD_LIBRARY_PATH="$K/tools" "$K/tools/dxil-spirv" --help 2>&1 | sed -n 1,5p; exit 1; }
in_image "$K/tools/glslang" --version > /dev/null
in_image "$K/tools/spirv-val" --version > /dev/null
for py_tool in cmake ninja; do in_image "$R/usr/python/bin/$py_tool" --version > /dev/null; done
in_image "$R/usr/lib/hookshot/emsdk/upstream/bin/llvm-objdump" --version > /dev/null
[ -f "$K/src/CMakeLists.txt" ] && [ -f "$K/src/tools/setup.py" ] || { echo "the FSR 4 kit has no fsr4-vk sources"; exit 1; }
# (a library that calls Vulkan, made as nx2/fsr4_linux.py makes AMD FSR 4's: the AppImage's clang and lld, its sysroot and
# the sysroot's Vulkan loader, the C++ runtime inside)
cat > "$TMP/vk.cpp" <<'CPP'
#include <vulkan/vulkan.h>
#include <string>
extern "C" int vk_probe(void) { uint32_t n = 0; return vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr) + (int)std::to_string(n).size(); }
CPP
S=$R/usr/lib/hookshot/sysroot
in_image "$R/usr/lib/hookshot/emsdk/upstream/bin/clang++" --sysroot="$S" -fuse-ld=lld -O2 -shared -fPIC \
    -static-libstdc++ -static-libgcc -I"$K/src/third_party/Vulkan-Headers/include" "$TMP/vk.cpp" -o "$TMP/vk.so" \
    "$S/usr/lib/x86_64-linux-gnu/libvulkan.so"
echo "AMD FSR 4's kit runs here"

echo "== AMD FidelityFX (FSR 3.1): libamd_fidelityfx_vk.so is in the image and loads, with its 5 entry points"
FFX=$R/usr/lib/hookshot/ffx
[ -f "$FFX/libamd_fidelityfx_vk.so" ] && [ -s "$FFX/libamd_fidelityfx_vk-LICENSE.txt" ] ||
    { echo "the image has no libamd_fidelityfx_vk.so with its licence"; exit 1; }
[ -s "$T/licenses/fidelityfx/libamd_fidelityfx_vk-LICENSE.txt" ] || { echo "the image has no FidelityFX licence under licenses/"; exit 1; }
# (loaded with the image's Python and no GPU: the library needs only libc and libvulkan.so.1, which dlopen finds on any PC with
# a Vulkan loader: CI's runner has libvulkan1, which appimage.sh needs to build)
in_image "$R/usr/python/bin/python3" -I -B -c '
import ctypes, sys
lib = ctypes.CDLL(sys.argv[1])
missing = [n for n in ("ffxCreateContext", "ffxDestroyContext", "ffxConfigure", "ffxQuery", "ffxDispatch") if not hasattr(lib, n)]
sys.exit("libamd_fidelityfx_vk.so lacks " + ", ".join(missing) if missing else 0)' "$FFX/libamd_fidelityfx_vk.so"
echo "libamd_fidelityfx_vk.so loads"

# (Python's own __pycache__ aside: AppRun runs Python with -I, which writes it where it can, and does without where it cannot)
written=$(find "$R" -type f -newer "$TMP/before" ! -path "*/__pycache__/*" -print)
if [ -n "$written" ]; then
    echo "a player's build wrote into the AppImage's tree, which is read-only on a player's PC:"
    echo "$written" | sed -n 1,20p
    exit 1
fi
echo "a player's build starts from the AppImage's tree"
