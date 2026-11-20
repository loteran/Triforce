// tests/test_debug_tab_layout.py builds and runs this: gpu/vk/debug_tab_layout.h, the Debug tab's cells' widths.
#include "../gpu/vk/debug_tab_layout.h"

#include <cmath>
#include <cstdio>

using mkwvk::debug_tab::Widths;
using mkwvk::debug_tab::widths;

static int failures;
#define CHECK(what, ...) do { if (!(what)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main() {
    // the tab as it measures in a 1280 x 720 window (the layout's unit is the window's height over 720): each half of the
    // column 317 wide, the names asking 181 and 167, the cells at their narrowest 147 and 158
    const float name[2] = {181, 167}, full[2] = {175, 186}, tight[2] = {147, 158};
    const float room = 2 * 317 + 24;
    {   // one column, and every cell one width: the widest any asks, the Developer row of more than two values among them
        const Widths w = widths(317, room, name, full, tight, 200);
        CHECK(!w.two, "two columns where neither fits its names and cells");
        CHECK(w.cell[0] == 200 && w.cell[1] == 200 && w.span == 200, "one column's cells are %g, %g and %g, not all 200", w.cell[0], w.cell[1], w.span);
        const Widths plain = widths(317, room, name, full, tight, 150);
        CHECK(plain.cell[0] == 186 && plain.span == 186, "with no wide Developer row the cells are not the tools' widest (%g, %g)", plain.cell[0], plain.span);
    }
    // the same at every window: the layout scales with the window's height, so what does not fit at 720 lines fits at none
    for (float k : {1.0f, 1.5f, 2.0f, 3.0f}) {
        const float n[2] = {name[0] * k, name[1] * k}, f[2] = {full[0] * k, full[1] * k}, t[2] = {tight[0] * k, tight[1] * k};
        const Widths w = widths(317 * k, room * k, n, f, t, 200 * k);
        CHECK(!w.two && std::fabs(w.span - 200 * k) < 0.01f && w.cell[0] == w.span && w.cell[1] == w.span, "at %g times the size the widths are not the same share", k);
    }
    {   // one column, a name that needs the room: the cells give way to their narrowest, and no further
        const float long_name[2] = {560, 167};
        const Widths w = widths(317, room, long_name, full, tight, 200);
        CHECK(!w.two && w.cell[0] == 158 && w.span == 158, "a long name leaves the cells %g, not their narrowest 158", w.cell[0]);
        const float longer[2] = {500, 167};
        CHECK(widths(317, room, longer, full, tight, 200).span == room - 500, "the cells do not take what the longest name leaves");
    }
    {   // a column wide enough for both: two columns, each column's cells alike, the spanning rows' cells alike
        const Widths w = widths(330, 2 * 330 + 24, name, full, tight, 200);
        CHECK(w.two, "one column where both fit");
        CHECK(w.cell[0] == 149 && w.cell[1] == 163, "two columns' cells are %g and %g, not what the names leave (149, 163)", w.cell[0], w.cell[1]);
        CHECK(w.span == 200, "the spanning rows' cells are %g, not the widest any of them asks", w.span);
        const Widths narrow = widths(330, 2 * 330 + 24, name, full, tight, 120);
        CHECK(narrow.span == narrow.cell[1], "the spanning rows' cells do not line up with the second column's (%g, %g)", narrow.span, narrow.cell[1]);
        const Widths wide = widths(400, 2 * 400 + 24, name, full, tight, 120);
        CHECK(wide.two && wide.cell[0] == 175 && wide.cell[1] == 186, "with room to spare the cells are not what they ask (%g, %g)", wide.cell[0], wide.cell[1]);
    }
    {   // one column only fits: no two columns
        const float n[2] = {100, 250};
        CHECK(!widths(317, room, n, full, tight, 150).two && widths(317, room, name, full, tight, 150).two == false, "two columns with one that does not fit");
    }
    // never under the narrowest, whatever is asked
    for (float span : {0.0f, 100.0f, 400.0f})
        for (float col : {200.0f, 317.0f, 500.0f}) {
            const Widths w = widths(col, 2 * col + 24, name, full, tight, span);
            CHECK(w.cell[0] >= tight[0] - 0.01f && w.cell[1] >= tight[1] - 0.01f && w.span >= (w.two ? w.cell[1] : tight[1]), "a cell under its narrowest (%g, %g)", span, col);
        }
    if (failures) return printf("%d failures\n", failures), 1;
    printf("ok\n");
    return 0;
}
