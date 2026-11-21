"""The Insert menu's Game language row: language.txt in the game folder, read as the host reads it and written back so that the
host and Setup's update both take the choice.

gpu/vk/game_language.h reads the file and rewrites it. It is checked here against the two readers there are, on the same
files: host/nn_applet.c's own desired_language, cut out and run (what the game will answer nn::oe::GetDesiredLanguage with),
and nx2/make_game.py's own folder_language (what an update of the folder keeps). And the menu's side, gpu/vk/vk_menu.cpp's
own file functions cut out and run in a folder of their own: the row's entries are the languages the file names, a choice
goes into the file with nothing else in it changed, a language the file does not name is never written, and a write that
cannot be made leaves the row on the file's choice.

    python -I -B tests/test_game_language.py [--cc gcc] [--cxx g++]
"""
import argparse
import ast
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc", "--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def block(text, at):
    """text from at to the end of the first {...} after it"""
    start = text.index("{", at)
    depth = 0
    for i in range(start, len(text)):
        depth += text[i] == "{"
        depth -= text[i] == "}"
        if depth == 0:
            return text[at:i + 1]
    raise SystemExit("no end for the block at %d" % at)


def definition(text, head):
    found = re.search(re.escape(head) + r"\([^;{]*\)\s*\{", text)
    check(found, "no definition of " + head)
    return block(text, found.start())


# ---- the three tables of the game's languages
def tables():
    applet = (ROOT / "host/nn_applet.c").read_text(encoding="utf-8")
    table = applet[applet.index("static const struct { const char *code, *name; } languages[] = {"):]
    table = table[:table.index("};") + 2]
    host = re.findall(r'\{"([^"]+)", "([^"]+)"\}', table)
    game = (ROOT / "nx2/make_game.py").read_text(encoding="utf-8")
    listed = game[game.index("LANGUAGES = ["):]
    setup = [tuple(pair) for pair in ast.literal_eval(listed[len("LANGUAGES = "):listed.index("]\n") + 1])]
    header = (ROOT / "gpu/vk/game_language.h").read_text(encoding="utf-8")
    ours = header[header.index("constexpr Language kLanguages[] = {"):]
    menu = re.findall(r'\{"([^"]+)", "([^"]+)"\}', ours[:ours.index("};")])
    check(host == setup, "host/nn_applet.c's languages and nx2/make_game.py's LANGUAGES differ")
    check(menu == host, "gpu/vk/game_language.h's languages are not host/nn_applet.c's, in its order:\n  %s\n  %s" % (menu, host))
    check(len(host) == 16 and len({c.lower() for c, _ in host}) == 16, "the game has 16 languages, each code once in any case")
    return applet, table, host, game


def template(languages, code):
    """language.txt as nx2/make_game.py writes it at a build (and host/nn_applet.c with en-US, when there is none)"""
    return ("# The game's language. Change the line below to one of the codes listed here, save, and start the game again.\n\n%s\n\n" % code +
            "".join("# %-8s %s\n" % l for l in languages))


HOST = r'''#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned long DWORD;
#ifndef _WIN32
#include <strings.h>
#define _stricmp strcasecmp
#endif
static const char *folder;
static DWORD hos_game_path_a(char *buf, DWORD size) { return (DWORD)snprintf(buf, size, "%s/Triforce.exe", folder); }
static void hos_log(const char *fmt, ...) { (void)fmt; }
@TABLE@
@READER@
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    folder = argv[1];
    puts(desired_language());
    return 0;
}
'''

OURS = r'''#include "@ROOT@/gpu/vk/game_language.h"
#include <cstdio>
#include <string>
namespace gl = mkwvk::game_language;
static std::string slurp(const char* path) {
    std::string s;
    if (FILE* f = fopen(path, "rb")) {
        char b[4096];
        for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
        fclose(f);
    }
    return s;
}
// read <file>: the choice's code (- none), then the codes offered; write <file> <code> <out>: the text with that code chosen
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    const std::string text = slurp(argv[2]);
    if (std::string(argv[1]) == "read") {
        const gl::File f = gl::read(text);
        std::printf("%s", f.chosen >= 0 ? gl::kLanguages[f.chosen].code : "-");
        int entries[gl::kCount];
        const int n = gl::offered(f, entries);
        for (int i = 0; i < n; i++) std::printf(" %s", gl::kLanguages[entries[i]].code);
        std::printf("\n");
        return 0;
    }
    if (argc < 5) return 2;
    int language = -1;
    for (int i = 0; i < gl::kCount; i++)
        if (std::string(gl::kLanguages[i].code) == argv[3]) language = i;
    const std::string out = gl::write(text, language);
    FILE* f = fopen(argv[4], "wb");
    if (!f) return 3;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return 0;
}
'''

MENU = r'''#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif
#include "@ROOT@/gpu/vk/game_language.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
namespace game_language = mkwvk::game_language;
/* (stand-ins: the options' folder, the log, and as much of the menu's rows and options as these functions touch) */
static std::string folder;
std::string ini_path() { return folder + "/triforce_options.ini"; }
static int logged;
void logf(const char* fmt, ...) { (void)fmt; logged++; }
struct { int language = 0, other = 5; } opts;
struct Row { int* value; int hi; };
Row rows[] = {{&opts.other, 9}, {&opts.language, 0}, {nullptr, 0}};
bool is_merged(const int*) { return false; }   /* ((Triforce, options_version 7) no merged row here */
@STATE@
@TEXT@
@HINT@
@INI_ROW@
@PATH@
@READ@
@WRITE@
@LOAD@
@STORE@
static std::string slurp() {
    std::string s;
    lang_read(s);
    return s;
}
static void put(const std::string& text) {
    FILE* f = fopen(lang_path().c_str(), "wb");
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    folder = argv[1];
    const std::string start = slurp();   /* (the test's file: Setup's text with de chosen) */
    CHECK(!start.empty());
    lang_load();
    /* the row: the file's 16 languages in the game's order, on the file's choice; not a line of the options file */
    CHECK(lang_count == 16 && rows[1].hi == 15 && rows[0].hi == 9 && opts.language == 4 && lang_saved == 4);
    CHECK(!std::strcmp(tx_language(4), "German") && !std::strcmp(tx_language(0), "English (US)") && !std::strcmp(tx_language(15), "Chinese (Traditional)"));
    CHECK(!std::strcmp(tx_language(-3), "English (US)") && !std::strcmp(tx_language(99), "Chinese (Traditional)"));
    CHECK(!ini_row(rows[1]) && ini_row(rows[0]) && !ini_row(rows[2]));
    CHECK(!hn_language(0, -1) && hn_language(0, 0) && hn_language(0, 1) && !hn_language(0, 2));
    /* nothing chosen anew: nothing written */
    lang_store();
    CHECK(slurp() == start && logged == 0);
    /* a choice: the file's choice line alone changes, and the row's entry is then the file's */
    opts.language = 12;   /* (ja) */
    lang_store();
    std::string now = slurp(), want = start;
    want.replace(want.find("\nde\n") + 1, 2, "ja");
    CHECK(now == want && lang_saved == 12 && opts.language == 12 && !lang_failed && logged == 1);
    CHECK(!hn_language(0, -1));
    FILE* left = fopen((lang_path() + ".new").c_str(), "rb");   /* (the file written beside it is gone: it took the file's place) */
    CHECK(!left);
    lang_store();   /* (again: nothing more to write) */
    CHECK(slurp() == want && logged == 1);
    /* the file edited by hand since (another choice, a note added): the row's next choice still changes its line alone */
    put("# mine\r\n  FR-ca   \r\n" + start);
    opts.language = 0;
    lang_store();
    CHECK(slurp() == "# mine\r\nen-US\r\n" + start && lang_saved == 0 && !lang_failed);
    /* the file no longer names the language chosen: not written, the row back on what it held, and said so */
    const std::string bare = "# only these now\n\nen-US\n\n# en-US    English (US)\n# fr       French (France)\n";
    put(bare);
    opts.language = 4;   /* (de: the row still lists it from the start) */
    lang_store();
    CHECK(slurp() == bare && opts.language == 0 && lang_saved == 0 && lang_failed);
    CHECK(hn_language(0, -1) == hn_language(0, 1));
    /* ...and one it does name is written, which clears that */
    opts.language = 2;   /* (fr) */
    lang_store();
    CHECK(slurp() == "# only these now\n\nfr\n\n# en-US    English (US)\n# fr       French (France)\n" && lang_saved == 2 && !lang_failed);
    /* read anew, as the next start does: two entries, on fr */
    lang_load();
    CHECK(lang_count == 2 && rows[1].hi == 1 && opts.language == 1 && !std::strcmp(tx_language(1), "French (France)"));
    /* the file gone: the choice cannot be kept, the row stays */
    remove(lang_path().c_str());
    opts.language = 0;
    lang_store();
    CHECK(opts.language == 1 && lang_failed && !fopen(lang_path().c_str(), "rb"));
    /* no file at a start, or one that names nothing: no entries (the row is not shown) */
    lang_load();
    CHECK(lang_count == 0 && rows[1].hi == 0 && opts.language == 0 && !*tx_language(0));
    put("# nothing here\n\nplain text\n");
    lang_load();
    CHECK(lang_count == 0);
    /* a choice with no list: one entry, nothing to choose (not shown either) */
    put("it\n");
    lang_load();
    CHECK(lang_count == 1 && opts.language == 0 && !std::strcmp(tx_language(0), "Italian"));
    /* a file far too large is not language.txt */
    put(std::string(70000, '#') + "\nde\n# de German\n# fr French\n");
    lang_load();
    CHECK(lang_count == 0);
    std::puts("game language, the menu's file: all checks passed");
    return 0;
}
'''


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, check=True, **kw).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    applet, table, languages, game = tables()
    codes = [c for c, _ in languages]
    # (Setup's own reader of a folder's language, as it is: an update keeps the folder's choice with it)
    setup = {"os": os, "LANGUAGES": languages}
    exec(definition_py(game, "def folder_language(out):"), setup)
    folder_language = setup["folder_language"]
    # (the host writes the same file when there is none, in English (US): the text Setup writes)
    check("# The game's language. Change the line below to one of the codes listed here, save, and start the game again.\\n\\nen-US\\n\\n" in applet
          and '"# %-8s %s\\n", languages[i].code, languages[i].name' in applet, "host/nn_applet.c no longer writes language.txt as nx2/make_game.py does")
    check("# The game's language. Change the line below to one of the codes listed here, save, and start the game again.\\n\\n%s\\n\\n" in game
          and '"# %-8s %s\\n" % l for l in LANGUAGES' in game, "nx2/make_game.py no longer writes language.txt as this test's template has it")

    env = {k: v for k, v in os.environ.items() if k != "HOS_LANGUAGE"}
    with tempfile.TemporaryDirectory(prefix="game-language-") as tmp:
        work = Path(tmp)
        host_c, host_exe = work / "host.c", work / "host.exe"
        host_c.write_text(HOST.replace("@TABLE@", table).replace("@READER@", definition(applet, "static const char *desired_language")),
                          encoding="utf-8")
        subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(host_c), "-o", str(host_exe)], check=True)
        ours_cpp, ours_exe = work / "ours.cpp", work / "ours.exe"
        ours_cpp.write_text(OURS.replace("@ROOT@", ROOT.as_posix()), encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ours_cpp), "-o", str(ours_exe)], check=True)

        folder = work / "game"
        folder.mkdir()
        txt = folder / "language.txt"

        def host_says(data):
            txt.write_bytes(data)
            return run([str(host_exe), str(folder)], env=env).strip()

        def ours_says(data):
            txt.write_bytes(data)
            out = run([str(ours_exe), "read", str(txt)]).split()
            return out[0], out[1:]

        def rewritten(data, code):
            txt.write_bytes(data)
            out = work / "out.txt"
            run([str(ours_exe), "write", str(txt), code, str(out)])
            return out.read_bytes()

        full = template(languages, "en-US").encode()
        listing = "".join("# %-8s %s\n" % l for l in languages).encode()
        long_comment = b"#" + b"x" * 78 + b"de and more\n"   # (a comment longer than the host's 79 bytes: its rest is no line)
        files = [template(languages, c).encode() for c in codes]                     # Setup's file, each language
        files += [full.replace(b"\n", b"\r\n"),                                      # Windows line ends
                  b"\xef\xbb\xbf" + full,                                            # Notepad's UTF-8
                  b"\xef\xbb\xbfko\n" + listing,                                     # a mark before the choice itself
                  full.replace(b"en-US\n", b"pt-br\n"), full.replace(b"en-US\n", b"ZH-HANS\n"),   # any case
                  full.replace(b"en-US\n", b"  \tes-419 \t # mine\n"),              # spaces, and a note after
                  full.replace(b"en-US\n", b"english\nxx-YY\nfr-CA\nde\n"),          # lines that name no language first
                  full.replace(b"en-US\n", b"en-USA\n"), full.replace(b"en-US\n", b"en\n"),   # near misses: none chosen
                  full.replace(b"en-US\n\n", b"ru\n"),                              # the list right under the choice
                  listing, b"", b"\n\n\n", b"   \n\t\n", b"nl", b"nl\r", b"pl\r\n",  # no choice; no line end; a bare CR
                  long_comment + b"it\n" + listing,                                  # "de and more" is not a line's start
                  b"x" * 79 + b"ja\n" + b"ko\n" + listing,                           # nor is what follows 79 bytes of anything
                  b" " * 80 + b"ja\nko\n" + listing,                                 # a choice the host cannot see past its 79 bytes
                  b" " * 60 + b"zh-Hant\n" + listing,                                # one it can
                  b"de" + b"x" * 40 + b"\nfr\n" + listing,                           # a word longer than a code
                  b"# fr-ca comes first here\n" + b"\n" * 120 + b"es\n" + listing,   # far down the file
                  full + b"ja\n",                                                    # two choices: the first
                  b"#en-GB\n#\tit\tItalian\n#  ru   Russian\n# zh-hans x\nen-gb\n",  # a list written by hand
                  ]
        for data in files:
            host = host_says(data)
            setup_before = folder_language(str(folder))
            chosen, offered = ours_says(data)
            # the row reads the file as the game will: the same choice, English (US) when the file makes none
            check((chosen if chosen != "-" else "en-US") == host,
                  "game_language::read chooses %s where the host answers %s, for %r" % (chosen, host, data[:120]))
            check(all(c in codes for c in offered) and (not offered or host in offered),
                  "the row would offer %s with the game on %s, for %r" % (offered, host, data[:120]))
            # every language the row offers, written: the host then answers it, Setup's update keeps it, and nothing else in
            # the file changed (the lines other than the choice's are byte for byte the same, in number and in order)
            for code in offered:
                out = rewritten(data, code)
                check(host_says(out) == code, "written for %s, the host answers %s: %r" % (code, host_says(out), out[:160]))
                # (Setup's reader wants the code alone on its line, as it is now written. It does not know a byte order mark
                # right before the choice; and a file on which it already read another language than the host does is not
                # this row's to settle: a choice hidden from the host behind 79 bytes of spaces)
                if setup_before in (None, host) and not out.startswith(b"\xef\xbb\xbf" + code.encode()):
                    check(folder_language(str(folder)) == code,
                          "written for %s, Setup's update reads %r: %r" % (code, folder_language(str(folder)), out[:160]))
                before, after = data.split(b"\n"), out.split(b"\n")
                if chosen != "-":
                    check(len(before) == len(after) and sum(a != b for a, b in zip(before, after)) <= 1,
                          "writing %s changed more than the choice's line: %r -> %r" % (code, data[:160], out[:160]))
                    check(rewritten(out, code) == out, "writing %s twice changed the file again" % code)
                else:   # (no choice line: one line added at the end, the rest as it was)
                    check(out.startswith(data) and out[len(data):].strip() == code.encode() and out.endswith(b"\n"),
                          "writing %s into a file with no choice: %r -> %r" % (code, data[:160], out[:160]))
            # a language the file does not name is never written
            for code in codes:
                if code not in offered:
                    check(rewritten(data, code) == data, "a file that does not name %s was written with it: %r" % (code, data[:120]))
        # what the offer is: the file's own list, and its choice
        check(ours_says(full) == ("en-US", codes), "Setup's file does not offer the game's 16 languages")
        check(ours_says(b"it\n") == ("it", ["it"]), "a file with a choice and no list offers more than that choice")
        check(ours_says(b"# de German\n# fr French\n") == ("-", []), "a list without English (US) and no choice: nothing sound to offer")
        check(ours_says(b"# en-US x\n# fr French\n") == ("-", ["en-US", "fr"]), "a list with English (US) and no choice")
        check(ours_says(b"# The de facto list\n# fr French\nfr\n") == ("fr", ["fr"]), "a comment's later words are not codes")
        # CR LF files keep their line ends, also for a choice added at the end
        check(rewritten(b"# en-US x\r\n# fr French\r\n", "fr") == b"# en-US x\r\n# fr French\r\nfr\r\n", "a CR LF file got a bare LF")
        check(rewritten(b"# en-US x\n# fr French", "fr") == b"# en-US x\n# fr French\nfr\n", "a choice added after a last line with no end")
        check(rewritten(full.replace(b"\n", b"\r\n"), "ko") == full.replace(b"en-US\n", b"ko\n").replace(b"\n", b"\r\n"), "CR LF kept")
        check(rewritten(b"\xef\xbb\xbfen-us  # x\n# ja J\n", "ja") == b"\xef\xbb\xbfja\n# ja J\n", "the byte order mark stays where it was")

        # ---- the menu's own file functions, in a folder of their own
        menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
        state = menu[menu.index("int lang_entries[gl::kCount];"):menu.index("const char* tx_language(int v) {")]
        menu_cpp, menu_exe = work / "menu.cpp", work / "menu.exe"
        fixture = MENU.replace("@ROOT@", ROOT.as_posix()).replace("@STATE@", "namespace gl = game_language;\n" + state)
        for mark, head in (("@TEXT@", "const char* tx_language"), ("@HINT@", "const char* hn_language"), ("@INI_ROW@", "bool ini_row"),
                           ("@PATH@", "std::string lang_path"), ("@READ@", "bool lang_read"), ("@WRITE@", "bool lang_write"),
                           ("@LOAD@", "void lang_load"), ("@STORE@", "void lang_store")):
            fixture = fixture.replace(mark, definition(menu, head))
        menu_cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(menu_cpp), "-o", str(menu_exe)], check=True)
        own = work / "menu-folder"
        own.mkdir()
        (own / "language.txt").write_bytes(template(languages, "de").encode())
        subprocess.run([str(menu_exe), str(own)], check=True, env=env)

    # ---- the row, and where its choice is kept
    row = re.search(r'\{TAB_SYSTEM, "Game language", &opts\.language, 0, 0, 1, tx_language,.*?hn_language, true, true\},', menu, re.S)
    check(row and "Applies after a restart" in row.group(0), "the Game language row is missing from the System tab, or takes no restart")
    rows = menu[menu.index("Row rows[] = {"):]
    check(rows.index('"Game language", &opts.language') < rows.index('"Game speed", &opts.realtime') < rows.index('"Discord status", &opts.discord'),
          "the Game language row is not before Game speed (other rows are added after Discord status)")
    save = definition(menu, "void save")
    check("lang_store();" in save and "if (ini_row(r)) put(ini_key(r)" in save, "save() does not write the choice to language.txt, or writes the row's number to the options file")
    init = definition(menu, "void menu_init")
    check("if (!ini_row(r) || strcmp(line, ini_key(r))) continue;" in init, "menu_init reads the Game language from the options file")
    check(init.index("lang_load();") < init.index("restart_init();"), "the row is not read from language.txt before the start values are kept")
    check("(rows[i].value != &opts.language || lang_count > 1)" in definition(menu, "bool row_shown"),
          "the Game language row shows with nothing to choose from")
    store = definition(menu, "void lang_store")
    check("gl::read(out).chosen == language" in store and "gl::write(text, language)" in store and "lang_read(text)" in store,
          "lang_store does not write through game_language.h on the file as it is now, or does not check what it is about to write")
    readme = (ROOT / "readme.md").read_text(encoding="utf-8")
    check("| Game language |" in readme and "Options, System, Game language" in readme, "readme.md does not say the Game language row")
    guide = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    check("The Game language row is not in this file" in guide, "docs/development.md does not say where the Game language is kept")
    print("PASS: the Game language row reads language.txt as the host does and writes it for the host and for Setup")


def definition_py(source, head):
    """A top-level Python function's text, from its def to the next top-level line"""
    start = source.index(head)
    rest = source[start:].split("\n")
    lines = [rest[0]]
    for line in rest[1:]:
        if line and not line[0].isspace():
            break
        lines.append(line)
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    main()
