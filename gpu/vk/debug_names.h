// (Triforce) The names on the Debug tab's Game flags, Cutscenes and Scan amiibo pages (vk_menu.cpp draws them), and what
// a page keeps: the list in groups, the choice, the cards shown. No Vulkan, no ImGui and no file here:
// tests/test_debug_names.py compiles this header and runs it on bytes of its own making.
//
// The game knows a flag (a Bool of its GameData) by the MurmurHash3 of its name and nothing else; its files list the
// Bools by hash (GameData/GameDataList.Product.<n>.byml.zs) and use their names in the event flows
// (Event/EventFlow/<event>.bfevfl.zs). So the flags' names are every text of the event flows whose hash is a Bool's, and
// the events are the event flows that hold a script (a flowchart: the others are a cutscene's timelines, which a script
// plays). After NX2navi's F9 panel, which found its names the same way; no line of it is here, and no name: both lists
// are made in memory from the player's own game files each run, and written nowhere.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "oot_debug_link.h"

namespace mkwvk::debug_names {

/// A BYML file's Data.Bool[i].Hash values, sorted, with no number twice: the Bools of a GameDataList (either byte order;
/// the game's is the big-endian one). false when the bytes are not such a file. Nothing outside the bytes is read
inline bool gamedata_bools(const std::vector<unsigned char>& b, std::vector<uint32_t>& out) {
    out.clear();
    const size_t n = b.size();
    if (n < 16 || !((b[0] == 'B' && b[1] == 'Y') || (b[0] == 'Y' && b[1] == 'B'))) return false;
    const bool big = b[0] == 'B';
    bool bad = false;   // (a read outside the file: everything read after it is 0, and the file is refused)
    auto u8 = [&](size_t o) -> uint32_t { return o < n ? b[o] : (bad = true, 0u); };
    auto u24 = [&](size_t o) -> uint32_t {
        if (o > n || n - o < 3) return bad = true, 0u;
        return big ? (uint32_t)b[o] << 16 | (uint32_t)b[o + 1] << 8 | b[o + 2] : (uint32_t)b[o + 2] << 16 | (uint32_t)b[o + 1] << 8 | b[o];
    };
    auto u32 = [&](size_t o) -> uint32_t {
        if (o > n || n - o < 4) return bad = true, 0u;
        return big ? (uint32_t)b[o] << 24 | (uint32_t)b[o + 1] << 16 | (uint32_t)b[o + 2] << 8 | b[o + 3]
                   : (uint32_t)b[o + 3] << 24 | (uint32_t)b[o + 2] << 16 | (uint32_t)b[o + 1] << 8 | b[o];
    };
    // the keys' table: 0xc2, how many, an offset each (and one past the last) from the table's start, the texts
    const size_t keys = u32(4), root = u32(12);
    if (bad || !keys || u8(keys) != 0xc2) return false;
    const uint32_t key_count = u24(keys + 1);
    auto key_index = [&](const char* name) -> int64_t {
        const size_t len = strlen(name);
        for (uint32_t i = 0; i < key_count && !bad; i++) {
            const size_t at = keys + (size_t)u32(keys + 4 + 4 * (size_t)i);
            if (!bad && at < n && n - at > len && !memcmp(b.data() + at, name, len + 1)) return i;
        }
        return -1;
    };
    const int64_t k_data = key_index("Data"), k_bool = key_index("Bool"), k_hash = key_index("Hash");
    if (bad || k_data < 0 || k_bool < 0 || k_hash < 0) return false;
    // a dictionary: 0xc1, how many, then eight bytes each: the key's index (three bytes), the value's type, the value
    auto member = [&](size_t dict, int64_t key, uint32_t* type) -> uint32_t {
        if (u8(dict) != 0xc1) return bad = true, 0u;
        const uint32_t count = u24(dict + 1);
        for (uint32_t i = 0; i < count && !bad; i++) {
            const size_t e = dict + 4 + 8 * (size_t)i;
            if (u24(e) == (uint32_t)key) return *type = u8(e + 3), u32(e + 4);
        }
        return *type = 0, 0u;
    };
    uint32_t type = 0;
    const size_t data = member(root, k_data, &type);
    if (bad || type != 0xc1) return false;
    const size_t bools = member(data, k_bool, &type);
    if (bad || type != 0xc0 || u8(bools) != 0xc0) return false;
    // an array: 0xc0, how many, a type byte each (to a multiple of four), then four bytes each
    const uint32_t count = u24(bools + 1);
    const size_t values = bools + 4 + (((size_t)count + 3) & ~(size_t)3);
    if (bad || count > n / 4) return false;
    out.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        if (u8(bools + 4 + i) != 0xc1) return out.clear(), false;
        const uint32_t hash = member(u32(values + 4 * (size_t)i), k_hash, &type);
        if (bad || (type != 0xd3 && type != 0xd1)) return out.clear(), false;   // (an unsigned number, or a signed one)
        if (hash) out.push_back(hash);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return !out.empty();
}

/// An event flow that holds a script: a little-endian BFEVFL whose header counts a flowchart at least (0x20)
inline bool event_has_script(const std::vector<unsigned char>& f) {
    return f.size() >= 0x24 && !memcmp(f.data(), "BFEVFL\0\0", 8) && f[0xc] == 0xff && f[0xd] == 0xfe && (f[0x20] | f[0x21] << 8) >= 1;
}

/// The texts of a file that are flags' names: every run of a name's characters, 3 to OOT_NAME_MAX of them with a 0 after,
/// whose hash is one of bools (sorted). Added to out (the same name more than once: make_list sorts that out)
inline void flag_names(const std::vector<unsigned char>& f, const std::vector<uint32_t>& bools, std::vector<std::string>& out) {
    const size_t n = f.size();
    for (size_t i = 0; i < n;) {
        if (!oot_name_char(f[i])) {
            i++;
            continue;
        }
        size_t end = i;
        while (end < n && oot_name_char(f[end])) end++;
        const size_t len = end - i;
        if (end < n && !f[end] && len >= 3 && len <= OOT_NAME_MAX &&
            std::binary_search(bools.begin(), bools.end(), oot_name_hash((const char*)f.data() + i, len)))
            out.emplace_back((const char*)f.data() + i, len);
        i = end;
    }
}

/// The event an event flow's file is: its name without ".bfevfl.zs" and without a version before that (".110"); "" when
/// the file is not an event flow's or its name not an event's
inline std::string event_of_file(const std::string& file) {
    static const char kEnd[] = ".bfevfl.zs";
    const size_t end = sizeof kEnd - 1;
    if (file.size() <= end || file.compare(file.size() - end, end, kEnd)) return "";
    std::string name = file.substr(0, file.size() - end);
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot + 1 < name.size() && name.find_first_not_of("0123456789", dot + 1) == std::string::npos) name.resize(dot);
    return oot_name_ok(name.c_str()) ? name : "";
}

// ---- a page's list: the names sorted, each once, in groups by what they begin with ----
struct Group {
    std::string title;
    int first = 0, count = 0;   // (its names: [first, first + count) of the list)
    bool begins = false;        // (its names all begin with its title: not Other, and not a list without groups)
};
struct List {
    std::vector<std::string> names;
    std::vector<uint32_t> hashes;   // (each name's, for the flags)
    std::vector<Group> groups;      // (never empty while there are names)
};
constexpr int kGroupMin = 6;   // (a beginning that fewer names share has no group of its own: they are in "Other")
/// What a name begins with: its text before the first _ or . (the whole name when it has neither)
inline std::string name_prefix(const std::string& name) {
    const size_t cut = name.find_first_of("_.");
    return cut == std::string::npos || cut == 0 ? name : name.substr(0, cut);
}
/// The list of some names (any order, any name any number of times). grouped false: one group of them all (the amiibo
/// folder's files)
inline List make_list(std::vector<std::string> names, bool grouped = true, const char* one_title = "All") {
    List list;
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    names.erase(std::remove(names.begin(), names.end(), std::string()), names.end());
    if (names.empty()) return list;
    std::vector<std::pair<std::string, std::string>> keyed;   // (the group's title, the name)
    keyed.reserve(names.size());
    if (!grouped) {
        for (std::string& name : names) keyed.emplace_back(one_title, std::move(name));
    } else {
        // (sorted, so the names of one beginning are together: counted in one pass)
        std::vector<std::string> prefixes;
        prefixes.reserve(names.size());
        for (const std::string& name : names) prefixes.push_back(name_prefix(name));
        std::vector<std::string> counted = prefixes;
        std::sort(counted.begin(), counted.end());
        for (size_t i = 0; i < names.size(); i++) {
            const auto range = std::equal_range(counted.begin(), counted.end(), prefixes[i]);
            const bool own = range.second - range.first >= kGroupMin;
            // ("\x7f" sorts Other after every group of a beginning: a name's characters are all below it)
            keyed.emplace_back(own ? prefixes[i] : std::string("\x7f"), std::move(names[i]));
        }
        std::sort(keyed.begin(), keyed.end());
    }
    for (size_t i = 0; i < keyed.size(); i++) {
        if (list.groups.empty() || keyed[i].first != keyed[i - 1].first) {
            Group g;
            g.title = keyed[i].first == "\x7f" ? "Other" : keyed[i].first;
            g.begins = grouped && keyed[i].first != "\x7f";
            g.first = (int)i;
            list.groups.push_back(std::move(g));
        }
        list.groups.back().count++;
        list.hashes.push_back(oot_name_hash(keyed[i].second.data(), keyed[i].second.size()));
        list.names.push_back(std::move(keyed[i].second));
    }
    return list;
}

/// A name as its card shows it: without the beginning its group is named for, and the _ or . after that (the group's
/// title says it once for all of them, and the room goes to where the names differ). The whole name in Other, in a list
/// without groups, and where nothing would be left
inline std::string card_label(const List& list, int group, int index) {
    const std::string& name = list.names[index];
    const Group& g = list.groups[group];
    if (!g.begins || name.size() <= g.title.size() + 1) return name;
    return name.substr(g.title.size() + 1);
}
/// A text that does not fit, cut in its middle: a third of what fits from its start, "..." and the rest from its end (the
/// game's names tell each other apart at their ends). fits(text): whether a text fits; it must hold for a shorter one when
/// it holds for a longer. The text itself when it fits, "..." when nothing more does. UTF-8 is cut between characters
template <class Fits>
std::string elide_middle(const std::string& text, Fits fits) {
    if (fits(text)) return text;
    const size_t n = text.size();
    auto cut = [&](size_t keep) {   // (keep bytes of the text at most: a head and a tail on character boundaries)
        size_t head = keep / 3, tail = n - (keep - head);
        while (head > 0 && ((unsigned char)text[head] & 0xc0) == 0x80) head--;
        while (tail < n && ((unsigned char)text[tail] & 0xc0) == 0x80) tail++;
        return text.substr(0, head) + "..." + text.substr(tail);
    };
    size_t lo = 0, hi = n ? n - 1 : 0;   // (the most bytes kept that fit: lo fits or is 0, more than hi does not)
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (fits(cut(mid))) lo = mid;
        else hi = mid - 1;
    }
    return cut(lo);
}

// ---- a page: a list, the group shown, the name chosen in it, and the cards as last drawn ----
struct Page {
    List list;
    int group = 0;      // (the group shown)
    int choice = 0;     // (the name chosen: its place in list.names, inside the group)
    int columns = 1, capacity = 1;   // (as last drawn: the cards in a row, on a page)
    int shown = 0;      // (the page of cards shown: the choice's, or another the wheel turned to)

    bool empty() const { return list.names.empty(); }
    int begin() const { return empty() ? 0 : list.groups[group].first; }
    int end() const { return empty() ? 0 : list.groups[group].first + list.groups[group].count; }
    int pages() const { return std::max(1, (end() - begin() + std::max(1, capacity) - 1) / std::max(1, capacity)); }
    /// The cards shown: [first, last) of list.names
    int first() const { return begin() + std::clamp(shown, 0, pages() - 1) * std::max(1, capacity); }
    int last() const { return std::min(end(), first() + std::max(1, capacity)); }
    /// The page of cards the choice is on
    void reveal() { shown = empty() ? 0 : (choice - begin()) / std::max(1, capacity); }
    /// A new list (the files were read, or the folder listed again): the choice kept by its name when the new list has it
    void set_list(List next) {
        const std::string was = !empty() && choice >= 0 && choice < (int)list.names.size() ? list.names[choice] : std::string();
        list = std::move(next);
        group = 0, choice = 0, shown = 0;
        if (empty()) return;
        const auto at = std::find(list.names.begin(), list.names.end(), was);
        if (!was.empty() && at != list.names.end()) {
            choice = (int)(at - list.names.begin());
            for (int g = 0; g < (int)list.groups.size(); g++)
                if (choice >= list.groups[g].first && choice < list.groups[g].first + list.groups[g].count) group = g;
        }
        reveal();
    }
    /// Another group: dir -1 the one before, 1 the one after, round the ends; its first name chosen
    void turn_group(int dir) {
        if (empty()) return;
        const int n = (int)list.groups.size();
        group = ((group + dir) % n + n) % n;
        choice = begin(), shown = 0;
    }
    /// The choice moved: by cards (Left and Right), rows (Up and Down), pages, or to the group's first or last name. It
    /// stays in the group; the page shown follows it
    void move(int cards, int rows, int by_pages, bool home, bool to_end) {
        if (empty()) return;
        const int step = cards + rows * std::max(1, columns) + by_pages * std::max(1, capacity);
        choice = home ? begin() : to_end ? end() - 1 : std::clamp(choice + step, begin(), end() - 1);
        reveal();
    }
    /// A card clicked: chosen, when it is one of the group's
    bool choose(int index) {
        if (empty() || index < begin() || index >= end()) return false;
        choice = index;
        return true;
    }
    /// The wheel: the page of cards shown, the choice where it is
    void turn_shown(int by) { shown = std::clamp(shown + by, 0, pages() - 1); }
    /// The hashes of the names shown now, and of the chosen one when the wheel turned its page away, at most OOT_WATCH_MAX:
    /// the flags whose states the page asks for
    std::vector<uint32_t> watched() const {
        std::vector<uint32_t> hashes;
        for (int i = first(); i < last() && (int)hashes.size() < OOT_WATCH_MAX; i++) hashes.push_back(list.hashes[i]);
        if (!empty() && (choice < first() || choice >= last()) && (int)hashes.size() < OOT_WATCH_MAX) hashes.push_back(list.hashes[choice]);
        return hashes;
    }
};

/// The flags' states as the game's side last said them, for the cards shown
struct States {
    std::vector<uint32_t> hashes;   // (the list asked for)
    std::string states;             // (a character each: 1, 0, - or ?; empty until an answer for this list came)
    /// The list asked for changed: true when a new W line is to be sent. The states known are forgotten then
    bool ask(const std::vector<uint32_t>& now) {
        if (now == hashes) return false;
        hashes = now, states.clear();
        return true;
    }
    /// A line of states: kept when it answers the list asked for
    bool take(const char* line) {
        std::string got(hashes.size(), '?');
        if (!oot_states_parse(line, oot_watch_sum(hashes.data(), (int)hashes.size()), (int)hashes.size(), got.data())) return false;
        states = std::move(got);
        return true;
    }
    /// A flag's state: 1, 0, - (the save data has no such flag) or ? (not known: no save loaded, or no answer yet)
    char of(uint32_t hash) const {
        for (size_t i = 0; i < hashes.size() && i < states.size(); i++)
            if (hashes[i] == hash) return states[i];
        return '?';
    }
};

/// The lines a page waits to send, a byte a read (HOS_OPT_DEBUG_ASK, HOS_OPT_AMIIBO_ASK): each ends with a line feed,
/// 0 while there is none. The game's side reads them only while the game runs, so of the lines that only say what the
/// page shows now (those beginning with `latest`: W) the newest alone is kept, and no more than kLinesMax wait
struct Lines {
    static constexpr int kLinesMax = 32;
    std::vector<std::string> waiting;
    size_t at = 0;   // (how much of waiting[0] was read)
    void push(const std::string& line, char latest = 0) {
        for (size_t i = at ? 1 : 0; latest && i < waiting.size();) {   // (an older one not yet begun: dropped)
            if (!waiting[i].empty() && waiting[i][0] == latest) waiting.erase(waiting.begin() + (long)i);
            else i++;
        }
        if ((int)waiting.size() < kLinesMax) waiting.push_back(line);
    }
    int take() {
        if (waiting.empty()) return 0;
        if (at < waiting[0].size()) return (unsigned char)waiting[0][at++];
        waiting.erase(waiting.begin()), at = 0;
        return '\n';
    }
    /// The same line as the last one waiting: a key held on a request repeats it, and one of them is enough
    bool repeats(const std::string& line) const { return !waiting.empty() && waiting.back() == line; }
    /// The lines waiting are given up but those beginning with `keep` and one already half read (the Debug tools were
    /// turned off: what was asked of the game and not yet taken is not done after all; a W still says what is shown)
    void drop_but(char keep) {
        for (size_t i = at ? 1 : 0; i < waiting.size();) {
            if (waiting[i].empty() || waiting[i][0] != keep) waiting.erase(waiting.begin() + (long)i);
            else i++;
        }
    }
};

} // namespace mkwvk::debug_names
