// The interface layer with a real face: text measured and drawn in points,
// letters beyond ASCII, typing, sliders, and the picture's signature.
#include "framework.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include <SDL3/SDL.h>

#include "engine/ui/canvas.hpp"
#include "engine/ui/font.hpp"
#include "engine/ui/ui.hpp"

namespace {
std::filesystem::path fonts() {
    std::filesystem::path at = "assets/fonts";
    for (int up = 0; up < 5 && !std::filesystem::exists(at); ++up) at = ".." / at;
    return at;
}

// A software renderer over a surface, and a Ui on it with the face.
struct Screen {
    SDL_Surface* surface = nullptr;
    SDL_Renderer* renderer = nullptr;
    ui::Ui ui;
    ui::Input input;
    bool ready = false;
    explicit Screen(float scale = 1.0f) {
        surface = SDL_CreateSurface(int(400 * scale), int(200 * scale), ui::Canvas::kFormat);
        renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
        if (!renderer) return;
        SDL_SetRenderScale(renderer, scale, scale);
        ui.init(renderer);
        const auto regular = ui::FontFace::load(fonts() / "NotoSans-Regular.ttf");
        const auto bold = ui::FontFace::load(fonts() / "NotoSans-Bold.ttf");
        if (!regular || !bold) return;
        ui.setFont(regular, bold, 15, scale);
        ready = true;
    }
    ~Screen() {
        ui.shutdown();
        if (renderer) SDL_DestroyRenderer(renderer);
        if (surface) SDL_DestroySurface(surface);
    }
    // One frame of the interface, built by `build`, painting or not.
    template<class F> std::uint64_t frame(F&& build, bool paint = true) {
        ui.begin(input, 400, 200);
        ui.painting(paint);
        build();
        ui.end();
        input.pressed = input.released = false;
        input.text.clear();
        input.backspace = false;
        return ui.signature();
    }
};
} // namespace

TEST(ui_font_measures_text_in_points_at_any_density) {
    Screen one(1.0f), two(2.0f);
    CHECK(one.ready && two.ready);
    if (!one.ready || !two.ready) return;
    const float w1 = one.ui.textWidth("Explore worlds"), w2 = two.ui.textWidth("Explore worlds");
    CHECK(w1 > 60 && w1 < 140);                 // proportional, not eight points a letter
    CHECK(std::abs(w1 - w2) < w1 * 0.06f);      // the same size in points, baked for twice the pixels
    CHECK(one.ui.textHeight() > 12 && one.ui.textHeight() < 20);
    CHECK(one.ui.textWidth("Explore", 1.0f, true) > one.ui.textWidth("Explore"));
}

TEST(ui_font_draws_cyrillic_and_cuts_between_letters) {
    Screen s;
    CHECK(s.ready);
    if (!s.ready) return;
    // Real glyphs: a word of Cyrillic is not a row of question marks.
    CHECK(std::abs(s.ui.textWidth("Долина") - s.ui.textWidth("??????")) > 1.0f);
    const std::string cut = s.ui.fit("Долина северного ветра", 60);
    CHECK(cut.size() < std::string("Долина северного ветра").size());
    CHECK(!cut.empty() && cut.back() == '.');
    // Whole letters only: every byte before the full stop belongs to a whole codepoint.
    std::size_t at = 0;
    const std::string body = cut.substr(0, cut.size() - 1);
    while (at < body.size()) CHECK(ui::nextCodepoint(body, at) != 0xFFFD);
    CHECK_EQ(ui::lastCodepointBytes("ab"), std::size_t(1));
    CHECK_EQ(ui::lastCodepointBytes("мир"), std::size_t(2));
}

TEST(ui_text_field_types_and_backspaces_whole_letters) {
    Screen s;
    CHECK(s.ready);
    if (!s.ready) return;
    std::string value = "Мир";
    const ui::Rect field{10, 10, 200, 32};
    const auto id = ui::widgetId("test.field");
    // Not focused: typing goes nowhere.
    s.input.text = "x";
    s.frame([&] { s.ui.textField(id, field, value); });
    CHECK_EQ(value, std::string("Мир"));
    CHECK(!s.ui.typing());
    // A click focuses it; then letters go in and a backspace takes one back.
    s.input.mouseX = 20; s.input.mouseY = 20; s.input.pressed = true; s.input.down = true;
    s.frame([&] { s.ui.textField(id, field, value); });
    CHECK(s.ui.typing());
    s.input.down = false; s.input.released = true;
    s.input.text = " ё2";
    s.frame([&] { s.ui.textField(id, field, value); });
    CHECK_EQ(value, std::string("Мир ё2"));
    s.input.backspace = true;
    s.frame([&] { s.ui.textField(id, field, value); });
    s.input.backspace = true;
    s.frame([&] { s.ui.textField(id, field, value); });
    CHECK_EQ(value, std::string("Мир "));
    // Enter gives the keyboard back.
    s.input.enter = true;
    s.frame([&] { s.ui.textField(id, field, value); });
    s.input.enter = false;
    CHECK(!s.ui.typing());
}

TEST(ui_slider_follows_a_drag_logarithmically_and_keeps_it_off_the_widget) {
    Screen s;
    CHECK(s.ready);
    if (!s.ready) return;
    float value = 10;
    const ui::Rect track{20, 20, 212, 20};   // 200 of travel after the knob's 12
    const auto id = ui::widgetId("test.slider");
    s.input.mouseX = 30; s.input.mouseY = 30; s.input.pressed = true; s.input.down = true;
    s.frame([&] { s.ui.slider(id, track, value, 4, 400, true); });
    CHECK(s.ui.capturing());
    // Halfway along a 4..400 log track is 40, whatever the pointer's height.
    s.input.mouseX = 20 + 6 + 100; s.input.mouseY = 150;
    s.frame([&] { s.ui.slider(id, track, value, 4, 400, true); });
    CHECK(std::abs(value - 40.0f) < 1.0f);
    s.input.down = false; s.input.released = true;
    s.frame([&] { s.ui.slider(id, track, value, 4, 400, true); });
    CHECK(!s.ui.capturing());
}

TEST(ui_signature_moves_with_the_picture_and_not_otherwise) {
    Screen s;
    CHECK(s.ready);
    if (!s.ready) return;
    const ui::Rect box{10, 10, 120, 34};
    const auto build = [&] { s.ui.button(ui::widgetId("test.button"), box, "Save"); };
    s.input.mouseX = s.input.mouseY = 300;
    const auto away = s.frame(build, false);
    CHECK_EQ(s.frame(build, false), away);            // nothing changed: nothing to paint
    CHECK_EQ(s.frame(build, true), away);             // painting or not, the same number
    s.input.mouseX = s.input.mouseY = 20;
    CHECK(s.frame(build, false) != away);             // hovered: lit, so a new picture
    // And the audit sees where every line went.
    std::vector<ui::Ui::TextBox> boxes;
    s.ui.audit(&boxes);
    s.frame([&] { s.ui.text(10, 60, "one", {}); s.ui.text(12, 62, "two", {}); }, false);
    s.ui.audit(nullptr);
    CHECK_EQ(boxes.size(), std::size_t(2));
    if (boxes.size() == 2) CHECK(boxes[0].box.w > 0 && boxes[0].box.h > 0);
}


TEST(ui_paint_cost_of_a_dense_screen_is_measured) {
    // What a repaint of the whole interface costs on a two-pixel-per-point
    // surface of the canvas's layout, primitive by primitive: printed, and
    // bounded loosely so a regression that makes hovering stutter fails here
    // first. (In ABGR8888, which SDL blends through a per-pixel format lookup,
    // and with corners as triangle fans, the dim was 44 ms and the panel 47.)
    SDL_Surface* surface = SDL_CreateSurface(2880, 1800, ui::Canvas::kFormat);
    SDL_Renderer* renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
    CHECK(renderer != nullptr);
    if (!renderer) return;
    SDL_SetRenderScale(renderer, 2, 2);
    ui::Ui ui;
    ui.init(renderer);
    ui.setFont(ui::FontFace::load(fonts() / "NotoSans-Regular.ttf"), ui::FontFace::load(fonts() / "NotoSans-Bold.ttf"),
               15, 2);
    ui::Input input;
    const auto time = [&](const char* what, auto&& draw) {
        const auto began = std::chrono::steady_clock::now();
        for (int i = 0; i < 5; ++i) {
            ui.begin(input, 1440, 900);
            draw();
            ui.end();
            SDL_FlushRenderer(renderer);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count() / 5;
        std::printf("  %-34s %6.2f ms\n", what, ms);
        return ms;
    };
    time("clear", [&] { SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0); SDL_RenderClear(renderer); });
    const double dim = time("full-screen dim (blended rect)", [&] { ui.rect({0, 0, 1440, 900}, {0, 0, 0, 0.5f}); });
    const double panel = time("large panel 1000x660 (rounded)", [&] { ui.panel({220, 120, 1000, 660}); });
    time("large rect 1000x660", [&] { ui.rect({220, 120, 1000, 660}, {0.1f, 0.1f, 0.1f, 0.96f}); });
    time("60 buttons", [&] {
        for (int i = 0; i < 60; ++i) ui.button(ui::widgetId("b", i), {20.0f + (i % 6) * 150, 20.0f + (i / 6) * 40, 140, 32}, "Button");
    });
    const double text = time("60 lines of text", [&] {
        for (int i = 0; i < 60; ++i) ui.text(20, 20.0f + i * 14, "The quick brown fox jumps over the lazy dog", {1, 1, 1, 1});
    });
    // The same panel, but only a status line's worth of it asked for.
    const ui::Rect corner{0, 832, 1440, 64};
    ui.paintOnly(&corner);
    const double clipped = time("the panel clipped to a strip", [&] { ui.panel({0, 600, 1440, 300}); });
    ui.paintOnly(nullptr);
    CHECK(text < 50);
    CHECK(dim < 20);
    CHECK(panel < 20);
    CHECK(clipped < panel);
    ui.shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(surface);
}

namespace {
// The alpha of one pixel of a canvas-layout surface.
int alphaAt(const SDL_Surface* s, int x, int y) {
    const auto* row = static_cast<const std::uint8_t*>(s->pixels) + std::size_t(y) * std::size_t(s->pitch);
    return row[std::size_t(x) * 4 + 3];
}
} // namespace

TEST(ui_rounded_panel_is_one_layer_with_its_corners_cut) {
    // Row by row, the corners are cut and nothing is laid twice: a translucent
    // rectangle is the same translucency everywhere inside it, with no seam
    // where the corner rows meet the body, and nothing at the corner's tip.
    for (const float scale : {1.0f, 2.0f}) {
        SDL_Surface* surface = SDL_CreateSurface(int(200 * scale), int(120 * scale), ui::Canvas::kFormat);
        SDL_Renderer* renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
        CHECK(renderer != nullptr);
        if (!renderer) return;
        SDL_SetRenderScale(renderer, scale, scale);
        ui::Ui ui;
        ui.init(renderer);
        ui.setFont(nullptr, nullptr, 15, scale);
        ui::Input input;
        ui.begin(input, 200, 120);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
        SDL_RenderClear(renderer);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        ui.roundRect({20, 20, 160, 80}, {1, 1, 1, 0.5f}, 10);
        ui.end();
        SDL_FlushRenderer(renderer);
        const int x0 = int(20 * scale), y0 = int(20 * scale), x1 = int(180 * scale), y1 = int(100 * scale);
        const int inside = alphaAt(surface, (x0 + x1) / 2, (y0 + y1) / 2);
        CHECK(inside > 110 && inside < 145);
        // Every row, at the middle of the rectangle: the same alpha.
        bool even = true;
        for (int y = y0; y < y1; ++y)
            if (alphaAt(surface, (x0 + x1) / 2, y) != inside) even = false;
        CHECK(even);
        // The corners' tips are outside it, the edges' middles inside.
        CHECK_EQ(alphaAt(surface, x0, y0), 0);
        CHECK_EQ(alphaAt(surface, x1 - 1, y1 - 1), 0);
        CHECK_EQ(alphaAt(surface, x0, (y0 + y1) / 2), inside);
        CHECK_EQ(alphaAt(surface, (x0 + x1) / 2, y1 - 1), inside);
        CHECK_EQ(alphaAt(surface, x1, (y0 + y1) / 2), 0);
        ui.shutdown();
        SDL_DestroyRenderer(renderer);
        SDL_DestroySurface(surface);
    }
}

TEST(ui_tiles_say_where_the_picture_changed) {
    Screen s;
    CHECK(s.ready);
    if (!s.ready) return;
    const ui::Rect left{10, 10, 120, 34}, right{260, 150, 120, 34};
    const auto build = [&] {
        s.ui.panel({0, 0, 400, 200});
        s.ui.button(ui::widgetId("test.left"), left, "Left");
        s.ui.button(ui::widgetId("test.right"), right, "Right");
    };
    s.ui.trackTiles(32);
    s.input.mouseX = s.input.mouseY = -100;
    s.frame(build, false);
    const auto before = s.ui.tileSignatures();
    // Nothing moved: nothing to repaint.
    s.frame(build, false);
    CHECK(ui::changedAreas(before, s.ui.tileSignatures(), s.ui.tileColumns(), s.ui.tileRows(), 32, 400, 200).empty());
    // The right button lit: the areas hold it, and stay clear of the other.
    s.input.mouseX = 300; s.input.mouseY = 160;
    s.frame(build, false);
    const auto areas = ui::changedAreas(before, s.ui.tileSignatures(), s.ui.tileColumns(), s.ui.tileRows(), 32, 400, 200);
    CHECK(!areas.empty());
    bool holds = false, clear = true;
    float area = 0;
    for (const auto& a : areas) {
        if (a.x <= right.x && a.y <= right.y && a.right() >= right.right() && a.bottom() >= right.bottom()) holds = true;
        if (a.x < left.right() && left.x < a.right() && a.y < left.bottom() && left.y < a.bottom()) clear = false;
        area += a.w * a.h;
        CHECK(a.right() <= 400 && a.bottom() <= 200);
    }
    CHECK(holds);
    CHECK(clear);
    CHECK(area < 400 * 200 * 0.25f);
    // Tiles of a different size of screen are all changed.
    CHECK_EQ(ui::changedAreas({1, 2}, s.ui.tileSignatures(), s.ui.tileColumns(), s.ui.tileRows(), 32, 400, 200).size(),
             std::size_t(1));
    s.ui.trackTiles(0);
}

TEST(ui_painting_one_area_leaves_the_rest_as_it_was) {
    Screen s;
    CHECK(s.ready);
    if (!s.ready) return;
    // All of it red, then only the left half asked for in blue.
    const auto fill = [&](const ui::Colour& c) { s.ui.rect({0, 0, 400, 200}, c); };
    s.frame([&] { fill({1, 0, 0, 1}); });
    const ui::Rect half{0, 0, 200, 200};
    s.ui.paintOnly(&half);
    s.frame([&] { fill({0, 0, 1, 1}); });
    s.ui.paintOnly(nullptr);
    SDL_FlushRenderer(s.renderer);
    const auto pixel = [&](int x, int y) {
        const auto* row = static_cast<const std::uint8_t*>(s.surface->pixels) + std::size_t(y) * std::size_t(s.surface->pitch);
        return std::array<int, 3>{row[x * 4 + 2], row[x * 4 + 1], row[x * 4 + 0]};   // R, G, B of B, G, R, A
    };
    CHECK(pixel(100, 100)[2] == 255 && pixel(100, 100)[0] == 0);
    CHECK(pixel(300, 100)[0] == 255 && pixel(300, 100)[2] == 0);
}


TEST(ui_scroll_region_moves_with_the_wheel_and_hides_what_is_outside_it) {
    Screen screen;
    if (!screen.ready) return;
    ui::Ui& ui = screen.ui;
    ui::Input& input = screen.input;
    const ui::Rect frame{20, 20, 200, 100};
    // Twenty rows of 30 points in a frame of 100: the lowest row it laid out
    // is where the content ends, and there is 500 points of it to scroll.
    int clicked = -1;
    const auto build = [&] {
        ui.begin(input, 400, 200);
        const ui::Rect content = ui.beginScroll(7, frame);
        float y = content.y;
        for (int i = 0; i < 20; ++i) {
            if (ui.button(100 + std::uint64_t(i), {content.x, y, content.w, 26}, "row")) clicked = i;
            y += 30;
        }
        ui.endScroll(y);
        ui.end();
    };
    build();
    build();   // the second build knows how tall the content is
    CHECK_EQ(ui.scrollOffset(7), 0.0f);
    // A button below the frame is not there to be pressed.
    input.mouseX = 60; input.mouseY = 130;
    input.pressed = true; input.down = true;
    build();
    input.pressed = false; input.down = false; input.released = true;
    build();
    input.released = false;
    CHECK_EQ(clicked, -1);
    // The wheel over the frame scrolls it, and is taken: the world under it
    // would not see it.
    input.mouseX = 60; input.mouseY = 60;
    input.wheel = -2;
    build();
    CHECK(ui.scrollOffset(7) > 0.0f);
    CHECK_EQ(input.wheel, 0.0f);
    // Never past the end.
    for (int i = 0; i < 20; ++i) { input.wheel = -5; build(); }
    CHECK_EQ(ui.scrollOffset(7), 500.0f);
    // Scrolled to the end, the last row sits in the frame and takes a click.
    input.mouseX = 60; input.mouseY = 105;
    input.pressed = true; input.down = true;
    build();
    input.pressed = false; input.down = false; input.released = true;
    build();
    input.released = false;
    CHECK_EQ(clicked, 19);
    // Dragging the bar to the top brings the first rows back.
    const float barX = frame.right() - ui::Ui::kScrollBar * 0.5f;
    input.mouseX = barX; input.mouseY = frame.bottom() - 8;
    input.pressed = true; input.down = true;
    build();
    input.pressed = false;
    input.mouseY = frame.y - 50;
    build();
    input.down = false; input.released = true;
    build();
    input.released = false;
    CHECK_EQ(ui.scrollOffset(7), 0.0f);
}

TEST(ui_scroll_region_marks_no_tile_outside_its_frame) {
    // What is scrolled out of the frame is not in the picture: moving it
    // must not ask for a repaint anywhere but the frame.
    Screen screen;
    if (!screen.ready) return;
    ui::Ui& ui = screen.ui;
    ui::Input input;
    const ui::Rect frame{0, 0, 128, 64};
    const auto tiles = [&](float offset) {
        ui.scrollTo(3, offset);
        ui.trackTiles(32);
        ui.begin(input, 400, 200);
        const ui::Rect content = ui.beginScroll(3, frame);
        for (int i = 0; i < 20; ++i) ui.rect({content.x, content.y + float(i) * 20, 100, 16}, ui.theme().slot);
        ui.endScroll(content.y + 400);
        ui.end();
        ui.trackTiles(0);
        return ui.tileSignatures();
    };
    tiles(0);
    const auto top = tiles(0), lower = tiles(100);
    const auto areas = ui::changedAreas(top, lower, ui.tileColumns(), ui.tileRows(), 32, 400, 200);
    CHECK(!areas.empty());
    for (const auto& a : areas) CHECK(a.bottom() <= frame.bottom() + 32.5f && a.right() <= frame.right() + 32.5f);
}
