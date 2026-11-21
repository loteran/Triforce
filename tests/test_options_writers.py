"""Two writers of triforce_options.ini (handoff §9): the game's Insert menu and a writer outside the game while it runs.

In Triforce the game's menu is the file's only writer of its own (Triforce Setup, the builders and the host's readers of the
file write none of it; nx2/make_game.py renames an old hookshot_options.ini at a build, with no game running): the other
writer is whoever changes the file while the game runs (the owner in a text editor, a tool). The menu wrote the whole file
with the values it held at every change, so a line changed outside was put back at its next save.

The menu's save() and its merge state are cut out of gpu/vk/vk_menu.cpp as they are and compiled with stand-ins for the rows
and the controls (gpu/vk/ini_merge.h and file_whole.h included as they are), then run on real files, with the file read as
menu_init reads it (each line into the base):
- one row changed outside and another in the game within the same second: both changes are in the file, and stay there
  at the game's later saves; a control (key_) changed outside likewise;
- one row changed on both sides: the game's (the later writer);
- a value the game holds that differs from the file's from the start (a migration, a clamp), with no change outside: the
  game's, as before;
- a line no row writes, added outside: kept, as before;
- RX_VK_OPTIONS_MERGE=0: the change made outside is lost, as before (what the merge prevents);
- the file is written whole (nothing named .part is left beside it), and in place when another program holds it open
  without letting it be replaced (Windows), so that the save is not lost.
The source: menu_init puts each line it reads into the base, save merges and writes whole before it falls back, and the
switch is in docs/development.md.

    python -I -B tests/test_options_writers.py [--cxx clang++]
"""
import argparse
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"
fails = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        fails.append(what)


def block(text, marker):
    start = text.index(marker)
    at = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[at] == "{") - (text[at] == "}")
        at += 1
    return text[start:at]


FIXTURE = r"""
#include "ini_merge.h"
#include "file_whole.h"
#include "merged_rows.h"   // ((Triforce, options_version 7) the lines save no longer writes, and the file's version)
namespace merged_rows = mkwvk::merged_rows;
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
enum { HOS_SW_N = 1, HOS_KEY_N = 2 };
enum { HOS_BIND_PAD, HOS_BIND_KEY };
struct Row { const char* name; int* value; };
int va = 0, vb = 0, vc = 0;
Row rows[] = {{"alpha", &va}, {"beta", &vb}, {"gamma", &vc}};
const char* ini_key(const Row& r) { return r.name; }
bool ini_row(const Row& r) { return r.value != nullptr; }
const char* const bind_names[HOS_KEY_N] = {"a", "up"};
int binds[2][HOS_KEY_N][2] = {};
struct Host { int (*binding)(int set, int i, int k); };
const Host host_v{[](int set, int i, int k) { return binds[set][i][k]; }};
const Host* host = &host_v;
bool can_bind() { return true; }
std::string mic_want;
std::string the_path;
std::string ini_path() { return the_path; }
void lang_store() {}
void logf(const char*, ...) {}
/* MERGE */
/* SAVE */
int* row_of(const std::string& k) { for (Row& r : rows) if (k == r.name) return r.value; return nullptr; }
// (as menu_init: each line into the base, the rows' values taken; and the controls, as host/nn_hid.c binds_load)
void load() {
    FILE* f = fopen(the_path.c_str(), "r");
    char line[80];
    while (f && fgets(line, sizeof line, f)) {
        /* LOAD_LINE */
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        if (int* v = row_of(line)) *v = atoi(eq + 1);
        if (!strcmp(line, "pad_a")) binds[HOS_BIND_PAD][0][0] = atoi(eq + 1);
        if (!strcmp(line, "key_a")) sscanf(eq + 1, "%d,%d", &binds[HOS_BIND_KEY][0][0], &binds[HOS_BIND_KEY][0][1]);
        if (!strcmp(line, "key_up")) sscanf(eq + 1, "%d,%d", &binds[HOS_BIND_KEY][1][0], &binds[HOS_BIND_KEY][1][1]);
    }
    if (f) fclose(f);
}
// (a writer outside the game: a line of the file set to key=value, or added, the rest as it is)
void outside(const std::string& key, const std::string& value) {
    std::ifstream in(the_path);
    std::stringstream out;
    std::string line;
    bool found = false;
    while (std::getline(in, line)) {
        if (line.rfind(key + "=", 0) == 0) line = key + "=" + value, found = true;
        out << line << "\n";
    }
    if (!found) out << key << "=" << value << "\n";
    in.close();
    std::ofstream(the_path, std::ios::trunc) << out.str();
}
#ifdef _WIN32
HANDLE held = INVALID_HANDLE_VALUE;
#endif
int main(int argc, char** argv) {
    the_path = argv[1];
    for (int i = 2; i < argc; i++) {
        const std::string step = argv[i];
        const size_t colon = step.find(':'), eq = step.find('=');
        const std::string what = step.substr(0, colon);
        const std::string key = colon == std::string::npos ? "" : step.substr(colon + 1, eq - colon - 1);
        const std::string value = eq == std::string::npos ? "" : step.substr(eq + 1);
        if (what == "load") load();
        else if (what == "save") save();
        else if (what == "outside") outside(key, value);
        else if (what == "game") {
            if (key == "key_a") sscanf(value.c_str(), "%d,%d", &binds[HOS_BIND_KEY][0][0], &binds[HOS_BIND_KEY][0][1]);
            else *row_of(key) = atoi(value.c_str());
        }
#ifdef _WIN32
        else if (what == "hold") {   // (another program has the file open, sharing reads and writes, not its replacement)
            held = CreateFileA(the_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (held == INVALID_HANDLE_VALUE) return 3;
        } else if (what == "release") CloseHandle(held);
#endif
        else return 2;
    }
    return 0;
}
"""


def lines(path):
    out = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k] = v
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx")
    a = ap.parse_args()
    menu = (VK / "vk_menu.cpp").read_text(encoding="utf-8")
    merge_at = menu.index("// ((Triforce, handoff §9) The options file's other writers.")
    merge = menu[merge_at:menu.index("bool options_merge_on() {", merge_at)] + block(menu, "bool options_merge_on() {")
    save = block(menu, "void save() {")
    init = block(menu, "void menu_init() {")
    load_line = "ini_merge::add_line(ini_base, line);"
    check(load_line in init and init.index(load_line) < init.index("*eq = 0;"),
          "vk_menu.cpp menu_init: each line read goes into the base before the line is cut at its '='")
    check(save.index("ini_merge::add_line(disk, line);") < save.index("ini_merge::pick(key, value, ini_base, disk, &taken)")
          < save.index("file_writers::write_whole(ini_path(), out)") < save.index("written = in_place();")
          < save.index("if (written) ini_base = std::move(now);"),
          "vk_menu.cpp save: the file read, each line merged, written whole (in place only when that fails), the base renewed")
    check("`RX_VK_OPTIONS_MERGE=0`" in (ROOT / "docs" / "development.md").read_text(encoding="utf-8"),
          "docs/development.md's switches table has RX_VK_OPTIONS_MERGE")
    cxx = (shutil.which("clang++") if os.name == "nt" else None) or a.cxx or "g++"
    with tempfile.TemporaryDirectory(prefix="triforce-options-") as tmp:
        src, exe = Path(tmp, "o.cpp"), Path(tmp, "o.exe")
        src.write_text(FIXTURE.replace("/* MERGE */", merge).replace("/* SAVE */", save).replace("/* LOAD_LINE */", load_line),
                       encoding="utf-8")
        subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-I" + str(VK), str(src), "-o", str(exe)], check=True, timeout=300)

        def run(name, start, steps, env_merge=None):
            path = Path(tmp, name + ".ini")
            path.write_text(start, encoding="utf-8")
            env = dict(os.environ)
            env.pop("RX_VK_OPTIONS_MERGE", None)
            if env_merge is not None:
                env["RX_VK_OPTIONS_MERGE"] = env_merge
            r = subprocess.run([str(exe), str(path)] + steps, env=env, timeout=60)
            check(r.returncode == 0, "%s: the fixture ran (%d)" % (name, r.returncode))
            check(not Path(str(path) + ".part").exists(), "%s: nothing is left beside the file" % name)
            return lines(path)

        start = "alpha=1\nbeta=1\ngamma=1\noptions_version=6\ndiscord_app_id=42\npad_a=3\nkey_a=65,0\nkey_up=87,0\n"
        got = run("both", start, ["load", "outside:beta=7", "game:alpha=2", "save"])
        check(got.get("alpha") == "2" and got.get("beta") == "7",
              "a row changed outside and another in the game within a second: both survive (%s)" % got)
        got = run("later", start, ["load", "outside:beta=7", "game:alpha=2", "save", "game:gamma=3", "save", "save"])
        check(got.get("alpha") == "2" and got.get("beta") == "7" and got.get("gamma") == "3",
              "and the change made outside stays at the game's later saves (%s)" % got)
        got = run("same", start, ["load", "outside:alpha=9", "game:alpha=4", "save"])
        check(got.get("alpha") == "4", "one row changed on both sides: the game's, the later writer (%s)" % got.get("alpha"))
        got = run("migrated", start, ["load", "game:gamma=5", "save"])
        check(got.get("gamma") == "5" and got.get("beta") == "1", "the game's own value with no change outside: the game's, as before")
        got = run("keys", start, ["load", "outside:key_up=38,0", "game:key_a=66,1", "save"])
        check(got.get("key_up") == "38,0" and got.get("key_a") == "66,1" and got.get("pad_a") == "3",
              "a control changed outside and another in the game: both survive (%s)" % {k: got.get(k) for k in ("key_up", "key_a")})
        got = run("added", start, ["load", "outside:hand_added=1", "game:alpha=2", "save"])
        check(got.get("hand_added") == "1" and got.get("discord_app_id") == "42", "lines no row writes are kept, as before")
        got = run("old", start, ["load", "outside:beta=7", "game:alpha=2", "save"], env_merge="0")
        check(got.get("alpha") == "2" and got.get("beta") == "1",
              "RX_VK_OPTIONS_MERGE=0: the change made outside is lost, as before (%s)" % got.get("beta"))
        if os.name == "nt":
            got = run("held", start, ["load", "hold", "game:alpha=2", "save", "release"])
            check(got.get("alpha") == "2", "a file another program holds open is written in place: the save is not lost")
    if fails:
        raise SystemExit("%d failed" % len(fails))
    print("all passed")


if __name__ == "__main__":
    main()
