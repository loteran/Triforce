// The game folder's path for the files the game keeps there (caches, the shader list, settings): host/hos_paths.h's rule, for
// this DLL. The game is bin\Triforce.exe beside its launcher since the launcher came; its files stay in the folder above bin.
// game_module_path answers as GetModuleFileNameA(nullptr, ...) does, with the folder's path when the exe is in a "bin" folder
// beside image.bin.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef min
#undef max
#include <cstring>
#include <string>

namespace mkwvk {
/// (Triforce) Long paths: the probe for image.bin is the one Win32 path game_module_path makes. Under MAX_PATH it is the A
/// function as before; from MAX_PATH on, the path (UTF-8: the exe's manifest makes that the process's code page) goes to
/// GetFileAttributesW with "\\?\" in front ("\\?\UNC\" for \\server\share), which takes it whatever Windows' own
/// LongPathsEnabled says (the A function does not, unless the manifest's longPathAware meets it). The probe was a MAX_PATH
/// buffer: a folder of 250 characters and more was taken for its bin folder. The path given back has no "\\?\" (as
/// host/hos_paths.h): its callers add names with '/' and show it
inline bool game_path_there(const std::string& path) {
#ifdef _WIN32
    if (path.size() < MAX_PATH - 1)
#endif
        return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;   // (Linux: host/posix's, any length)
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), nullptr, 0);
    if (n <= 0) return false;
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), w.data(), n);
    const bool already = w.size() > 2 && w[0] == L'\\' && w[1] == L'\\' && (w[2] == L'?' || w[2] == L'.');
    const bool unc = !already && w.size() > 2 && w[0] == L'\\' && w[1] == L'\\';
    const bool drive = w.size() > 2 && w[1] == L':' && (w[2] == L'\\' || w[2] == L'/');
    if (!already && (unc || drive)) {
        for (wchar_t& c : w)
            if (c == L'/') c = L'\\';
        w = unc ? L"\\\\?\\UNC" + w.substr(1) : L"\\\\?\\" + w;
    }
    return GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES;
#endif
}

inline DWORD game_module_path(char* buf, DWORD size) {
    DWORD n = GetModuleFileNameA(nullptr, buf, size);
    if (!n || n >= size) return n;
    char* name = buf + n;
    while (name > buf && name[-1] != '\\' && name[-1] != '/') name--;
    if (name - buf < 5) return n;
    char* dir = name - 1;
    char* bin = dir;
    while (bin > buf && bin[-1] != '\\' && bin[-1] != '/') bin--;
    if (dir - bin != 3 || _strnicmp(bin, "bin", 3) != 0) return n;
    const size_t root = (size_t)(bin - buf);
    if (!game_path_there(std::string(buf, root) + "image.bin")) return n;   // ((Triforce) any length: game_path_there)
    std::memmove(buf + root, name, std::strlen(name) + 1);
    return (DWORD)std::strlen(buf);
}
}  // namespace mkwvk
