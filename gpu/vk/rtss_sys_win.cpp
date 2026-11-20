// (Triforce) The real interface of rtss_limit.h's Sys: Windows, and RivaTuner Statistics Server as it is installed.
// Every call that reaches RTSS, its files or the registry is in this file and nowhere else; rtss_limit.cpp decides
// when, and the tests run rtss_limit.cpp against a stand-in instead, so nothing here is ever run by a test.
//
// What it does to RTSS, all of it: reads its install folder from the registry key its installer wrote; reads its
// shared memory; reads its profile files; loads its hooks library (RTSSHooks64.dll) and calls six of its exports:
// LoadProfile, SaveProfile, DeleteProfile, GetProfileProperty, SetProfileProperty and UpdateProfiles. It writes no
// file of RTSS's itself, and never starts or stops RTSS.
#include "rtss_limit.h"

#ifdef _WIN32

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "vk_internal.h"
#include "vk_pace.h"
#include "vk_paths.h"

namespace mkwvk::rtss {

namespace {

typedef void (*ProfileFn)(LPCSTR name);
typedef BOOL (*PropertyFn)(LPCSTR property, LPBYTE data, DWORD size);
typedef void (*UpdateFn)(void);

HMODULE dll;
ProfileFn fn_load, fn_save, fn_delete;
PropertyFn fn_get, fn_set;
UpdateFn fn_update;

uint64_t now_ns() { return (uint64_t)host->now_ns(); }
uint64_t hosted_ns() { return pace::presented_ns(); }
void log_line(const char* line) { logf("%s", line); }

bool exe_name(char* out, size_t n) {
    char path[MAX_PATH];
    const DWORD len = GetModuleFileNameA(nullptr, path, sizeof path);
    if (!len || len >= sizeof path) return false;
    const char* base = path;
    for (const char* p = path; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    return snprintf(out, n, "%s", base) < (int)n && *out;
}

bool state_path(char* out, size_t n) {   // (the game folder, where the options file is: vk_paths.h)
    char exe[MAX_PATH];
    const DWORD len = game_module_path(exe, MAX_PATH);
    if (!len || len >= MAX_PATH) return false;
    std::string dir(exe, len);
    dir = dir.substr(0, dir.find_last_of("\\/") + 1) + "triforce_rtss_profile.txt";
    return snprintf(out, n, "%s", dir.c_str()) < (int)n;
}

char* file_read(const char* path, size_t* n) {
    const HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER size;
    char* text = nullptr;
    if (GetFileSizeEx(f, &size) && size.QuadPart >= 0 && size.QuadPart < (1 << 20) && (text = (char*)malloc((size_t)size.QuadPart + 1))) {
        DWORD got = 0;
        if (ReadFile(f, text, (DWORD)size.QuadPart, &got, nullptr) && got == (DWORD)size.QuadPart) text[got] = 0, *n = got;
        else free(text), text = nullptr;
    }
    CloseHandle(f);
    return text;
}

int64_t file_size(const char* path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return -1;
    return (int64_t)((uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow);
}

/// The game's own state file, replaced in one step: written beside it, then moved over it
bool file_write(const char* path, const void* data, size_t n) {
    const std::string tmp = std::string(path) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool wrote = fwrite(data, 1, n, f) == n;
    if (fclose(f) || !wrote) return remove(tmp.c_str()), false;
    if (MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING)) return true;
    remove(tmp.c_str());
    return false;
}

void file_delete(const char* path) { remove(path); }

bool install_dir(char* out, size_t n) {
    DWORD size = (DWORD)n;
    return RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Unwinder\\RTSS", "InstallDir", RRF_RT_REG_SZ, nullptr, out, &size) ==
               ERROR_SUCCESS && *out;
}

uint64_t file_stamp(const char* path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return 0;
    return ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime) | 1;
}

/// RTSS's shared memory, opened for reading and closed again at each look
void shared(RtssApp* a) {
    memset(a, 0, sizeof *a);
    const HANDLE m = OpenFileMappingA(FILE_MAP_READ, FALSE, "RTSSSharedMemoryV2");
    if (!m) return;
    const uint8_t* p = (const uint8_t*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    MEMORY_BASIC_INFORMATION mi;
    if (p && VirtualQuery(p, &mi, sizeof mi)) rtss_parse(p, mi.RegionSize, GetCurrentProcessId(), a);
    if (p) UnmapViewOfFile(p);
    CloseHandle(m);
}

bool hooks_load(const char* dll_path) {
    if (dll) return true;
    dll = LoadLibraryA(dll_path);
    if (!dll) return false;
    fn_load = (ProfileFn)(void*)GetProcAddress(dll, "LoadProfile");
    fn_save = (ProfileFn)(void*)GetProcAddress(dll, "SaveProfile");
    fn_delete = (ProfileFn)(void*)GetProcAddress(dll, "DeleteProfile");
    fn_get = (PropertyFn)(void*)GetProcAddress(dll, "GetProfileProperty");
    fn_set = (PropertyFn)(void*)GetProcAddress(dll, "SetProfileProperty");
    fn_update = (UpdateFn)(void*)GetProcAddress(dll, "UpdateProfiles");
    if (fn_load && fn_save && fn_delete && fn_get && fn_set && fn_update) return true;
    FreeLibrary(dll);
    dll = nullptr;
    return false;
}

// (a name is never empty here: to RTSS an empty name is its global profile, which the game never loads, saves or removes)
void load_profile(const char* name) {
    if (name && *name) fn_load(name);
}
void save_profile(const char* name) {
    if (name && *name) fn_save(name);
}
void delete_profile(const char* name) {
    if (name && *name) fn_delete(name);
}
bool get_property(const char* property, uint32_t* value) {
    DWORD v = *value;
    const bool ok = fn_get(property, (LPBYTE)&v, sizeof v) != 0;
    if (ok) *value = v;
    return ok;
}
bool set_property(const char* property, uint32_t value) {
    DWORD v = value;
    return fn_set(property, (LPBYTE)&v, sizeof v) != 0;
}
void update_profiles() { fn_update(); }

const Sys real = {now_ns,      hosted_ns, log_line,   exe_name,     state_path,     file_read,    file_size,    file_write,     file_delete,
                  install_dir, file_stamp, shared,    hooks_load,   load_profile,   save_profile, delete_profile, get_property, set_property,
                  update_profiles};

} // namespace

const Sys* real_sys() { return &real; }

} // namespace mkwvk::rtss

#else

namespace mkwvk::rtss {
const Sys* real_sys() { return nullptr; }   // (RivaTuner Statistics Server is a Windows program)
} // namespace mkwvk::rtss

#endif
