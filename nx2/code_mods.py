"""(Triforce) Code mods: the mods' patches of the game's code, applied before the game is translated.

The game's code is compiled ahead of time, so a patch of it cannot be applied while the game runs (host/mods.c loads the
RomFS mods then). A build applies them instead: nx2/make_game.py calls build_step() on its own copy of the game's code
(<work>/exefs, which the lift reads), never on the player's files and never on the copy the game folder keeps for updates
(.triforce_exefs). The patched bytes are in the modules the lift key hashes (make_game.py lift_key), so a patch added,
taken away, switched off or edited makes the next build translate the game again, and nothing else does.

Where the patches are, as emulators keep them and as host/mods.c finds its mods:

    <game folder>/mods/<name>/exefs/<build id>.ips       IPS ("PATCH" ... "EOF") or IPS32 ("IPS32" ... "EEOF")
    <game folder>/mods/<name>/exefs/<any name>.pchtxt    IPSwitch text, its build id in the @nsobid- line

A mod may sit deeper (mods/contents/<title id>/<name>/exefs): any folder up to four levels down that holds a romfs, exefs
or cheats folder is a mod, and nothing below it is looked at. A folder whose name starts with '-' or '_' is off, one that
starts with '.' is skipped. Mods are taken in the order of their names (ASCII letters' case ignored), a mod's patch files
in the order of theirs, and where two write the same byte the later one wins; the build says so.

A patch names the build id of the module it is for (main, a subsdk, sdk or rtld): the .ips file's name, the .pchtxt
file's @nsobid- line, hex, trailing zeros left out or not. One for another build is refused with a line that says so.
Offsets are as emulators and Atmosphere read them: they count the NSO's 0x100-byte header before the module's image (a
.pchtxt offset after its "@flag offset_shift" is added). A file is applied whole or not at all: one that is cut short,
that cannot be read, or that writes outside the module's three segments (.text, .rodata, .data) is refused. That
leaves out code caves, patches that put new code into the empty space after .text: the translator has not been tried on
them.

What a build did is kept in the game folder, triforce_code_mods.txt (RECORD, write_record): the next build, an update
(--update) among them, reads the mods folder again, builds the same patches in again and says which of the last build's
are gone or changed; the game compares the file with the mods folder at its start (host/mods.c, host/mod_code.h) and
says in its log which patch files wait for a build.

Standard library only (lz4 too for a module that is LZ4-compressed, as the translator's own loader needs it). Not
imported by the lift: this file is not part of the lift key (make_game.py LIFT_KEY_NOT), what it does to the code is.
"""
import hashlib
import os
import re
import struct

NSO_HEADER = 0x100
SEGMENTS = (".text", ".rodata", ".data")
PATCH_SUFFIXES = (".ips", ".pchtxt")
PARTS = ("romfs", "exefs", "cheats")   # (a folder holding one of these is a mod: host/mods.c find_mods)
MOD_DEPTH = 4
RECORD = "triforce_code_mods.txt"
RECORD_HEAD = ("# The code patches (.ips, .pchtxt) this game was built with: nx2/code_mods.py writes it at every build, and\n"
               "# the game reads it at its start. Tab separated: applied or refused, the file's FNV-1a 64 and size, its\n"
               "# path under the mods folder, then the module and the bytes changed, or why it was refused.\n")


class Refused(ValueError):
    """A patch file that is not applied; the text says why"""


# ---- the formats: [(offset in the module's image, bytes)] in the file's order

def parse_ips(data):
    """The writes of an IPS or IPS32 file. Refused when it is neither, or cut short"""
    if data[:5] == b"PATCH":
        width, end = 3, b"EOF"
    elif data[:5] == b"IPS32":
        width, end = 4, b"EEOF"
    else:
        raise Refused("it is not an IPS file (no PATCH or IPS32 at its start)")
    writes, at = [], 5
    while True:
        # (the end's letters could be a record's offset too: they are the end only where the file ends, a truncation
        # length of up to three bytes after them)
        if data[at:at + len(end)] == end and len(data) - at - len(end) <= 3:
            return writes
        if at + width + 2 > len(data):
            raise Refused("it is cut short (no %s at its end)" % end.decode())
        offset = int.from_bytes(data[at:at + width], "big")
        size = int.from_bytes(data[at + width:at + width + 2], "big")
        at += width + 2
        if size:
            body = data[at:at + size]
            at += size
            if len(body) != size:
                raise Refused("it is cut short (a record of %d bytes at file offset %#x has %d)" % (size, at - size, len(body)))
        else:   # (a run: its length, then the one byte it repeats)
            if at + 3 > len(data):
                raise Refused("it is cut short (in a run at file offset %#x)" % at)
            body = data[at + 2:at + 3] * int.from_bytes(data[at:at + 2], "big")
            at += 3
        if offset < NSO_HEADER:
            raise Refused("it writes the module's header (offset %#x: an offset counts the 0x100-byte NSO header)" % offset)
        if body:
            writes.append((offset - NSO_HEADER, bytes(body)))


ESCAPES = {"a": 7, "b": 8, "f": 12, "n": 10, "r": 13, "t": 9, "v": 11, "\\": 92, "'": 39, '"': 34, "?": 63, "0": 0}
PATCH_LINE = re.compile(r"([0-9A-Fa-f]{8})[ \t]+(\S.*)$")


def pchtxt_value(text, little_endian, where):
    """The bytes of a patch line's value: hex digits in byte order (reversed after @little-endian), or a string in double
    quotes with C's one-letter escapes, as UTF-8 and without a zero after it"""
    if text.startswith('"'):
        out, i = bytearray(), 1
        while i < len(text) and text[i] != '"':
            ch = text[i]
            if ch == "\\":
                i += 1
                if i >= len(text) or text[i] not in ESCAPES:
                    raise Refused("%s: an escape it does not know in a string" % where)
                out.append(ESCAPES[text[i]])
            else:
                out += ch.encode("utf-8")
            i += 1
        if i >= len(text):
            raise Refused("%s: a string without its closing quote" % where)
        return bytes(out)
    digits = re.split(r"[ \t/]", text, maxsplit=1)[0]
    if len(digits) % 2 or not re.fullmatch(r"[0-9A-Fa-f]+", digits):
        raise Refused("%s: %r is not a value (hex digits, two a byte)" % (where, digits[:40]))
    value = bytes.fromhex(digits)
    return value[::-1] if little_endian else value


def parse_pchtxt(text):
    """(build id or None, writes) of an IPSwitch file. Patch lines count between @enabled and @disabled, @stop ends the
    file; a line that is neither a directive nor "<8 hex digits> <value>" is a title or a note and is passed over. A
    patch line whose value cannot be read refuses the file"""
    build, shift, enabled, little, writes = None, 0, False, False, []
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("//") or line.startswith("#"):
            continue
        if line.startswith("@"):
            word = line.split()[0].lower()
            if word == "@stop":
                break
            if word.startswith("@nsobid-"):
                build = line.split()[0][len("@nsobid-"):]
            elif word == "@enabled":
                enabled = True
            elif word == "@disabled":
                enabled = False
            elif word == "@little-endian":
                little = True
            elif word == "@big-endian":
                little = False
            elif word == "@flag":
                f = line.split()
                if len(f) >= 3 and f[1] == "offset_shift":
                    try:
                        shift = int(f[2], 0)
                    except ValueError:
                        raise Refused("line %d: %r is not a number for offset_shift" % (number, f[2])) from None
            continue
        m = PATCH_LINE.match(line)
        if not m or not enabled:
            continue
        value = pchtxt_value(m.group(2), little, "line %d" % number)
        offset = int(m.group(1), 16) + shift
        if offset < NSO_HEADER:
            raise Refused("line %d: it writes the module's header (offset %#x after the shift: an offset counts the "
                          "0x100-byte NSO header)" % (number, offset))
        if value:
            writes.append((offset - NSO_HEADER, value))
    return build, writes


def same_build(named, build_id):
    """Whether a patch's build id (hex, any case, trailing zeros left out or not) is a module's (hex of its 32 bytes)"""
    named = named.lower()
    return bool(re.fullmatch(r"[0-9a-f]{1,64}", named)) and named.ljust(64, "0") == build_id.lower().ljust(64, "0")


# ---- the mods folder, as host/mods.c walks it

def name_key(name):
    """A name as host/mods.c orders it (_stricmp: bytes, ASCII letters' case ignored)"""
    return bytes(b + 32 if 65 <= b <= 90 else b for b in name.encode("utf-8", "surrogateescape"))


def find_mods(root):
    """[(name, folder)] of the mods under root, by name: a mod's name is its path under root, with /"""
    out = []

    def walk(folder, name, depth):
        if depth > 0 and any(os.path.exists(os.path.join(folder, part)) for part in PARTS):
            out.append((name, folder))
            return
        if depth >= MOD_DEPTH:
            return
        try:
            entries = sorted(os.listdir(folder))
        except OSError:
            return
        for entry in entries:
            sub = os.path.join(folder, entry)
            if entry[:1] in (".", "-", "_") or not os.path.isdir(sub):
                continue
            walk(sub, name + "/" + entry if name else entry, depth + 1)

    if os.path.isdir(root):
        walk(root, "", 0)
    return sorted(out, key=lambda mod: name_key(mod[0]))


def patch_files(root):
    """[(path under the mods folder, file)] of the mods' code patches, in the order they are applied"""
    out = []
    for name, folder in find_mods(root):
        exefs = os.path.join(folder, "exefs")
        try:
            entries = sorted(os.listdir(exefs), key=name_key) if os.path.isdir(exefs) else []
        except OSError:
            entries = []
        for entry in entries:
            path = os.path.join(exefs, entry)
            if entry.lower().endswith(PATCH_SUFFIXES) and os.path.isfile(path):
                out.append(("%s/exefs/%s" % (name, entry), path))
    return out


def fnv64(data):
    """FNV-1a 64 of a file's bytes, 16 hex digits: what the record names a patch file's contents by (host/mod_code.h
    makes the same number)"""
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % h


# ---- the modules of the copy that is translated

class Module:
    """One NSO of the exefs: its build id and its three segments; the image only once a patch is for it"""

    def __init__(self, path):
        self.path, self.name = path, os.path.basename(path)
        with open(path, "rb") as f:
            self.head = f.read(NSO_HEADER)
        if len(self.head) < NSO_HEADER or self.head[:4] != b"NSO0":
            raise ValueError("%s is not a game module" % path)
        self.build_id = self.head[0x40:0x60].hex()
        self.flags = struct.unpack_from("<I", self.head, 0xC)[0]
        # (each: file offset, memory offset, size)
        self.segments = [struct.unpack_from("<3I", self.head, 0x10 + 0x10 * i) for i in range(3)]
        self.file_sizes = struct.unpack_from("<3I", self.head, 0x60)
        self.raw = self.image = self.original = None

    def load(self):
        if self.image is not None:
            return
        if self.flags & 0x80:
            raise ValueError("%s is still ZBIC-compressed (nx2/unpack.py makes it plain first)" % self.path)
        with open(self.path, "rb") as f:
            self.raw = f.read()
        self.image = bytearray(max(memory + size for _, memory, size in self.segments))
        for i, ((offset, memory, size), stored) in enumerate(zip(self.segments, self.file_sizes)):
            data = self.raw[offset:offset + stored]
            if self.flags & (1 << i):
                import lz4.block   # (as third_party/a64-kit/tools/nso.py reads it)
                data = lz4.block.decompress(data, uncompressed_size=size)
            if len(data) != size:
                raise ValueError("%s: its %s has %d bytes, %d expected" % (self.path, SEGMENTS[i], len(data), size))
            self.image[memory:memory + size] = data
        self.original = bytes(self.image)

    def segment_of(self, offset, size):
        """The segment that holds all of [offset, offset + size), or None"""
        for i, (_, memory, length) in enumerate(self.segments):
            if memory <= offset and offset + size <= memory + length:
                return i
        return None

    def write(self):
        """The module written again as a plain NSO with the image as it is now: the header, what lies between it and
        the first segment (the module's name), the three segments one after the other, their hashes made again"""
        head = bytearray(self.head)
        body = bytearray(self.raw[NSO_HEADER:max(NSO_HEADER, min(offset for offset, _, _ in self.segments))])
        for i, (_, memory, size) in enumerate(self.segments):
            data = bytes(self.image[memory:memory + size])
            struct.pack_into("<I", head, 0x10 + 0x10 * i, NSO_HEADER + len(body))
            struct.pack_into("<I", head, 0x60 + 4 * i, size)
            head[0xA0 + 0x20 * i:0xC0 + 0x20 * i] = hashlib.sha256(data).digest()
            body += data
        struct.pack_into("<I", head, 0xC, self.flags & ~7)   # (no segment compressed; the hash bits stay)
        tmp = self.path + ".part"   # (make_game.py check_modules passes over a .part)
        with open(tmp, "wb") as f:
            f.write(head)
            f.write(body)
        os.replace(tmp, self.path)


def load_modules(exefs):
    """The exefs's modules by name: every file of it that is an NSO (main.npdm is none)"""
    out = {}
    for name in sorted(os.listdir(exefs)):
        path = os.path.join(exefs, name)
        if os.path.isfile(path) and not name.endswith(".part"):
            with open(path, "rb") as f:
                magic = f.read(4)
            if magic == b"NSO0":
                out[name] = Module(path)
    return out


# ---- applying

def read_patch(rel, data):
    """(the build id the file names, its writes). Refused when the file cannot be read as what its name says it is"""
    if rel.lower().endswith(".ips"):
        named = os.path.splitext(os.path.basename(rel))[0]
        if not re.fullmatch(r"[0-9A-Fa-f]{1,64}", named):
            raise Refused("its name is not a build id (an .ips patch is named after the build id of the code it is for)")
        return named, parse_ips(data)
    build, writes = parse_pchtxt(data.decode("utf-8-sig", errors="replace"))
    if build is None:
        raise Refused("it has no @nsobid- line (the build id of the code it is for)")
    if not re.fullmatch(r"[0-9A-Fa-f]{1,64}", build):
        raise Refused("its @nsobid- line is not a build id: %r" % build[:70])
    return build, writes


def check_writes(module, writes):
    """Refused when a write is not whole inside one of the module's segments"""
    for offset, data in writes:
        if module.segment_of(offset, len(data)) is None:
            end = max(memory + size for _, memory, size in module.segments)
            where = "past the end of its image (%#x)" % end if offset + len(data) > end else \
                "outside its .text, .rodata and .data (the empty space between them: a code cave is not supported)"
            raise Refused("it writes %d bytes at %s+%#x, %s" % (len(data), module.name, offset, where))


def apply(exefs, mods_root, say=print):
    """The mods' code patches applied to the modules in exefs, in place. Returns the record's entries: a dict per patch
    file, in the order they were taken: state ("applied" or "refused"), path (under the mods folder), fnv, size, and
    module and changed (the bytes it changed that no later patch wrote over) or why. Says a line per file"""
    files = patch_files(mods_root)
    if not files:
        return []
    modules = load_modules(exefs)
    game = ", ".join("%s %s" % (m.name, m.build_id.rstrip("0")[:40] or "0") for m in modules.values())
    entries, owner = [], {}   # (owner: {(module, offset): the entry whose byte is there})
    for rel, path in files:
        entry = {"state": "refused", "path": rel, "fnv": "0" * 16, "size": 0, "module": "", "changed": 0, "why": ""}
        entries.append(entry)
        try:
            try:
                with open(path, "rb") as f:
                    data = f.read()
            except OSError as e:
                raise Refused("it cannot be read (%s)" % (e.strerror or e)) from None
            entry["fnv"], entry["size"] = fnv64(data), len(data)
            named, writes = read_patch(rel, data)
            module = next((m for m in modules.values() if same_build(named, m.build_id)), None)
            if module is None:
                raise Refused("it is for build %s, which is not this game's code (%s)" % (named.lower()[:40], game))
            try:
                module.load()
            except (OSError, ValueError) as e:
                raise Refused("the module it is for cannot be read (%s)" % e) from None
            check_writes(module, writes)
        except Refused as e:
            entry["why"] = str(e)
            say("code mods: %s is not applied: %s" % (rel, e))
            continue
        entry["state"], entry["module"] = "applied", module.name
        over = {}
        for offset, data in writes:
            for i, b in enumerate(data):
                at = (module.name, offset + i)
                before = owner.get(at)
                if before is not None and before is not entry and module.image[offset + i] != b:
                    over[before["path"]] = over.get(before["path"], 0) + 1
                module.image[offset + i] = b
                owner[at] = entry
        for earlier, count in over.items():
            say("code mods: %s writes over %d bytes of %s (it comes later by name: its bytes are the ones built in)"
                % (rel, count, earlier))
    for (name, offset), entry in owner.items():   # (what each changed in the end: a byte is the last patch's that wrote it)
        if modules[name].image[offset] != modules[name].original[offset]:
            entry["changed"] += 1
    for entry in entries:
        if entry["state"] == "applied":
            say("code mods: %s applied to %s: %d bytes changed%s" % (
                entry["path"], entry["module"], entry["changed"],
                "" if entry["changed"] else " (it writes what the game has there already, or a later patch wrote over it)"))
    written = []
    for module in modules.values():
        if module.image is not None and bytes(module.image) != module.original:
            module.write()
            written.append(module.name)
    if written:
        say("code mods: the game is translated with %s patched. If the translation stops or the game misbehaves, switch the "
            "mod off (a - before its folder's name) and build again." % ", ".join(written))
    return entries


# ---- what the game folder remembers

def record_text(entries):
    lines = []
    for e in entries:
        tail = (e["module"], str(e["changed"])) if e["state"] == "applied" else (e["why"].replace("\t", " ").replace("\n", " "),)
        path = e["path"].replace("\t", "?").replace("\n", "?").replace("\r", "?")   # (a line a path, whatever the name)
        lines.append("\t".join((e["state"], e["fnv"], str(e["size"]), path) + tail))
    return RECORD_HEAD + "".join(line + "\n" for line in lines)


def read_record(out):
    """The entries of the game folder's record, [] when it has none (a folder built before code mods, or with none)"""
    entries = []
    try:
        with open(os.path.join(out, RECORD), encoding="utf-8", errors="replace") as f:
            for line in f:
                f_ = line.rstrip("\r\n").split("\t")
                if line.startswith("#") or len(f_) < 5 or f_[0] not in ("applied", "refused"):
                    continue
                entry = {"state": f_[0], "fnv": f_[1], "path": f_[3], "module": "", "changed": 0, "why": ""}
                try:
                    entry["size"] = int(f_[2])
                    if f_[0] == "applied":
                        entry["module"], entry["changed"] = f_[4], int(f_[5]) if len(f_) > 5 else 0
                    else:
                        entry["why"] = f_[4]
                except ValueError:
                    continue
                entries.append(entry)
    except OSError:
        pass
    return entries


def write_record(out, entries):
    """The record of a finished build, written whole beside it and put in its place; with no patch files there is no
    record (a game folder without code mods is as it was before them)"""
    path = os.path.join(out, RECORD)
    if not entries:
        try:
            os.remove(path)
        except OSError:
            pass
        return
    with open(path + ".new", "w", encoding="utf-8", newline="\n") as f:
        f.write(record_text(entries))
    os.replace(path + ".new", path)


def since_last(before, now):
    """Lines about what differs from the build before, for a build that takes the folder's patches again (an update):
    the patches of that build that are gone, changed or no longer applied (a new one has its own line already)"""
    lines = []
    was = {e["path"]: e for e in before if e["state"] == "applied"}
    have = {e["path"]: e for e in now}
    for path, old in was.items():
        new = have.get(path)
        if new is None:
            lines.append("code mods: %s was built into this game folder before and is gone from its mods folder (or "
                         "switched off): this build is made without it" % path)
        elif new["state"] != "applied":
            lines.append("code mods: %s was built into this game folder before and is refused now: this build is made "
                         "without it" % path)
        elif (new["fnv"], new["size"]) != (old["fnv"], old["size"]):
            lines.append("code mods: %s changed since the build before: this build has it as it is now" % path)
    return lines


def mods_folder(out):
    """The mods folder a build reads: the game folder's, or HOS_MODS=<folder> as the game itself takes it"""
    return os.environ.get("HOS_MODS") or os.path.join(out, "mods")


def build_step(exefs, out, say=print):
    """nx2/make_game.py, once its copy of the game's code is there and is the supported build: the patches of the game
    folder's mods applied to that copy, with a line for each and for what changed since the folder's last build. Returns
    the entries for write_record, which make_game.py calls when the build is done"""
    before = read_record(out)
    entries = apply(exefs, mods_folder(out), say)
    for line in since_last(before, entries):
        say(line)
    applied = [e for e in entries if e["state"] == "applied"]
    if entries:
        say("code mods: %d of %d patch files applied (%s)" % (len(applied), len(entries), mods_folder(out)))
    return entries
