/* (Triforce) One game at a time for a game folder (handoff §5). Two copies of the game started on one folder (a second click on
   the icon while the first was still loading, the launcher and a shortcut) wrote the same saves, triforce_options.ini, logs and
   shader caches over each other, and the second's log rotation took the first's log away. The first holds a named mutex for as
   long as it runs (Windows lets go of it when the process ends, however it ends); a second one for the same folder says which
   process holds it, in a message box, and ends. The name comes from the folder's path, so the lab's copy and the owner's game,
   other folders, never meet, and a builder (Triforce Setup's own lock is named after Setup's folder) never meets the game.
   Its process id and executable are in a small named shared section beside the mutex, for the second one's message.
   HOS_SINGLE_INSTANCE=0: no lock (as before). Plain logic, with no part of the game: tests/test_game_instance.py runs it */
#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

typedef struct { DWORD pid; wchar_t exe[1024]; } HosInstanceWho;

/* The lock's name for a folder: one for each spelling of it (case, '/' or '\', a separator at its end, "\\?\" in front), a
   64-bit FNV-1a of that spelling. Local\: this user's session (a game folder is one user's). (Triforce: "\\?\UNC\server\share"
   is \\server\share, and a drive alone, "C:", is its root "C:\". With only "\\?\" taken off the first was "UNC\server\share"
   and the second the working directory on that drive: GetFullPathNameW put both under the working directory, and the name
   changed with the directory a copy was started from) */
static inline void hos_instance_name(const wchar_t *folder, wchar_t *out, size_t cap) {
    wchar_t full[1100], spelled[1100];
    const wchar_t *p = folder;
    if (p[0] == L'\\' && p[1] == L'\\' && p[2] == L'?' && p[3] == L'\\') {
        p += 4;
        if (_wcsnicmp(p, L"UNC\\", 4) == 0) {
            _snwprintf(spelled, 1100, L"\\\\%ls", p + 4);
            spelled[1099] = 0;
            p = spelled;
        }
    }
    if ((p[0] | 32) >= L'a' && (p[0] | 32) <= L'z' && p[1] == L':' && !p[2]) {
        _snwprintf(spelled, 1100, L"%lc:\\", p[0]);
        spelled[1099] = 0;
        p = spelled;
    }
    DWORD n = GetFullPathNameW(p, 1100, full, NULL);
    if (!n || n >= 1100) {
        wcsncpy(full, p, 1099);
        full[1099] = 0;
        n = (DWORD)wcslen(full);
    }
    for (DWORD i = 0; i < n; i++)
        if (full[i] == L'/') full[i] = L'\\';
    while (n > 3 && full[n - 1] == L'\\') full[--n] = 0;   /* (C:\ keeps its own) */
    CharUpperBuffW(full, n);
    uint64_t h = 14695981039346656037ull;
    for (DWORD i = 0; i < n; i++) h = (h ^ full[i]) * 1099511628211ull;
    _snwprintf(out, cap, L"Local\\Triforce-Game-%016llx", (unsigned long long)h);
    out[cap - 1] = 0;
}

/* The game folder of the game's own path (hos_game_path_w's), in place: the path up to its last separator. (Triforce: a folder
   at the root of a drive keeps its separator, "C:\" and not "C:", which is the working directory on drive C) */
static inline void hos_instance_folder_of(wchar_t *path) {
    size_t n = wcslen(path);
    while (n && path[n - 1] != L'\\' && path[n - 1] != L'/') n--;
    if (n >= 2 && path[n - 2] == L':') path[n] = 0;
    else path[n ? n - 1 : 0] = 0;
}

/* 1: this process holds the folder now (it waited up to wait_ms for one that was ending: a restart, a close just before);
   0: another process does, *who what it said of itself (pid 0: it said nothing yet). Where no lock can be made at all nobody
   is refused. The handles are kept until the process ends */
static inline int hos_instance_take(const wchar_t *folder, DWORD wait_ms, HosInstanceWho *who) {
    wchar_t name[96], shared[112];
    hos_instance_name(folder, name, 96);
    _snwprintf(shared, 112, L"%ls-who", name);
    shared[111] = 0;
    memset(who, 0, sizeof *who);
    HANDLE m = CreateMutexW(NULL, FALSE, name);
    if (!m) return GetLastError() != ERROR_ACCESS_DENIED;   /* (denied: it is there, held by a copy run as administrator) */
    const DWORD got = WaitForSingleObject(m, wait_ms);
    if (got != WAIT_OBJECT_0 && got != WAIT_ABANDONED) {   /* (abandoned: its holder ended without letting go: ours now) */
        if (got == WAIT_TIMEOUT) {
            const HANDLE s = OpenFileMappingW(FILE_MAP_READ, FALSE, shared);
            const HosInstanceWho *v = s ? (const HosInstanceWho *)MapViewOfFile(s, FILE_MAP_READ, 0, 0, sizeof *v) : NULL;
            if (v) {
                who->pid = v->pid;
                wcsncpy(who->exe, v->exe, 1023);
                UnmapViewOfFile(v);
            }
            if (s) CloseHandle(s);
        }
        CloseHandle(m);
        return got != WAIT_TIMEOUT;   /* (WAIT_FAILED: no lock to be had, nobody refused) */
    }
    const HANDLE s = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(HosInstanceWho), shared);
    HosInstanceWho *v = s ? (HosInstanceWho *)MapViewOfFile(s, FILE_MAP_WRITE, 0, 0, sizeof *v) : NULL;
    if (v) {
        GetModuleFileNameW(NULL, v->exe, 1023);
        v->exe[1023] = 0;
        v->pid = GetCurrentProcessId();   /* (last: a reader that finds a process id finds its exe too) */
        UnmapViewOfFile(v);   /* (the section stays while s is open) */
    }
    return 1;
}

/* The second copy's message: what holds the folder, and what to do */
static inline void hos_instance_message(const wchar_t *folder, const HosInstanceWho *who, wchar_t *out, size_t cap) {
    wchar_t holder[1200];
    if (who->pid) _snwprintf(holder, 1200, L"process %lu, %ls", (unsigned long)who->pid, who->exe);
    else _snwprintf(holder, 1200, L"another copy that is starting");
    holder[1199] = 0;
    _snwprintf(out, cap,
               L"Triforce is already running from this game folder:\n\n%ls\n\nIt is held by %ls.\n\nUse the window that is open, or "
               L"close it and start the game again. (Two copies of one game folder would write the same saves, settings and "
               L"shader caches over each other.)",
               folder, holder);
    out[cap - 1] = 0;
}
#endif
