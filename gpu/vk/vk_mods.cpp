// (Triforce) The Mods page's engine: GameBanana's list, details, pictures, and installs (vk_mods.h says what each does).
#include "vk_mods.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <wincodec.h>
#undef min
#undef max
#include "vk_paths.h"
#endif

namespace mkwvk {
void logf(const char* fmt, ...);
}

namespace mkwvk::mods {
namespace {

constexpr int kGame = 26242;   // (GameBanana: The Legend of Zelda: Ocarina of Time (Switch 2))

// ((Triforce) 11-18, his "make sure the multiplayer mod is included" and "Download, unzip and start it") Programs of their own
// posted in the section, not romfs mods: unpacked into kits\<folder> beside the game and started with their own start file in
// a window of their own. Only the ones named here: a download is never started because of what it holds
struct Kit {
    bool wip;            // (GameBanana's Wip/<id> or Mod/<id>)
    int id;
    const char* folder;  // (kits\<folder>)
    const char* start;   // (the file that starts it, in the kit's top folder)
};
constexpr Kit kKits[] = {
    {true, 105323, "Squadala", "Start Squadala.bat"},   // (daurosk's Squadala!: online co-op, github.com/daurosk/oot-remake-multiplayer)
};
const Kit* kit_for(bool wip, int id) {
    for (const Kit& k : kKits)
        if (k.wip == wip && k.id == id) return &k;
    return nullptr;
}

// ---- a small JSON reader (GameBanana's answers): objects, arrays, strings with their escapes, numbers, true/false/null ----
struct J {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<J> a;
    // (an object's keys and values side by side: a vector of pair<string, J> needs J complete, which libstdc++ holds to
    // (the Linux build failed on it), libc++ and Microsoft's library do not)
    std::vector<std::string> keys;
    std::vector<J> vals;
    const J& operator[](const char* k) const {
        static const J none;
        if (t == Obj)
            for (size_t i = 0; i < keys.size(); i++)
                if (keys[i] == k) return vals[i];
        return none;
    }
    const J& operator[](size_t i) const {
        static const J none;
        return t == Arr && i < a.size() ? a[i] : none;
    }
    std::string str() const { return t == Str ? s : t == Num ? std::to_string((long long)n) : std::string(); }
    long long num() const { return t == Num ? (long long)n : t == Str ? atoll(s.c_str()) : 0; }
};
struct Parser {
    const char* p;
    const char* e;
    void ws() {
        while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    }
    static void utf8(std::string& out, unsigned c) {
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) out += (char)(0xC0 | c >> 6), out += (char)(0x80 | (c & 0x3F));
        else if (c < 0x10000) out += (char)(0xE0 | c >> 12), out += (char)(0x80 | (c >> 6 & 0x3F)), out += (char)(0x80 | (c & 0x3F));
        else out += (char)(0xF0 | c >> 18), out += (char)(0x80 | (c >> 12 & 0x3F)), out += (char)(0x80 | (c >> 6 & 0x3F)), out += (char)(0x80 | (c & 0x3F));
    }
    bool string(std::string& out) {
        if (p >= e || *p != '"') return false;
        p++;
        while (p < e && *p != '"') {
            if (*p != '\\') { out += *p++; continue; }
            if (++p >= e) return false;
            const char c = *p++;
            switch (c) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': break;
            case 'b': case 'f': break;
            case 'u': {
                auto hex4 = [&](unsigned& v) {
                    if (e - p < 4) return false;
                    v = 0;
                    for (int i = 0; i < 4; i++) {
                        const char h = p[i];
                        v = v * 16 + (h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0);
                    }
                    p += 4;
                    return true;
                };
                unsigned v;
                if (!hex4(v)) return false;
                if (v >= 0xD800 && v < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {   // (a pair: one character past U+FFFF)
                    p += 2;
                    unsigned lo;
                    if (hex4(lo) && lo >= 0xDC00 && lo < 0xE000) v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(out, v);
                break;
            }
            default: out += c;   // (\" \\ \/)
            }
        }
        if (p >= e) return false;
        p++;
        return true;
    }
    bool value(J& v, int depth = 0) {
        if (depth > 64) return false;
        ws();
        if (p >= e) return false;
        if (*p == '{') {
            v.t = J::Obj;
            p++;
            ws();
            if (p < e && *p == '}') { p++; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!string(k)) return false;
                ws();
                if (p >= e || *p != ':') return false;
                p++;
                v.keys.push_back(std::move(k));
                v.vals.emplace_back();
                if (!value(v.vals.back(), depth + 1)) return false;
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == '}') { p++; return true; }
                return false;
            }
        }
        if (*p == '[') {
            v.t = J::Arr;
            p++;
            ws();
            if (p < e && *p == ']') { p++; return true; }
            for (;;) {
                v.a.emplace_back();
                if (!value(v.a.back(), depth + 1)) return false;
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == ']') { p++; return true; }
                return false;
            }
        }
        if (*p == '"') { v.t = J::Str; return string(v.s); }
        if (!strncmp(p, "true", std::min<size_t>(4, e - p))) { v.t = J::Bool, v.b = true, p += 4; return true; }
        if (!strncmp(p, "false", std::min<size_t>(5, e - p))) { v.t = J::Bool, p += 5; return true; }
        if (!strncmp(p, "null", std::min<size_t>(4, e - p))) { p += 4; return true; }
        char* end = nullptr;
        std::string num(p, std::min<size_t>(64, e - p));
        v.n = strtod(num.c_str(), &end);
        if (end == num.c_str()) return false;
        v.t = J::Num;
        p += end - num.c_str();
        return true;
    }
};
bool parse(const std::string& text, J& out) {
    Parser ps{text.data(), text.data() + text.size()};
    return ps.value(out);
}

/// GameBanana's descriptions are HTML: the text alone, a line where a paragraph or a break was
std::string plain(const std::string& html) {
    std::string out;
    for (size_t i = 0; i < html.size();) {
        if (html[i] == '<') {
            const size_t end = html.find('>', i);
            if (end == std::string::npos) break;
            std::string tag = html.substr(i + 1, end - i - 1);
            for (char& c : tag) c = (char)tolower((unsigned char)c);
            if (!tag.compare(0, 2, "br") || !tag.compare(0, 2, "/p") || !tag.compare(0, 3, "/li") || !tag.compare(0, 3, "/h") ||
                !tag.compare(0, 4, "/div"))
                out += '\n';
            else if (!tag.compare(0, 2, "li")) out += "- ";
            i = end + 1;
            continue;
        }
        if (html[i] == '&') {
            static const struct { const char* name; const char* text; } ents[] = {
                {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"}, {"&nbsp;", " "},
                {"&rsquo;", "\xE2\x80\x99"}, {"&lsquo;", "\xE2\x80\x98"}, {"&ldquo;", "\xE2\x80\x9C"}, {"&rdquo;", "\xE2\x80\x9D"},
                {"&hellip;", "..."}, {"&mdash;", "\xE2\x80\x94"}, {"&ndash;", "\xE2\x80\x93"}};
            bool done = false;
            for (const auto& en : ents)
                if (!html.compare(i, strlen(en.name), en.name)) { out += en.text, i += strlen(en.name), done = true; break; }
            if (done) continue;
        }
        out += html[i++];
    }
    // (runs of blank lines and spaces: one each)
    std::string tidy;
    int newlines = 0;
    bool space = false;
    for (char c : out) {
        if (c == '\r' || c == '\t') c = ' ';
        if (c == '\n') { if (++newlines <= 2 && !tidy.empty()) tidy += '\n'; space = false; continue; }
        if (c == ' ') { if (!space && !tidy.empty() && tidy.back() != '\n') tidy += ' '; space = true; continue; }
        newlines = 0, space = false;
        tidy += c;
    }
    while (!tidy.empty() && (tidy.back() == '\n' || tidy.back() == ' ')) tidy.pop_back();
    if (tidy.size() > 1400) tidy = tidy.substr(0, 1400) + "...";
    return tidy;
}

std::string safe_name(const std::string& name) {   // (a folder name: no characters Windows refuses, not too long)
    std::string s;
    for (unsigned char c : name) {
        if (c < 32 || strchr("<>:\"/\\|?*", c)) continue;
        s += (char)c;
    }
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
    while (!s.empty() && (s[0] == '_' || s[0] == '-' || s[0] == '.' || s[0] == ' ')) s.erase(0, 1);   // (never "off" by its name)
    if (s.size() > 60) s.resize(60);
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
    return s.empty() ? std::string("mod") : s;
}

std::string json_text(const std::string& s) {   // (a JSON string's inside)
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += (char)c;
        else if (c == '\n') o += "\\n";
        else if (c < 32) continue;
        else o += (char)c;
    }
    return o;
}

// ---- state ----
std::mutex mu;
// ((Triforce) 11-17, a search: two lists. browse, the section page by page as before; found, a search's results (GameBanana's
// own search: Util/Search/Results, whole words, then the section's name filter when that finds none). The menu reads the active
// one; clearing a search goes back to browse as it was. gen: a list's requests in flight belong to the list's current search;
// an answer for an older one is dropped)
struct FeedData {
    std::vector<Mod> mods;
    Feed feed = Feed::Idle;
    std::string error;
    int pages_loaded = 0, total = 0;
    bool complete = false;
    unsigned gen = 0;
    std::string query;          // (found: the search's text, trimmed)
    bool name_filter = false;   // (found: the name filter's pages, the whole-word search having found none)
    // ((Triforce) 11-18) the next page asked for already (they come one after another until the list is whole: before, only
    // when the cursor neared the list's end, so the wheel, or a category's few cards, showed a page or two of 9 pages)
    bool queued = false;
    int retries = 0;            // (a failed page tried again, a few times, before the list stops)
};
FeedData browse, found;
FeedData* active = &browse;
/// ((Triforce) mu held) a list emptied: the menu's textures of its pictures handed back, for it to let go (vk_menu.cpp)
std::vector<void*> clear_list(FeedData& fd) {
    std::vector<void*> textures;
    for (Mod& m : fd.mods)
        if (m.texture) textures.push_back(m.texture), m.texture = nullptr;
    fd.mods.clear(), fd.pages_loaded = 0, fd.total = 0, fd.complete = false, fd.error.clear(), fd.queued = false, fd.retries = 0;
    return textures;
}
InstallState inst;
std::atomic<bool> cancel_flag{false};

// (the work: a queue on one thread for the list, details and installs; pictures on a second, so an install never waits for them)
struct Worker {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::function<void()>> q;
    bool started = false;
    void push(std::function<void()> f, bool front = false) {
        {
            std::lock_guard<std::mutex> g(m);
            if (front) q.push_front(std::move(f));
            else q.push_back(std::move(f));
            if (!started) {
                started = true;
                std::thread([this] {
                    for (;;) {
                        std::function<void()> f;
                        {
                            std::unique_lock<std::mutex> g(m);
                            cv.wait(g, [this] { return !q.empty(); });
                            f = std::move(q.front());
                            q.pop_front();
                        }
                        f();
                    }
                }).detach();
            }
        }
        cv.notify_one();
    }
};
Worker& work() { static Worker* w = new Worker; return *w; }
Worker& pictures() { static Worker* w = new Worker; return *w; }

#ifdef _WIN32
std::string game_folder() {
    char p[MAX_PATH * 2];
    const DWORD n = game_module_path(p, sizeof p);
    if (!n) return ".";
    std::string s(p, n);
    const size_t cut = s.find_last_of("\\/");
    return cut == std::string::npos ? std::string(".") : s.substr(0, cut);
}

// ---- WinHTTP, loaded when first used (the DLL links user32 alone) ----
struct Http {
    HMODULE m = nullptr;
    decltype(&WinHttpOpen) open = nullptr;
    decltype(&WinHttpConnect) connect = nullptr;
    decltype(&WinHttpOpenRequest) request = nullptr;
    decltype(&WinHttpSendRequest) send = nullptr;
    decltype(&WinHttpReceiveResponse) receive = nullptr;
    decltype(&WinHttpQueryHeaders) query = nullptr;
    decltype(&WinHttpReadData) read = nullptr;
    decltype(&WinHttpCloseHandle) close = nullptr;
    decltype(&WinHttpCrackUrl) crack = nullptr;
    decltype(&WinHttpSetOption) option = nullptr;
    decltype(&WinHttpSetTimeouts) timeouts = nullptr;
    HINTERNET session = nullptr;
    bool ok = false;
    Http() {
        m = LoadLibraryA("winhttp.dll");
        if (!m) return;
#define GET(f, n) f = (decltype(f))(void*)GetProcAddress(m, n)
        GET(open, "WinHttpOpen"); GET(connect, "WinHttpConnect"); GET(request, "WinHttpOpenRequest"); GET(send, "WinHttpSendRequest");
        GET(receive, "WinHttpReceiveResponse"); GET(query, "WinHttpQueryHeaders"); GET(read, "WinHttpReadData");
        GET(close, "WinHttpCloseHandle"); GET(crack, "WinHttpCrackUrl"); GET(option, "WinHttpSetOption"); GET(timeouts, "WinHttpSetTimeouts");
#undef GET
        if (!open || !connect || !request || !send || !receive || !query || !read || !close || !crack) return;
        session = open(L"Triforce mod browser", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) session = open(L"Triforce mod browser", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return;
        if (timeouts) timeouts(session, 15000, 15000, 30000, 60000);
        if (option) {
            DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;   // (GameBanana's /dl/ links send to its file server)
            option(session, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect);
            DWORD protocols = 0x00000800 /*TLS 1.2*/ | 0x00002000 /*TLS 1.3*/;
            option(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
        }
        ok = true;
    }
};
Http& http() { static Http* h = new Http; return *h; }

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

/// GET url: the body to `sink` piece by piece (with the length the server says, 0 unknown); false with *err on failure
bool get(const std::string& url, const std::function<bool(const char*, size_t, uint64_t)>& sink, std::string* err) {
    Http& h = http();
    if (!h.ok) { *err = "Windows' internet library (winhttp.dll) could not be loaded"; return false; }
    const std::wstring w = wide(url);
    URL_COMPONENTS u{};
    u.dwStructSize = sizeof u;
    wchar_t host[256] = {}, path[2048] = {};
    u.lpszHostName = host, u.dwHostNameLength = 255;
    u.lpszUrlPath = path, u.dwUrlPathLength = 2047;
    wchar_t extra[2048] = {};
    u.lpszExtraInfo = extra, u.dwExtraInfoLength = 2047;
    if (!h.crack(w.c_str(), 0, 0, &u)) { *err = "not a web address: " + url; return false; }
    std::wstring object = std::wstring(path) + extra;
    HINTERNET c = h.connect(h.session, host, u.nPort, 0);
    if (!c) { *err = "could not reach " + url.substr(0, 60) + " (error " + std::to_string(GetLastError()) + ")"; return false; }
    HINTERNET r = h.request(c, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                            u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    bool ok = false;
    if (r && h.send(r, L"Accept: application/json, */*\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && h.receive(r, nullptr)) {
        DWORD status = 0, size = sizeof status;
        h.query(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        wchar_t len_text[32] = {};
        DWORD len_size = sizeof len_text;
        uint64_t length = 0;
        if (h.query(r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len_text, &len_size, WINHTTP_NO_HEADER_INDEX))
            length = _wcstoui64(len_text, nullptr, 10);
        if (status != 200) {
            *err = "GameBanana answered " + std::to_string(status);
        } else {
            ok = true;
            std::vector<char> buf(1 << 16);
            for (;;) {
                DWORD got = 0;
                if (!h.read(r, buf.data(), (DWORD)buf.size(), &got)) { ok = false, *err = "the download stopped (error " + std::to_string(GetLastError()) + ")"; break; }
                if (!got) break;
                if (!sink(buf.data(), got, length)) { ok = false, *err = "cancelled"; break; }
            }
        }
    } else {
        *err = "no answer from " + url.substr(0, 60) + " (error " + std::to_string(GetLastError()) + "): check the internet connection";
    }
    if (r) h.close(r);
    h.close(c);
    return ok;
}
bool get_text(const std::string& url, std::string& out, std::string* err) {
    out.clear();
    return get(url, [&](const char* p, size_t n, uint64_t) { out.append(p, n); return out.size() < (64u << 20); }, err);
}

// ---- pictures: WIC (Windows' own JPEG/PNG/WebP decoders), loaded when first used ----
const GUID kClsidWic = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
const GUID kIidWicFactory = {0xec5ec8a9, 0xc395, 0x4314, {0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70}};
const GUID kRgba = {0xf5c7ad2d, 0x6a8d, 0x43dd, {0xa7, 0xa8, 0xa2, 0x99, 0x35, 0x26, 0x1a, 0xe9}};   // (GUID_WICPixelFormat32bppRGBA)
bool decode(const std::string& bytes, int max_w, std::vector<unsigned char>& rgba, int* w, int* h) {
    static HMODULE ole = LoadLibraryA("ole32.dll");
    if (!ole) return false;
    static auto co_init = (HRESULT(WINAPI*)(LPVOID, DWORD))(void*)GetProcAddress(ole, "CoInitializeEx");
    static auto co_create = (HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*))(void*)GetProcAddress(ole, "CoCreateInstance");
    if (!co_init || !co_create) return false;
    static thread_local bool inited = false;
    if (!inited) co_init(nullptr, COINIT_MULTITHREADED), inited = true;
    IWICImagingFactory* f = nullptr;
    if (FAILED(co_create(kClsidWic, nullptr, CLSCTX_INPROC_SERVER, kIidWicFactory, (void**)&f)) || !f) return false;
    bool ok = false;
    IWICStream* st = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICBitmapScaler* sc = nullptr;
    IWICFormatConverter* cv = nullptr;
    UINT iw = 0, ih = 0;
    if (SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory((BYTE*)bytes.data(), (DWORD)bytes.size())) &&
        SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
        SUCCEEDED(fr->GetSize(&iw, &ih)) && iw && ih) {
        UINT tw = iw, th = ih;
        if ((int)tw > max_w) th = std::max(1u, (UINT)((double)ih * max_w / iw)), tw = (UINT)max_w;
        IWICBitmapSource* src = fr;
        if ((tw != iw || th != ih) && SUCCEEDED(f->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(fr, tw, th, WICBitmapInterpolationModeFant)))
            src = sc;
        else tw = iw, th = ih;
        if (SUCCEEDED(f->CreateFormatConverter(&cv)) &&
            SUCCEEDED(cv->Initialize(src, kRgba, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
            rgba.resize((size_t)tw * th * 4);
            if (SUCCEEDED(cv->CopyPixels(nullptr, tw * 4, (UINT)rgba.size(), rgba.data()))) ok = true, *w = (int)tw, *h = (int)th;
        }
    }
    if (cv) cv->Release();
    if (sc) sc->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    if (st) st->Release();
    f->Release();
    return ok;
}

// ---- files ----
bool is_dir(const std::string& p) {
    const DWORD a = GetFileAttributesW(wide(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
std::vector<std::string> subdirs(const std::string& dir) {
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wide(dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        char name[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, nullptr, nullptr);
        out.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}
std::vector<std::string> files_in(const std::string& dir) {
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wide(dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char name[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, nullptr, nullptr);
        out.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}
void remove_tree(const std::string& dir) {
    for (const std::string& f : files_in(dir)) {
        const std::wstring p = wide(dir + "\\" + f);
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(p.c_str());
    }
    for (const std::string& d : subdirs(dir)) remove_tree(dir + "\\" + d);
    RemoveDirectoryW(wide(dir).c_str());
}
std::string read_file(const std::string& path) {
    std::string out;
    HANDLE h = CreateFileW(wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) out.append(buf, got);
    CloseHandle(h);
    return out;
}
bool write_file(const std::string& path, const std::string& text) {
    HANDLE h = CreateFileW(wide(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    const bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &put, nullptr) && put == text.size();
    CloseHandle(h);
    return ok;
}

struct Installed {
    int id;
    bool wip;
    int64_t date;
};
/// ((Triforce) a triforce_mod.json / triforce_kit.json: a WiP's says "gamebanana_model": "Wip" (a Mod's, from before, none)
bool info_is_wip(const J& j) { return j["gamebanana_model"].str() == "Wip"; }
/// The folders in the mods folder that came from GameBanana, and the kits installed: the id -> the file's date there
std::vector<Installed> installed_ids() {
    std::vector<Installed> out;
    const std::string root = game_folder() + "\\mods";
    for (const std::string& d : subdirs(root)) {
        J j;
        const std::string text = read_file(root + "\\" + d + "\\triforce_mod.json");
        if (text.empty() || !parse(text, j)) continue;
        if (const long long id = j["gamebanana_id"].num()) out.push_back({(int)id, info_is_wip(j), (int64_t)j["file_date"].num()});
    }
    for (const Kit& k : kKits) {
        J j;
        const std::string text = read_file(game_folder() + "\\kits\\" + k.folder + "\\triforce_kit.json");
        if (!text.empty() && parse(text, j) && (int)j["gamebanana_id"].num() == k.id) out.push_back({k.id, k.wip, (int64_t)j["file_date"].num()});
    }
    return out;
}
void mark_installed() {   // (mu held)
    const auto ids = installed_ids();
    for (FeedData* fd : {&browse, &found})
        for (Mod& m : fd->mods) {
            m.installed = m.update = false;
            for (const auto& [id, wip, date] : ids)
                if (id == m.id && wip == m.wip) {
                    m.installed = true;
                    int64_t newest = 0;
                    for (const File& f : m.files) newest = std::max(newest, f.added);
                    m.update = newest > date && date > 0;
                }
        }
}
/// ((Triforce) mu held) every entry for mod id, in either list (a mod a search found is in both): what came for it goes to each
template <typename F> void each_with_id(int id, bool wip, F f) {
    for (FeedData* fd : {&browse, &found})
        for (Mod& m : fd->mods)
            if (m.id == id && m.wip == wip) f(m);   // ((Triforce) a WiP's ids and a Mod's are counted apart on GameBanana)
}
/// ((Triforce) a search's text in a URL: its UTF-8 bytes, letters, digits and - _ . ~ as they are, the rest %XX. Sent in
/// another code page GameBanana drops every filter, and & or + cut or change the query)
std::string url_encode(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string o;
    for (unsigned char ch : s) {
        const bool plain_ascii = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
        if (plain_ascii || ch == '-' || ch == '_' || ch == '.' || ch == '~') o += (char)ch;
        else o += '%', o += hex[ch >> 4], o += hex[ch & 15];
    }
    return o;
}

// ---- the list ----
void fetch_page(FeedData* fd, unsigned gen, int page) {
    std::string url, query;
    bool name_filter = false;
    {
        std::lock_guard<std::mutex> g(mu);
        if (fd->gen != gen) return;   // ((Triforce) another search since: not asked any more)
        query = fd->query, name_filter = fd->name_filter;
        fd->queued = false;
    }
    // ((Triforce) 11-18) mods and WiPs (_csvModelInclusions: the section's requests, questions and tools left out by GameBanana,
    // 15 a page either way): a WiP with files is often a mod all the same, and the online co-op (a kit) is posted as one
    char head[200];
    std::string wip_url;   // (GameBanana's search takes one kind a request: the WiPs' asked too, with the first page)
    if (fd == &browse) {
        snprintf(head, sizeof head, "https://gamebanana.com/apiv11/Game/%d/Subfeed?_sSort=default&_csvModelInclusions=Mod,Wip&_nPage=%d", kGame, page);
        url = head;
    } else if (!name_filter) {   // (GameBanana's own search, as its search page asks it: whole words, the best matches first)
        snprintf(head, sizeof head, "https://gamebanana.com/apiv11/Util/Search/Results?_sModelName=Mod&_idGameRow=%d&_sOrder=best_match&_nPage=%d&_sSearchString=", kGame, page);
        url = head + url_encode(query);
        if (page == 1) {
            snprintf(head, sizeof head, "https://gamebanana.com/apiv11/Util/Search/Results?_sModelName=Wip&_idGameRow=%d&_sOrder=best_match&_nPage=1&_sSearchString=", kGame);
            wip_url = head + url_encode(query);
        }
    } else {   // (the section's name filter, as the game's page asks it: a part of a name, 3 letters at least)
        snprintf(head, sizeof head, "https://gamebanana.com/apiv11/Game/%d/Subfeed?_sSort=default&_csvModelInclusions=Mod,Wip&_nPage=%d&_sName=", kGame, page);
        url = head + url_encode(query);
    }
    std::string text, err;
    J j, jw;
    const bool ok = get_text(url.c_str(), text, &err) && parse(text, j);
    std::string wtext, werr;
    const bool wok = ok && !wip_url.empty() && get_text(wip_url.c_str(), wtext, &werr) && parse(wtext, jw);
    if (ok && !wip_url.empty() && !wok) logf("mods: search \"%s\": the WiPs could not be read (%s): the mods alone", query.c_str(), werr.c_str());
    std::lock_guard<std::mutex> g(mu);
    if (fd->gen != gen) return;   // ((Triforce) an answer for an older search: dropped)
    const char* what = fd == &browse ? "page" : name_filter ? "search (names)" : "search";
    if (!ok) {
        fd->error = err.empty() ? "GameBanana's answer could not be read" : err;
        logf("mods: %s %d: %s", what, page, fd->error.c_str());
        // ((Triforce) 11-18) a page that failed with cards already listed: tried again after 2, 4, 8 s; then the list stops
        // there (load_more, or the page opened again, goes on from it)
        if (!fd->mods.empty() && fd->retries < 3) {
            const int wait_ms = 2000 << fd->retries++;
            fd->queued = true;
            std::thread([fd, gen, page, wait_ms] {
                std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
                work().push([fd, gen, page] { fetch_page(fd, gen, page); });
            }).detach();
            return;
        }
        fd->feed = Feed::Failed;
        return;
    }
    fd->retries = 0;
    fd->total = (int)j["_aMetadata"]["_nRecordCount"].num() + (wok ? (int)jw["_aMetadata"]["_nRecordCount"].num() : 0);
    fd->complete = j["_aMetadata"]["_bIsComplete"].t == J::Bool && j["_aMetadata"]["_bIsComplete"].b;
    int added = 0;
    std::vector<Mod>& list_now = fd->mods;
    std::vector<const J*> records;
    for (const J& r : j["_aRecords"].a) records.push_back(&r);
    if (wok)
        for (const J& r : jw["_aRecords"].a) records.push_back(&r);
    for (const J* rp : records) {
        const J& r = *rp;
        const std::string model = r["_sModelName"].str();
        if (model != "Mod" && model != "Wip") continue;   // (the section's requests, questions and tools: not mods)
        // ((Triforce) a WiP with nothing to download (a showcase) left out; a search's records may not say: kept, its details tell)
        if (model == "Wip" && r["_bHasFiles"].t == J::Bool && !r["_bHasFiles"].b) continue;
        // ((Triforce) the search's records name their game: another game's, only when the query's filters were lost, left out)
        if (r["_aGame"].t == J::Obj && (int)r["_aGame"]["_idRow"].num() != kGame) continue;
        Mod m;
        m.id = (int)r["_idRow"].num();
        m.wip = model == "Wip";
        m.kit = kit_for(m.wip, m.id) != nullptr;
        if (std::any_of(list_now.begin(), list_now.end(), [&](const Mod& o) { return o.id == m.id && o.wip == m.wip; })) continue;
        if (fd == &found)   // ((Triforce) one the section's list has already: its details and its picture's file taken along)
            for (const Mod& b : browse.mods)
                if (b.id == m.id && b.wip == m.wip) {
                    m.details_state = b.details_state == 1 ? 0 : b.details_state;
                    m.text = b.text, m.files = b.files, m.downloads = b.downloads, m.thumb_file = b.thumb_file;
                    break;
                }
        m.name = r["_sName"].str();
        m.author = r["_aSubmitter"]["_sName"].str();
        m.category = r["_aRootCategory"]["_sName"].str();
        m.page = r["_sProfileUrl"].str();
        m.likes = (int)r["_nLikeCount"].num();
        m.views = (int)r["_nViewCount"].num();
        m.updated = r["_tsDateModified"].num() ? r["_tsDateModified"].num() : r["_tsDateAdded"].num();
        const J& img = r["_aPreviewMedia"]["_aImages"][(size_t)0];
        if (img.t == J::Obj) {
            // (530 wide when GameBanana made that size: sharp in the details; the cards draw it smaller)
            const std::string file = !img["_sFile530"].str().empty() ? img["_sFile530"].str()
                                   : !img["_sFile220"].str().empty() ? img["_sFile220"].str() : img["_sFile"].str();
            m.thumb_url = img["_sBaseUrl"].str() + "/" + file;
        }
        list_now.push_back(std::move(m));
        added++;
    }
    fd->pages_loaded = page;
    // ((Triforce) the whole-word search found none: the section's name filter, which finds a part of a name ("majora", "zeld"))
    if (fd == &found && !name_filter && page == 1 && list_now.empty() && fd->complete && fd->query.size() >= 3) {
        fd->name_filter = true, fd->pages_loaded = 0, fd->complete = false;
        logf("mods: search \"%s\": no whole-word matches; the names next", fd->query.c_str());
        work().push([fd, gen] { fetch_page(fd, gen, 1); }, true);
        return;
    }
    fd->feed = Feed::Ready;
    mark_installed();
    if (fd == &browse)
        logf("mods: page %d: %d mods (%zu in the list, %d mods and WiPs in the section)", page, added, list_now.size(), fd->total);
    else
        logf("mods: %s \"%s\" page %d: %d mods (%zu found, about %d)", what, fd->query.c_str(), page, added, list_now.size(), fd->total);
    // ((Triforce) 11-18) the next page at once, behind the details and pictures asked for, until GameBanana says the list is whole
    // (before: only when the cursor came near the list's end; the wheel, and a category's few cards, never asked)
    if (!fd->complete && !fd->queued) {
        fd->queued = true;
        work().push([fd, gen, page] { fetch_page(fd, gen, page + 1); });
    }
}

void fetch_details(int id, bool wip) {
    std::string text, err;
    char url[256];
    snprintf(url, sizeof url, "https://gamebanana.com/apiv11/%s/%d/ProfilePage", wip ? "Wip" : "Mod", id);
    J j;
    const bool ok = get_text(url, text, &err) && parse(text, j);
    std::vector<File> files;
    if (ok)
        for (const J& f : j["_aFiles"].a) {
            File fl;
            fl.id = (int)f["_idRow"].num();
            fl.name = f["_sFile"].str();
            fl.description = f["_sDescription"].str();
            fl.url = f["_sDownloadUrl"].str();
            fl.bytes = (uint64_t)f["_nFilesize"].num();
            fl.added = f["_tsDateAdded"].num();
            if (!fl.url.empty()) files.push_back(fl);
        }
    const std::string body = ok ? plain(j["_sText"].str()) : std::string();
    std::lock_guard<std::mutex> g(mu);
    bool any = false;
    each_with_id(id, wip, [&](Mod& m) {   // ((Triforce) the section's list and a search's: each entry for the mod still waiting)
        if (m.details_state == 2) return;
        any = true;
        if (!ok) { m.details_state = 3; return; }
        m.text = body;
        m.downloads = (int)j["_nDownloadCount"].num();
        m.files = files;
        m.file_choice = 0;
        if (m.kit)   // ((Triforce) 11-18) a kit's Windows file first (Squadala posts a -windows and a -linux zip)
            for (size_t k = 0; k < files.size(); k++)
                if (files[k].name.find("windows") != std::string::npos || files[k].name.find("Windows") != std::string::npos) { m.file_choice = (int)k; break; }
        m.details_state = 2;
    });
    if (!ok && any) logf("mods: details of %s %d: %s", wip ? "WiP" : "mod", id, err.c_str());
    if (ok && any) mark_installed();
}

void fetch_thumb(int id, bool wip, std::string url, std::string kept) {
    std::string bytes = std::move(kept), err;   // ((Triforce) kept: downloaded before; the menu let its picture go)
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    const bool had = !bytes.empty();
    const bool ok = (had || get(url, [&](const char* p, size_t n, uint64_t) { bytes.append(p, n); return bytes.size() < (8u << 20); }, &err)) &&
                    decode(bytes, 440, rgba, &w, &h);
    std::lock_guard<std::mutex> g(mu);
    // ((Triforce) each entry for the mod that asked (state 1) gets the pixels; the file is kept in every entry for it)
    each_with_id(id, wip, [&](Mod& m) {
        if (ok && !had && bytes.size() < (1u << 20)) m.thumb_file = bytes;   // (a picture under 1 MB kept)
        if (m.thumb_state != 1) return;
        if (ok) m.thumb_rgba = rgba, m.thumb_w = w, m.thumb_h = h, m.thumb_state = 2;
        else m.thumb_state = 3;
    });
}

// ---- installing ----
bool tar_extract(const std::string& archive, const std::string& into, std::string* err) {
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, sizeof sys);
    const std::string tar = std::string(sys) + "\\tar.exe";
    if (GetFileAttributesA(tar.c_str()) == INVALID_FILE_ATTRIBUTES) { *err = "Windows' tar.exe was not found (Windows 10 1803 or newer has it)"; return false; }
    std::wstring cmd = L"\"" + wide(tar) + L"\" -xf \"" + wide(archive) + L"\" -C \"" + wide(into) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        *err = "tar.exe could not start (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    for (;;) {
        if (WaitForSingleObject(pi.hProcess, 200) == WAIT_OBJECT_0) break;
        if (cancel_flag.load()) { TerminateProcess(pi.hProcess, 1); WaitForSingleObject(pi.hProcess, 5000); break; }
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (cancel_flag.load()) { *err = "cancelled"; return false; }
    if (code != 0) { *err = "the file could not be unpacked (tar.exe ended with " + std::to_string(code) + "): it may be damaged, or a kind tar does not read"; return false; }
    return true;
}

/// The folders holding romfs (or exefs) under dir, as host/mods.c finds mods (four levels down at most); a folder of the
/// game's RomFS folders with no romfs above them (some mods are packed so) counts as a romfs itself
void find_roots(const std::string& dir, int depth, std::vector<std::string>& out, std::vector<std::string>& bare) {
    if (is_dir(dir + "\\romfs") || is_dir(dir + "\\exefs")) { out.push_back(dir); return; }
    static const char* const romfs_dirs[] = {"Model", "UI", "Tex", "Pack", "Sound", "Event", "Banc", "Effect", "Mals", "Font2", "Logic",
                                             "Phive", "Scene", "System", "GameData", "Voice", "Movie", "AS", "AI", "Game", "Env"};
    int hits = 0;
    for (const std::string& d : subdirs(dir))
        for (const char* r : romfs_dirs) hits += _stricmp(d.c_str(), r) == 0;
    if (hits >= 1) { bare.push_back(dir); return; }   // (the game's own folders here: this folder is the RomFS's inside)
    if (depth >= 5) return;
    for (const std::string& d : subdirs(dir)) find_roots(dir + "\\" + d, depth + 1, out, bare);
}

int next_number(const std::string& root) {   // (after the highest "NN-" there: a new mod wins over those installed before)
    int hi = 0;
    for (const std::string& d : subdirs(root)) {
        const char* s = d.c_str();
        while (*s == '_' || *s == '-') s++;
        int n = 0, digits = 0;
        while (s[digits] >= '0' && s[digits] <= '9') n = n * 10 + (s[digits] - '0'), digits++;
        if (digits >= 2 && s[digits] == '-') hi = std::max(hi, n);
    }
    return hi + 1;
}

void set_state(Job j, const std::string& message = std::string()) {
    std::lock_guard<std::mutex> g(mu);
    inst.job = j;
    if (!message.empty()) inst.message = message;
}

/// ((Triforce) 11-18, from run_install) The file downloaded into stage_dir (made anew, hidden; its folder made) and unpacked
/// into stage_dir\unpacked, the progress in inst as it comes; false: the state set (Failed or Cancelled), stage_dir gone
bool download_unpack(const Mod& m, const File& f, const std::string& stage_dir) {
    const std::string root = stage_dir.substr(0, stage_dir.find_last_of('\\'));
    CreateDirectoryW(wide(root).c_str(), nullptr);
    remove_tree(stage_dir);
    CreateDirectoryW(wide(stage_dir).c_str(), nullptr);
    SetFileAttributesW(wide(stage_dir).c_str(), FILE_ATTRIBUTE_HIDDEN);
    std::string name = safe_name(f.name.empty() ? "mod.zip" : f.name);
    const std::string archive = stage_dir + "\\" + name;
    logf("mods: installing %s (%d), file %s (%llu bytes) from %s", m.name.c_str(), m.id, f.name.c_str(), (unsigned long long)f.bytes, f.url.c_str());
    // (download, the progress as it comes)
    // ((Triforce) 11-19, ticket "Mods not installing": "no answer from https://gamebanana.com/dl/... (error 12002)", WinHTTP's
    // timeout, twice for one player while GameBanana answered here in half a second. A download that got no answer or stopped is
    // tried again from the start, three times in all, 2 and 5 s apart; each try and the outcome go to the log)
    std::string err;
    bool got = false, written = true;
    for (int attempt = 1;; attempt++) {
    HANDLE out = CreateFileW(wide(archive).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) { set_state(Job::Failed, "Could not write in " + root + " (error " + std::to_string(GetLastError()) + ")."); return false; }
    {
        std::lock_guard<std::mutex> g(mu);
        inst.done = 0, inst.rate = 0;
    }
    err.clear();
    auto t0 = std::chrono::steady_clock::now();
    auto last = t0;
    uint64_t last_done = 0;
    written = true;
    got = get(f.url, [&](const char* p, size_t n, uint64_t length) {
        DWORD put = 0;
        if (!WriteFile(out, p, (DWORD)n, &put, nullptr) || put != n) { written = false; return false; }
        std::lock_guard<std::mutex> g(mu);
        inst.done += n;
        if (length) inst.total = length;
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last).count();
        if (dt >= 0.25) {
            const double r = (inst.done - last_done) / dt;
            inst.rate = inst.rate > 0 ? inst.rate * 0.7 + r * 0.3 : r;
            last = now, last_done = inst.done;
        }
        return !cancel_flag.load();
    }, &err);
    CloseHandle(out);
    if (got || !written || cancel_flag.load() || attempt >= 3 ||
        (err.rfind("no answer", 0) != 0 && err.rfind("the download stopped", 0) != 0 && err.rfind("could not reach", 0) != 0))
        break;
    logf("mods: %s: try %d of 3 failed (%s), trying again", m.name.c_str(), attempt, err.c_str());
    for (int w = 0; w < (attempt == 1 ? 20 : 50) && !cancel_flag.load(); w++) Sleep(100);
    }
    if (cancel_flag.load()) { remove_tree(stage_dir); logf("mods: %s: cancelled", m.name.c_str()); set_state(Job::Cancelled, "Cancelled: nothing was installed."); return false; }
    if (!got || !written) {
        logf("mods: %s: not installed: %s", m.name.c_str(), !written ? "the file could not be written" : err.c_str());
        remove_tree(stage_dir);
        set_state(Job::Failed, !written ? "The file could not be written (is the drive full?)." : "The download failed: " + err + ".");
        return false;
    }
    // (unpack)
    set_state(Job::Unpacking);
    const std::string unpacked = stage_dir + "\\unpacked";
    CreateDirectoryW(wide(unpacked).c_str(), nullptr);
    if (!tar_extract(archive, unpacked, &err)) {
        remove_tree(stage_dir);
        set_state(cancel_flag.load() ? Job::Cancelled : Job::Failed, cancel_flag.load() ? "Cancelled: nothing was installed." : "It could not be unpacked: " + err + ".");
        return false;
    }
    return true;
}

// ((Triforce) 11-18) ---- a kit: a program of its own (kKits), in kits\<its folder> beside the game ----
/// The folder under dir (dir itself, or 3 levels down at most) whose files hold the kit's start file
std::string find_kit_top(const std::string& dir, const char* start, int depth = 0) {
    for (const std::string& f : files_in(dir))
        if (_stricmp(f.c_str(), start) == 0) return dir;
    if (depth >= 3) return std::string();
    for (const std::string& d : subdirs(dir)) {
        const std::string t = find_kit_top(dir + "\\" + d, start, depth + 1);
        if (!t.empty()) return t;
    }
    return std::string();
}
/// from's files moved over into's (folders made, files there replaced; what into has more stays: the kit's own settings and
/// the game it built); false with the first file that could not be put (in use: the kit is running)
bool move_over(const std::string& from, const std::string& into, std::string* err) {
    CreateDirectoryW(wide(into).c_str(), nullptr);
    for (const std::string& f : files_in(from)) {
        const std::wstring a = wide(from + "\\" + f), b = wide(into + "\\" + f);
        SetFileAttributesW(b.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!MoveFileExW(a.c_str(), b.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            *err = into + "\\" + f + " (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
    }
    for (const std::string& d : subdirs(from))
        if (!move_over(from + "\\" + d, into + "\\" + d, err)) return false;
    return true;
}
/// The kit's start file run in a console of its own, from its folder (cmd /c: a .bat, or any start file Windows runs)
bool start_kit(const std::string& dir, const Kit& k, std::string* err) {
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, sizeof sys);
    std::wstring cmd = L"\"" + wide(std::string(sys) + "\\cmd.exe") + L"\" /c \"\"" + wide(dir + "\\" + k.start) + L"\"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, wide(dir).c_str(), &si, &pi)) {
        *err = "error " + std::to_string(GetLastError());
        return false;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

void run_kit(const Mod& m, const File& f, const Kit& k) {
    const std::string kits = game_folder() + "\\kits", dir = kits + "\\" + k.folder, stage_dir = kits + "\\.triforce-download";
    std::string err;
    // (this file installed already, and its start file there: started again, nothing downloaded)
    bool have = false;
    {
        J j;
        const std::string text = read_file(dir + "\\triforce_kit.json");
        have = !text.empty() && parse(text, j) && (int)j["gamebanana_id"].num() == k.id && (int64_t)j["file_date"].num() >= f.added &&
               GetFileAttributesW(wide(dir + "\\" + k.start).c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    if (!have) {
        logf("mods: %s is a kit: into %s", m.name.c_str(), dir.c_str());
        if (!download_unpack(m, f, stage_dir)) return;
        const std::string top = find_kit_top(stage_dir + "\\unpacked", k.start);
        if (top.empty()) {
            remove_tree(stage_dir);
            set_state(Job::Failed, std::string("This download has no \"") + k.start + "\" in it: GameBanana's file may have changed. Nothing was installed.");
            return;
        }
        if (!move_over(top, dir, &err)) {
            remove_tree(stage_dir);
            set_state(Job::Failed, "It could not be put into kits\\" + std::string(k.folder) + ": " + err + ". If " + m.name +
                                       " is running, close it and try again.");
            return;
        }
        remove_tree(stage_dir);
        write_file(dir + "\\triforce_kit.json",
                   "{\n  \"name\": \"" + json_text(m.name) + "\",\n  \"author\": \"" + json_text(m.author) + "\",\n  \"page\": \"" + json_text(m.page) +
                       "\",\n  \"gamebanana_id\": " + std::to_string(m.id) + ",\n  \"gamebanana_model\": \"" + (m.wip ? "Wip" : "Mod") +
                       "\",\n  \"file\": \"" + json_text(f.name) + "\",\n  \"file_date\": " + std::to_string((long long)f.added) +
                       ",\n  \"installed_by\": \"Triforce's Mods menu\"\n}\n");
        std::lock_guard<std::mutex> g(mu);
        mark_installed();
    }
    if (!start_kit(dir, k, &err)) {
        set_state(Job::Failed, std::string(k.start) + " could not be started (" + err + "). It is in kits\\" + k.folder +
                                   " beside the game: start it from there.");
        return;
    }
    logf("mods: kit %s %s, started: %s\\%s", m.name.c_str(), have ? "installed already" : "installed", dir.c_str(), k.start);
    {
        std::lock_guard<std::mutex> g(mu);
        inst.kit = true;
    }
    set_state(Job::Done, std::string(have ? "Started" : "Installed into kits\\" + std::string(k.folder) + " and started") +
                             " in a window of its own. Follow its steps there.");
}

void run_install(Mod m, File f) {
    if (const Kit* k = kit_for(m.wip, m.id)) { run_kit(m, f, *k); return; }   // ((Triforce) 11-18) a program of its own: into kits, not mods
    const std::string game = game_folder(), root = game + "\\mods", stage_dir = root + "\\.triforce-download";
    if (!download_unpack(m, f, stage_dir)) return;
    std::string err;
    const std::string unpacked = stage_dir + "\\unpacked";
    std::vector<std::string> roots, bare;
    find_roots(unpacked, 0, roots, bare);
    if (roots.empty() && !bare.empty()) {   // (packed without a romfs folder: one made around the game's folders)
        const std::string made = stage_dir + "\\wrapped";
        CreateDirectoryW(wide(made).c_str(), nullptr);
        if (MoveFileW(wide(bare[0]).c_str(), wide(made + "\\romfs").c_str())) roots.push_back(made);
    }
    if (roots.empty()) {
        remove_tree(stage_dir);
        set_state(Job::Failed, "This download has no romfs folder in it, so it is not a mod Triforce can load (it may need its own "
                               "installer, or be for another version of the game).");
        return;
    }
    // (a reinstall or an update: the folders this mod had go, and the new one takes the first one's place in the order and
    // its on or off)
    int number = 0;
    bool was_off = false;
    std::vector<std::string> old;
    for (const std::string& d : subdirs(root)) {
        J j;
        const std::string text = read_file(root + "\\" + d + "\\triforce_mod.json");
        if (text.empty() || !parse(text, j) || j["gamebanana_id"].num() != m.id) continue;
        if (old.empty()) {
            const char* s = d.c_str();
            was_off = *s == '_' || *s == '-';
            while (*s == '_' || *s == '-') s++;
            int n = 0, digits = 0;
            while (s[digits] >= '0' && s[digits] <= '9') n = n * 10 + (s[digits] - '0'), digits++;
            if (digits >= 2 && s[digits] == '-') number = n;
        }
        old.push_back(root + "\\" + d);
    }
    std::vector<std::pair<std::string, std::string>> aside;   // (where each old folder was, where it waits)
    for (size_t k = 0; k < old.size(); k++) {   // (out of the way first: removed once the new one is in, put back if it fails)
        const std::string gone = stage_dir + "\\old-" + std::to_string(k);
        if (MoveFileW(wide(old[k]).c_str(), wide(gone).c_str())) aside.emplace_back(old[k], gone);
        else logf("mods: the old %s could not be moved aside (error %lu)", old[k].c_str(), GetLastError());
    }
    if (!old.empty()) logf("mods: %s was installed before (%zu folder(s)): replaced, its place %d kept", m.name.c_str(), old.size(), number);
    // (each folder that holds romfs: one mod in the mods folder, numbered after the ones there; more than one = the mod's
    // options: the first on, the others off ('_' in front), to be chosen in Setup's Mods page)
    const std::string base = safe_name(m.name);
    if (!number) number = next_number(root);
    std::vector<std::string> placed;
    for (size_t i = 0; i < roots.size(); i++) {
        std::string label = base;
        if (roots.size() > 1) {
            std::string leaf = roots[i].substr(roots[i].find_last_of('\\') + 1);
            if (leaf != "unpacked") label += " - " + safe_name(leaf);
        }
        char prefix[16];
        snprintf(prefix, sizeof prefix, "%s%02d-", i || was_off ? "_" : "", number++);
        std::string target = root + "\\" + prefix + label;
        for (int k = 2; is_dir(target); k++) target = root + "\\" + prefix + label + " (" + std::to_string(k) + ")";
        if (!MoveFileW(wide(roots[i]).c_str(), wide(target).c_str())) {
            err = "could not move it into " + target + " (error " + std::to_string(GetLastError()) + ")";
            break;
        }
        int64_t date = f.added;
        const std::string info = "{\n  \"name\": \"" + json_text(roots.size() > 1 ? label : m.name) + "\",\n  \"author\": \"" + json_text(m.author) +
                                 "\",\n  \"page\": \"" + json_text(m.page) + "\",\n  \"gamebanana_id\": " + std::to_string(m.id) +
                                 (m.wip ? ",\n  \"gamebanana_model\": \"Wip\"" : "") +   // ((Triforce) 11-18: a WiP's id is its own)
                                 ",\n  \"file\": \"" + json_text(f.name) + "\",\n  \"file_date\": " + std::to_string((long long)date) +
                                 ",\n  \"installed_by\": \"Triforce's Mods menu\"\n}\n";
        write_file(target + "\\triforce_mod.json", info);
        placed.push_back(target);
    }
    if (placed.empty())   // (nothing new in: the old ones back where they were)
        for (const auto& [was, gone] : aside) MoveFileW(wide(gone).c_str(), wide(was).c_str());
    remove_tree(stage_dir);
    if (placed.empty()) { set_state(Job::Failed, "It could not be installed: " + err + "." + (aside.empty() ? "" : " The one installed before is kept.")); return; }
    {
        std::lock_guard<std::mutex> g(mu);
        mark_installed();
    }
    std::string where = placed[0].substr(placed[0].find_last_of('\\') + 1);
    std::string msg = "Installed into mods\\" + where + ".";
    if (placed.size() > 1) msg += " It came with " + std::to_string(placed.size()) + " options: the first is on, the others are off. Choose in Triforce Setup's Mods page.";
    logf("mods: installed %s into %zu folder(s), the first %s", m.name.c_str(), placed.size(), placed[0].c_str());
    set_state(Job::Done, msg);
}
#endif   // _WIN32

std::string ini_path() {
#ifdef _WIN32
    return game_folder() + "\\triforce_mods.ini";
#else
    return "triforce_mods.ini";
#endif
}

}  // namespace

bool available() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

void lock() { mu.lock(); }
void unlock() { mu.unlock(); }
std::vector<Mod>& list() { return active->mods; }

Feed feed_state(std::string* error) {
    std::lock_guard<std::mutex> g(mu);
    if (error) *error = active->error;
    return active->feed;
}
bool more() {
    std::lock_guard<std::mutex> g(mu);
    return !active->complete;
}
int total() {
    std::lock_guard<std::mutex> g(mu);
    return active->total;
}

std::vector<void*> open() {
    std::vector<void*> textures;
#ifdef _WIN32
    {
        std::lock_guard<std::mutex> g(mu);
        // ((Triforce) the page opened again: the section's list, a search left behind ended)
        if (active == &found) textures = clear_list(found), found.gen++, found.query.clear(), found.feed = Feed::Idle, active = &browse;
        if (browse.feed == Feed::Loading) return textures;
        if (browse.feed == Feed::Ready && !browse.mods.empty()) { mark_installed(); return textures; }
        // ((Triforce) 11-18) stopped on a failed page with cards listed: on from that page, the cards kept
        if (browse.feed == Feed::Failed && !browse.mods.empty() && !browse.queued) {
            browse.feed = Feed::Ready, browse.error.clear(), browse.retries = 0, browse.queued = true;
            const unsigned gen = browse.gen;
            const int page = browse.pages_loaded + 1;
            work().push([gen, page] { fetch_page(&browse, gen, page); });
            mark_installed();
            return textures;
        }
        browse.feed = Feed::Loading;
        // ((Triforce) the list emptied after a failure: its pictures' textures handed back, not left behind)
        for (void* t : clear_list(browse)) textures.push_back(t);
    }
    const unsigned gen = ++browse.gen;
    work().push([gen] { fetch_page(&browse, gen, 1); });
#endif
    return textures;
}

void load_more() {
#ifdef _WIN32
    int page;
    FeedData* fd;
    unsigned gen;
    {
        std::lock_guard<std::mutex> g(mu);
        fd = active;
        // ((Triforce) 11-18) the pages come by themselves (fetch_page): this one only for a list that stopped on a failed page
        if (fd->complete || fd->queued || fd->feed == Feed::Loading || fd->mods.empty()) return;
        fd->feed = Feed::Ready, fd->error.clear(), fd->retries = 0, fd->queued = true;
        page = fd->pages_loaded + 1, gen = fd->gen;
    }
    work().push([fd, gen, page] { fetch_page(fd, gen, page); });
#endif
}

std::vector<void*> search(const std::string& text) {
    // (trimmed; GameBanana's search wants 2 to 100 characters: fewer is no search, more is cut)
    size_t a = text.find_first_not_of(" \t"), b = text.find_last_not_of(" \t");
    std::string q = a == std::string::npos ? std::string() : text.substr(a, b - a + 1);
    if (q.size() > 100) {
        q.resize(100);
        while (!q.empty() && ((unsigned char)q.back() & 0xC0) == 0x80) q.pop_back();   // (not half a character)
        if (!q.empty() && (unsigned char)q.back() >= 0xC0) q.pop_back();
    }
    if (q.size() < 2) return search_clear();
    std::vector<void*> textures;
    unsigned gen;
    {
        std::lock_guard<std::mutex> g(mu);
        if (active == &found && found.query == q && found.feed != Feed::Failed) return textures;   // (the same search)
        textures = clear_list(found);
        found.query = q, found.name_filter = false, found.feed = Feed::Loading;
        gen = ++found.gen;
        active = &found;
    }
#ifdef _WIN32
    logf("mods: search \"%s\"", q.c_str());
    work().push([gen] { fetch_page(&found, gen, 1); }, true);   // (before the section's next page)
#else
    (void)gen;
#endif
    return textures;
}
std::vector<void*> search_clear() {
    std::lock_guard<std::mutex> g(mu);
    if (active != &found && found.mods.empty()) return {};
    std::vector<void*> textures = clear_list(found);
    found.gen++, found.query.clear(), found.feed = Feed::Idle;
    active = &browse;
    return textures;
}
std::string search_text() {
    std::lock_guard<std::mutex> g(mu);
    return active == &found ? found.query : std::string();
}
bool search_by_name() {
    std::lock_guard<std::mutex> g(mu);
    return active == &found && found.name_filter;
}

void want_details(int i) {
#ifdef _WIN32
    int id;
    bool wip;
    {
        std::lock_guard<std::mutex> g(mu);
        std::vector<Mod>& l = active->mods;
        if (i < 0 || i >= (int)l.size() || l[i].details_state) return;
        id = l[i].id, wip = l[i].wip;
        each_with_id(id, wip, [](Mod& m) { if (!m.details_state) m.details_state = 1; });
    }
    work().push([id, wip] { fetch_details(id, wip); }, true);   // (the one looked at: before more pages)
#else
    (void)i;
#endif
}

void want_thumb(int i) {
#ifdef _WIN32
    int id;
    bool wip;
    std::string url, kept;
    {
        std::lock_guard<std::mutex> g(mu);
        std::vector<Mod>& l = active->mods;
        if (i < 0 || i >= (int)l.size() || l[i].thumb_state) return;
        if (l[i].thumb_url.empty()) { l[i].thumb_state = 3; return; }
        l[i].thumb_state = 1;
        id = l[i].id, wip = l[i].wip, url = l[i].thumb_url, kept = l[i].thumb_file;
    }
    pictures().push([id, wip, url, kept] { fetch_thumb(id, wip, url, kept); });
#else
    (void)i;
#endif
}

void install(int i) {
#ifdef _WIN32
    Mod m;
    File f;
    {
        std::lock_guard<std::mutex> g(mu);
        std::vector<Mod>& l = active->mods;
        if (inst.job == Job::Downloading || inst.job == Job::Unpacking) return;
        if (i < 0 || i >= (int)l.size() || l[i].files.empty()) return;
        m = l[i];
        m.thumb_rgba.clear();
        f = m.files[std::clamp(m.file_choice, 0, (int)m.files.size() - 1)];
        inst = InstallState{};
        inst.job = Job::Downloading, inst.mod_id = m.id, inst.mod_name = m.name, inst.file_name = f.name, inst.total = f.bytes;
    }
    cancel_flag = false;
    work().push([m, f] { run_install(m, f); }, true);
#else
    (void)i;
#endif
}

void cancel() { cancel_flag = true; }

InstallState install_state() {
    std::lock_guard<std::mutex> g(mu);
    return inst;
}
void clear_done() {
    std::lock_guard<std::mutex> g(mu);
    if (inst.job == Job::Done || inst.job == Job::Failed || inst.job == Job::Cancelled) inst = InstallState{};
}

std::atomic<int> accepted_known{-1};   // (read once: the menu asks every frame)
bool accepted() {
#ifdef _WIN32
    if (accepted_known.load() < 0) accepted_known = read_file(ini_path()).find("accepted=1") != std::string::npos ? 1 : 0;
    return accepted_known.load() == 1;
#else
    return true;
#endif
}
void accept() {
#ifdef _WIN32
    write_file(ini_path(), "; Triforce's Mods menu: its warning was accepted (delete this file to see it again)\naccepted=1\n");
    accepted_known = 1;
#endif
}

std::string mods_folder() {
#ifdef _WIN32
    return game_folder() + "\\mods";
#else
    return "mods";
#endif
}

std::string game_folder_path() {
#ifdef _WIN32
    return game_folder();
#else
    return ".";
#endif
}

bool decode_image(const std::string& bytes, int max_w, std::vector<unsigned char>& rgba, int* w, int* h) {
#ifdef _WIN32
    return decode(bytes, max_w, rgba, w, h);
#else
    (void)bytes, (void)max_w, (void)rgba, (void)w, (void)h;
    return false;
#endif
}

}  // namespace mkwvk::mods
