// (Triforce) Mods from GameBanana, inside the game's Options (vk_menu.cpp draws the Mods page; this is its engine): the
// game's section on GameBanana (game 26242, "The Legend of Zelda: Ocarina of Time (Switch 2)") read page by page with its
// public API, each mod's picture and details, and an install: the file downloaded, unpacked with Windows' own tar (zip, 7z,
// rar), the folder that holds romfs moved into the game folder's mods folder (host/mods.c reads it at the next start) with
// triforce_mod.json beside it (its name, author and page: Setup's Mods page shows them). Network, unpacking and pictures run
// on threads of their own; the menu only reads the state here. Linux: not available yet (the page says so).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mkwvk::mods {

struct File {
    int id = 0;
    std::string name, description, url;
    uint64_t bytes = 0;
    int64_t added = 0;   // (unix seconds)
};

struct Mod {
    int id = 0;
    std::string name, author, category, page;
    int likes = 0, views = 0, downloads = -1;   // (downloads: from the details, -1 until they came)
    int64_t updated = 0;
    std::string thumb_url;
    // (the details, once fetched: details_state 0 not asked, 1 asked, 2 here, 3 failed)
    int details_state = 0;
    std::string text;   // (the mod's description, as plain text)
    std::vector<File> files;
    int file_choice = 0;
    // (the picture: thumb_state 0 not asked, 1 asked, 2 pixels ready (thumb_rgba), 3 failed; the menu makes its texture once)
    int thumb_state = 0;
    int thumb_w = 0, thumb_h = 0;
    std::vector<unsigned char> thumb_rgba;
    std::string thumb_file;    // (the picture as downloaded: a picture the menu let go far out of view is made again from it)
    void* texture = nullptr;   // (ImTextureData*, the menu's)
    // (installed: a folder in mods with this mod's id in its triforce_mod.json; update: a file newer than the one installed)
    bool installed = false, update = false;
    // ((Triforce) 11-18) wip: posted on GameBanana as a WiP (a work in progress with files: many are mods all the same; its
    // ids are its own, Wip/<id>). kit: a program of its own on the kits list (vk_mods.cpp kKits), not a romfs mod: installed
    // into kits\<its folder> beside the game and started from there (Squadala!, the online co-op)
    bool wip = false, kit = false;
};

enum class Feed { Idle, Loading, Ready, Failed };
enum class Job { None, Downloading, Unpacking, Done, Failed, Cancelled };

struct InstallState {
    Job job = Job::None;
    int mod_id = 0;
    std::string mod_name, file_name, message;   // (message: why it failed, or what was installed where)
    uint64_t done = 0, total = 0;
    double rate = 0;   // (bytes a second, smoothed)
    bool kit = false;  // ((Triforce) 11-18) a kit's: Done = started in its own window (no restart)
};

/// Whether this build can browse and install (Windows: yes; Linux: not yet)
bool available();
/// The page opened: the first page of the list asked for when there is none yet (again after a failure); a search left
/// behind ended. Returns the textures (ImTextureData*) of the pictures of a list emptied, for the menu to let go
std::vector<void*> open();
/// The list as it is now: the section's, or a search's results while one is on (the menu thread's copy; changes while pages
/// and details come in: lock()/unlock() around reads)
std::vector<Mod>& list();
void lock();
void unlock();
Feed feed_state(std::string* error = nullptr);
bool more();          // (GameBanana has more pages than the list holds)
/// ((Triforce) 11-18) The next page, when none is loading or queued. Pages come one after another by themselves from open()
/// and search() until the list is whole; this only starts again a list that stopped on a failed page
void load_more();
int total();          // (GameBanana's count: the section's mods and WiPs; a search's results, about)
/// ((Triforce) 11-17) A search of the section's mods: GameBanana's own (whole words, best matches first), then a part of a
/// name when that finds none. Fewer than 2 characters (trimmed) ends the search. The list is its results until search_clear,
/// which goes back to the section's list as it was. Both return the textures of the pictures of the results let go (the
/// menu's to free; call outside lock()/unlock())
std::vector<void*> search(const std::string& text);
std::vector<void*> search_clear();
std::string search_text();   // (the search on: its text; none: empty)
bool search_by_name();       // (the results are the names' (no whole-word match))
/// The details (description, files, downloads) and the picture of mod i asked for, once each
void want_details(int i);
void want_thumb(int i);
/// Install mod i's chosen file; Cancel stops a download or unpacking under way
void install(int i);
void cancel();
InstallState install_state();
void clear_done();   // (Done/Failed/Cancelled seen: back to None)
/// The install's warning, accepted once (kept in triforce_mods.ini beside the game)
bool accepted();
void accept();
/// The mods folder (host/mods.c's: "mods" beside the game)
std::string mods_folder();
/// The game folder (the one Triforce.exe is in, as host/hos_paths.h has it)
std::string game_folder_path();
/// A JPEG / PNG / WebP file's pixels (RGBA, at most max_w wide), with Windows' own decoders; false on Linux
bool decode_image(const std::string& bytes, int max_w, std::vector<unsigned char>& rgba, int* w, int* h);

}  // namespace mkwvk::mods
