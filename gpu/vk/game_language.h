/* The Insert menu's Game language row (vk_menu.cpp), as plain code: what language.txt in the game folder chooses and offers,
   and the file's text with another language chosen. No Vulkan, no menu: tests/test_game_language.py checks it.

   The game's language is the host's to read, at the start (host/nn_applet.c desired_language): HOS_LANGUAGE, else the
   file's first line that is not blank or a comment and whose first word is one of the game's codes, in any case; English
   (US) when there is none. Setup's update keeps a folder's choice by the same line, and takes it only when the line is the
   code alone, spelled as the game spells it (nx2/make_game.py folder_language). The file lists the codes in its comments
   ("# pt-BR    Portuguese (Brazil)"), as the host and Setup write it.

   The row offers the languages the file itself names, as its choice or in that list, that the game has; it writes the
   choice as the code alone on the choice's line, and no code the file does not name. */
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace mkwvk::game_language {

struct Language {
    const char* code;
    const char* name;
};
/// The game's languages: host/nn_applet.c's table and nx2/make_game.py's LANGUAGES, in their order (the test holds the
/// three together)
constexpr Language kLanguages[] = {
    {"en-US", "English (US)"},        {"en-GB", "English (UK)"}, {"fr", "French (France)"},        {"fr-CA", "French (Canada)"},
    {"de", "German"},                 {"it", "Italian"},         {"es", "Spanish (Spain)"},        {"es-419", "Spanish (Latin America)"},
    {"pt-BR", "Portuguese (Brazil)"}, {"nl", "Dutch"},           {"pl", "Polish"},                 {"ru", "Russian"},
    {"ja", "Japanese"},               {"ko", "Korean"},          {"zh-Hans", "Chinese (Simplified)"}, {"zh-Hant", "Chinese (Traditional)"},
};
constexpr int kCount = sizeof kLanguages / sizeof kLanguages[0];
constexpr int kDefault = 0;   // (English (US): the host's answer when the file chooses nothing)
inline unsigned bit(int language) { return 1u << language; }

/// A code's place in kLanguages, in any case (the host compares so: "pt-br" is pt-BR); -1 when the game has no such code
inline int find(std::string_view code) {
    for (int i = 0; i < kCount; i++) {
        const std::string_view k = kLanguages[i].code;
        if (k.size() != code.size()) continue;
        bool same = true;
        for (size_t c = 0; c < k.size() && same; c++) {
            const auto lower = [](char x) { return x >= 'A' && x <= 'Z' ? (char)(x - 'A' + 'a') : x; };
            same = lower(k[c]) == lower(code[c]);
        }
        if (same) return i;
    }
    return -1;
}

/// What a language.txt says
struct File {
    int chosen = -1;        ///< the language its choice line names (kLanguages' index); -1: it has no such line
    unsigned offered = 0;   ///< a bit a language the file names: its choice, and each "# <code> ..." line
    size_t at = 0, len = 0; ///< the choice line's text to replace: after a byte order mark, without its line ending
    bool crlf = false;      ///< its lines end with CR LF
};

/// The first word of s (after spaces and tabs), up to a space, a tab or a line's end
inline std::string_view word(std::string_view s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) a++;
    size_t b = a;
    while (b < s.size() && s[b] != ' ' && s[b] != '\t' && s[b] != '\r' && s[b] != '\n') b++;
    return s.substr(a, b - a);
}

/// The file as the host reads it, line by line: what a line longer than its 79 bytes holds past them is not a line's start,
/// a byte order mark may open any line, and the choice is the first line that is neither blank nor a comment and names a
/// language of the game (a line that names none is passed over, as there)
inline File read(std::string_view text) {
    File f;
    f.crlf = text.find("\r\n") != std::string_view::npos;
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        size_t stop = end;   // (the line without its ending)
        if (stop > start && text[stop - 1] == '\r') stop--;
        std::string_view line = text.substr(start, stop - start);
        size_t lead = 0;   // (bytes of the line before what the host looks at)
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) lead = 3;
        // (the host's buffer: 79 bytes of the line, cut at a zero byte. It keeps 31 bytes of the word, more than any code has: a
        // longer word names no language there or here)
        std::string_view seen = line.substr(0, 79);
        if (const size_t zero = seen.find('\0'); zero != std::string_view::npos) seen = seen.substr(0, zero);
        seen = seen.substr(lead < seen.size() ? lead : seen.size());
        size_t blank = 0;
        while (blank < seen.size() && (seen[blank] == ' ' || seen[blank] == '\t')) blank++;
        const std::string_view rest = seen.substr(blank);
        if (!rest.empty() && rest[0] == '#') {   // (a comment: a language it lists, "# <code>   <name>")
            const int listed = find(word(rest.substr(1)));
            if (listed >= 0) f.offered |= bit(listed);
        } else if (!rest.empty() && f.chosen < 0) {
            const int named = find(word(rest));
            if (named >= 0) f.chosen = named, f.offered |= bit(named), f.at = start + lead, f.len = stop - (start + lead);
        }
        start = end + 1;
    }
    return f;
}

/// The file's text with `language` chosen: its choice line replaced by the code alone (as Setup's update reads it), every
/// other byte as it was; with no choice line, the code on a line of its own at the end. The text as it is when the file
/// does not name that language: never a code it does not list
inline std::string write(std::string_view text, int language) {
    const File f = read(text);
    if (language < 0 || language >= kCount || !(f.offered & bit(language))) return std::string(text);
    const std::string_view code = kLanguages[language].code;
    std::string out(text);
    if (f.chosen >= 0) {
        out.replace(f.at, f.len, code);
        return out;
    }
    const char* const eol = f.crlf ? "\r\n" : "\n";
    if (!out.empty() && out.back() != '\n') out += eol;
    out += code;
    out += eol;
    return out;
}

/// The languages a file offers, in kLanguages' order, into out (kCount of room): how many. None unless the language in
/// effect (the file's choice, else the host's English (US)) is among them: the row then has nothing sound to show
inline int offered(const File& f, int* out) {
    const int now = f.chosen >= 0 ? f.chosen : kDefault;
    if (!(f.offered & bit(now))) return 0;
    int n = 0;
    for (int i = 0; i < kCount; i++)
        if (f.offered & bit(i)) out[n++] = i;
    return n;
}

} // namespace mkwvk::game_language
