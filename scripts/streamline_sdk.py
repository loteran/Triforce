"""NVIDIA's Streamline files for a build with NVIDIA DLSS frame generation (HOOKSHOT_STREAMLINE_SDK, gpu/vk/vk_dlssg.cpp).

NVIDIA's files are not part of Triforce and are never committed or bundled. The sl.*.dll files and the headers are under
Streamline's own MIT license (license.txt); nvngx_dlssg.dll is under the NVIDIA RTX SDKs License (nvngx_dlss.license.txt),
which licenses it for systems with NVIDIA GPUs. Both texts are fetched beside them: read them before using the files.

This takes them out of NVIDIA's Streamline SDK release at one pinned version, each file checked against its SHA-256,
into <sdk folder>, at the paths they have in the SDK:
  - include/*.h: what the build reads (only gpu/vk/vk_dlssg.cpp sees them; nothing of Streamline is linked);
  - bin/x64/: the six DLLs the game loads at run time (sl.interposer, sl.common, sl.dlss_g, sl.pcl, sl.reflex, nvngx_dlssg);
  - license.txt and bin/x64/nvngx_dlss.license.txt.
The release is one archive of 276 MB. Only the parts of it that hold these files are downloaded (HTTP range requests,
about 12 MB); a server that does not serve ranges gets the whole archive read once instead. A folder that already is
NVIDIA's whole SDK, unpacked, serves as it is.

Usage:  python -I scripts/streamline_sdk.py [--accept-nvidia-license] [--game <game folder>] [<sdk folder>]
  <sdk folder>: default work/streamline-sdk
  --game: the DLLs and the two license texts copied into <game folder>/dlssg too, where the game looks for them
          (nx2/make_game.py does the same for the folder it builds)
  --accept-nvidia-license: the licenses accepted without the question (scripts, Setup's NVIDIA DLSS tick)
Then build with HOOKSHOT_STREAMLINE_SDK=<sdk folder>. Windows only.
"""
import argparse
import hashlib
import io
import os
import shutil
import sys
import urllib.request
import zipfile

VERSION = "2.14.16"
URL = "https://github.com/NVIDIA-RTX/Streamline/releases/download/v%s/streamline-sdk-v%s.zip" % (VERSION, VERSION)
SIZE = 276016170
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LICENSE_URL = "https://github.com/NVIDIA-RTX/Streamline/blob/v%s/license.txt" % VERSION

# (path in NVIDIA's SDK, its SHA-256)
HEADERS = [
    ("include/sl.h", "e1e81a7428d15b30db37587e9469bd68a56d630d820a4accec0aee3b17e157dd"),
    ("include/sl_appidentity.h", "1337385ac9867d66fa6beb34c750e79aaf25a74a156a47e83f24937299dade87"),
    ("include/sl_consts.h", "17de74afda2cb96204ff380464bed926f660d92015fe0304dff4e329509c88b9"),
    ("include/sl_core_api.h", "328dc3a2c1dee579c200ca97e2d5b1ec38c893be4ba2745ef5ad506af7172e81"),
    ("include/sl_core_types.h", "f420cb052ce14489fc9fa0d933c3ed81a010deb01fa13b1aa8d45da1c5ba3234"),
    ("include/sl_deepdvc.h", "c6086ffe6b1e00588368fdb91e4ab9dd9617624e0e4344cd503a2ca73e46d1a4"),
    ("include/sl_device_wrappers.h", "af7741305b1a468c3a83efaa2005ecd53d2ffd7aadb8ec6868c9dcb4a8a8e3a1"),
    ("include/sl_directsr.h", "65d4e68b2a0f0425ea4d381379172197b81352a50a5e399a8e8f51953e765c71"),
    ("include/sl_dlss.h", "d2c8c61fa71794c8ba424cebce5b3079ebfab8e47ed98b4b00cf8b257ecfd6b5"),
    ("include/sl_dlss_d.h", "6e51cf9815301c597d0657970a668147e34a20db5a76d0af254ec11248f5c4fe"),
    ("include/sl_dlss_g.h", "1fc18cbe004e280df1f787276d08a1b28b8a8c4c65856fbaa659f56dff6a915d"),
    ("include/sl_helpers.h", "48629edbf25dfdb6444c91235fc278f0f9a5878243fae83204c5906ce9eacd8a"),
    ("include/sl_helpers_vk.h", "82604567d239d00ac0ac2270cec744f6f11266e1d9d5187a95d4954f76d8c41c"),
    ("include/sl_hooks.h", "cfeff70a52e1cc012cf4c955a15cc9d0b290f7e084dcb3c0168a9af06ff8f122"),
    ("include/sl_matrix_helpers.h", "a65758d85abba1e12845d266d7d5e36aaefa952383cdf093f3143cd6d8653341"),
    ("include/sl_nis.h", "f45be3cf149200e1eae081cd144c95726a7a290a89822a5d52b9f538e52484aa"),
    ("include/sl_nvperf.h", "025f9f1c8482e75a90f8b8efccdc9cfc53a9fc8b05ba3acc6c3783708b66bd69"),
    ("include/sl_pcl.h", "f43c5135fc8d5349ccd345e424f8cf1f61953a9f17e9897204b0741a3ab7b0fb"),
    ("include/sl_reflex.h", "3b623a1189e04a686384d224a58c4ad9974c4e6e3204077676f6ec529475164c"),
    ("include/sl_result.h", "2a0f6c12863bdc00b38910a5ec85d1f083c5671ee817a920afe626ac2a9100f7"),
    ("include/sl_security.h", "a36b1c20394402bee7ce738089a5a1305ff6e9dd9dd517566154ce834f7c32b4"),
    ("include/sl_struct.h", "28deda67ea1a74371dd4f33ff4842b7db03b2db007001da6db06f6982297928d"),
    ("include/sl_template.h", "af2d4a61361f2603872dcb6edc86cc9e18912762a5c03e6a782ff39a8c3b871b"),
    ("include/sl_version.h", "2a43f565b45a07d4a4259dc61038ec93a7fb8ce7cb3509ea1fbeeb217f9a1ce8"),
]
# What the game loads at run time: without all of it the game leaves DLSS frame generation off (vk_dlssg.cpp early)
RUNTIME = [
    ("bin/x64/sl.interposer.dll", "b1d7395aebd7b9722841d64dcf7433680020b6a8e421f851c16d5ba0c2182edf"),
    ("bin/x64/sl.common.dll", "38eea3b4209afc455636ea372b5933d65fcc13becec6c84d101dcfc4d8648f75"),
    ("bin/x64/sl.dlss_g.dll", "3ed9538df724993bea8a050195a6a78b9c8c8a566d5b05ade477aab3c390e90f"),
    ("bin/x64/sl.pcl.dll", "5698c38e722318681f215c9fe68e45b9bedd51378acc699d8eee26e961e57770"),
    ("bin/x64/sl.reflex.dll", "443577e869352436ff7ec8f4d9c90418d3e95368436ce219261e0f4f07a38122"),
    ("bin/x64/nvngx_dlssg.dll", "8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7"),
]
LICENSES = [
    ("license.txt", "7b6f23e7d6f3ad6292f9308d2b42cdc3d82ae4e9b2abb55f230279d83bedd43d"),
    ("bin/x64/nvngx_dlss.license.txt", "3027f23ca5a46dd9cb8183fbd522983a86f64d7daac5982912bf9f214671f294"),
]
# The license texts as they are named beside the DLLs in a game folder
GAME_LICENSE_NAMES = {"license.txt": "streamline-LICENSE.txt", "bin/x64/nvngx_dlss.license.txt": "nvidia-rtx-sdks-LICENSE.txt"}
GAME_DIR = "dlssg"

NOTICE = ("NVIDIA DLSS frame generation uses NVIDIA's Streamline (its sl.*.dll files, under Streamline's MIT license) and "
          "nvngx_dlssg.dll (under the NVIDIA RTX SDKs License, for systems with NVIDIA GPUs). They are NVIDIA's, not part of "
          "Triforce: " + LICENSE_URL)


def files():
    return HEADERS + RUNTIME + LICENSES


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def missing(sdk):
    """The pinned files that are not in sdk, or are other files than the pinned ones"""
    return [name for name, digest in files()
            if not os.path.isfile(os.path.join(sdk, *name.split("/"))) or sha256_file(os.path.join(sdk, *name.split("/"))) != digest]


def complete(sdk):
    """sdk holds every pinned file: what nx2/build.py and the game need"""
    return bool(sdk) and os.path.isdir(sdk) and not missing(sdk)


class RangeReader(io.RawIOBase):
    """The archive at url as a seekable file that downloads only what is read (HTTP range requests)"""

    def __init__(self, url, size, opener=None):
        self.url, self.size, self.pos = url, size, 0
        self.opener = opener or urllib.request.urlopen
        self.fetched = 0

    def readable(self):
        return True

    def seekable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, offset, whence=io.SEEK_SET):
        self.pos = offset if whence == io.SEEK_SET else self.pos + offset if whence == io.SEEK_CUR else self.size + offset
        return self.pos

    def readinto(self, buffer):
        want = min(len(buffer), self.size - self.pos)
        if want <= 0:
            return 0
        request = urllib.request.Request(self.url, headers={"Range": "bytes=%d-%d" % (self.pos, self.pos + want - 1),
                                                            "User-Agent": "streamline-sdk-fetch"})
        with self.opener(request, timeout=120) as r:
            if getattr(r, "status", 206) != 206:   # (the whole archive would come: the caller reads it once instead)
                raise ValueError("the server does not serve parts of the archive")
            data = r.read()
        if len(data) != want:
            raise ValueError("a part of the archive came short (%d of %d bytes)" % (len(data), want))
        buffer[:want] = data
        self.pos += want
        self.fetched += want
        return want


def open_archive(url=URL, size=SIZE, opener=None, say=print):
    """(zip file, how it is read): by ranges, or downloaded whole into memory when the server serves none"""
    reader = RangeReader(url, size, opener)
    try:
        return zipfile.ZipFile(io.BufferedReader(reader, buffer_size=1 << 20)), reader
    except (ValueError, zipfile.BadZipFile, OSError) as e:
        say("NVIDIA's archive is read whole (%s): %d MB" % (e, size >> 20))
        request = urllib.request.Request(url, headers={"User-Agent": "streamline-sdk-fetch"})
        with (opener or urllib.request.urlopen)(request, timeout=600) as r:
            data = r.read()
        if len(data) != size:
            raise ValueError("NVIDIA's archive is %d bytes, not the %d of Streamline %s" % (len(data), size, VERSION))
        return zipfile.ZipFile(io.BytesIO(data)), None


def fetch(sdk, say=print, archive=None):
    """Every pinned file into sdk that is not there yet. archive: an open zipfile to take them from (tests); else NVIDIA's
    release. Raises OSError or ValueError; a file that is not the pinned one is never left in sdk"""
    need = missing(sdk) if os.path.isdir(sdk) else [name for name, _ in files()]
    if not need:
        say("NVIDIA's Streamline %s is in %s already" % (VERSION, sdk))
        return
    reader = None
    if archive is None:
        say("NVIDIA's Streamline %s: %d files from %s" % (VERSION, len(need), URL))
        archive, reader = open_archive(say=say)
    digests = dict(files())
    names = {info.filename.replace("\\", "/"): info for info in archive.infolist()}
    for name in need:
        info = names.get(name) or next((i for n, i in names.items() if n.endswith("/" + name)), None)
        if info is None:
            raise ValueError("NVIDIA's archive has no %s" % name)
        data = archive.read(info)
        if hashlib.sha256(data).hexdigest() != digests[name]:
            raise ValueError("%s is not the file of Streamline %s (another SHA-256)" % (name, VERSION))
        dst = os.path.join(sdk, *name.split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        tmp = dst + ".part"
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, dst)
    if reader is not None:
        say("NVIDIA's Streamline %s: %d files, %.1f MB downloaded of the archive's %d MB" %
            (VERSION, len(need), reader.fetched / 1048576.0, SIZE >> 20))


def install_runtime(sdk, game):
    """The DLLs and the license texts from sdk into <game>/dlssg. Returns the folder"""
    dst = os.path.join(game, GAME_DIR)
    os.makedirs(dst, exist_ok=True)
    for name, _ in RUNTIME:
        shutil.copyfile(os.path.join(sdk, *name.split("/")), os.path.join(dst, os.path.basename(name)))
    for name, _ in LICENSES:
        shutil.copyfile(os.path.join(sdk, *name.split("/")), os.path.join(dst, GAME_LICENSE_NAMES[name]))
    return dst


def remove_runtime(game):
    """A game folder built without frame generation after one built with it loses NVIDIA's files"""
    dst = os.path.join(game, GAME_DIR)
    if not os.path.isdir(dst):
        return False
    known = {os.path.basename(name) for name, _ in RUNTIME} | set(GAME_LICENSE_NAMES.values())
    for name in os.listdir(dst):
        if name in known:
            os.remove(os.path.join(dst, name))
    try:
        os.rmdir(dst)
    except OSError:
        pass
    return True


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("sdk", nargs="?", default=os.path.join(ROOT, "work", "streamline-sdk"))
    p.add_argument("--game")
    p.add_argument("--accept-nvidia-license", action="store_true")
    a = p.parse_args()
    print(NOTICE)
    if not a.accept_nvidia_license:
        if input("Accept NVIDIA's licenses and download the files? [y/N] ").strip().lower() not in ("y", "yes"):
            print("Nothing was downloaded.")
            return 1
    try:
        fetch(a.sdk)
    except (OSError, ValueError) as e:
        print("NVIDIA's Streamline could not be downloaded: %s" % e)
        return 1
    print("Build with HOOKSHOT_STREAMLINE_SDK=%s" % a.sdk)
    if a.game:
        print("NVIDIA's files for the game: %s" % install_runtime(a.sdk, a.game))
    return 0


if __name__ == "__main__":
    sys.exit(main())
