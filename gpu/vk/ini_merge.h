// (Triforce, handoff §9) triforce_options.ini has more than one writer: the game's Insert menu (vk_menu.cpp save, the whole
// file at every change) and whoever else changes it while the game runs (the owner in a text editor, a tool, a launcher of
// another make). The menu wrote the values it held, so a line changed outside since the game read the file was put back as the
// game had it at its next save: that change was lost, however long before the save it came. Now each line the game writes is
// merged by its key: the game's value where the game changed it since it last read or wrote the file (its base), else what the
// file has now. Two changes made within a second, one in the game and one outside, both stay; one row changed on both sides is
// the game's (the later writer). The game itself takes an outside change at its next start, as a file read at the start is.
// Plain logic: tests/test_options_writers.py compiles it alone
#pragma once
#include <map>
#include <string>

namespace ini_merge {
using Map = std::map<std::string, std::string>;

/// A value as lines are compared: without its line end and the spaces around it
inline std::string trim(const std::string& v) {
    size_t a = 0, b = v.size();
    while (a < b && (v[a] == ' ' || v[a] == '\t')) a++;
    while (b > a && (v[b - 1] == '\n' || v[b - 1] == '\r' || v[b - 1] == ' ' || v[b - 1] == '\t')) b--;
    return v.substr(a, b - a);
}

/// One line of the file into the map, when it is key=value (a key of something before the '=')
inline void add_line(Map& m, const std::string& line) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos || eq == 0) return;
    m[line.substr(0, eq)] = trim(line.substr(eq + 1));
}

/// The value the game writes for key: its own (game) where it changed it since base was taken (base: the game's values as of
/// its last read or write of the file; a key not in base is the game's), else the file's as it is now (disk), and the game's
/// where the file has no such line. *taken: set when the file's was taken over the game's
inline std::string pick(const std::string& key, const std::string& game, const Map& base, const Map& disk, bool* taken) {
    const auto b = base.find(key);
    if (b == base.end() || b->second != trim(game)) return game;   // (changed in the game, or new to the file)
    const auto d = disk.find(key);
    if (d == disk.end() || d->second == b->second) return game;
    if (taken) *taken = true;
    return d->second;
}
}  // namespace ini_merge
