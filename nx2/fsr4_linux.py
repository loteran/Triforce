"""(linux) AMD FSR 4 for the Linux game, made on the player's PC when they ask for it (the builder's AMD FSR 4 tick, which
passes make_game.py --fetch-fsr4 --accept-amd-license). AMD ships FSR 4 for Windows only. A Vulkan FSR 4 upscaler for
Linux is built here (fsr4-vk, GPL-3.0), with the model and shaders it needs taken from AMD's own upscaler DLL of the FSR
SDK 2.3.0, which is downloaded from AMD's release and checked against its SHA-256. Nothing of AMD's is in the repository
or in the AppImage: the download, the extraction and the build happen on this PC, and the result stays in the game folder.

Nothing has to be installed for it, on any distribution: the AppImage carries fsr4-vk's sources and its host tools built
with GCC (HOOKSHOT_FSR4_KIT, scripts/linux/deps.sh "AMD FSR 4's kit"), CMake and Ninja in its Python, and its own clang,
lld, llvm-objdump and sysroot (HOOKSHOT_SYSROOT), which the library is compiled and linked with.

The result, amd_fidelityfx_upscaler_vk.so, goes beside the game, where gpu/vk/vk_fsr4.cpp looks for it (library_dirs) and
fsr4_wants_descriptor_buffer makes the device with what it needs. The game's back end must be built with AMD FSR 4
(HOOKSHOT_FSR; the AppImage's is). Deleting the file turns FSR 4 off again.

Steps:
  1. AMD's FSR SDK 2.3.0 release archive (about 120 MB) into the cache, checked; its DX12 upscaler DLL taken out, checked.
  2. fsr4-vk's sources copied from the kit into the cache (the AppImage is read-only).
  3. Its asset setup: the DLL read (llvm-objdump), its shaders converted and checked with the kit's tools.
  4. The library compiled and linked with the AppImage's clang against its sysroot.
  5. The library and its licence notices copied beside the game.

    python3 -I nx2/fsr4_linux.py --out <game folder> --accept-amd-license
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

SDK_URL = ('https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/v2.3.0/'
           'FidelityFX-Samples-v2.3.0-prebuilt.zip')
SDK_SHA256 = 'f90890b9323bb2f4f2404ac4cdc9395e8495ecdac6f7aa0bcdf1ad1848422273'
SDK_LICENSE_URL = 'https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.3.0/docs/license.md'
DLL_MEMBER = 'Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release/amd_fidelityfx_upscaler_dx12.dll'
DLL_SHA256 = 'd0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46'   # FSR 4.1.1.2740
LIBRARY = 'amd_fidelityfx_upscaler_vk.so'
NOTICES = 'amd_fidelityfx_upscaler_vk-NOTICES'   # a folder beside the library: its licence and third-party notices

CACHE = Path(os.environ.get('XDG_CACHE_HOME') or Path.home() / '.cache') / 'hookshot' / 'fsr4'


def progress(fraction):
    """the step's progress as make_game.py's @@progress line (its own stage is said by make_game.py)"""
    print('@@progress %.3f' % fraction, flush=True)


def say(text):
    print(text, flush=True)


def fail(message):
    print(f'AMD FSR 4: {message}', flush=True)
    sys.exit(1)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def download(url, dest, want, part):
    """dest, fetched once and checked; a file there that does not match is fetched again."""
    if dest.is_file() and sha256(dest) == want:
        return
    tmp = dest.with_suffix(dest.suffix + '.part')
    say(f'Downloading {url}')
    try:
        with urllib.request.urlopen(url, timeout=60) as r, open(tmp, 'wb') as f:
            total, got = int(r.headers.get('Content-Length') or 0), 0
            while block := r.read(1 << 20):
                f.write(block)
                got += len(block)
                if total:
                    progress(part * got / total)
    except OSError as e:
        tmp.unlink(missing_ok=True)
        fail(f'the download of {url} failed ({e})')
    if sha256(tmp) != want:
        tmp.unlink(missing_ok=True)
        fail(f'{url} does not match its SHA-256: not used')
    tmp.replace(dest)


def run(cmd, cwd=None, env=None):
    say('$ ' + ' '.join(str(c) for c in cmd))
    p = subprocess.Popen([str(c) for c in cmd], cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, errors='replace')
    for line in p.stdout:
        print(line, end='', flush=True)
    if p.wait():
        fail(f'{Path(str(cmd[0])).name} {cmd[1] if len(cmd) > 1 else ""} ended with code {p.returncode}')


def kit():
    """The AppImage's FSR 4 kit (HOOKSHOT_FSR4_KIT): src/ (fsr4-vk's sources) and tools/ (its host tools)"""
    k = Path(os.environ.get('HOOKSHOT_FSR4_KIT', ''))
    if not os.environ.get('HOOKSHOT_FSR4_KIT') or not (k / 'src' / 'CMakeLists.txt').is_file() or \
            not (k / 'tools' / 'dxil-spirv').is_file():
        fail('this needs the AppImage\'s FSR 4 kit (HOOKSHOT_FSR4_KIT; from a source checkout, scripts/linux/deps.sh '
             'makes it in build-linux/deps/fsr4)')
    return k


def compilers():
    """clang, clang++ and the folder with llvm-objdump: the AppImage's (EMSDK), else the system's"""
    llvm = Path(os.environ['EMSDK']) / 'upstream' / 'bin' if os.environ.get('EMSDK') else None
    if llvm and (llvm / 'clang++').is_file():
        return llvm / 'clang', llvm / 'clang++', llvm
    cc, cxx = shutil.which('clang'), shutil.which('clang++')
    if not (cc and cxx):
        fail('no clang: run it from the AppImage, or install clang and lld')
    return Path(cc), Path(cxx), Path(cxx).parent


def tools_env(k, llvm):
    """The kit's host tools for fsr4-vk's setup (its toolchain.py reads these overrides and builds none itself), CMake and
    Ninja from this Python, and llvm-objdump"""
    t = k / 'tools'
    lib = sorted(t.glob('libdxil-spirv-c-shared.so.*'))
    if not lib:
        fail(f'the kit has no dxil-spirv library in {t}')
    env = dict(os.environ)
    py_bin = Path(sys.executable).parent
    env['PATH'] = os.pathsep.join([str(py_bin), str(llvm), env.get('PATH', '')])
    env['LD_LIBRARY_PATH'] = os.pathsep.join(filter(None, [str(t), env.get('LD_LIBRARY_PATH')]))
    env.update(DXIL_SPIRV=str(t / 'dxil-spirv'), DXIL_SPIRV_LIBRARY=str(lib[-1]), FSR4_SPIRV_TOOLS=str(t))
    for name in ('cmake', 'ninja', 'llvm-objdump'):
        if not shutil.which(name, path=env['PATH']):
            fail(f'{name} is missing from the AppImage')
    mem_gb = os.sysconf('SC_PAGE_SIZE') * os.sysconf('SC_PHYS_PAGES') >> 30
    env['FSR4_BUILD_JOBS'] = str(max(1, min(os.cpu_count() or 1, mem_gb // 2)))
    return env


def sources(k):
    """fsr4-vk's sources in the cache, from the kit; copied again when the kit carries another commit"""
    src = CACHE / 'src'
    want = (k / 'src' / 'FSR4_VK_COMMIT').read_text().strip() if (k / 'src' / 'FSR4_VK_COMMIT').is_file() else ''
    have = (src / 'FSR4_VK_COMMIT').read_text().strip() if (src / 'FSR4_VK_COMMIT').is_file() else None
    if have != want:
        shutil.rmtree(src, ignore_errors=True)
        shutil.copytree(k / 'src', src, symlinks=True)
    return src


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--out', type=Path, required=True, help='the folder the game program is in (triforce)')
    ap.add_argument('--accept-amd-license', action='store_true',
                    help=f'agree to the licence of AMD\'s FSR SDK ({SDK_LICENSE_URL}) for the DLL it reads')
    a = ap.parse_args()
    if not a.accept_amd_license:
        fail(f'the DLL comes under AMD\'s FSR SDK licence ({SDK_LICENSE_URL}): pass --accept-amd-license to agree')
    if not (a.out / 'triforce').is_file():
        fail(f'{a.out} holds no built game')
    k = kit()
    cc, cxx, llvm = compilers()
    env = tools_env(k, llvm)
    CACHE.mkdir(parents=True, exist_ok=True)

    zip_path = CACHE / 'FidelityFX-Samples-v2.3.0-prebuilt.zip'
    download(SDK_URL, zip_path, SDK_SHA256, 0.3)
    dll = CACHE / 'amd_fidelityfx_upscaler_dx12.dll'
    if not dll.is_file() or sha256(dll) != DLL_SHA256:
        with zipfile.ZipFile(zip_path) as z, z.open(DLL_MEMBER) as f, open(dll, 'wb') as o:
            shutil.copyfileobj(f, o)
        if sha256(dll) != DLL_SHA256:
            dll.unlink()
            fail('the upscaler DLL in AMD\'s archive does not match its SHA-256')
    progress(0.3)

    src = sources(k)
    run([sys.executable, 'tools/setup.py', '--dll', dll], cwd=src, env=env)
    progress(0.6)

    # the library: the AppImage's clang and lld against its sysroot (glibc 2.35, so it loads on that and newer), with its
    # C++ runtime inside; the overlay shader with the kit's glslang and spirv-val; Vulkan's headers from the sources, and
    # the sysroot's loader to link against (the game's own loader is the one used at run time)
    build = src / 'build' / 'linux'
    shutil.rmtree(build, ignore_errors=True)   # (a cache made by another compiler or kit is not reused)
    sysroot = os.environ.get('HOOKSHOT_SYSROOT', '')
    t = k / 'tools'
    cfg = ['cmake', '-S', src, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
           '-DFSR4_EMBEDDED_ASSETS=ON', f'-DFSR4_ASSET_ROOT={src / "assets"}',
           f'-DCMAKE_C_COMPILER={cc}', f'-DCMAKE_CXX_COMPILER={cxx}', f'-DCMAKE_ASM_COMPILER={cc}',
           '-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld -static-libstdc++ -static-libgcc',
           f'-DPython3_EXECUTABLE={sys.executable}',
           f'-DFSR4_GLSLANG_VALIDATOR={t / "glslang"}', f'-DFSR4_SPIRV_VAL={t / "spirv-val"}',
           f'-DVulkan_INCLUDE_DIR={src / "third_party" / "Vulkan-Headers" / "include"}']
    if sysroot:
        cfg += [f'-DCMAKE_SYSROOT={sysroot}',
                f'-DVulkan_LIBRARY={Path(sysroot) / "usr" / "lib" / "x86_64-linux-gnu" / "libvulkan.so"}']
    run(cfg, env=env)
    run(['cmake', '--build', build, '--parallel', env['FSR4_BUILD_JOBS']], env=env)
    built = build / LIBRARY
    if not built.is_file():
        fail(f'the build wrote no {LIBRARY}')
    progress(0.95)

    tmp = a.out / (LIBRARY + '.part')
    shutil.copy2(built, tmp)
    tmp.replace(a.out / LIBRARY)
    notes = a.out / NOTICES
    shutil.rmtree(notes, ignore_errors=True)
    notes.mkdir()
    for f in ('LICENSE', 'THIRD_PARTY_NOTICES.md'):
        if (src / f).is_file():
            shutil.copy2(src / f, notes / f)
    if (src / 'LICENSES').is_dir():
        shutil.copytree(src / 'LICENSES', notes / 'LICENSES')
    progress(1.0)
    say(f'AMD FSR 4 for Linux: {a.out / LIBRARY}. The game offers it on the Upscaler row where the card runs it; '
        f'delete the file to turn it off.')


if __name__ == '__main__':
    main()
