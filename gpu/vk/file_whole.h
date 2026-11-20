// (Triforce, handoff §8) A whole file at once, for file_writers.h: written as <path>.part and put in the old one's place, so
// that a write cut short (the process ended in it, a full disk) leaves the old file or the new one, never part of one
#pragma once
#include "file_writers.h"
#include <cstdio>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace file_writers {
/// rename over an existing file, as each platform does it
inline bool replace_file(const std::string& from, const std::string& to) {
#ifdef _WIN32
    return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

/// The whole file: <path>.part written and closed, then put in path's place; false (and no .part left) when any of it fails,
/// and then path is as it was. Inside a Writing of its own (refused once the end has begun)
inline bool write_whole(const std::string& path, const void* data, size_t n) {
    Writing w;
    if (!w) return false;
    const std::string part = path + ".part";
    FILE* f = std::fopen(part.c_str(), "wb");
    if (!f) return false;
    bool ok = !n || std::fwrite(data, 1, n, f) == n;
    ok = std::fclose(f) == 0 && ok;
    ok = ok && replace_file(part, path);
    if (!ok) std::remove(part.c_str());
    return ok;
}
inline bool write_whole(const std::string& path, const std::string& text) { return write_whole(path, text.data(), text.size()); }
}  // namespace file_writers
