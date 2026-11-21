"""The title screen's Options entry: "Options / ZELDA NOTES Setting" reads "Options / Triforce" (nx2/prompt_assets.py
title_messages, host/prompts.c title_messages).

The text is label 0002 of LayoutMsg/Title_00.msbt in each language's messages (Mals/<language>.Product.110.sarc.zs). Setup's
Update rewrites that one text in the player's own files, the host serves the language the game shows. Checked here, on
archives and messages made up by the test (no game file is read):

- The MSBT: label 0002 is the game's word for Options, its size tag as it was, the game's separator (" / " for a rest in
  brackets) and TITLE_TEXT, then its end; the other texts, the labels and every other section byte for byte as they were;
  the TXT2 offsets, each section's size and padding to 16, and the file's size agree. In UTF-8 and UTF-16, a tag after the
  rest (the Japanese text's ruby) dropped with it, and a longer TITLE_TEXT too.
- The SARC: every other file byte for byte the same, the files before it at their places, those after it at places of
  the same alignment (the bytes between them kept), the nodes and the archive's size agreeing, the names' table and the
  data's offset as they were. A text with no size tag, a file that is no MSBT, an archive without the title's file: None.
- The size table's entries: a hashed one by the path's CRC-32, a named one over it.
- title_messages end to end on a romfs the test makes (the dictionaries' pack, a size table, two languages' messages):
  the file is written in lower case under mals/, as a zstd frame without the dictionary, and is the rewritten archive; a
  language whose archive is not the known one, would not fit its size table entry, or would be larger than the game's
  file (a longer TITLE_TEXT, the archive grown into the entry's allowance), is not written.
- The host: its table of languages is host/nn_applet.c's, in its order, each on its own Mals file; title_messages cut out
  and run serves the file of the language the game shows, not another language's, and nothing without the file or without
  the prompts folder; hos_prompts_romfs asks it for the Mals keys.

    python -I -B tests/test_title_label.py [--cc gcc]
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]
RUN_ALL_MODULES = ["zstandard"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import prompt_assets as P  # noqa: E402

SIZE_TAG = b"\x0e" + struct.pack("<HHHH", 0, 2, 2, 0x50)   # (group 0 type 2, one u16: the rest drawn at 80 %)
# the Mals files' language names, as the game's romfs has them
MALS_NAMES = {"usen", "euen", "eufr", "usfr", "eude", "euit", "eues", "uses", "uspt", "eunl", "eupl", "euru", "jpja", "krko",
              "cnzh", "twzh"}


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def tag16(group, kind, params):
    return b"\x0e\x00" + struct.pack("<HHH", group, kind, len(params)) + params


def section(magic, body, pad=b"\xab"):
    s = magic + struct.pack("<I", len(body)) + bytes(8) + body
    return s + pad * (-len(s) % 16)


def msbt(texts, encoding=0, atr_after=False):
    """an MSBT of the texts (bytes, each with its end), labelled 0000, 0001, ... in LBL1's hash slots, with an ATR1 before
    TXT2 (or after it)"""
    slots = 7
    by_slot = [[] for _ in range(slots)]
    for i in range(len(texts)):
        name = "%04d" % i
        h = 0
        for c in name.encode():
            h = (h * 0x492 + c) & 0xFFFFFFFF
        by_slot[h % slots].append((name, i))
    head, labels = bytearray(struct.pack("<I", slots)), bytearray()
    first = 4 + 8 * slots
    for entries in by_slot:
        head += struct.pack("<II", len(entries), first + len(labels))
        for name, i in entries:
            labels += bytes([len(name)]) + name.encode() + struct.pack("<I", i)
    lbl1 = section(b"LBL1", bytes(head + labels))
    offs, at = [], 4 + 4 * len(texts)
    for t in texts:
        offs.append(at)
        at += len(t)
    txt2 = section(b"TXT2", struct.pack(f"<I{len(texts)}I", len(texts), *offs) + b"".join(texts))
    atr1 = section(b"ATR1", struct.pack("<II", len(texts), 0))
    sections = lbl1 + (txt2 + atr1 if atr_after else atr1 + txt2)
    size = 0x20 + len(sections)
    head = b"MsgStdBn\xff\xfe\x00\x00" + bytes([encoding, 3]) + struct.pack("<H", 3) + b"\x00\x00" + struct.pack("<I", size)
    return head + bytes(0x20 - len(head)) + sections


def sarc_hash(name):
    h = 0
    for c in name.encode():
        h = (h * 0x65 + (c if c < 128 else c - 256)) & 0xFFFFFFFF
    return h


def sarc(files):
    """a SARC of files [(name, data, alignment)]: its nodes by hash, the data in the nodes' order, each file at a place of its
    alignment"""
    files = sorted(files, key=lambda f: sarc_hash(f[0]))
    names, offs = b"", []
    for name, _, _ in files:
        offs.append(len(names) // 4)
        names += name.encode() + b"\0"
        names += bytes(-len(names) % 4)
    hl = 0x14
    data_at = hl + 12 + 16 * len(files) + 8 + len(names)
    data_at += -data_at % 0x80
    data, nodes = bytearray(), b""
    for (name, d, align), off in zip(files, offs):
        data += bytes(-(data_at + len(data)) % align)
        nodes += struct.pack("<IIII", sarc_hash(name), 0x01000000 | off, len(data), len(data) + len(d))
        data += d
    sfat = b"SFAT" + struct.pack("<HHI", 12, len(files), 0x65) + nodes
    sfnt = b"SFNT" + struct.pack("<HH", 8, 0) + names
    head = b"SARC" + struct.pack("<HHIIHH", hl, 0xfeff, data_at + len(data), data_at, 0x100, 0)
    return (head + sfat + sfnt).ljust(data_at, b"\0") + bytes(data)


def members(b):
    """{name: (start, end)} of a SARC, and its nodes' table and names' table as bytes"""
    hl, = struct.unpack_from("<H", b, 4)
    data_at, = struct.unpack_from("<I", b, 12)
    n, = struct.unpack_from("<H", b, hl + 6)
    names = hl + 12 + 16 * n + 8
    out = {}
    for i in range(n):
        _, attr, s, e = struct.unpack_from("<IIII", b, hl + 12 + 16 * i)
        o = names + (attr & 0xFFFFFF) * 4
        out[b[o:b.index(b"\0", o)].decode()] = (data_at + s, data_at + e)
    return out


def texts_of(m):
    """{label: its text} of an MSBT, checking its sizes on the way"""
    check(struct.unpack_from("<I", m, 0x12)[0] == len(m), "the MSBT's size is not its length")
    sections = P.msbt_sections(m)
    check(sections is not None and len(sections) == struct.unpack_from("<H", m, 0xe)[0], "the MSBT's sections do not read")
    by = {s[0]: s for s in sections}
    strings = P.txt2_strings(by[b"TXT2"][2])
    check(strings is not None, "the TXT2 offsets do not read")
    return {k: strings[i] for k, i in P.msbt_labels(by[b"LBL1"][2]).items()}, sections


OTHERS = [("EventFlowMsg/Dm_A.msbt", os.urandom(608), 8), ("LocationMsg/Location.msbt", os.urandom(1584), 8),
          ("LayoutMsg/Logo_00.msbt", os.urandom(200), 16), ("Font/Glyphs.bfttf", os.urandom(3000), 0x80),
          ("LayoutMsg/Title_00x.msbt", os.urandom(77), 8)]


def check_msbt():
    eight = b"Options" + SIZE_TAG
    cases = [   # (the rest after the tag, the separator kept)
        (b" / ZELDA NOTES Setting", b" / "), (b" (inkl. ZELDA NOTES-Einstellungen)", b" / "),
        (b"/ZELDA NOTES \xec\x84\xa4\xec\xa0\x95", b"/"), ("／ZELDA NOTES設定".encode(), "／".encode()),
        (" | Configuração do ZELDA NOTES".encode(), b" | "),
        (b" / ZELDA NOTES" + b"\x0e" + struct.pack("<HHH", 0, 0, 16) + struct.pack("<HH", 2, 12) + "せってい".encode()
         + "設定".encode(), b" / "),   # (the Japanese: a ruby tag in the rest, dropped with it)
    ]
    for rest, sep in cases:
        for atr_after in (False, True):
            texts = [b"New Game\0", b"Continue\0", eight + rest + b"\0"]
            m = msbt(texts, atr_after=atr_after)
            new = P.title_msbt(m)
            check(new is not None, "a title MSBT with %r was not rewritten" % rest)
            got, sections = texts_of(new)
            check(got["0002"] == eight + sep + P.TITLE_TEXT.encode() + b"\0",
                  "label 0002 reads %r for %r" % (got["0002"], rest))
            check(got["0000"] == texts[0] and got["0001"] == texts[1], "another text changed")
            old_sections = P.msbt_sections(m)
            for (magic, r, body, pad), (magic0, r0, body0, pad0) in zip(sections, old_sections):
                check(magic == magic0 and r == r0, "a section's head changed")
                if magic != b"TXT2":
                    check(body == body0 and pad == pad0, "%s changed" % magic.decode())
                check((16 + len(body) + len(pad)) % 16 == 0 and set(pad) <= {0xab}, "%s is not padded to 16" % magic.decode())
            check(new[:0x12] == m[:0x12] and new[0x16:0x20] == m[0x16:0x20], "the MSBT's header changed beyond its size")
            check(P.title_msbt(new) == new, "rewriting the rewritten MSBT changed it")
    # UTF-16: the same
    t16 = "Options".encode("utf-16-le") + tag16(0, 2, struct.pack("<H", 0x50)) + " / ZELDA NOTES Setting\0".encode("utf-16-le")
    m16 = msbt(["New Game\0".encode("utf-16-le"), "Continue\0".encode("utf-16-le"), t16], encoding=1)
    got, _ = texts_of(P.title_msbt(m16))
    check(got["0002"] == "Options".encode("utf-16-le") + tag16(0, 2, struct.pack("<H", 0x50)) +
          (" / " + P.TITLE_TEXT + "\0").encode("utf-16-le"), "the UTF-16 text reads %r" % got["0002"])
    # a longer text: the MSBT grows, still whole
    longer = "Triforce, the port's own settings and a few more words"
    got, _ = texts_of(P.title_msbt(msbt([b"New Game\0", b"Continue\0", eight + b" / ZELDA NOTES Setting\0"]), longer))
    check(got["0002"] == eight + b" / " + longer.encode() + b"\0", "a longer text reads %r" % got["0002"])
    # not the title's: None
    check(P.title_msbt(msbt([b"New Game\0", b"Continue\0", b"Options / ZELDA NOTES Setting\0"])) is None, "a text with no size tag")
    check(P.title_msbt(msbt([b"New Game\0", b"Continue\0"])) is None, "no label 0002")
    check(P.title_msbt(b"MsgStdBn" + bytes(40)) is None and P.title_msbt(os.urandom(64)) is None, "no MSBT")
    bad = bytearray(msbt([b"a\0", b"b\0", eight + b" / x\0"]))
    struct.pack_into("<I", bad, 0x12, len(bad) + 16)
    check(P.title_msbt(bytes(bad)) is None, "an MSBT whose size is not its length")
    return len(cases) * 2 + 2


def check_sarc():
    title = msbt([b"New Game\0", b"Continue\0", b"Options" + SIZE_TAG + b" / ZELDA NOTES Setting\0"])
    n = 0
    for text in (P.TITLE_TEXT, "Triforce" + " and much more" * 9):   # (smaller, then larger than the game's)
        b = sarc(OTHERS + [(P.TITLE_MSBT, title, 8)])
        new = P.title_sarc(b, text)
        check(new is not None, "the archive was not rewritten")
        old, now = members(b), members(new)
        check(list(old) == list(now), "the archive's files changed")
        at = old[P.TITLE_MSBT]
        for name, (s, e) in old.items():
            s2, e2 = now[name]
            if name == P.TITLE_MSBT:
                check(s2 == s, "the title's file moved")
                got, _ = texts_of(new[s2:e2])
                check(got["0002"] == b"Options" + SIZE_TAG + b" / " + text.encode() + b"\0", "the archive's label 0002: %r" % got["0002"])
                continue
            check(new[s2:e2] == b[s:e], "%s changed" % name)
            if s < at[0]:
                check(s2 == s, "%s, before the title's file, moved" % name)
            else:
                align = next(a for f, _, a in OTHERS if f == name)
                check(s2 % align == 0 and s2 >= now[P.TITLE_MSBT][1], "%s lost its alignment %#x or overlaps" % (name, align))
        check(struct.unpack_from("<I", new, 8)[0] == len(new), "the archive's size is not its length")
        data_at, = struct.unpack_from("<I", b, 12)
        hl = 0x14
        nn, = struct.unpack_from("<H", b, hl + 6)
        nodes_end = hl + 12 + 16 * nn
        check(new[12:16] == b[12:16] and new[nodes_end:data_at] == b[nodes_end:data_at], "the names' table or data offset changed")
        for i in range(nn):
            check(new[hl + 12 + 16 * i:hl + 20 + 16 * i] == b[hl + 12 + 16 * i:hl + 20 + 16 * i], "a node's hash or name changed")
        n += 1
    # the title's file last in the data: the archive ends with it
    files = [(P.TITLE_MSBT, title, 8)]
    b = sarc(files)
    new = P.title_sarc(b)
    check(new is not None and members(new)[P.TITLE_MSBT][1] == len(new), "an archive of the title's file alone")
    # not the ones known
    check(P.title_sarc(sarc(OTHERS)) is None, "an archive without the title's file")
    check(P.title_sarc(sarc(OTHERS + [(P.TITLE_MSBT, os.urandom(192), 8)])) is None, "a title file that is no MSBT")
    check(P.title_sarc(b"SARC" + bytes(60)) is None and P.title_sarc(b"not an archive") is None, "no SARC")
    return n


def size_table(hashed, named):
    """a RESTBL: {path: size} hashed by CRC-32, {path: size} by name (160 bytes)"""
    rows = sorted((zlib.crc32(p.encode()), s) for p, s in hashed.items())
    b = b"RESTBL" + struct.pack("<IIII", 1, 160, len(rows), len(named))
    b += b"".join(struct.pack("<II", h, s) for h, s in rows)
    b += b"".join(p.encode().ljust(160, b"\0") + struct.pack("<I", s) for p, s in named.items())
    return b


def check_size_table():
    t = size_table({"Mals/USen.Product.110.sarc": 1000, "Other/A.bin": 5, "Mals/EUde.Product.110.sarc": 77},
                   {"Mals/EUde.Product.110.sarc": 2000})
    check(P.size_entry(t, "Mals/USen.Product.110.sarc") == 1000, "a hashed entry was not found")
    check(P.size_entry(t, "Mals/EUde.Product.110.sarc") == 2000, "a named entry does not win over a hashed one")
    check(P.size_entry(t, "Mals/JPja.Product.110.sarc") is None, "an entry where there is none")


def check_end_to_end():
    import zstandard
    dictionary = b"the test's own dictionary for its made-up romfs: " + bytes(range(256)) * 4
    packer = zstandard.ZstdCompressor(level=3, dict_data=zstandard.ZstdCompressionDict(dictionary))
    title = msbt([b"New Game\0", b"Continue\0", b"Options" + SIZE_TAG + b" / ZELDA NOTES Setting\0"])
    us = sarc(OTHERS + [(P.TITLE_MSBT, title, 8)])
    de = sarc(OTHERS + [(P.TITLE_MSBT, msbt([b"Neues Spiel\0", b"Fortsetzen\0", b"Optionen" + SIZE_TAG +
                                                b" (inkl. ZELDA NOTES-Einstellungen)\0"]), 8)])
    jp = sarc(OTHERS)   # (no title's file: not the one known)
    with tempfile.TemporaryDirectory(prefix="title-label-") as tmp:
        romfs, out = Path(tmp) / "romfs", Path(tmp) / "prompts"
        (romfs / "Pack").mkdir(parents=True)
        (romfs / "Mals").mkdir()
        (romfs / "System" / "Resource").mkdir(parents=True)
        dics = sarc([("zs.zsdic", dictionary, 8), ("pack.zsdic", b"another one", 8)])
        (romfs / "Pack" / "ZsDic.pack.zs").write_bytes(zstandard.ZstdCompressor().compress(dics))
        for name, b in (("USen", us), ("EUde", de), ("JPja", jp), ("KRko", us)):
            (romfs / "Mals" / f"{name}.Product.110.sarc.zs").write_bytes(packer.compress(b))
        table = size_table({"Mals/USen.Product.110.sarc": len(us) + 336, "Mals/EUde.Product.110.sarc": len(de) + 320,
                            "Mals/KRko.Product.110.sarc": len(us) - 64},   # (KRko: too small for the file)
                           {})
        (romfs / "System" / "Resource" / "ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs").write_bytes(packer.compress(table))
        dic = P.zs_dictionary(str(romfs))
        check(dic == dictionary, "the made-up romfs's dictionary does not read")
        done = P.title_messages(str(romfs), dic, str(out))
        check(done == ["EUde", "USen"], "the languages written: %s" % done)
        check(sorted(os.listdir(out / "mals")) == ["eude.product.110.sarc.zs", "usen.product.110.sarc.zs"],
              "the files written: %s" % sorted(os.listdir(out / "mals")))
        for name, b in (("usen", us), ("eude", de)):
            raw = (out / "mals" / f"{name}.product.110.sarc.zs").read_bytes()
            got = zstandard.ZstdDecompressor().decompress(raw)   # (no dictionary: as the game reads the host's frames)
            check(got == P.title_sarc(b), "%s: not the rewritten archive" % name)
            at = members(got)[P.TITLE_MSBT]
            texts, _ = texts_of(got[at[0]:at[1]])
            check(texts["0002"].endswith(b" / " + P.TITLE_TEXT.encode() + b"\0"), "%s: label 0002 reads %r" % (name, texts["0002"]))
        # a longer text: the archives grow past the game's files but stay within their entries (the allowance the game keeps
        # over each file, 336 and 320 bytes here): not written, the entry holds no more than the game's file
        longer = "Triforce PC port settings, graphics and controls"
        for name, b, allow in (("USen", us, 336), ("EUde", de, 320)):
            grown = P.title_sarc(b, longer)
            check(grown is not None and len(b) < len(grown) <= len(b) + allow,
                  "%s: the longer text does not grow the archive within its entry (%d -> %s)"
                  % (name, len(b), grown and len(grown)))
        out2 = Path(tmp) / "prompts-longer"
        done = P.title_messages(str(romfs), dic, str(out2), longer)
        check(done == [], "an archive larger than the game's file was written: %s" % done)
        check(not (out2 / "mals").exists() or not os.listdir(out2 / "mals"), "files written for a longer text")


HARNESS = r'''#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_PATH 260
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFul
typedef long LONG;
static const char *folder, *language;
static int has_prompts;
static unsigned long GetFileAttributesA(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return INVALID_FILE_ATTRIBUTES;
    fclose(f);
    return 0;
}
static LONG InterlockedExchange(volatile LONG *p, LONG v) { const LONG old = *p; *p = v; return old; }
static void hos_log(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
    fputc('\n', stderr);
}
static int setup(char *dir, size_t cap, int *face) {
    *face = 0;
    snprintf(dir, cap, "%s/", folder);
    return has_prompts;
}
const char *hos_desired_language(void) { return language; }
@CUT@
/* folder, language, has prompts, key: "1 <path>" served, "0" not */
int main(int argc, char **argv) {
    if (argc < 5) return 2;
    folder = argv[1], language = argv[2], has_prompts = atoi(argv[3]);
    char out[MAX_PATH] = "";
    const int ok = title_messages(argv[4], out, sizeof out);
    printf("%d %s\n", ok, ok ? out : "");
    return 0;
}
'''


def check_host(cc):
    applet = (ROOT / "host/nn_applet.c").read_text(encoding="utf-8")
    prompts = (ROOT / "host/prompts.c").read_text(encoding="utf-8")
    table = applet[applet.index("static const struct { const char *code, *name; } languages[] = {"):]
    codes = re.findall(r'\{"([^"]+)", "[^"]+"\}', table[:table.index("};")])
    start = prompts.index("static const struct { const char *code, *mals; } title_languages[] = {")
    ours = re.findall(r'\{"([^"]+)", "([^"]+)"\}', prompts[start:prompts.index("};", start)])
    check([c for c, _ in ours] == codes, "host/prompts.c title_languages is not nn_applet.c's languages, in order")
    check({m for _, m in ours} == MALS_NAMES and len(ours) == 16, "the languages' Mals files are not the game's 16")
    check(dict(ours)["en-US"] == "usen" and dict(ours)["ja"] == "jpja" and dict(ours)["fr-CA"] == "usfr"
          and dict(ours)["en-GB"] == "euen", "a language is on another's Mals file")
    check("const char *hos_desired_language(void) { return desired_language(); }" in applet,
          "host/nn_applet.c does not give prompts.c the game's language")
    romfs_fn = prompts[prompts.index("int hos_prompts_romfs(const char *key"):]
    check('if (!strncmp(key, "mals/", 5) && kn > 8 && !strcmp(key + kn - 8, ".sarc.zs")) return title_messages(key, out, cap);'
          in romfs_fn, "hos_prompts_romfs does not ask title_messages for the Mals files")
    fn = prompts.index("static int title_messages(")
    end = prompts.index("\n}\n", fn) + 3
    harness = HARNESS.replace("@CUT@", prompts[start:end])
    with tempfile.TemporaryDirectory(prefix="title-host-") as tmp:
        c, exe = Path(tmp) / "title.c", Path(tmp) / "title"
        c.write_text(harness, encoding="utf-8")
        subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
        game = Path(tmp) / "game"
        (game / "prompts" / "mals").mkdir(parents=True)
        for name in ("usen", "jpja"):
            (game / "prompts" / "mals" / f"{name}.product.110.sarc.zs").write_bytes(b"made by the test")

        def ask(lang, key, has=1):
            got = subprocess.run([str(exe), str(game), lang, str(has), key], check=True, capture_output=True, text=True).stdout.split()
            return got[1] if got[0] == "1" else None

        key = "mals/usen.product.110.sarc.zs"
        check(ask("en-US", key) == "%s/prompts/%s" % (game, key), "en-US: the US English file was not served")
        check(ask("ja", "mals/jpja.product.110.sarc.zs") is not None, "ja: the Japanese file was not served")
        check(ask("ja", key) is None, "ja: the US English file was served")
        check(ask("en-GB", "mals/euen.product.110.sarc.zs") is None, "en-GB: served with no file")
        check(ask("en-US", key, has=0) is None, "served without the prompts folder")
        check(ask("xx", key) is not None, "a language not in the table does not fall back to English (US)'s file")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    n = check_msbt()
    k = check_sarc()
    check_size_table()
    check_end_to_end()
    check_host(a.cc)
    print("ok: %d title texts rewritten, %d archives with every other file kept, the size table, the build and the host" % (n, k))


if __name__ == "__main__":
    main()
