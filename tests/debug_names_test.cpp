// tests/test_debug_names.py builds and runs this: gpu/vk/debug_names.h on files of this test's own making (a GameDataList
// and event flows laid out as the game's are, with names and numbers that are the test's own).
#include "../gpu/vk/debug_names.h"

#include <cstdio>
#include <cstdlib>
#include <set>

using namespace mkwvk::debug_names;

static int failures;
#define CHECK(what, ...) do { if (!(what)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t hash_of(const std::string& s) { return oot_name_hash(s.data(), s.size()); }

// ---- a BYML file written: dictionaries, arrays and numbers, in either byte order ----
struct Byml {
    bool big;
    std::vector<unsigned char> b;
    explicit Byml(bool big_) : big(big_) {}
    void u8(unsigned v) { b.push_back((unsigned char)v); }
    void u24(uint32_t v) {
        if (big) u8(v >> 16), u8(v >> 8), u8(v);
        else u8(v), u8(v >> 8), u8(v >> 16);
    }
    void u32(uint32_t v) {
        if (big) u8(v >> 24), u8(v >> 16), u8(v >> 8), u8(v);
        else u8(v), u8(v >> 8), u8(v >> 16), u8(v >> 24);
    }
    void put32(size_t at, uint32_t v) {
        for (int i = 0; i < 4; i++) b[at + i] = (unsigned char)(v >> (big ? 24 - 8 * i : 8 * i));
    }
    size_t here() const { return b.size(); }
    void align() { while (b.size() & 3) u8(0); }
};
static const char* const kKeys[] = {"Bool", "Data", "Hash", "Int", "MetaData", "ResetTypeValue"};
enum { K_BOOL, K_DATA, K_HASH, K_INT, K_META, K_RESET, K_COUNT };

/// A GameDataList: Data.Bool and Data.Int, each an array of {Hash, ResetTypeValue}; bool_type the type the Bools' hashes
/// are written with. with_bool false: a file with no Bool at all
static std::vector<unsigned char> game_data(bool big, const std::vector<uint32_t>& bools, const std::vector<uint32_t>& ints, unsigned bool_type = 0xd3,
                                            bool with_bool = true) {
    Byml y(big);
    y.u8(big ? 'B' : 'Y'), y.u8(big ? 'Y' : 'B');
    if (big) y.u8(0), y.u8(7);
    else y.u8(7), y.u8(0);
    y.u32(0), y.u32(0), y.u32(0);   // (the keys' table, the texts' table: none, the root)
    y.put32(4, (uint32_t)y.here());
    {   // the keys: 0xc2, their number, an offset each and one past the last, the texts
        const size_t table = y.here();
        y.u8(0xc2), y.u24(K_COUNT);
        size_t at = 4 + 4 * (K_COUNT + 1);
        for (int i = 0; i < K_COUNT; i++) y.u32((uint32_t)at), at += strlen(kKeys[i]) + 1;
        y.u32((uint32_t)at);
        for (int i = 0; i < K_COUNT; i++)
            for (const char* t = kKeys[i];; t++) {
                y.u8((unsigned char)*t);
                if (!*t) break;
            }
        y.align();
        (void)table;
    }
    auto entry = [&](int key, unsigned type, uint32_t value) { y.u24((uint32_t)key), y.u8(type), y.u32(value); };
    auto list = [&](const std::vector<uint32_t>& hashes, unsigned type) {   // (the dictionaries first, then the array of them)
        std::vector<uint32_t> at;
        for (uint32_t h : hashes) {
            at.push_back((uint32_t)y.here());
            y.u8(0xc1), y.u24(2);
            entry(K_HASH, type, h), entry(K_RESET, 0xd1, 8);
        }
        const uint32_t array = (uint32_t)y.here();
        y.u8(0xc0), y.u24((uint32_t)hashes.size());
        for (size_t i = 0; i < hashes.size(); i++) y.u8(0xc1);
        y.align();
        for (uint32_t a : at) y.u32(a);
        return array;
    };
    const uint32_t bool_array = list(bools, bool_type), int_array = list(ints, 0xd3);
    const uint32_t data = (uint32_t)y.here();
    y.u8(0xc1), y.u24(with_bool ? 2 : 1);
    if (with_bool) entry(K_BOOL, 0xc0, bool_array);
    entry(K_INT, 0xc0, int_array);
    const uint32_t meta = (uint32_t)y.here();
    y.u8(0xc1), y.u24(0);
    y.put32(12, (uint32_t)y.here());
    y.u8(0xc1), y.u24(2);
    entry(K_DATA, 0xc1, data), entry(K_META, 0xc1, meta);
    return y.b;
}

static void test_game_data() {
    const std::vector<uint32_t> bools = {0xcea220d3u, 0x00000005u, 0xffffffffu, 0x80000000u, 0x00000005u, 0x12345678u};
    const std::vector<uint32_t> ints = {0x0badf00du, 0x00000006u};
    const std::vector<uint32_t> want = {0x00000005u, 0x12345678u, 0x80000000u, 0xcea220d3u, 0xffffffffu};
    for (int big = 0; big < 2; big++) {
        const std::vector<unsigned char> file = game_data(big != 0, bools, ints);
        std::vector<uint32_t> got;
        CHECK(gamedata_bools(file, got) && got == want, "%s-endian: the Bools' hashes are not read sorted and once each (%d of them)", big ? "big" : "little", (int)got.size());
        CHECK(gamedata_bools(game_data(big != 0, bools, ints, 0xd1), got) && got == want, "hashes written as signed numbers are not read");
        CHECK(!gamedata_bools(game_data(big != 0, bools, ints, 0xa0), got) && got.empty(), "a Bool whose Hash is a text is taken");
        CHECK(!gamedata_bools(game_data(big != 0, bools, ints, 0xd3, false), got) && got.empty(), "a file with no Bool is taken (the Ints' hashes are not flags)");
        CHECK(!gamedata_bools(game_data(big != 0, {}, ints), got), "a file with an empty Bool list is taken");
        // every shorter file: refused or read, never past its end (no crash here, and what is read is still sorted)
        for (size_t cut = 0; cut < file.size(); cut++) {
            std::vector<unsigned char> part(file.begin(), file.begin() + (long)cut);
            part.shrink_to_fit();
            const bool ok = gamedata_bools(part, got);
            CHECK(!ok || std::is_sorted(got.begin(), got.end()), "a cut file's hashes are not sorted");
            if (cut < 16) CHECK(!ok, "a file of %d bytes is taken", (int)cut);
        }
        CHECK(!gamedata_bools(std::vector<unsigned char>(file.begin(), file.end() - 17), got), "a file cut inside its root's first entry is taken");
        // any byte changed: no crash, and nothing but sorted hashes comes back
        uint32_t seed = 12345;
        for (int round = 0; round < 20000; round++) {
            std::vector<unsigned char> bent = file;
            for (int k = 0; k < 1 + round % 3; k++) {
                seed = seed * 1664525u + 1013904223u;
                bent[(seed >> 8) % bent.size()] = (unsigned char)(seed >> 24);
            }
            if (gamedata_bools(bent, got)) CHECK(std::is_sorted(got.begin(), got.end()) && !got.empty(), "a bent file's hashes are not sorted");
        }
        std::vector<unsigned char> other = file;
        other[0] = 'X';
        CHECK(!gamedata_bools(other, got), "a file that does not start BY or YB is taken");
    }
    std::vector<uint32_t> got = {1, 2, 3};
    CHECK(!gamedata_bools({}, got) && got.empty(), "an empty file is taken, or the result kept");
}

/// An event flow's header and texts: the magic, the byte order mark, how many flowcharts and timelines, then each text
/// as the files keep them (its length, its bytes, a 0)
static std::vector<unsigned char> event_flow(int flowcharts, int timelines, const std::vector<std::string>& texts) {
    std::vector<unsigned char> f(0x48, 0);
    memcpy(f.data(), "BFEVFL\0\0", 8);
    f[9] = 4, f[0xc] = 0xff, f[0xd] = 0xfe, f[0xe] = 3;
    f[0x20] = (unsigned char)flowcharts, f[0x21] = (unsigned char)(flowcharts >> 8), f[0x22] = (unsigned char)timelines;
    for (const std::string& t : texts) {
        f.push_back((unsigned char)t.size()), f.push_back((unsigned char)(t.size() >> 8));
        f.insert(f.end(), t.begin(), t.end());
        f.push_back(0);
        if (f.size() & 1) f.push_back(0);
    }
    return f;
}

static void test_event_flows() {
    CHECK(event_has_script(event_flow(1, 0, {})) && event_has_script(event_flow(3, 2, {})) && event_has_script(event_flow(0x100, 0, {})), "an event flow with a flowchart is not an event");
    CHECK(!event_has_script(event_flow(0, 1, {})) && !event_has_script(event_flow(0, 0, {})), "a timeline alone is an event");
    std::vector<unsigned char> f = event_flow(1, 0, {});
    f[0] = 'X';
    CHECK(!event_has_script(f), "a file that is not an event flow is an event");
    f = event_flow(1, 0, {});
    f[0xc] = 0xfe, f[0xd] = 0xff;
    CHECK(!event_has_script(f), "a big-endian event flow's count is read as a little-endian one");
    f = event_flow(1, 0, {});
    f.resize(0x23);
    CHECK(!event_has_script(f) && !event_has_script({}), "a cut header is read");

    // the flags' names: the texts whose hash is a Bool's
    const std::vector<std::string> flags = {"Stand_InFlag_A", "Stand_InFlag_B", "Npc_Stand_007_Talked", "abc", std::string(OOT_NAME_MAX, 'z')};
    std::vector<uint32_t> bools;
    for (const std::string& n : flags) bools.push_back(hash_of(n));
    bools.push_back(hash_of("ab")), bools.push_back(hash_of(std::string(OOT_NAME_MAX + 1, 'y'))), bools.push_back(hash_of("Not There"));
    bools.push_back(hash_of("NoZero")), bools.push_back(hash_of("Tail"));
    std::sort(bools.begin(), bools.end());
    std::vector<std::string> texts = {"Stand_InFlag_A", "SomeActor", "Npc_Stand_007_Talked", "ab", "abc", std::string(OOT_NAME_MAX, 'z'),
                                      std::string(OOT_NAME_MAX + 1, 'y'), "Not There", "Stand_InFlag_A", "Stand_InFlag_B"};
    f = event_flow(1, 0, texts);
    f.insert(f.end(), {'N', 'o', 'Z', 'e', 'r', 'o', '!', 0});   // (no 0 after the name's characters)
    f.insert(f.end(), {'T', 'a', 'i', 'l'});                      // (the file ends in the name)
    std::vector<std::string> got;
    flag_names(f, bools, got);
    std::multiset<std::string> names(got.begin(), got.end());
    CHECK(names.count("Stand_InFlag_A") == 2 && names.count("Stand_InFlag_B") == 1 && names.count("Npc_Stand_007_Talked") == 1 && names.count("abc") == 1 &&
          names.count(std::string(OOT_NAME_MAX, 'z')) == 1, "a flag's name in an event flow is not found");
    CHECK(got.size() == 6, "%d names found, 6 wanted: a text that is no Bool's name, too short, too long, not ended by a 0 or cut by the file's end is taken", (int)got.size());
    got.clear();
    flag_names(f, {}, got);
    flag_names({}, bools, got);
    CHECK(got.empty(), "names are found with no Bools, or in no file");

    CHECK(event_of_file("Dm_Stand_In.bfevfl.zs") == "Dm_Stand_In" && event_of_file("Dm_Stand_In.110.bfevfl.zs") == "Dm_Stand_In" &&
          event_of_file("Stand.In.bfevfl.zs") == "Stand.In", "an event's name from its file's");
    CHECK(event_of_file("Dm_Stand_In.bfevfl") == "" && event_of_file(".bfevfl.zs") == "" && event_of_file("two words.bfevfl.zs") == "" &&
          event_of_file("readme.txt") == "" && event_of_file("") == "", "a file that is no event flow is an event");
}

static void test_lists() {
    std::vector<std::string> names;
    for (int i = 0; i < 9; i++) names.push_back("Npc_Stand_" + std::to_string(i));
    for (int i = 0; i < kGroupMin; i++) names.push_back("Dungeon.Stand_" + std::to_string(i));
    for (int i = 0; i < kGroupMin - 1; i++) names.push_back("Few_" + std::to_string(i));
    names.push_back("Alone"), names.push_back("Zebra_1"), names.push_back("_odd"), names.push_back("");
    names.push_back("Npc_Stand_3"), names.push_back("Npc_Stand_3"), names.push_back("Alone");   // (the same names again)
    const List list = make_list(names);
    const int total = 9 + kGroupMin + (kGroupMin - 1) + 3;
    CHECK((int)list.names.size() == total && list.hashes.size() == list.names.size(), "%d names in the list, %d wanted", (int)list.names.size(), total);
    CHECK(std::set<std::string>(list.names.begin(), list.names.end()).size() == list.names.size(), "a name is in the list twice");
    CHECK(list.groups.size() == 3 && list.groups[0].title == "Dungeon" && list.groups[1].title == "Npc" && list.groups[2].title == "Other",
          "the groups are not Dungeon, Npc and Other");
    int at = 0;
    for (const Group& g : list.groups) {
        CHECK(g.first == at && g.count > 0, "the groups do not follow one another through the list");
        CHECK(g.title == "Other" || g.count >= kGroupMin, "the group %s has %d names", g.title.c_str(), g.count);
        CHECK(std::is_sorted(list.names.begin() + g.first, list.names.begin() + g.first + g.count), "the group %s is not sorted", g.title.c_str());
        for (int i = g.first; i < g.first + g.count; i++) {
            CHECK(g.title == "Other" || name_prefix(list.names[i]) == g.title, "%s is in the group %s", list.names[i].c_str(), g.title.c_str());
            CHECK(list.hashes[i] == hash_of(list.names[i]), "a name's hash is another's");
        }
        at += g.count;
    }
    CHECK(at == total, "the groups do not hold every name once");
    CHECK(list.groups[2].count == kGroupMin - 1 + 3, "Other does not hold the names of the small groups");
    CHECK(make_list({}).names.empty() && make_list({}).groups.empty() && make_list({"", ""}).names.empty(), "an empty list has a name or a group");
    const List one = make_list({"b.bin", "a.bin", "Npc_1.bin", "a.bin"}, false, "Your figures");
    CHECK(one.groups.size() == 1 && one.groups[0].title == "Your figures" && one.groups[0].count == 3 && one.names[0] == "Npc_1.bin" && one.names[2] == "b.bin",
          "a list without groups is not one sorted group");
    CHECK(name_prefix("A_b.c") == "A" && name_prefix("A.b_c") == "A" && name_prefix("Abc") == "Abc" && name_prefix("_x") == "_x", "what a name begins with");

    // a page: the choice stays in its group, the page shown follows it
    Page p;
    CHECK(p.empty() && p.begin() == 0 && p.end() == 0 && p.first() == 0 && p.last() == 0 && p.watched().empty(), "an empty page shows something");
    p.move(1, 1, 1, false, false), p.turn_group(1), p.turn_shown(3), p.reveal();
    CHECK(p.choice == 0 && p.group == 0 && !p.choose(0), "an empty page moves");
    p.set_list(list);
    p.columns = 2, p.capacity = 4;
    CHECK(p.group == 0 && p.choice == 0 && p.begin() == 0 && p.end() == kGroupMin && p.pages() == 2, "a new list does not start at its first group");
    p.move(1, 0, 0, false, false);
    CHECK(p.choice == 1, "Right does not go to the next card");
    p.move(0, 1, 0, false, false);
    CHECK(p.choice == 3 && p.shown == 0, "Down does not go a row of two on");
    p.move(0, 1, 0, false, false);
    CHECK(p.choice == 5 && p.shown == 1 && p.first() == 4 && p.last() == kGroupMin, "the page shown does not follow the choice");
    p.move(0, 5, 3, false, false);
    CHECK(p.choice == kGroupMin - 1, "the choice leaves its group at the end");
    p.move(-100, 0, 0, false, false);
    CHECK(p.choice == 0 && p.shown == 0, "the choice leaves its group at the start");
    p.move(0, 0, 1, false, false);
    CHECK(p.choice == 4 && p.shown == 1, "Page Down does not go a page on");
    p.move(0, 0, 0, false, true);
    CHECK(p.choice == kGroupMin - 1, "End");
    p.move(0, 0, 0, true, false);
    CHECK(p.choice == 0, "Home");
    p.turn_shown(5);
    CHECK(p.shown == 1 && p.choice == 0 && p.first() == 4, "the wheel moves the choice, or turns past the last page");
    p.turn_shown(-5);
    CHECK(p.shown == 0, "the wheel turns before the first page");
    p.turn_group(1);
    CHECK(p.group == 1 && p.choice == kGroupMin && p.begin() == kGroupMin && p.end() == kGroupMin + 9 && p.shown == 0, "the next group");
    CHECK(!p.choose(0) && !p.choose(kGroupMin + 9) && p.choose(kGroupMin + 2) && p.choice == kGroupMin + 2, "a click on a card of another group");
    p.turn_group(-1), p.turn_group(-1);
    CHECK(p.group == 2 && p.choice == p.begin(), "the group before the first is not the last");
    p.turn_group(1);
    CHECK(p.group == 0, "the group after the last is not the first");
    // the flags asked for: those shown, never more than a W line holds
    p.capacity = 4, p.shown = 1;
    const std::vector<uint32_t> shown = p.watched();   // (the choice is the group's first name: on the page the wheel turned away)
    CHECK(shown.size() == 3 && shown[0] == list.hashes[4] && shown[1] == list.hashes[5] && shown[2] == list.hashes[0],
          "the flags asked for are not those shown and the chosen one");
    p.choose(5);
    CHECK(p.watched().size() == 2, "the chosen flag is asked for twice when it is shown");
    p.turn_group(1), p.capacity = 500;
    CHECK(p.watched().size() == 9, "a group smaller than a page");
    {
        std::vector<std::string> many;
        for (int i = 0; i < 200; i++) many.push_back("Many_" + std::to_string(1000 + i));
        Page big;
        big.set_list(make_list(many));
        big.capacity = 150;
        CHECK((int)big.watched().size() == OOT_WATCH_MAX, "more flags are asked for than a line holds");
    }
    // a new list: the choice kept by its name
    p.turn_group(-1), p.choose(3);
    const std::string chosen = p.list.names[p.choice];
    names.push_back("Aaa_first_1");
    for (int i = 0; i < kGroupMin; i++) names.push_back("Aaa_" + std::to_string(i));
    p.set_list(make_list(names));
    CHECK(p.list.names[p.choice] == chosen && p.list.groups[p.group].title == "Dungeon" && p.choice >= p.begin() && p.choice < p.end(), "a new list loses the choice");
    p.set_list(make_list({"Only_1"}));
    CHECK(p.choice == 0 && p.group == 0, "a list without the chosen name does not start at its first");
    p.set_list(List());
    CHECK(p.empty() && p.choice == 0, "an empty list keeps a choice");
}

static void test_states_and_lines() {
    States s;
    const std::vector<uint32_t> a = {0x11, 0x22, 0x33}, b = {0x22, 0x33, 0x44};
    CHECK(s.of(0x11) == '?', "a state is known before any was said");
    CHECK(s.ask(a) && !s.ask(a), "the same list is asked for twice, or a new one not");
    char line[64];
    snprintf(line, sizeof line, "%08x 10-", (unsigned)oot_watch_sum(a.data(), 3));
    CHECK(s.take(line) && s.of(0x11) == '1' && s.of(0x22) == '0' && s.of(0x33) == '-' && s.of(0x44) == '?', "the states said are not the cards'");
    CHECK(s.ask(b) && s.of(0x22) == '?', "the states of the list before are kept for another list");
    CHECK(!s.take(line) && s.of(0x22) == '?', "an answer to the list before is taken for this one");
    snprintf(line, sizeof line, "%08x ???", (unsigned)oot_watch_sum(b.data(), 3));
    CHECK(s.take(line) && s.of(0x44) == '?', "no save loaded");
    CHECK(!s.take("") && !s.take("12345678") && !s.take("nonsense"), "a line that is not states is taken");
    CHECK(s.ask({}) && !s.ask({}), "the empty list");

    Lines l;
    CHECK(l.take() == 0, "a line where none waits");
    l.push("F 00000011 1");
    std::string got;
    for (int ch; (ch = l.take()) != 0;) got += (char)ch;
    CHECK(got == "F 00000011 1\n" && l.take() == 0, "a line is not read byte by byte with its line feed: \"%s\"", got.c_str());
    l.push("");
    CHECK(l.take() == '\n' && l.take() == 0, "an empty line is not a line feed alone");
    // of the lines that say what is shown, the newest alone; one being read is finished
    l.push("W 00000001", 'W'), l.push("F 00000011 0"), l.push("W 00000002", 'W'), l.push("W 00000003", 'W');
    got.clear();
    for (int ch; (ch = l.take()) != 0;) got += (char)ch;
    CHECK(got == "F 00000011 0\nW 00000003\n", "the older W lines are sent: \"%s\"", got.c_str());
    l.push("W 00000001", 'W');
    CHECK(l.take() == 'W', "take");
    l.push("W 00000002", 'W'), l.push("W 00000003", 'W');
    got = "W";
    for (int ch; (ch = l.take()) != 0;) got += (char)ch;
    CHECK(got == "W 00000001\nW 00000003\n", "a line half read is dropped, or an older one kept: \"%s\"", got.c_str());
    for (int i = 0; i < 3 * Lines::kLinesMax; i++) l.push("F 00000011 1");
    int lines = 0;
    for (int ch; (ch = l.take()) != 0;) lines += ch == '\n';
    CHECK(lines == Lines::kLinesMax, "%d lines wait, %d at most", lines, Lines::kLinesMax);
    // a key held on a request: the same line is not queued behind itself; another one is
    CHECK(!l.repeats("F 00000011 1"), "a line repeats one that is not waiting");
    l.push("F 00000011 1");
    CHECK(l.repeats("F 00000011 1") && !l.repeats("F 00000011 0") && !l.repeats("F 00000012 1"), "which line repeats the last one waiting");
    // the Debug tools turned off: what was asked and not taken is given up; what is shown is still said; a line half read ends
    l.push("E Dm_Stand_In"), l.push("W 00000001", 'W'), l.push("F 00000012 0");
    l.drop_but('W');
    got.clear();
    for (int ch; (ch = l.take()) != 0;) got += (char)ch;
    CHECK(got == "W 00000001\n", "the lines given up are sent, or the W line is not: \"%s\"", got.c_str());
    l.push("F 00000011 1"), l.push("F 00000012 1"), l.push("W", 'W');
    CHECK(l.take() == 'F', "take");
    l.drop_but('W');
    got = "F";
    for (int ch; (ch = l.take()) != 0;) got += (char)ch;
    CHECK(got == "F 00000011 1\nW\n", "a line half read is cut off, or one not begun is sent: \"%s\"", got.c_str());
    l.drop_but('W');
    CHECK(l.take() == 0, "dropping from no lines");
}

static void test_cards() {
    // a card's name: without what its group is named for; whole in Other and in a list without groups
    std::vector<std::string> names;
    for (int i = 0; i < kGroupMin; i++) names.push_back("Npc_Stand_" + std::to_string(i)), names.push_back("Dungeon.Room_" + std::to_string(i));
    names.push_back("Npc"), names.push_back("Alone_1"), names.push_back("Npcs_1");
    const List list = make_list(names);
    for (int g = 0; g < (int)list.groups.size(); g++)
        for (int i = list.groups[g].first; i < list.groups[g].first + list.groups[g].count; i++) {
            const std::string label = card_label(list, g, i), &name = list.names[i];
            if (list.groups[g].title == "Other") CHECK(label == name, "a name in Other is cut: %s", label.c_str());
            else if (name == "Npc") CHECK(label == "Npc", "the name that is its group's title is left empty");
            else CHECK(list.groups[g].title + name[list.groups[g].title.size()] + label == name && !label.empty(), "the card of %s says %s", name.c_str(), label.c_str());
        }
    const List files = make_list({"Npc_1.bin", "b.bin"}, false, "Npc");
    CHECK(card_label(files, 0, 0) == "Npc_1.bin", "a file's name is cut where the list has no groups of names");
    // a name too long for its card: cut in its middle, the end kept the longer; never longer than what fits
    const std::string name = "Special_Npc_Stand_In_Child_SomewhereFarAway_Talk_Event47";   // (no dot: the test finds the cut by its three)
    for (size_t room = 0; room <= name.size() + 2; room++) {
        const std::string cut = elide_middle(name, [&](const std::string& t) { return t.size() <= room; });
        if (room >= name.size()) {
            CHECK(cut == name, "a name that fits is cut");
            continue;
        }
        CHECK(cut.size() <= std::max<size_t>(room, 3) && cut.find("...") != std::string::npos, "room for %d: \"%s\"", (int)room, cut.c_str());
        const size_t dots = cut.find("..."), head = dots, tail = cut.size() - dots - 3;
        CHECK(name.compare(0, head, cut, 0, head) == 0 && name.compare(name.size() - tail, tail, cut, dots + 3, tail) == 0, "the cut is not the name's start and end: \"%s\"", cut.c_str());
        CHECK(tail >= head && (room < 3 || cut.size() == room), "the end is not the longer part, or room is left: %d for \"%s\"", (int)room, cut.c_str());
    }
    CHECK(elide_middle(std::string(), [](const std::string&) { return false; }) == "...", "nothing fits and nothing is there");
    // UTF-8: cut between characters (three bytes each here)
    const std::string kana = "\xe3\x83\xaa\xe3\x83\xb3\xe3\x82\xaf\xe3\x81\xae\xe3\x83\x95\xe3\x82\xa3\xe3\x82\xae\xe3\x83\xa5\xe3\x82\xa2";
    for (size_t room = 3; room < kana.size(); room++) {
        const std::string cut = elide_middle(kana, [&](const std::string& t) { return t.size() <= room; });
        const size_t dots = cut.find("...");
        CHECK(dots != std::string::npos && dots % 3 == 0 && (cut.size() - dots - 3) % 3 == 0, "a character is cut in two: room %d", (int)room);
    }
}

int main() {
    test_game_data();
    test_event_flows();
    test_lists();
    test_states_and_lines();
    test_cards();
    if (failures) return printf("%d failures\n", failures), 1;
    printf("ok\n");
    return 0;
}
