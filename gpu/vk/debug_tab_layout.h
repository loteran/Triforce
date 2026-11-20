// (Triforce) The Debug tab's value cells: how wide they are, and whether the tools stand in two columns (vk_menu.cpp
// draw_debug_rows measures the texts and draws; the arithmetic is here so that tests/test_debug_tab_layout.py can run it).
//
// The tab has the Debug tools switch across its top, the tools under their groups' headings in one column or two, and the
// Developer rows across its foot. The cells of what stands under one another are one width, so their left edges line up:
// a column's tools alike, and the switch and the Developer rows, which span the columns, alike. A Developer row of more
// than two values asks more than Off and On: before, it alone was wider than the rows around it.
#pragma once

#include <algorithm>

namespace mkwvk::debug_tab {

struct Widths {
    bool two = false;    // (the tools in two columns)
    float cell[2] = {};  // (a column's tools' cells; both the same in one column)
    float span = 0;      // (the switch's and the Developer rows' cells)
};

/// col_w: a column's width when there are two; room: the tab's width. name_need: the longest name of each column with its
/// room in the name's cell; cell_full and cell_tight: what the column's widest cell asks, and its narrowest. span_need: what
/// the widest of the switch's and the Developer rows' cells asks.
/// Two columns only when each holds its longest name whole beside its cells at their narrowest; the cells are then as wide
/// as the name leaves them, up to what they ask, and the spanning cells as wide as the second column's (they end at the
/// same edge) or as they ask. In one column every cell is one width: what the widest of all asks, less only when the
/// longest name needs the room, and never under the narrowest
inline Widths widths(float col_w, float room, const float name_need[2], const float cell_full[2], const float cell_tight[2], float span_need) {
    Widths w;
    w.two = col_w >= name_need[0] + cell_tight[0] && col_w >= name_need[1] + cell_tight[1];
    if (w.two) {
        for (int k = 0; k < 2; k++) w.cell[k] = std::max(cell_tight[k], std::min(cell_full[k], col_w - name_need[k]));
        w.span = std::max(span_need, w.cell[1]);
    } else {
        const float widest = std::max({cell_full[0], cell_full[1], span_need});
        const float narrowest = std::max(cell_tight[0], cell_tight[1]);
        w.cell[0] = w.cell[1] = w.span = std::max(std::min(widest, room - std::max(name_need[0], name_need[1])), narrowest);
    }
    return w;
}

} // namespace mkwvk::debug_tab
