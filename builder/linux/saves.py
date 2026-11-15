"""Triforce's saves on Linux: find this game's saves in an emulator's NAND or an NX2navi game folder and copy them into
Triforce's own save folder (setup/Saves.cs does the same on Windows). Pure logic, no Tk.

The files are the same everywhere (account.sav, option.sav, slot_00..05/{caption,progress}.sav), so importing is a copy. What gets
replaced is copied to <hookshot>/save-backup/<YYYYmmdd-HHMMSS> first, and the new set is swapped in from a sibling folder.
"""
import datetime
import os
import re
import shutil
import time

TITLE_ID = "0400486024DEC000"
TRIFORCE_UID = "5a3c9e1f0b7d24680000000000000001"   # (host/nn_applet.c user_uid)
# (nand-style emulators: data folder name, shown name, flatpak ids)
NAND_EMULATORS = (
    ("eden", "Eden", ("dev.eden_emu.eden",)),
    ("yuzu", "yuzu", ("org.yuzu_emu.yuzu",)),
    ("suyu", "suyu", ("dev.suyu_emu.suyu",)),
    ("sudachi", "Sudachi", ()),
    ("citron", "Citron", ("org.citron_emu.citron",)),
    ("torzu", "torzu", ()),
    ("EvilOOT", "EvilOOT", ()),
)
RYUJINX_FLATPAK = "org.ryujinx.Ryujinx"
# (programs that keep the saves open and write them back when they close)
RUNNING_NAMES = ("hookshot", "triforce", "evilot", "eden", "yuzu", "suyu", "sudachi", "citron", "torzu", "ryujinx", "ocarina")


class SaveSet:
    def __init__(self, name, folder, slots=0, last=0.0):
        self.name, self.folder, self.slots, self.last = name, folder, slots, last

    def label(self):
        return "%s — %s — %d slot%s — %s" % (self.name, self.folder, self.slots, "" if self.slots == 1 else "s", when(self.last))


def when(t):
    return time.strftime("%d %b %Y %H:%M", time.localtime(t)) if t else "never"


def home():
    return os.path.expanduser("~")


def data_home():
    xdg = os.environ.get("XDG_DATA_HOME")
    return xdg if xdg and os.path.isabs(xdg) else os.path.join(home(), ".local", "share")


def config_home():
    xdg = os.environ.get("XDG_CONFIG_HOME")
    return xdg if xdg and os.path.isabs(xdg) else os.path.join(home(), ".config")


def hookshot_dir():
    return os.path.join(data_home(), "hookshot")


def triforce_folder(game=None):
    """Where the game keeps its saves (host/hos_main.c saves_dir): ~/.local/share/hookshot/save/<uid>. A user/ folder left
    beside the executable (it could not be moved there) still counts when the data folder has no saves yet."""
    main = os.path.join(hookshot_dir(), "save", TRIFORCE_UID)
    if game:
        legacy = os.path.join(game, "user", "save", TRIFORCE_UID)
        if os.path.isdir(legacy) and os.listdir(legacy) and not os.path.isdir(main):
            return legacy
    return main


def backup_root(dest):
    """save-backup beside the save folder (<hookshot>/save-backup, or <game>/user/save-backup for a legacy place)."""
    return os.path.join(os.path.dirname(os.path.dirname(dest)), "save-backup")


def is_save_set(d):
    return os.path.isdir(d) and (os.path.isfile(os.path.join(d, "option.sav")) or os.path.isfile(os.path.join(d, "account.sav"))
                                 or os.path.isfile(os.path.join(d, "slot_00", "progress.sav")))


def save_files(d):
    """the .sav files of a set, relative: top level and one slot folder down."""
    out = []
    try:
        for name in sorted(os.listdir(d)):
            p = os.path.join(d, name)
            if os.path.isfile(p) and name.endswith(".sav"):
                out.append(name)
            elif os.path.isdir(p) and name.startswith("slot_"):
                out += [os.path.join(name, n) for n in sorted(os.listdir(p)) if n.endswith(".sav") and os.path.isfile(os.path.join(p, n))]
    except OSError:
        pass
    return out


def describe(name, d):
    files = save_files(d)
    slots = sum(1 for n in (os.listdir(d) if os.path.isdir(d) else []) if n.startswith("slot_") and os.path.isfile(os.path.join(d, n, "progress.sav")))
    last = 0.0
    for f in files:
        try:
            last = max(last, os.path.getmtime(os.path.join(d, f)))
        except OSError:
            pass
    return SaveSet(name, d, slots, last)


def navi_save(d):
    """The save an NX2navi game would load, given its game folder or its profile folder: the highest finished commit in
    profile/save-tx/ready-<n> (newer than the account folder until the game's next start swaps it in), else the newest
    profile/save/<32 hex>. None when there is none."""
    for profile in (os.path.join(d, "profile"), d):
        saves = os.path.join(profile, "save")
        if not os.path.isdir(saves):
            continue
        tx = os.path.join(profile, "save-tx")
        ready = []
        if os.path.isdir(tx):
            for n in os.listdir(tx):
                m = re.fullmatch(r"ready-(\d{1,10})", n)
                if m and is_save_set(os.path.join(tx, n)):
                    ready.append((int(m.group(1)), os.path.join(tx, n)))
        if ready:
            return max(ready)[1]
        accounts = [os.path.join(saves, n) for n in os.listdir(saves) if re.fullmatch(r"[0-9a-fA-F]{32}", n) and is_save_set(os.path.join(saves, n))]
        if accounts:
            return max(accounts, key=lambda a: describe("", a).last)
    return None


def _subdirs(d):
    try:
        return sorted(os.path.join(d, n) for n in os.listdir(d) if os.path.isdir(os.path.join(d, n)))
    except OSError:
        return []


def find_in(d):
    """A chosen folder: the save set it is, an NX2navi game folder's save, or the first one inside it (a few levels down)."""
    if is_save_set(d):
        return d
    navi = navi_save(d)
    if navi:
        return navi
    queue, looked = [(d, 0)], 0
    while queue and looked < 4000:
        cur, depth = queue.pop(0)
        for s in _subdirs(cur):
            looked += 1
            if is_save_set(s):
                return s
            if depth < 6:
                queue.append((s, depth + 1))
    return None


def find_saves(extra_dirs=(), game=None):
    """Every save set of this game on this PC, newest first. extra_dirs: folders that may be NX2navi game folders or hold
    some (the builder's earlier output folders, ~/Games/*, the parent of the output folder). The set Triforce already
    uses is left out."""
    found, seen = [], set()
    mine = os.path.realpath(triforce_folder(game))

    def add(name, d):
        try:
            full = os.path.realpath(d)
            if full != mine and full not in seen and is_save_set(full):
                seen.add(full)
                found.append(describe(name, full))
        except OSError:
            pass

    def nand(name, root):   # (root/nand/user/save/0000000000000000/<user>/<title>)
        users = os.path.join(root, "nand", "user", "save", "0000000000000000")
        for u in _subdirs(users):
            add(name, os.path.join(u, TITLE_ID))

    def ryujinx(root):      # (root/bis/user/save/<index>/0: the index is Ryujinx's own, so the files tell which one is this game's)
        for s in _subdirs(os.path.join(root, "bis", "user", "save")):
            add("Ryujinx", os.path.join(s, "0"))

    data, conf = data_home(), config_home()
    for folder, name, flatpaks in NAND_EMULATORS:
        nand(name, os.path.join(data, folder))
        for app in flatpaks:
            nand(name + " (flatpak)", os.path.join(home(), ".var", "app", app, "data", folder))
    ryujinx(os.path.join(conf, "Ryujinx"))
    ryujinx(os.path.join(home(), ".var", "app", RYUJINX_FLATPAK, "config", "Ryujinx"))
    ryujinx(os.path.join(data, "Ryujinx"))

    candidates = []
    for d in [os.path.join(home(), "Games")] + list(extra_dirs):
        if not d or not os.path.isdir(d):
            continue
        candidates.append(d)
        candidates += _subdirs(d)
    for d in candidates:
        try:
            navi = navi_save(d)
        except OSError:
            navi = None
        if navi:
            add("NX2navi", navi)
    return sorted(found, key=lambda s: -s.last)


def running_game(game=None):
    """The name of a program that has these saves open (it would write over the copy when it closes), or None. Triforce has
    no lock file: the processes are looked at (/proc), by program name or by running from the game folder."""
    root = os.path.realpath(game) + os.sep if game else None
    me = os.getpid()
    try:
        pids = [p for p in os.listdir("/proc") if p.isdigit() and int(p) != me]
    except OSError:
        return None
    procs = []
    for pid in pids:
        try:
            exe = os.path.realpath(os.readlink("/proc/%s/exe" % pid))
            with open("/proc/%s/comm" % pid) as f:
                procs.append((exe, f.read().strip().lower()))
        except OSError:
            continue
    for exe, _ in procs:   # (what runs from the game folder first)
        if root and exe.startswith(root):
            return os.path.basename(exe)
    for exe, comm in procs:
        base = os.path.basename(exe).lower()
        for n in RUNNING_NAMES:
            if base == n or base == n + ".appimage" or comm == n:
                return "NX2navi" if n == "ocarina" else n
    return None


def import_saves(src, dest, backup_base=None, game=None, running=running_game):
    """Replace dest with a copy of src's .sav files. The old dest is copied to <backup_base>/<YYYYmmdd-HHMMSS> first
    (default: save-backup beside the save folder). Returns the backup folder, or None when there was nothing to back up.
    Raises RuntimeError when a game is running or src is dest, OSError when the copy fails (dest is then untouched)."""
    if (busy := running(game)):
        raise RuntimeError("Close %s first: it would write its own saves back over the copy when it closes." % busy)
    src, dest = os.path.realpath(src), os.path.realpath(dest) if os.path.exists(dest) else os.path.abspath(dest)
    if src == dest:
        raise RuntimeError("That is the save folder Triforce already uses.")
    files = save_files(src)
    if not files:
        raise RuntimeError("There are no saves in %s." % src)
    parent = os.path.dirname(dest)
    os.makedirs(parent, exist_ok=True)
    tmp, old = dest + ".new", dest + ".old"
    for d in (tmp, old):
        shutil.rmtree(d, ignore_errors=True)
    try:
        for rel in files:
            to = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(to), exist_ok=True)
            shutil.copy2(os.path.join(src, rel), to)
        backup = None
        if os.path.isdir(dest) and os.listdir(dest):
            base = backup_base or backup_root(dest)
            backup = os.path.join(base, datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
            n = 1
            while os.path.exists(backup):   # (two imports within one second)
                n += 1
                backup = os.path.join(base, datetime.datetime.now().strftime("%Y%m%d-%H%M%S") + "-%d" % n)
            shutil.copytree(dest, backup)
            os.rename(dest, old)
        os.rename(tmp, dest)
    except BaseException:
        shutil.rmtree(tmp, ignore_errors=True)
        if os.path.isdir(old) and not os.path.isdir(dest):
            os.rename(old, dest)
        raise
    shutil.rmtree(old, ignore_errors=True)
    return backup
