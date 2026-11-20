// (Triforce) The Inventory page's change asked of the game and not yet taken by it (vk_menu.cpp keeps it in cheat_action;
// host/hos_main.c takes it at Link's update, which does not run while the game's own Options screen has the game paused).
// The arithmetic is here so that tests/test_debug_pending.py can run it.
//
// An action is OOT_CHEAT_ITEM_ACTION + item * 256 + op: op 0..127 adds op - 64 to the item's amount, op 200 unlocks it (1)
// or relocks it (0). Only one action waits at a time. Before, each press replaced the last: ten presses of Right on an
// amount in the paused Options screen made it one more, and the card showed the amount as it was; a second press of Unlock
// unlocked it all the same. Now a press adds to the same item's waiting change, and the card shows what it will be.
#pragma once

#include <algorithm>

#include "oot_cheat_items.h"

namespace mkwvk::debug_pending {

/// The change to item's amount the waiting action asks (0: none, or the action is not that item's amount)
inline int amount_delta(int pending, int item) {
    const int base = OOT_CHEAT_ITEM_ACTION + item * 256;
    return pending >= base && pending < base + 128 ? pending - base - 64 : 0;
}
/// Whether the waiting action is item's unlock or relock
inline bool toggles(int pending, int item) { return pending == OOT_CHEAT_ITEM_ACTION + item * 256 + 200; }

/// The action after a step (dir 1 or -1) on item's amount, count what the game last said it has (0 or more): the item's
/// waiting change and the step together, kept to what the game allows (0 to 999) and to what one action says (-64 to 63).
/// Another item's waiting action is replaced, as before
inline int amount_step(int pending, int item, int count, int dir) {
    const int delta = std::clamp(amount_delta(pending, item) + dir, std::max(-64, -count), std::min(63, 999 - count));
    return OOT_CHEAT_ITEM_ACTION + item * 256 + 64 + delta;
}
/// The action after a press of Unlock or Relock on item: a second press before the game took the first takes it back (0)
inline int toggle(int pending, int item) { return toggles(pending, item) ? 0 : OOT_CHEAT_ITEM_ACTION + item * 256 + 200; }

/// What item's card shows: count, what the game last said, with the waiting change made (count < 0, unknown: as it is)
inline int shown(int pending, int item, int count) {
    if (count < 0) return count;
    if (toggles(pending, item)) return count > 0 ? 0 : 1;
    return std::clamp(count + amount_delta(pending, item), 0, 999);
}

} // namespace mkwvk::debug_pending
