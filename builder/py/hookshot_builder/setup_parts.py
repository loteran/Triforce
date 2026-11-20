"""The setup's work (builder/setup/setup.c, the same rules): a release's hookshot-setup.txt names its tag and its parts (one zip each,
with its SHA-256 and size); each part is downloaded (or moved over from the installed version when its checksum did not change),
checked and unpacked into versions\\<tag>.part, which becomes versions\\<tag> once complete. The version installed stays, the others
and the downloads go.

hookshot-setup.txt, one item a line:
    tag <tag>
    part <sha256> <bytes> <folder under the install> <top folder in the zip to rename, or -> <new name, or -> <url>
    run <the WPF builder, relative to the install> (not needed here: the tools are what this app uses)"""
import hashlib
import json
import os
import re
import shutil
import time
import urllib.error
import urllib.request
import zipfile

from . import REPO, paths

UA = "HookshotBuilder-Setup/2.0"
WAITS = (3, 6, 12, 20, 30, 45, 60)   # (8 tries over ~3 minutes: GitHub's release downloads answered 504 for hours on 2026-11-08)
OWN = ("python", "emsdk", "cpm", "third_party", "wgpu-native")
MARGIN = 32 << 20   # ((Triforce) what a disk keeps free beyond what is about to be written to it: setup/Guard.cs Space.Margin)
REUSED = "reused.txt"   # ((Triforce) in versions\<tag>.part: the folders moved over from the installed version, reuse_part)
DEVICES = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"}
LOCK_WAIT = 20   # ((Triforce) seconds an install waits for another one into the same folder before it says so: setup.c SETUP_LOCK_WAIT_MS)


class SetupError(Exception):
    pass


def unsafe_member(name):
    """(Triforce) Why a zip's member must not be unpacked under its name, None when it may (setup/Mods.cs Unsafe, the same
    rules): a path from the disk's root, a colon (a drive's letter, a hidden stream of another file), a path through "..", a
    name Windows keeps for a device (CON, NUL, COM1, lpt2.txt: with or without an extension), a name that ends in a dot or a
    space (Windows drops them, so the file would land under another name), a character Windows does not allow. zipfile's
    extractall drops some of these quietly and writes the file under another name; a device's name it writes to the device"""
    n = name.replace("\\", "/")
    if not n.strip():
        return "a member without a name"
    if n.startswith("/"):
        return "a path that starts at the disk's root"
    if ":" in n:
        return "a name with a colon or a drive's letter"
    for p in n.split("/"):
        if p in ("", "."):
            continue
        if p == "..":
            return "a path that leaves its own folder (..)"
        if p[-1] in ". ":
            return "a name that ends in a dot or a space, which Windows drops"
        stem = p.split(".", 1)[0].rstrip(" ").upper()
        # (COM1 to COM9 and LPT1 to LPT9; Windows takes the raised digits 1, 2 and 3 for digits there too)
        if stem in DEVICES or (len(stem) == 4 and stem[:3] in ("COM", "LPT") and stem[3] in "123456789\xb9\xb2\xb3"):
            return "a name Windows keeps for a device (%s)" % p
        if any(ord(c) < 32 or c in '<>"|?*' for c in p):
            return "a name with a character Windows does not allow"
    return None


def check_zip(z, dest, what):
    """(Triforce) Every member's name checked (unsafe_member) and the room for all of it asked before the first file is
    written: such a zip is refused whole"""
    need = 0
    for info in z.infolist():
        why = unsafe_member(info.filename)
        if why:
            raise SetupError("%s holds %s (\"%s\"): it was not unpacked." % (what, why, info.filename[:120]))
        need += info.file_size + 4096   # (a file takes whole clusters)
    room(dest, need, what)


def free_bytes(folder):
    """The free bytes of the disk that holds folder (asked of the nearest folder above it that is there); None when unknown"""
    d = os.path.abspath(folder)
    while not os.path.isdir(d):
        up = os.path.dirname(d)
        if up == d:
            return None
        d = up
    try:
        return shutil.disk_usage(d).free
    except OSError:
        return None


def room(folder, need, what):
    """(Triforce) SetupError before anything is written when the disk of folder has no room for need bytes and the margin"""
    free = free_bytes(folder)
    if free is not None and free < need + MARGIN:
        raise SetupError("There is no room for %s on %s: it takes %d MB there, and %d MB are free. Make room and try again."
                         % (what, os.path.splitdrive(os.path.abspath(folder))[0] or folder, (need + MARGIN + (1 << 20) - 1) >> 20,
                            free >> 20))


class Part:
    def __init__(self, sha, size, dest, frm, to, url):
        self.sha, self.size, self.dest, self.frm, self.to, self.url = sha, int(size), dest, frm, to, url


def retryable(status):
    """an answer worth trying again: none, a server error, a download cut off half way (200), a timeout or the API's rate limit"""
    return not status or status >= 500 or status in (200, 408, 429, 403)


class Http:
    def __init__(self):
        self.status = 0

    def get(self, url, sink=None, expect=0, accept=None, progress=None, cancel=None):
        """GET following redirects (never to http); the bytes to sink, or returned. Sets self.status; raises on failure"""
        if not url.startswith("https://"):
            raise SetupError("Only https downloads are allowed: " + url)
        req = urllib.request.Request(url, headers={"User-Agent": UA, **({"Accept": accept} if accept else {})})
        self.status = 0
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                if not r.geturl().startswith("https://"):
                    raise SetupError("A download was redirected away from https")
                self.status = r.status
                got, out = 0, []
                while True:
                    if cancel and cancel():
                        raise SetupError("cancelled")
                    b = r.read(1 << 16)
                    if not b:
                        break
                    got += len(b)
                    if sink:
                        sink(b)
                    else:
                        out.append(b)
                    if progress and expect:
                        progress(min(1.0, got / expect))
                if expect and got != expect:
                    self.status = 200   # (cut off half way: worth another try)
                    raise SetupError("The download ended early (%d of %d MB)" % (got >> 20, expect >> 20))
                return None if sink else b"".join(out)
        except urllib.error.HTTPError as e:
            self.status = e.code
            raise SetupError("GitHub returned HTTP %d" % e.code) from None
        except (urllib.error.URLError, OSError, TimeoutError) as e:
            if not isinstance(e, SetupError):
                self.status = self.status if self.status and self.status != 200 else 0
                raise SetupError("Could not reach GitHub (%s)" % getattr(e, "reason", e)) from None
            raise


def api_link_for(http, url):
    """https://github.com/<owner>/<repo>/releases/download/<tag>/<name> (or .../latest/download/<name>) -> the API's own link to
    that file, which redirects to a fresh download (GitHub's cached release links answered 504 for hours while its API answered)"""
    m = re.match(r"https://github\.com/([^/]+/[^/]+)/releases/(?:download/([^/]+)|latest/download)/([^/?]+)$", url)
    if not m:
        return None
    repo, tag, name = m.groups()
    api = "https://api.github.com/repos/%s/releases/%s" % (repo, ("tags/" + tag) if tag else "latest")
    try:
        doc = json.loads(http.get(api, accept="application/vnd.github+json"))
        for a in doc.get("assets", []):
            if a.get("name") == name and str(a.get("url", "")).startswith("https://api.github.com/"):
                return a["url"]
    except (SetupError, ValueError):
        pass
    return None


def manifest_url(tag=None):
    """(an update to a named release reads THAT release's list: the latest/download link can answer with the release before it for
    a while after a publish. HOOKSHOT_SETUP_LIST=<url>: another list, for testing a release before it is the latest)"""
    e = os.environ.get("HOOKSHOT_SETUP_LIST")
    if e:
        return e
    if tag:
        return "https://github.com/%s/releases/download/%s/hookshot-setup.txt" % (REPO, tag)
    return "https://github.com/%s/releases/latest/download/hookshot-setup.txt" % REPO


def read_manifest(status, tag=None, cancel=None):
    """(tag, parts) of the release's list, tried again on a server error or no answer"""
    http, url = Http(), manifest_url(tag)
    text, last = None, ""
    for attempt in range(len(WAITS) + 1):
        if attempt:
            status("GitHub didn't answer (%s). Trying again in %d s..." % (http.status or "no answer", WAITS[attempt - 1]))
            time.sleep(WAITS[attempt - 1])
            status("Looking for the latest Hookshot Builder...")
        try:
            text = http.get(url, cancel=cancel).decode("utf-8", "replace")
            break
        except SetupError as e:
            last = str(e)
            first = http.status
            if not first or first >= 500:   # (the API's link instead)
                api = api_link_for(http, url)
                if api:
                    try:
                        text = http.get(api, accept="application/octet-stream", cancel=cancel).decode("utf-8", "replace")
                        break
                    except SetupError:
                        http.status = first
            if not retryable(http.status):
                break
    if text is None:
        if http.status == 404:
            raise SetupError("The latest Hookshot release has no setup files yet. Download it from the releases page for now: "
                             "https://github.com/%s/releases/latest" % REPO)
        if http.status:
            raise SetupError("GitHub's download servers are failing right now (an error 500 or above, tried 8 times over 3 minutes). "
                             "This is on GitHub's side: try again in a few minutes, or later today.")
        raise SetupError("Could not reach GitHub to get the Hookshot Builder (%s). Check your internet connection and try again." % last)
    got_tag, parts = "", []
    for line in text.splitlines():
        p = line.split()
        if len(p) >= 2 and p[0] == "tag":
            got_tag = p[1]
        elif len(p) == 7 and p[0] == "part":
            parts.append(Part(*p[1:]))
    if not got_tag or not parts:
        raise SetupError("The release's setup list could not be read. Try again later.")
    if tag and tag != got_tag:
        raise SetupError("GitHub answered with the setup list of %s, not %s (its download links can lag right after a release). "
                         "Try the update again in a few minutes." % (got_tag, tag))
    return got_tag, parts


def part_folder(p):
    """The folder a part unpacks into, relative to a version's folder (None: the part has none of its own)"""
    if p.to != "-" and p.dest != ".":
        return os.path.join(p.dest, p.to).replace("/", os.sep)
    b = p.url.rsplit("/", 1)[-1]
    for own in OWN:
        if b.startswith("HookshotBuilder-%s-" % own):
            return os.path.join("HookshotBuilder", "tools", own)
    return None


def reuse_part(tag, p, install):
    """A part whose checksum the installed version lists in its parts.txt (and whose folder it has) is moved over from it instead of
    downloaded: that version is deleted after the install anyway"""
    rel = part_folder(p)
    was = paths.read_small(os.path.join(paths.versions(), "current.txt"))
    if not rel or not was or was == tag:
        return False
    if p.sha not in paths.read_small(os.path.join(paths.versions(), was, "parts.txt")):
        return False
    src, dst = os.path.join(paths.versions(), was, rel), os.path.join(install, rel)
    if not os.path.exists(src):
        return False
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    # ((Triforce) written down before it moves: the installed version is the rollback point, and a cancelled or failed install
    # moves it back (undo_reuse). Before, a cancel left the installed version without the folders it lent and nothing ran)
    try:
        journal = os.path.join(install, REUSED)
        new = not os.path.isfile(journal)
        with open(journal, "a", encoding="utf-8") as f:
            f.write(("%s\n" % was if new else "") + rel + "\n")
            f.flush()
            os.fsync(f.fileno())
    except OSError:
        return False
    try:
        os.replace(src, dst)
        return True
    except OSError:
        return False


def undo_reuse(work):
    """(Triforce) The folders reuse_part moved from the installed version into the unfinished one (work), moved back: what a
    cancelled, failed or cut-off install leaves is the version that was installed, whole. Each goes back only where the
    installed version has none; the list is removed once all went back"""
    journal = os.path.join(work, REUSED)
    try:
        with open(journal, encoding="utf-8") as f:
            lines = [l.rstrip("\n") for l in f if l.strip()]
    except OSError:
        return True
    if not lines:
        return True
    was, ok = lines[0], True
    for rel in lines[1:]:
        if os.path.isabs(rel) or ".." in rel.replace("\\", "/").split("/"):
            continue
        src, back = os.path.join(work, rel), os.path.join(paths.versions(), was, rel)
        if not os.path.exists(src) or os.path.exists(back):
            continue
        try:
            os.makedirs(os.path.dirname(back), exist_ok=True)
            os.replace(src, back)
        except OSError:
            ok = False
    if ok:
        try:
            os.remove(journal)
        except OSError:
            pass
    return ok


def undo_every_reuse():
    """(Triforce) undo_reuse for every unfinished version (versions\\*.part) that holds a reused.txt. Before, only the <tag>.part
    of the release being installed now was undone: an install of another release cut off before left the installed version's
    folders in its own <tag>.part, and the installed version ran without them. Returns the first such folder whose folders
    could not all go back (each is tried), None when nothing is left to move back"""
    v, left = paths.versions(), None
    try:
        names = sorted(os.listdir(v))
    except OSError:
        return None
    for name in names:
        work = os.path.join(v, name)
        if name.lower().endswith(".part") and os.path.isfile(os.path.join(work, REUSED)) and not undo_reuse(work):
            left = left or work
    return left


def lent(folder):
    """(Triforce) True when folder is an unfinished version (a versions\\*.part) that still holds folders of another version
    (its reused.txt): only undo_reuse may let it go, never a delete (core.freeable)"""
    return os.path.basename(folder.rstrip("\\/")).lower().endswith(".part") and os.path.isfile(os.path.join(folder, REUSED))


def lock_name(base=None):
    """(Triforce) The name of the lock every install into base holds (setup.c setup_lock_name and Reuse.cs LockName, the same):
    Local\\HookshotBuilder-Setup-<a 64-bit FNV-1a of the folder's full path with '\\' between its parts, none at its end, a to z
    in upper case, taken a UTF-16 unit at a time>. Local\\: this user's session"""
    full = os.path.abspath(base or paths.base()).replace("/", "\\").rstrip("\\")
    data, h = full.encode("utf-16-le", "surrogatepass"), 14695981039346656037   # (a name, not a secret)
    for i in range(0, len(data), 2):
        c = data[i] | data[i + 1] << 8
        h = ((h ^ (c - 32 if 97 <= c <= 122 else c)) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return "Local\\HookshotBuilder-Setup-%016x" % h


class Lock:
    """(Triforce) The lock of the setup's folder, held (release() lets go of it; one that holds nothing where no lock could be
    made at all). Windows' named mutex belongs to the thread that took it: released on that thread"""

    def __init__(self, handle=None, file=None):
        self.handle, self.file = handle, file

    def release(self):
        if self.handle:
            k = ctypes_kernel32()
            k.ReleaseMutex(self.handle)
            k.CloseHandle(self.handle)
            self.handle = None
        if self.file:
            self.file.close()   # (the flock goes with the file)
            self.file = None


def ctypes_kernel32():
    import ctypes
    from ctypes import wintypes
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.CreateMutexW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR]
    k.CreateMutexW.restype = wintypes.HANDLE
    k.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    k.WaitForSingleObject.restype = wintypes.DWORD
    k.ReleaseMutex.argtypes = [wintypes.HANDLE]
    k.CloseHandle.argtypes = [wintypes.HANDLE]
    return k


def take_lock(wait=LOCK_WAIT, base=None):
    """(Triforce) The setup's folder for this install alone: Lock, or None when another install (this app's, or setup.c's)
    held it for wait seconds. Before, two installs into one folder ran at once, and one's sweep (undo_every_reuse) gave back
    the folders the other had just moved into its <tag>.part: that one then finished without its Python. Windows lets go of
    the mutex when its holder ends, however it ends; elsewhere a lock file (setup.lock in the folder) is flock'ed. Where no
    lock can be made at all nobody is refused"""
    base = base or paths.base()
    if os.name == "nt":
        k = ctypes_kernel32()
        h = k.CreateMutexW(None, False, lock_name(base))
        if not h:   # (5: it is there, and belongs to a setup run as administrator)
            import ctypes
            return None if ctypes.get_last_error() == 5 else Lock()
        if k.WaitForSingleObject(h, int(max(0, wait) * 1000)) in (0, 0x80):   # (0x80: its holder ended without letting go: ours)
            return Lock(handle=h)
        k.CloseHandle(h)
        return None
    try:
        import fcntl
        os.makedirs(base, exist_ok=True)
        f = open(os.path.join(base, "setup.lock"), "a")
    except (ImportError, OSError):
        return Lock()
    end = time.monotonic() + max(0, wait)
    while True:
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return Lock(file=f)
        except OSError:
            if time.monotonic() >= end:
                f.close()
                return None
            time.sleep(0.2)


def rmtree(p):
    shutil.rmtree(p, ignore_errors=True)


def installed(tag):
    d = os.path.join(paths.versions(), tag)
    return d if os.path.isfile(os.path.join(d, "installed.txt")) and os.path.isfile(
        os.path.join(d, "HookshotBuilder", "tools", "nx2", "make_game.py")) else None


def current():
    """The version installed last, its folder (None: none yet)"""
    tag = paths.read_small(os.path.join(paths.versions(), "current.txt"))
    return installed(tag) if tag else None


def install(status, progress=lambda f: None, tag=None, repair=False, cancel=None):
    """Installs the release (the latest, or tag) unless it is there already; returns (tag, its folder)"""
    # ((Triforce) one install at a time into the setup's folder, its sweep included (take_lock): another one waits for it, a while)
    lock = take_lock(0)
    if lock is None:
        status("Another Hookshot Builder setup is installing. Waiting for it to finish...")
        lock = take_lock(LOCK_WAIT)
    if lock is None:
        raise SetupError("Another Hookshot Builder setup is installing into %s right now. Wait for it to finish (or close it), "
                         "then try again." % paths.base())
    try:
        return install_locked(status, progress, tag, repair, cancel)
    finally:
        lock.release()


def install_locked(status, progress, tag, repair, cancel):
    """install's work, with the setup's folder held"""
    # ((Triforce) an install cut off before (the app closed), of this release or another: what it moved over from the installed
    # version goes back first, before the installed version is looked at, and a part folder whose folders could not all go back
    # is kept, not deleted with them)
    left = undo_every_reuse()
    if left:
        raise SetupError("The last install could not be undone: %s still holds folders of the installed version. Close every "
                         "Hookshot window and try again." % left)
    tag, parts = read_manifest(status, tag, cancel)
    versions, downloads = paths.versions(), os.path.join(paths.base(), "downloads")
    final = os.path.join(versions, tag)
    if repair or not installed(tag):
        work = final + ".part"
        rmtree(work)
        rmtree(final)
        os.makedirs(work, exist_ok=True)
        os.makedirs(downloads, exist_ok=True)
        try:
            unpack_parts(status, progress, tag, parts, work, downloads, cancel)
        except BaseException:
            # ((Triforce) a cancel or a failure: the installed version gets back what was moved from it, and the partial goes)
            if undo_reuse(work):
                rmtree(work)
            raise
        try:
            os.replace(work, final)
        except OSError:
            if undo_reuse(work):
                rmtree(work)
            raise SetupError("Could not finish installing into %s." % final) from None
        try:
            os.remove(os.path.join(final, REUSED))
        except OSError:
            pass
        paths.write_small(os.path.join(final, "installed.txt"), tag)
        write_parts(final, parts)
    elif not os.path.isfile(os.path.join(final, "parts.txt")):
        write_parts(final, parts)
    prune(tag, parts)
    rmtree(downloads)   # (installed: its zips are not kept; an update moves unchanged folders over instead)
    return tag, final


def unpack_parts(status, progress, tag, parts, work, downloads, cancel):
    """Each part into work: moved over from the installed version, or downloaded (when not there already), checked and unpacked"""
    n = len(parts)
    for i, p in enumerate(parts, 1):
        if cancel and cancel():
            raise SetupError("cancelled")
        if reuse_part(tag, p, work):
            status("Part %d of %d unchanged: kept from the installed version" % (i, n))
            continue
        zpath = os.path.join(downloads, p.sha + ".zip")
        if not (os.path.isfile(zpath) and sha256_file(zpath) == p.sha):
            download_part(status, progress, p, i, n, zpath, cancel)
        status("Unpacking part %d of %d..." % (i, n))
        progress(-1)
        dest = work if p.dest == "." else os.path.join(work, p.dest.replace("/", os.sep))
        os.makedirs(dest, exist_ok=True)
        try:
            with zipfile.ZipFile(zpath) as z:
                check_zip(z, dest, "Part %d of %d" % (i, n))   # ((Triforce) every name, and the room, before the first file)
                z.extractall(dest)
        except (OSError, zipfile.BadZipFile) as e:
            raise SetupError("Unpacking part %d of %d failed: %s" % (i, n, e)) from None
        if p.frm != "-" and p.to != "-":   # (llvm-mingw-<version>-ucrt-x86_64 -> llvm-mingw)
            src, dst = os.path.join(dest, p.frm), os.path.join(dest, p.to)
            if not os.path.exists(dst):
                try:
                    os.replace(src, dst)
                except OSError:
                    raise SetupError("Could not set up part %d of %d (renaming %s)." % (i, n, p.frm)) from None
    if not os.path.isfile(os.path.join(work, "HookshotBuilder", "tools", "nx2", "make_game.py")):
        raise SetupError("The build tools are missing after unpacking. Your antivirus may have removed them: allow the folder %s "
                         "and try again." % paths.base())


def download_part(status, progress, p, i, n, zpath, cancel):
    http, part = Http(), zpath + ".part"
    # ((Triforce) room for the size the list names, asked before the first byte: a full disk stopped the download half way with
    # Windows' own words for it. The part is written from its first byte at each try, so no address or mark is kept beside it)
    room(os.path.dirname(zpath), p.size, "part %d of %d" % (i, n))
    for attempt in range(len(WAITS) + 1):
        if attempt:
            status("Part %d of %d: GitHub didn't answer (%s). Trying again in %d s..." % (i, n, http.status or "no answer",
                                                                                          WAITS[attempt - 1]))
            time.sleep(WAITS[attempt - 1])
        status("Downloading part %d of %d (%d MB)..." % (i, n, p.size >> 20))
        h = hashlib.sha256()
        try:
            with open(part, "wb") as f:
                def sink(b):
                    f.write(b)
                    h.update(b)
                url = api_link_for(http, p.url) if attempt % 2 == 1 else None   # (the API's link, every other try)
                if url:
                    http.get(url, sink, p.size, "application/octet-stream", progress, cancel)
                else:
                    http.get(p.url, sink, p.size, None, progress, cancel)
        except SetupError as e:
            if str(e) == "cancelled":
                raise
            if not retryable(http.status):
                break
            continue
        except OSError:
            raise SetupError("Could not write to %s." % os.path.dirname(zpath)) from None
        progress(-1)
        if h.hexdigest() != p.sha:
            os.remove(part)
            raise SetupError("Part %d of %d did not download correctly (its checksum differs). Try again." % (i, n))
        os.replace(part, zpath)
        return
    try:
        os.remove(part)
    except OSError:
        pass
    raise SetupError(("Downloading part %d of %d failed: GitHub's download servers are failing right now (tried 8 times over 3 "
                      "minutes). This is on GitHub's side. The parts already downloaded are kept: try again in a few minutes."
                      if retryable(http.status) else
                      "Downloading part %d of %d failed. Check your connection and free space, then try again.") % (i, n))


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def write_parts(install_dir, parts):
    """The checksums of the parts this version holds in folders of their own (reuse_part reads them at the next update)"""
    paths.write_small(os.path.join(install_dir, "parts.txt"), "".join(p.sha + "\n" for p in parts if part_folder(p)))


def prune(tag, parts):
    """Only the version installed stays (a folder still in use cannot be renamed: it goes at a later run), and downloads the list
    no longer names go"""
    versions = paths.versions()
    was = paths.read_small(os.path.join(versions, "current.txt"))
    if was != tag:
        if was:
            paths.write_small(os.path.join(versions, "previous.txt"), was)
        paths.write_small(os.path.join(versions, "current.txt"), tag)
    for name in os.listdir(versions):
        p = os.path.join(versions, name)
        if not os.path.isdir(p) or name == tag:
            continue
        if name.endswith(".gone"):
            rmtree(p)
            continue
        try:
            os.replace(p, p + ".gone")
            rmtree(p + ".gone")
        except OSError:
            pass
