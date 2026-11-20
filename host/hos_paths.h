/* The game folder's path, for the files the game keeps there (image.bin, romfs, saves, settings, caches, logs).
   On Windows the game is bin\Triforce.exe since the launcher came: Triforce.exe in the folder is the launcher (the builder's
   setup in launch mode), the game and its DLLs are in bin\, the game's files stay in the folder. hos_game_path_a/_w answer as
   GetModuleFileName(NULL, ...) does, but with the folder's path when the exe is in a "bin" folder beside image.bin, so code that
   strips the exe's name and adds a file's finds the folder's file. Modules (DLLs) are still loaded from beside the exe itself. */
#pragma once
#include <string.h>
#include <wchar.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdlib.h>

/* (Triforce) Long paths. The probe for image.bin below is the one Win32 path these helpers make. A path of MAX_PATH and more
   goes to GetFileAttributesW with "\\?\" in front ("\\?\UNC\" for \\server\share), which takes it whatever Windows' own
   LongPathsEnabled says; GetFileAttributesA does not past MAX_PATH (only when the manifest's longPathAware, nx2/win_manifest.py,
   meets LongPathsEnabled). The probe was a MAX_PATH buffer: a folder of 250 characters and more, whose bin\Triforce.exe a
   caller with a larger buffer had (discord_rpc.c's 1024), was taken for its bin folder. The path given back has no "\\?\": its
   callers add names with '/', which "\\?\" would keep as they are, and show it in messages and the log. path: a whole wide path,
   n its length. 1: there is a file or folder of that name */
static inline int hos_path_there_w(const wchar_t *path, size_t n) {
    const int already = path[0] == L'\\' && path[1] == L'\\' && (path[2] == L'?' || path[2] == L'.');
    const int unc = !already && path[0] == L'\\' && path[1] == L'\\';
    const int drive = path[0] && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
    if (n < MAX_PATH - 1 || already || !(unc || drive)) return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    wchar_t *w = (wchar_t *)malloc((n + 9) * sizeof *w);
    if (!w) return 0;
    wcscpy(w, unc ? L"\\\\?\\UNC" : L"\\\\?\\");
    size_t at = wcslen(w);
    for (size_t i = unc ? 1 : 0; i < n; i++) w[at++] = path[i] == L'/' ? L'\\' : path[i];
    w[at] = 0;
    const int there = GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
    free(w);
    return there;
}

/* (the A helper's probe: a short one as it always was, through the A function; a long one as UTF-8, the process's code page
   with the manifest's activeCodePage, made wide) */
static inline int hos_path_there_a(const char *path) {
    const size_t n = strlen(path);
    if (n < MAX_PATH - 1) return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    const int wn = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wn <= 0) return 0;
    wchar_t *w = (wchar_t *)malloc((size_t)wn * sizeof *w);
    if (!w) return 0;
    const int there = MultiByteToWideChar(CP_UTF8, 0, path, -1, w, wn) > 0 && hos_path_there_w(w, wcslen(w));
    free(w);
    return there;
}

static inline DWORD hos_game_path_a(char *buf, DWORD size) {
    DWORD n = GetModuleFileNameA(NULL, buf, size);
    if (!n || n >= size) return n;
    char *name = n ? buf + n : buf;
    while (name > buf && name[-1] != '\\' && name[-1] != '/') name--;
    if (name - buf < 5) return n;
    char *dir = name - 1;   /* (the separator before the exe's name) */
    char *bin = dir;
    while (bin > buf && bin[-1] != '\\' && bin[-1] != '/') bin--;
    if (dir - bin != 3 || _strnicmp(bin, "bin", 3) != 0) return n;
    const size_t root = (size_t)(bin - buf);   /* (the folder, with its separator) */
    char *probe = (char *)malloc(root + 10);   /* ((Triforce) as long as the folder's path is: hos_path_there_w) */
    if (!probe) return n;
    memcpy(probe, buf, root);
    strcpy(probe + root, "image.bin");
    const int there = hos_path_there_a(probe);
    free(probe);
    if (!there) return n;
    memmove(buf + root, name, strlen(name) + 1);   /* (folder\Triforce.exe) */
    return (DWORD)strlen(buf);
}

static inline DWORD hos_game_path_w(wchar_t *buf, DWORD size) {
    DWORD n = GetModuleFileNameW(NULL, buf, size);
    if (!n || n >= size) return n;
    wchar_t *name = buf + n;
    while (name > buf && name[-1] != L'\\' && name[-1] != L'/') name--;
    if (name - buf < 5) return n;
    wchar_t *dir = name - 1;
    wchar_t *bin = dir;
    while (bin > buf && bin[-1] != L'\\' && bin[-1] != L'/') bin--;
    if (dir - bin != 3 || _wcsnicmp(bin, L"bin", 3) != 0) return n;
    const size_t root = (size_t)(bin - buf);
    wchar_t *probe = (wchar_t *)malloc((root + 10) * sizeof *probe);   /* ((Triforce) hos_path_there_w) */
    if (!probe) return n;
    wmemcpy(probe, buf, root);
    wcscpy(probe + root, L"image.bin");
    const int there = hos_path_there_w(probe, root + 9);
    free(probe);
    if (!there) return n;
    wmemmove(buf + root, name, wcslen(name) + 1);
    return (DWORD)wcslen(buf);
}
#else
#include <unistd.h>
#include <windows.h>   /* (linux: host/posix's, for DWORD and GetModuleFileNameA) */
/* (Linux) the same bin/ layout when the folder was built by the packaged builder: hookshot in the folder is the
   launcher script, and the game is bin/hookshot. A development build has no bin/ and this is GetModuleFileName. */
static inline DWORD hos_game_path_a(char *buf, DWORD size) {
    DWORD n = GetModuleFileNameA(NULL, buf, size);
    if (!n || n >= size) return n;
    char *name = buf + n;
    while (name > buf && name[-1] != '/') name--;
    if (name - buf < 5) return n;
    char *dir = name - 1;
    char *bin = dir;
    while (bin > buf && bin[-1] != '/') bin--;
    if (dir - bin != 3 || memcmp(bin, "bin", 3) != 0) return n;
    char probe[MAX_PATH + 16];
    const size_t root = (size_t)(bin - buf);
    if (root + 10 >= sizeof probe) return n;
    memcpy(probe, buf, root);
    memcpy(probe + root, "image.bin", 10);
    if (access(probe, F_OK) != 0) return n;
    memmove(buf + root, name, strlen(name) + 1);
    return (DWORD)strlen(buf);
}
#endif
