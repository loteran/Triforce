"""AMD's FSR 4 libraries for a build with AMD FSR 4 (HOOKSHOT_FSR=1, gpu/vk/vk_fsr4.cpp).

The build itself needs nothing from here: AMD's headers are in third_party/fidelityfx. What the game needs at run time,
beside its exe, are AMD's two signed libraries of FidelityFX SDK 2.4.0, which this fetches from AMD's own repository at
that tag, each checked against its SHA-256:
  - amd_fidelityfx_loader_vk.dll (39 KB)
  - amd_fidelityfx_upscaler_vk.dll (36 MB)
They are AMD's, under the SDK's MIT license (third_party/fidelityfx/LICENSE.md, copied beside them). Without them the
game runs as before and the Upscaler row does not offer AMD FSR 4; with them FSR 4 is offered where AMD's library says
the card runs it (Radeon RX 6000, 7000 and 9000 cards with a current driver).

(Triforce) One more library of the same release, from the same folder of AMD's repository, for AMD FSR 4's frame
generation (the Frame generation row's AMD FSR 4, gpu/vk/vk_fsr4.cpp fsr4fg):
  - amd_fidelityfx_framegeneration_vk.dll (35 MB)
It is fetched after the two and is not required: when it cannot be had, or is not the pinned file, that is said (with
the size and SHA-256 that came, for the table) and the game is built with FSR 4 as before, its Frame generation row
without AMD FSR 4. The license text beside the game lists it among AMD's MIT files.

Usage:  python -I scripts/fsr_sdk.py [--game <game folder>] [<folder>]
  <folder>: where the two files are kept between builds; default work/fsr-sdk
  --game: copied beside the game's exe too (nx2/make_game.py --fsr does the same for the folder it builds)
Windows only: on Linux the game looks for a Vulkan FSR 4 library of its own (see gpu/vk/vk_fsr4.cpp).
"""
import argparse
import hashlib
import os
import shutil
import sys
import urllib.request

VERSION = "2.4.0"
BASE = "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v%s/Kits/FidelityFX/signedbin/" % VERSION
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (file, its size, its SHA-256)
RUNTIME = [
    ("amd_fidelityfx_loader_vk.dll", 39184, "38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9"),
    ("amd_fidelityfx_upscaler_vk.dll", 36135696, "4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7"),
]
# (Triforce) AMD FSR 4's frame generation library: wanted, not required (the docstring). Its size and SHA-256 are those of
# the file as NX2navi's releases carried it from the same tag (v2.4.0), read from a copy on the build PC whose two
# libraries above match their pins; a file of another size or SHA-256 from AMD's repository is refused and reported
OPTIONAL = [
    ("amd_fidelityfx_framegeneration_vk.dll", 36806928, "d002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4"),
]
LICENSE_SOURCE = os.path.join(ROOT, "third_party", "fidelityfx", "LICENSE.md")
LICENSE_NAME = "amd-fidelityfx-LICENSE.md"

NOTICE = ("AMD FSR 4 uses AMD's signed libraries of FidelityFX SDK %s (MIT license), downloaded from AMD's repository: "
          "github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK" % VERSION)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def missing(folder):
    """The files that are not in folder, or are other files than the pinned ones"""
    return [name for name, _, digest in RUNTIME
            if not os.path.isfile(os.path.join(folder, name)) or sha256_file(os.path.join(folder, name)) != digest]


def complete(folder):
    return bool(folder) and os.path.isdir(folder) and not missing(folder)


def have_optional(folder):
    """(Triforce) The optional files that are in folder as pinned: AMD FSR 4's frame generation library"""
    return [name for name, _, digest in OPTIONAL
            if os.path.isfile(os.path.join(folder, name)) and sha256_file(os.path.join(folder, name)) == digest]


def download(url):
    request = urllib.request.Request(url, headers={"User-Agent": "fsr-sdk-fetch"})
    with urllib.request.urlopen(request, timeout=300) as r:
        return r.read()


def fetch(folder, say=print, get=download):
    """Every pinned file into folder that is not there yet. Raises OSError or ValueError; a file that is not the pinned
    one is never left in folder"""
    need = missing(folder) if os.path.isdir(folder) else [name for name, _, _ in RUNTIME]
    have = have_optional(folder) if os.path.isdir(folder) else []
    more = [name for name, _, _ in OPTIONAL if name not in have]
    if not need and not more:
        say("AMD's FSR 4 libraries are in %s already" % folder)
        return
    os.makedirs(folder, exist_ok=True)
    table = {name: (size, digest) for name, size, digest in RUNTIME + OPTIONAL}

    def one(name):
        size, digest = table[name]
        say("AMD FSR 4: %s (%.1f MB) from AMD's repository" % (name, size / 1048576.0))
        data = get(BASE + name)
        got = hashlib.sha256(data).hexdigest()
        if len(data) != size or got != digest:
            raise ValueError("%s is not the pinned file of FidelityFX SDK %s (it has %d bytes and the SHA-256 %s; pinned: %d "
                             "bytes, %s)" % (name, VERSION, len(data), got, size, digest))
        tmp = os.path.join(folder, name + ".part")
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, os.path.join(folder, name))

    for name in need:
        one(name)
    # (Triforce) the frame generation library, after the two: a failure here is said and leaves FSR 4 the upscaler as it is
    for name in more:
        try:
            one(name)
        except (OSError, ValueError) as e:
            say("AMD FSR 4 frame generation: %s could not be had (%s): the game is built without it, its Frame generation "
                "row without AMD FSR 4" % (name, e))


def install_runtime(folder, game):
    """AMD's two libraries and their license text from folder beside the game's exe, and (Triforce) the frame generation
    library when folder has the pinned one; a game folder that had it loses it when folder does not. Returns what was placed"""
    placed = []
    optional = have_optional(folder)
    for name in [name for name, _, _ in RUNTIME] + optional:
        shutil.copyfile(os.path.join(folder, name), os.path.join(game, name))
        placed.append(os.path.join(game, name))
    for name, _, _ in OPTIONAL:   # (not left from a build before, beside a loader it may not go with)
        if name not in optional and os.path.isfile(os.path.join(game, name)):
            os.remove(os.path.join(game, name))
    if os.path.isfile(LICENSE_SOURCE):
        shutil.copyfile(LICENSE_SOURCE, os.path.join(game, LICENSE_NAME))
    return placed


def remove_runtime(game):
    """A game folder built without FSR 4 after one built with it loses AMD's files. True when any was there"""
    gone = False
    for name in [name for name, _, _ in RUNTIME + OPTIONAL] + [LICENSE_NAME]:
        path = os.path.join(game, name)
        if os.path.isfile(path):
            os.remove(path)
            gone = True
    return gone


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("folder", nargs="?", default=os.path.join(ROOT, "work", "fsr-sdk"))
    p.add_argument("--game")
    a = p.parse_args()
    print(NOTICE)
    try:
        fetch(a.folder)
    except (OSError, ValueError) as e:
        print("AMD's FSR 4 libraries could not be downloaded: %s" % e)
        return 1
    if a.game:
        for path in install_runtime(a.folder, a.game):
            print("AMD FSR 4: %s" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
