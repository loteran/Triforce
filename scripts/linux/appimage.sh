#!/bin/bash
# The Linux AppImage: Hookshot Builder with everything it needs to build the game on the player's PC. No game data and
# nothing made from it is in it: the game is built from the player's own copy, as on Windows.
#
#   scripts/linux/deps.sh        (first: the compiler, Python, FFmpeg, SDL2, headers into build-linux/deps)
#   scripts/linux/appimage.sh    -> build-linux/Triforce-<version>-x86_64.AppImage
# The same image builds the game and, with --play, starts the one it saved.
#
# Built on the oldest system it is for (CI: Ubuntu 22.04): the sysroot it carries is that system's C library, so the
# game it builds runs on glibc 2.35 and newer. work/ (hot_funcs.txt, hot_order.txt, hookshot_pieces.prof,
# image/chunks.txt: addresses and counts from playing, no game code) is carried along when it is there.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
B=$ROOT/build-linux
DEPS=${DEPS:-$B/deps}
APPDIR=$B/AppDir
VERSION=${HOOKSHOT_VERSION:-$(git describe --tags --always --match "v[0-9]*" 2>/dev/null || echo dev)}
export EMSDK=$DEPS/emsdk RX_CPM=$DEPS/cpm
PY=$DEPS/python/bin/python3

# ---- the Vulkan back end, built here once (it needs no game) ----
# (with AMD FSR 4: only AMD's MIT API headers are compiled in, and nothing of it acts until a Vulkan FSR 4 library is
# beside the game, gpu/vk/vk_fsr4.cpp fsr4_wants_descriptor_buffer; the game built from this image offers it then)
VKW=$B/vk-work
rm -rf "$VKW/sa/build/triforce_vk.so"
# ((Triforce) 11-20, HOOKSHOT_FSR=1: with AMD FSR 4 (gpu/vk/vk_fsr4.cpp). A player's build reuses this back end as it is
# (AppRun's HOOKSHOT_VK_PREBUILT), so built without it no Linux player could have FSR 4, whatever they set. With it, the
# Upscaler row offers FSR 4 where the player's own Vulkan FSR 4 library (amd_fidelityfx_upscaler_vk.so) is in the game folder
# or RX_VK_FSR_DIR and runs on their GPU; without the library it is not offered, as before. AMD ships no Linux library and
# none is packed here)
OOT_WORK=$VKW RX_MARCH=x86-64-v3 RX_VK_ONLY=1 RX_RELEASE=1 HOOKSHOT_FSR=1 "$PY" -I nx2/build.py --standalone --jobs "${JOBS:-$(nproc)}"
VK=$VKW/sa/build/triforce_vk.so
[ -f "$VK" ] || { echo "triforce_vk.so was not built"; exit 1; }

# ---- the tools' native helpers, built now: the AppImage is read-only. a64emit with its C++ runtime inside (the one this system
# builds with is newer than older systems have), and the stamp build_emit.py checks, so it is not built again ----
E=third_party/a64-kit/lift
"$EMSDK/upstream/bin/clang++" -std=c++20 -O2 -shared -fPIC -static-libstdc++ -static-libgcc -Wno-everything \
    -o "$E/liba64emit.so" "$E/a64emit.cpp"
"$PY" -I -c "import hashlib, sys; f = ['$E/a64emit.cpp', '$E/suyu/arm64_to_c.h']; open('$E/liba64emit.so.sha1', 'w').write(hashlib.sha1(b''.join(open(x, 'rb').read() for x in f)).hexdigest())"
"$PY" -I third_party/a64-kit/lift/build_emit.py
"$PY" -I -c "import sys; sys.path.insert(0, 'nx2'); import zbic; zbic.lib()"

rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/share/hookshot" "$APPDIR/usr/lib/hookshot"
T=$APPDIR/usr/share/hookshot
L=$APPDIR/usr/lib/hookshot

# ---- the tools (the sources the player's build compiles, and the scripts) ----
cp -a nx2 host scripts LICENSE readme.md assets "$T/"
printf '%s
' "$VERSION" > "$T/VERSION"   # (no git history in here: nx2/version.py reads the release name from it)
mkdir -p "$T/builder" "$T/gpu/render" "$T/gpu/vk" "$T/third_party"
cp -a builder/linux "$T/builder/"
cp -a gpu/render/*.h "$T/gpu/render/"
# (every gpu/vk header the host's sources include: v2.0.6 shipped without oot_cheat_items.h and no Linux build finished, issue 990.
# tests/test_appimage_headers.py checks that this covers what host/ includes)
for h in $(grep -rhoE 'gpu/vk/[A-Za-z0-9_]+[.]h' host | sort -u); do if [ -f "$h" ]; then cp -a "$h" "$T/gpu/vk/"; fi; done
# ((Triforce) zstd's decoder here, with what host/ compiles: host/host_zstd.h includes its headers since the host's zstd
# decoder for the game's files, and tests/test_appimage_source_headers.py checks the copy up to the next line)
cp -a third_party/zstd "$T/third_party/"
cp -a third_party/opus "$T/third_party/"
cp -a third_party/kenney_input "$T/third_party/"
# ((Triforce) v0.5.0: SMAA's lookup textures, which gpu/vk/vk_aa.cpp includes: left out of v0.5.0's first AppImage, and every
# Linux build stopped at vk_aa.cpp. scripts/linux/check_player_build.sh now checks every relative include of the image's sources)
cp -a third_party/smaa "$T/third_party/"
# ((Triforce) 11-20, ticket "launcher error when updating on 0.5.2 from 0.3.7": AMD FSR 4's API headers, which gpu/vk/vk_fsr4.cpp
# includes when the build has FSR 4 (--fsr, HOOKSHOT_FSR=1, or --update of a folder built with it): left out, and the update of
# such a folder stopped at vk_fsr4.cpp ("api/include/ffx_api.h file not found"))
cp -a third_party/fidelityfx "$T/third_party/"
# (NVIDIA DLSS, when ticked in the builder: scripts/ngx_sdk.py and the Vulkan back end's sources, compiled with HOOKSHOT_NGX_SDK)
mkdir -p "$T/scripts"
cp scripts/ngx_sdk.py "$T/scripts/"
cp -a gpu/vk gpu/compat gpu/third_party "$T/gpu/"
cp -a third_party/imgui third_party/fonts "$T/third_party/"
# (the AMD FidelityFX API headers the back end compiles against in a player's build without the prebuilt: nx2/build.py's ffx3_flags
# (vk_ffx3.cpp, vk_ffx_fg.cpp: every build) and fsr_flags (vk_fsr4.cpp: FSR 4 ticked). Missing, DLSS's build stopped after 77 minutes on 'ffx_api.h' not found.
# scripts/linux/check_player_build.sh compiles the back end from this tree, so the next one is found in CI)
cp -a third_party/ffx_api_114 third_party/fidelityfx "$T/third_party/"
mkdir -p "$T/third_party/zbic" "$T/third_party/a64-kit"
cp -a third_party/zbic/zbic.so third_party/zbic/LICENSE third_party/zbic/PROVENANCE.md "$T/third_party/zbic/"
cp -a third_party/a64-kit/* "$T/third_party/a64-kit/"
rm -f "$T/third_party/a64-kit/tools/lld-link.exe"
if [ -d work ]; then
    mkdir -p "$T/work/image"
    for f in hot_funcs.txt hot_order.txt hookshot_pieces.prof pipeline_states.bin; do [ -f "work/$f" ] && cp "work/$f" "$T/work/"; done
    [ -f work/image/chunks.txt ] && cp work/image/chunks.txt "$T/work/image/"
fi
find "$T" -name __pycache__ -prune -exec rm -rf {} +

# ---- what it builds with ----
cp -a "$DEPS/python" "$APPDIR/usr/python"
# (linux fork: upstream's WebKitGTK launcher, launcher/out/triforce-launcher, is not shipped: this fork keeps its builder window)
cp "$DEPS/tk/libtcl9tk9.0.so" "$DEPS/tk/libXft.so.2" "$DEPS/tk/libXrender.so.1" "$APPDIR/usr/python/lib/"
# pip writes its console scripts (cmake, ninja, pip...) with this machine's absolute interpreter path as the shebang,
# which does not exist once the AppImage is mounted elsewhere. Swap it for pip's own relocatable launcher: a sh line
# that runs the python3 beside the script, and that Python then reads the rest as a plain string and the script.
for f in "$APPDIR/usr/python/bin/"*; do
    [ -f "$f" ] && [ ! -L "$f" ] || continue
    IFS= read -r first < "$f" 2>/dev/null || continue
    [ "$first" = "#!$DEPS/python/bin/python3" ] || [ "$first" = "#!$DEPS/python/bin/python3.12" ] || continue
    { printf '#!/bin/sh\n'"'''"'exec'"'"' "$(dirname -- "$(realpath -- "$0")")/python3" "$0" "$@"\n'"' '''"'\n'
      tail -n +2 "$f"; } > "$f.tmp" && chmod --reference="$f" "$f.tmp" && mv "$f.tmp" "$f"
done
if grep -l "^#!$DEPS/" "$APPDIR/usr/python/bin/"* 2>/dev/null; then echo "a script above still points at the build tree"; exit 1; fi
mkdir -p "$L/emsdk"
cp -a "$DEPS/emsdk/upstream" "$L/emsdk/"
cp -a "$DEPS/ffmpeg" "$L/ffmpeg"
cp "$VK" "$L/hookshot_vk.so"
cp "$DEPS/sdl2/libSDL2-2.0.so.0" "$L/libSDL2-2.0.so.0"
# (AMD FSR 4's kit, scripts/linux/deps.sh: fsr4-vk's sources and host tools, for nx2/fsr4_linux.py on the player's PC)
cp -a "$DEPS/fsr4" "$L/fsr4"
mkdir -p "$T/licenses/fsr4"
cp -a "$DEPS/fsr4/licenses/." "$T/licenses/fsr4/"
# (AMD FidelityFX, scripts/linux/deps.sh: FSR 3.1 upscaler and frame generation, MIT. make_game.py puts it beside every game)
mkdir -p "$L/ffx"
cp "$DEPS/ffx/libamd_fidelityfx_vk.so" "$DEPS/ffx/libamd_fidelityfx_vk-LICENSE.txt" "$L/ffx/"
mkdir -p "$T/licenses/fidelityfx" && cp "$DEPS/ffx/libamd_fidelityfx_vk-LICENSE.txt" "$T/licenses/fidelityfx/"
for d in "$DEPS"/cpm/*/*; do
    mkdir -p "$T/cpm/${d#"$DEPS/cpm/"}"
    cp -a "$d/include" "$T/cpm/${d#"$DEPS/cpm/"}/"
done
cp -a "$DEPS/licenses" "$T/licenses"   # (LLVM, FFmpeg, dav1d, SDL2: what this AppImage carries besides Hookshot)
# ---- the licences of the third-party parts compiled into hookshot_vk.so (tests/test_appimage_licenses.py) ----
mkdir -p "$T/licenses/imgui" "$T/licenses/zstd" "$T/licenses/libertinus"
cp third_party/imgui/LICENSE.txt third_party/imgui/PROVENANCE.md "$T/licenses/imgui/"
cp third_party/zstd/LICENSE third_party/zstd/PROVENANCE.md "$T/licenses/zstd/"
cp third_party/fonts/libertinus/OFL.txt third_party/fonts/PROVENANCE.md "$T/licenses/libertinus/"
mkdir -p "$T/licenses/ffx_api_114" "$T/licenses/fidelityfx_fsr4"
cp third_party/ffx_api_114/LICENSE.txt "$T/licenses/ffx_api_114/"
cp third_party/fidelityfx/LICENSE.md "$T/licenses/fidelityfx_fsr4/"
mkdir -p "$T/licenses/fidelityfx"
cp third_party/fidelityfx/LICENSE.md "$T/licenses/fidelityfx/"
# ---- end of the licences ----

# the sysroot: this system's C library (headers, start files, the libraries to link against) and GCC's runtime pieces
S=$L/sysroot
mkdir -p "$S/usr/lib/x86_64-linux-gnu" "$S/usr/lib/gcc/x86_64-linux-gnu" "$S/lib64"
ln -s usr/lib "$S/lib"
cp -a /usr/include "$S/usr/"
rm -rf "$S/usr/include/c++" "$S/usr/include/x86_64-linux-gnu/c++" "$S/usr/include/llvm"* "$S/usr/include/clang"*
M=/usr/lib/x86_64-linux-gnu
for f in crt1.o crti.o crtn.o Scrt1.o rcrt1.o libc.so libc.so.6 libc_nonshared.a libm.so libm.so.6 libm.a libmvec.so libmvec.so.1 \
         libmvec.a libpthread.so.0 libpthread.a libdl.so.2 libdl.a librt.so.1 librt.a libutil.a libgcc_s.so.1 ld-linux-x86-64.so.2; do
    # System aliases can have absolute targets (e.g. libmvec.so). Carry their bytes;
    # update parts refuse absolute-target symlinks, and must not use the host's library.
    [ -e "$M/$f" ] && cp -aL "$M/$f" "$S/usr/lib/x86_64-linux-gnu/" || true
done
cp -aL /lib64/ld-linux-x86-64.so.2 "$S/lib64/"
# (the Vulkan loader, to link AMD FSR 4's library against, nx2/fsr4_linux.py: the game never links it, and at run time the
# library uses the loader the game already has. Its licence beside the others)
cp -aL "$M/libvulkan.so.1" "$S/usr/lib/x86_64-linux-gnu/libvulkan.so"
mkdir -p "$T/licenses/vulkan-loader" && cp /usr/share/doc/libvulkan1/copyright "$T/licenses/vulkan-loader/copyright"
GCC=$(ls -d /usr/lib/gcc/x86_64-linux-gnu/*/ | sort -V | tail -1)
GV=$(basename "$GCC")
G=$S/usr/lib/gcc/x86_64-linux-gnu/$GV
mkdir -p "$G" "$S/usr/include/c++" "$S/usr/include/x86_64-linux-gnu/c++"
cp -aL "/usr/include/c++/$GV" "$S/usr/include/c++/"
cp -aL "/usr/include/x86_64-linux-gnu/c++/$GV" "$S/usr/include/x86_64-linux-gnu/c++/"
for f in crtbegin.o crtbeginS.o crtbeginT.o crtend.o crtendS.o libgcc.a libgcc_eh.a libgcc_s.so libstdc++.a libstdc++fs.a libstdc++exp.a; do
    [ -e "$GCC/$f" ] && cp -aL "$GCC/$f" "$G/" || true
done
# (libgcc_s.so is a linker script naming /lib/x86_64-linux-gnu/libgcc_s.so.1: found in the sysroot through lib -> usr/lib)

# ---- check: the bundled compiler builds and links a program against the sysroot, and it runs ----
cat > "$B/t.c" <<'EOF'
#include <pthread.h>
#include <stdio.h>
#include <math.h>
static void *f(void *p) { return p; }
int main(void) { pthread_t t; pthread_create(&t, 0, f, 0); pthread_join(t, 0); printf("ok %g\n", sqrt(2.0)); return 0; }
EOF
"$L/emsdk/upstream/bin/clang" --sysroot="$S" -fuse-ld=lld -O2 -march=x86-64-v3 "$B/t.c" -o "$B/t" -lpthread -lm
"$B/t"
cat > "$B/t.cpp" <<'EOF'
#include <algorithm>
#include <filesystem>
#include <format>
#include <string>
extern "C" int t_cpp(int v) { return (int)std::format("{}", v).size(); }
EOF
"$L/emsdk/upstream/bin/clang++" --sysroot="$S" -fuse-ld=lld -std=c++20 -O2 -march=x86-64-v3 -shared -fPIC -static-libstdc++ -static-libgcc \
    "$B/t.cpp" -o "$B/t.so" -ldl -lpthread -lm

# ---- the AppDir's own files ----
cp builder/linux/AppRun "$APPDIR/AppRun"
cp builder/linux/hookshot-builder.desktop "$APPDIR/"
# (RecompLauncher reads this from inside the AppImage, at /recomplauncher.json)
cp recomplauncher.json "$APPDIR/recomplauncher.json"
cp assets/triforce.png "$APPDIR/triforce.png"   # (Triforce's logo: the file the desktop entry's Icon=triforce names)
cp assets/triforce.png "$APPDIR/.DirIcon"
mkdir -p "$APPDIR/usr/share/icons/hicolor/256x256/apps"
cp assets/triforce.png "$APPDIR/usr/share/icons/hicolor/256x256/apps/triforce.png"

# (the update list, beside AppRun and not inside a hashed part: triforce-linux.txt names each part's checksum)
# (the parts are served from the linux-parts release of the repository this is built in, where
# scripts/linux/publish_parts.sh uploads them: without --base the list named NotRexed/Hookshot's release, which a fork's
# CI does not upload to, so a fork's update list named parts that were not there)
python3 scripts/linux/make_parts.py "$APPDIR" "$VERSION" "$B/parts" \
    --base "https://github.com/${GITHUB_REPOSITORY:-NotRexed/Hookshot}/releases/download/linux-parts"

OUT=$B/Triforce-$VERSION-x86_64.AppImage
rm -f "$OUT"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$DEPS/appimagetool" --runtime-file "$DEPS/runtime-x86_64" --comp zstd "$APPDIR" "$OUT"
echo "$OUT: $(du -h "$OUT" | cut -f1)"
