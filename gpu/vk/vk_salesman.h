// (Triforce) The Happy Mask Salesman beside the Mods page's cards: his own model, textures and animations, read from the
// player's game files at run time (Model/Npc_Hylia_MN_005*.bfres.zs, Tex/*_Npc_Hylia_MN_005_*.bntx.zs: nothing of the game is
// shipped), drawn on a thread of his own by a small textured rasterizer into a picture the menu shows (vk_menu.cpp draw_mods).
// He wears Majora's Mask (PigeonMage's model, CC BY 4.0, shipped as triforce_assets\majora_mask.*) on his head bone, and lifts
// it to smile at the big moments (smile()). Nothing runs while the page is not shown.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mkwvk::salesman {

enum class State { Idle, Loading, Ready, Failed };

/// His files read (once, on his thread) from the romfs folder; the mask from the game folder's triforce_assets
void start(const std::string& romfs);
State state(std::string* why = nullptr);
/// While the Mods page shows him (frames made at w x h pixels); off: his thread rests
void show(bool on, int w, int h);
/// The mask lifted for `seconds` (his joyful pose, his smile), then put back on
void smile(float seconds);
/// The newest picture when it is newer than *seq: RGBA (straight alpha), w x h
bool take(std::vector<unsigned char>& rgba, int* w, int* h, uint64_t* seq);

}  // namespace mkwvk::salesman
