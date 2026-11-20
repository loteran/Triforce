// tests/test_debug_pending.py builds and runs this: gpu/vk/debug_pending.h, the Inventory's change waiting for the game.
#include "../gpu/vk/debug_pending.h"

#include <cstdio>

namespace dp = mkwvk::debug_pending;

static int failures;
#define CHECK(what, ...) do { if (!(what)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// what host/hos_main.c oot_inventory_edit makes of an action, for an item that has `have`
static int applied(int action, int item, int have) {
    const int op = action - OOT_CHEAT_ITEM_ACTION - item * 256;
    int want = have;
    if (op == 200) want = have > 0 ? 0 : 1;
    else if (op >= 0 && op < 128) want = have + op - 64;
    return want < 0 ? 0 : want > 999 ? 999 : want;
}

int main() {
    // ten presses of Right on an amount the game has not taken yet (its paused Options screen): ten more, not one
    int pending = 0;
    for (int i = 0; i < 10; i++) pending = dp::amount_step(pending, 7, 5, 1);
    CHECK(applied(pending, 7, 5) == 15 && dp::shown(pending, 7, 5) == 15, "ten steps on 5 give %d, shown %d", applied(pending, 7, 5), dp::shown(pending, 7, 5));
    CHECK(dp::shown(pending, 8, 3) == 3 && dp::amount_delta(pending, 8) == 0, "another item's card shows this item's change");
    // back and forth: they cancel; at 0 Left asks nothing below it, so a Right after it is one more
    pending = dp::amount_step(pending, 7, 5, -1);
    CHECK(dp::shown(pending, 7, 5) == 14, "a step back is not taken off");
    pending = 0;
    for (int i = 0; i < 3; i++) pending = dp::amount_step(pending, 2, 0, -1);
    pending = dp::amount_step(pending, 2, 0, 1);
    CHECK(applied(pending, 2, 0) == 1 && dp::shown(pending, 2, 0) == 1, "Left at 0 three times then Right gives %d", applied(pending, 2, 0));
    // the ends: 999 at most, and what one action can say (-64 to 63)
    pending = 0;
    for (int i = 0; i < 5; i++) pending = dp::amount_step(pending, 3, 997, 1);
    CHECK(dp::shown(pending, 3, 997) == 999 && applied(pending, 3, 997) == 999, "past 999: %d", dp::shown(pending, 3, 997));
    pending = 0;
    for (int i = 0; i < 100; i++) pending = dp::amount_step(pending, 3, 10, 1);
    CHECK(dp::amount_delta(pending, 3) == 63 && applied(pending, 3, 10) == 73, "a hundred steps say %d", dp::amount_delta(pending, 3));
    pending = 0;
    for (int i = 0; i < 100; i++) pending = dp::amount_step(pending, 3, 500, -1);
    CHECK(dp::amount_delta(pending, 3) == -64 && applied(pending, 3, 500) == 436, "a hundred steps back say %d", dp::amount_delta(pending, 3));
    // another item's waiting action is replaced, as before; the first item shows what the game said
    pending = dp::amount_step(dp::amount_step(0, 4, 2, 1), 5, 9, 1);
    CHECK(dp::shown(pending, 4, 2) == 2 && dp::shown(pending, 5, 9) == 10, "a change of another item is shown");
    // the last item of the table: its actions are not another item's
    pending = dp::amount_step(0, 83, 1, 1);
    CHECK(dp::amount_delta(pending, 82) == 0 && !dp::toggles(pending, 82) && dp::amount_delta(pending, 83) == 1, "an item's action read as another's");
    // unlock: shown unlocked at once; a second press before the game took it takes it back
    pending = dp::toggle(0, 12);
    CHECK(dp::shown(pending, 12, 0) == 1 && dp::shown(pending, 12, 1) == 0 && applied(pending, 12, 0) == 1, "an unlock waiting is not shown");
    CHECK(dp::toggle(pending, 12) == 0, "a second press does not take the first back");
    CHECK(dp::toggle(dp::amount_step(0, 12, 1, 1), 12) == OOT_CHEAT_ITEM_ACTION + 12 * 256 + 200, "an unlock does not replace a waiting amount");
    // nothing waiting, and an action that is not the Inventory's (a teleport, the clock): the counts as the game said them
    for (int other : {0, 999, 1005, 104, 2999})
        CHECK(dp::shown(other, 0, 4) == 4 && dp::shown(other, 0, -1) == -1 && dp::amount_delta(other, 0) == 0, "action %d changes a card", other);
    if (failures) return printf("%d failures\n", failures), 1;
    printf("ok\n");
    return 0;
}
