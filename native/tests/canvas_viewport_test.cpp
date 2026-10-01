// Run: c++ -std=c++17 -Inative native/tests/canvas_viewport_test.cpp native/ui/canvas.cpp -o /tmp/pt-viewport-test && /tmp/pt-viewport-test
#include <cassert>
#include "ui/canvas.h"

int main() {
    using namespace pt::ui;
    Canvas original, tall;
    original.clear();
    original.draw_text("EFFECTS", 10, 10, 0xFFFFFFFF, 2, 3);
    tall.set_height(600);
    tall.clear();
    tall.draw_text("EFFECTS", 10, 10, 0xFFFFFFFF, 2, 3);
    // A taller viewport must not change the glyphs or their spacing.
    for (int y = 0; y < DESIGN_H; ++y)
        for (int x = 0; x < DESIGN_W; ++x)
            assert(original.pixels()[y * DESIGN_W + x] == tall.pixels()[y * DESIGN_W + x]);
    tall.set_clip(0, 500, DESIGN_W, 100);
    tall.fill_rect(0, 499, DESIGN_W, 102, 0xFF123456);
    assert(tall.pixels()[499 * DESIGN_W] == 0xFF000000);
    assert(tall.pixels()[500 * DESIGN_W] == 0xFF123456);
    assert(tall.pixels()[599 * DESIGN_W + 639] == 0xFF123456);
    tall.set_height(DESIGN_H);
    assert(tall.height() == DESIGN_H);
    tall.clear();
    tall.fill_rect(0, 479, DESIGN_W, 100, 0xFFABCDEF);
    assert(tall.pixels()[479 * DESIGN_W + 639] == 0xFFABCDEF);
}
