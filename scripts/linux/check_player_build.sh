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

# ((Triforce) v0.5.0: the dry run stops before the compile, so a header the compile includes went unseen: v0.5.0's first
# AppImage had no third_party/smaa and every Linux build stopped at gpu/vk/vk_aa.cpp. Each "../" include of the sources the
# image compiles must be in the image)
echo "== the sources' relative includes"
missing=0
while IFS= read -r src; do
    d=$(dirname "$src")
    for inc in $(grep -hoE '^[[:space:]]*#[[:space:]]*include[[:space:]]*"\.\./[^"]+"' "$src" | sed -E 's/.*"(.*)"/\1/'); do
        [ -f "$d/$inc" ] || { echo "missing: $inc (included by ${src#"$T"/})"; missing=1; }
    done
done < <(find "$T/host" "$T/gpu" -type f \( -name "*.c" -o -name "*.cpp" -o -name "*.h" \))
[ "$missing" = 0 ] || exit 1
echo "every relative include of the image's sources is in it"

echo "== nx2/build.py --standalone --dry-run (as nx2/make_game.py runs it, with no game)"
# (-B: no __pycache__ written into the image's tree. make_game.py's other settings: nx2/make_game.py, the compile's run())
(cd "$T" && in_image env OOT_WORK="$TMP/work" RX_RELEASE=1 A64_HOT_CHUNK=1 PYTHONIOENCODING=utf-8 \
    "$R/usr/python/bin/python3" -I -B -u -X utf8 nx2/build.py --standalone --jobs 1 --dry-run)

# (a player's build without the prebuilt back end, which is every build with NVIDIA DLSS ticked (HOOKSHOT_VK_PREBUILT dropped): nx2/build.py
# compiles the back end from this tree. A header the image leaves out stopped those builds 77 minutes in ('ffx_api.h' not found,
# 2026-11). The same build.py start-up as above, with the back end's headers found where a player's are (the image's cpm/) and HOOKSHOT_VK_SYNTAX=1:
# every gpu/vk/*.cpp parsed (-fsyntax-only) with the flags the real compile uses, plain and with AMD FSR 4 ticked (its headers are
# third_party/fidelityfx; the SDK DLL is not needed to compile). NVIDIA DLSS only with CHECK_NGX=1 (below). Not covered: RenoDX (its shaders are fetched on the player's PC). CHECK_VK_SYNTAX=0 skips it.)
if [ "${CHECK_VK_SYNTAX:-1}" = "1" ]; then
    JOBS=$(nproc 2>/dev/null || echo 2)
    for fsr in 0 1; do
        echo "== the Vulkan back end compiled from the image's tree, syntax only (as a player's build without the prebuilt, HOOKSHOT_FSR=$fsr)"
        fsr_env=(); [ "$fsr" = 1 ] && fsr_env=(HOOKSHOT_FSR=1)
        (cd "$T" && in_image env -u HOOKSHOT_VK_PREBUILT ${fsr_env[@]+"${fsr_env[@]}"} HOOKSHOT_VK_SYNTAX=1 OOT_WORK="$TMP/work" RX_RELEASE=1 \
            A64_HOT_CHUNK=1 PYTHONIOENCODING=utf-8 \
            "$R/usr/python/bin/python3" -I -B -u -X utf8 nx2/build.py --standalone --jobs "$JOBS" --dry-run)
    done
    # (NVIDIA DLSS ticked, the case that drops the prebuilt: NVIDIA's headers and static library fetched by the image's own
    # scripts/ngx_sdk.py --build-only into a temp folder (nothing of it is kept or goes into the image), or CHECK_NGX_SDK=<folder>
    # of one fetched already. CHECK_NGX=1 turns it on (CI: the Ubuntu 22.04 job; needs the network), off by default.)
    if [ "${CHECK_NGX:-0}" = "1" ]; then
        NGX=${CHECK_NGX_SDK:-$TMP/ngx-sdk}
        if [ ! -f "$NGX/lib/libnvsdk_ngx.a" ]; then
            echo "== NVIDIA's DLSS SDK, headers and static library (scripts/ngx_sdk.py --build-only)"
            (cd "$T" && in_image "$R/usr/python/bin/python3" -I -B scripts/ngx_sdk.py --build-only --accept-nvidia-license "$NGX")
        fi
        for fsr in 0 1; do
            echo "== the Vulkan back end with NVIDIA DLSS, syntax only (HOOKSHOT_FSR=$fsr)"
            fsr_env=(); [ "$fsr" = 1 ] && fsr_env=(HOOKSHOT_FSR=1)
            (cd "$T" && in_image env -u HOOKSHOT_VK_PREBUILT ${fsr_env[@]+"${fsr_env[@]}"} HOOKSHOT_NGX_SDK="$NGX" HOOKSHOT_VK_SYNTAX=1 \
                OOT_WORK="$TMP/work" RX_RELEASE=1 A64_HOT_CHUNK=1 PYTHONIOENCODING=utf-8 \
                "$R/usr/python/bin/python3" -I -B -u -X utf8 nx2/build.py --standalone --jobs "$JOBS" --dry-run)
        done
    fi
fi

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
# (loaded with the image's Python and no GPU. The library needs only libc and libvulkan.so.1, which a player's PC always has
# (its GPU driver needs it) but a clean container does not. So the sysroot's loader (appimage.sh copies the real
# libvulkan.so.1 there, for the FSR 4 kit's linking; the AppImage never ships it at run time) is put on LD_LIBRARY_PATH for
# this one load. If it cannot be loaded either, the ELF is read instead: the 5 symbols exported, and nothing NEEDED but
# libvulkan.so.1, libm, libc and ld-linux.)
cat > "$TMP/ffx_check.py" <<'PY'
import ctypes, struct, sys
path, mode = sys.argv[1], sys.argv[2]
SYMS = ("ffxCreateContext", "ffxDestroyContext", "ffxConfigure", "ffxQuery", "ffxDispatch")
if mode == "load":
    lib = ctypes.CDLL(path)
    missing = [n for n in SYMS if not hasattr(lib, n)]
else:
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 1:
        sys.exit("not a 64-bit little-endian ELF")
    shoff = struct.unpack_from("<Q", d, 0x28)[0]
    shentsize, shnum = struct.unpack_from("<HH", d, 0x3A)
    sh = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize) for i in range(shnum)]
    dynsym = next(s for s in sh if s[1] == 11)   # SHT_DYNSYM
    dynstr = sh[dynsym[6]]
    def name(tab, off):
        o = tab[4] + off
        return d[o:d.index(b"\0", o)].decode()
    exported = set()
    for i in range(dynsym[5] // 24):
        st_name, st_info, _o, st_shndx = struct.unpack_from("<IBBH", d, dynsym[4] + i * 24)
        if st_shndx != 0 and st_name:
            exported.add(name(dynstr, st_name))
    missing = [n for n in SYMS if n not in exported]
    dyn = next(s for s in sh if s[1] == 6)       # SHT_DYNAMIC
    strtab = sh[dyn[6]]
    needed = []
    for i in range(dyn[5] // 16):
        tag, val = struct.unpack_from("<qQ", d, dyn[4] + i * 16)
        if tag == 1:
            needed.append(name(strtab, val))
    allowed = ("libvulkan.so.1", "libm.so", "libc.so", "ld-linux")
    extra = [n for n in needed if not n.startswith(allowed)]
    if extra:
        sys.exit("libamd_fidelityfx_vk.so needs " + ", ".join(extra) + " besides libvulkan.so.1, libm, libc, ld-linux")
sys.exit("libamd_fidelityfx_vk.so lacks " + ", ".join(missing) if missing else 0)
PY
mkdir -p "$TMP/vkloader"
ln -sf "$S/usr/lib/x86_64-linux-gnu/libvulkan.so" "$TMP/vkloader/libvulkan.so.1"
if in_image env LD_LIBRARY_PATH="$TMP/vkloader" "$R/usr/python/bin/python3" -I -B "$TMP/ffx_check.py" "$FFX/libamd_fidelityfx_vk.so" load; then
    :
else
    echo "(the sysroot's libvulkan is not loadable here: reading the ELF instead)"
    in_image "$R/usr/python/bin/python3" -I -B "$TMP/ffx_check.py" "$FFX/libamd_fidelityfx_vk.so" elf
fi
echo "libamd_fidelityfx_vk.so loads"

# (Python's own __pycache__ aside: AppRun runs Python with -I, which writes it where it can, and does without where it cannot)
written=$(find "$R" -type f -newer "$TMP/before" ! -path "*/__pycache__/*" -print)
if [ -n "$written" ]; then
    echo "a player's build wrote into the AppImage's tree, which is read-only on a player's PC:"
    echo "$written" | sed -n 1,20p
    exit 1
fi
echo "a player's build starts from the AppImage's tree"
