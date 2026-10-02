#include "engine/ui/profiler_window.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace ui {
namespace {
namespace pf = engine::profile;

constexpr float kWide = 1500, kHigh = 980;
constexpr float kGlyph = 8;   // SDL's debug font
constexpr SDL_Color kBack{18, 20, 24, 255}, kPanel{28, 31, 37, 255}, kGrid{58, 62, 70, 255};
constexpr SDL_Color kText{220, 224, 230, 255}, kSoft{140, 146, 156, 255}, kAccent{255, 196, 80, 255};
constexpr SDL_Color kGood{86, 190, 110, 255}, kWarn{230, 190, 60, 255}, kBad{230, 80, 70, 255};
constexpr SDL_Color kWaiting{70, 74, 84, 255};

std::string format(const char* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

std::string bytes(double b) {
    if (b >= 1024.0 * 1024 * 1024) return format("%.2f GB", b / (1024.0 * 1024 * 1024));
    if (b >= 1024.0 * 1024) return format("%.1f MB", b / (1024.0 * 1024));
    if (b >= 1024.0) return format("%.1f KB", b / 1024.0);
    return format("%.0f B", b);
}

// A colour of its own for every function, stable between runs.
SDL_Color colourOf(const char* name) {
    std::uint32_t h = 2166136261u;
    for (const char* c = name ? name : ""; *c; ++c) h = (h ^ std::uint8_t(*c)) * 16777619u;
    const float hue = float(h % 360u) / 60.0f;
    const float x = 1.0f - std::fabs(std::fmod(hue, 2.0f) - 1.0f);
    float r = 0, g = 0, b = 0;
    if (hue < 1) { r = 1; g = x; } else if (hue < 2) { r = x; g = 1; } else if (hue < 3) { g = 1; b = x; }
    else if (hue < 4) { g = x; b = 1; } else if (hue < 5) { r = x; b = 1; } else { r = 1; b = x; }
    const auto mix = [](float v) { return std::uint8_t(80 + v * 120); };
    return {mix(r), mix(g), mix(b), 255};
}

SDL_Color frameColour(double ms) { return ms <= 16.7 ? kGood : ms <= 33.4 ? kWarn : kBad; }

std::string fit(std::string s, float width) {
    const auto room = std::size_t(std::max(0.0f, width / kGlyph));
    if (s.size() > room) s = room > 2 ? s.substr(0, room - 1) + "~" : std::string();
    return s;
}

} // namespace

ProfilerWindow::~ProfilerWindow() { close(); }

void ProfilerWindow::open() {
    if (window_) return;
    window_ = SDL_CreateWindow("Campfire profiler (sampling)", int(kWide), int(kHigh), SDL_WINDOW_RESIZABLE);
    if (!window_) return;
    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (!renderer_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        return;
    }
    SDL_SetRenderVSync(renderer_, 0);
    pf::setEnabled(true);
}

void ProfilerWindow::close() {
    pf::setEnabled(false);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    renderer_ = nullptr;
    window_ = nullptr;
}

bool ProfilerWindow::hasMouse() const { return window_ && SDL_GetMouseFocus() == window_; }
bool ProfilerWindow::hasKeyboard() const { return window_ && SDL_GetKeyboardFocus() == window_; }

void ProfilerWindow::say(std::string what) {
    status_ = std::move(what);
    statusUntil_ = SDL_GetTicks() + 6000;
}

bool ProfilerWindow::handle(const SDL_Event& event) {
    if (!window_) return false;
    SDL_Window* on = SDL_GetWindowFromEvent(&event);
    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && on && on != window_) {
        // The game's window: with this one open SDL would not say quit.
        SDL_Event quit{};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
        return false;
    }
    if (on != window_) return false;
    switch (event.type) {
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: close(); return true;
        case SDL_EVENT_KEY_DOWN: {
            const SDL_Keycode key = event.key.key;
            if (key == SDLK_ESCAPE) close();
            else if (event.key.repeat && key != SDLK_LEFT && key != SDLK_RIGHT) return true;
            else if (key == SDLK_SPACE) { paused_ = !paused_; if (!paused_) selectedIndex_ = -1; }
            else if (key == SDLK_T) writeTrace();
            else if (key == SDLK_C) { pf::clear(); frames_.clear(); selectedIndex_ = -1; }
            else if (key == SDLK_A) allThreads_ = !allThreads_;
            else if (key == SDLK_S) bySelf_ = !bySelf_;
            else if (key == SDLK_EQUALS || key == SDLK_KP_PLUS || key == SDLK_MINUS || key == SDLK_KP_MINUS) {
                const bool faster = key == SDLK_EQUALS || key == SDLK_KP_PLUS;
                const auto now = pf::interval().count();
                pf::setInterval(std::chrono::microseconds(faster ? std::max<long long>(100, now / 2) : std::min<long long>(20000, now * 2)));
                say(format("sampling every %.2f ms", double(pf::interval().count()) / 1000.0));
            } else if ((key == SDLK_LEFT || key == SDLK_RIGHT) && !frames_.empty()) {
                paused_ = true;
                const auto current = selected();
                auto index = current ? std::int64_t(current->index) : std::int64_t(frames_.back()->index);
                index += key == SDLK_LEFT ? -1 : 1;
                selectedIndex_ = std::clamp<std::int64_t>(index, std::int64_t(frames_.front()->index),
                                                          std::int64_t(frames_.back()->index));
                viewSpan_ = 0;
            }
            return true;
        }
        case SDL_EVENT_MOUSE_MOTION:
            mouseX_ = event.motion.x;
            mouseY_ = event.motion.y;
            if (dragging_ && viewSpan_ > 0)
                viewStart_ = dragFromStart_ - double(mouseX_ - dragFromX_) * viewSpan_ / double(std::max(1.0f, timelineRect_.w));
            return true;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            const SDL_FPoint p{event.button.x, event.button.y};
            if (SDL_PointInRectFloat(&p, &graphRect_) && !frames_.empty()) {
                const std::size_t shown = std::min<std::size_t>(frames_.size(), std::size_t(std::max(1.0f, graphRect_.w)));
                const float bar = graphRect_.w / float(shown);
                const auto at = std::size_t(std::clamp((p.x - graphRect_.x) / bar, 0.0f, float(shown - 1)));
                selectedIndex_ = std::int64_t(frames_[frames_.size() - shown + at]->index);
                paused_ = true;
                viewSpan_ = 0;
            } else if (SDL_PointInRectFloat(&p, &timelineRect_)) {
                dragging_ = true;
                dragFromX_ = p.x;
                dragFromStart_ = viewStart_;
            } else if (SDL_PointInRectFloat(&p, &functionsRect_) && p.y < functionsRect_.y + 18) {
                overSecond_ = !overSecond_;
            }
            return true;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP: dragging_ = false; return true;
        case SDL_EVENT_MOUSE_WHEEL: {
            const SDL_FPoint p{mouseX_, mouseY_};
            if (SDL_PointInRectFloat(&p, &timelineRect_)) {
                if (SDL_GetModState() & SDL_KMOD_SHIFT) {
                    laneScroll_ = std::max(0.0f, laneScroll_ - event.wheel.y * 24.0f);
                } else if (viewSpan_ > 0) {
                    const double t = double((mouseX_ - timelineRect_.x) / std::max(1.0f, timelineRect_.w));
                    const double at = viewStart_ + t * viewSpan_;
                    viewSpan_ = std::clamp(viewSpan_ * (event.wheel.y > 0 ? 0.8 : 1.25), 0.01, 10000.0);
                    viewStart_ = at - t * viewSpan_;
                }
            }
            return true;
        }
        default: return true;
    }
}

std::shared_ptr<const pf::FrameRecord> ProfilerWindow::selected() const {
    if (frames_.empty()) return nullptr;
    if (selectedIndex_ < 0) return frames_.back();
    for (auto it = frames_.rbegin(); it != frames_.rend(); ++it)
        if (std::int64_t((*it)->index) == selectedIndex_) return *it;
    return frames_.back();
}

void ProfilerWindow::writeTrace() {
    const std::string file = "campfire_trace.json";
    std::string why;
    if (pf::writeChromeTrace(file, frames_, &why))
        say("wrote " + file + " (" + std::to_string(frames_.size()) + " frames): chrome://tracing or ui.perfetto.dev");
    else
        say("could not write the trace: " + why);
}

void ProfilerWindow::text(float x, float y, const std::string& s, SDL_Color c, float scale) {
    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
    if (scale != 1.0f) {
        SDL_SetRenderScale(renderer_, scale, scale);
        SDL_RenderDebugText(renderer_, x / scale, y / scale, s.c_str());
        SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    } else {
        SDL_RenderDebugText(renderer_, x, y, s.c_str());
    }
}

void ProfilerWindow::fill(float x, float y, float w, float h, SDL_Color c) {
    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
    const SDL_FRect r{x, y, w, h};
    SDL_RenderFillRect(renderer_, &r);
}

void ProfilerWindow::draw() {
    if (!window_) return;
    const std::uint64_t ticks = SDL_GetTicks();
    if (ticks - lastDraw_ < 50) return;
    lastDraw_ = ticks;
    if (!paused_) frames_ = pf::frames();
    int w = int(kWide), h = int(kHigh);
    SDL_GetWindowSize(window_, &w, &h);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, kBack.r, kBack.g, kBack.b, 255);
    SDL_RenderClear(renderer_);
    drawHeader(6);
    float y = drawFrameGraph(40, 140);
    y = drawTimeline(y + 8, std::max(220.0f, float(h) * 0.42f));
    const float rest = float(h) - y - 16;
    functionsRect_ = {8, y + 8, float(w) * 0.62f - 12, rest};
    drawFunctions(functionsRect_.x, functionsRect_.y, functionsRect_.w, functionsRect_.h);
    drawCounters(float(w) * 0.62f, y + 8, float(w) * 0.38f - 8, rest);
    SDL_RenderPresent(renderer_);
}

void ProfilerWindow::drawHeader(float y) {
    double sum = 0, peak = 0;
    std::size_t n = 0;
    std::uint64_t allocs = 0, allocBytes = 0;
    std::int64_t windowNs = 0;
    for (auto it = frames_.rbegin(); it != frames_.rend() && windowNs < 1000000000; ++it) {
        sum += (*it)->millis();
        peak = std::max(peak, (*it)->millis());
        allocs += (*it)->allocations;
        allocBytes += (*it)->allocatedBytes;
        windowNs += (*it)->end - (*it)->begin;
        ++n;
    }
    const double avg = n ? sum / double(n) : 0;
    const double fps = windowNs > 0 ? double(n) * 1e9 / double(windowNs) : 0;
    text(8, y, format("%.0f fps", fps), frameColour(avg), 2.0f);
    std::string line = format("frame %.2f ms avg", avg) + format(" / %.2f max (1 s)", peak);
    if (!frames_.empty()) {
        const auto& last = *frames_.back();
        line += "   heap " + (pf::allocationsHooked() ? bytes(double(last.liveBytes)) : std::string("(no hooks)")) +
                "   resident " + bytes(double(last.residentBytes)) + "   allocs " +
                format("%.0f/s", double(allocs) * (windowNs > 0 ? 1e9 / double(windowNs) : 0)) + " (" +
                bytes(double(allocBytes)) + "/s)";
        line += format("   sampling %.2f ms", double(pf::interval().count()) / 1000.0);
    }
    text(140, y + 2, line, kText);
    text(140, y + 16,
         paused_ ? "PAUSED - Space resumes, <- -> step frames, click a bar"
                 : "Space pause  click a bar  <- ->  T trace  A all threads  S self/incl  +/- rate  C clear  Esc",
         paused_ ? kAccent : kSoft);
    if (!status_.empty() && SDL_GetTicks() < statusUntil_) text(140, y + 28, status_, kAccent);
}

float ProfilerWindow::drawFrameGraph(float y, float height) {
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    const float x = 8, width = float(w) - 16;
    fill(x, y, width, height, kPanel);
    graphRect_ = {x, y, width, height};
    if (frames_.empty()) { text(x + 8, y + 8, "sampling - no frames yet", kSoft); return y + height; }
    const std::size_t shown = std::min<std::size_t>(frames_.size(), std::size_t(std::max(1.0f, width)));
    std::vector<double> ms(shown);
    for (std::size_t i = 0; i < shown; ++i) ms[i] = frames_[frames_.size() - shown + i]->millis();
    auto sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    const double p99 = sorted[std::min(sorted.size() - 1, std::size_t(double(sorted.size()) * 0.99))];
    const double top = std::clamp(std::max(40.0, p99 * 1.25), 40.0, 250.0);
    const auto yOf = [&](double v) { return y + height - float(std::min(v, top) / top) * (height - 14); };
    for (const double line : {16.7, 33.3, 66.7}) {
        if (line > top) continue;
        fill(x, yOf(line), width, 1, kGrid);
        text(x + width - 64, yOf(line) - 10, format("%.1f ms", line), kSoft);
    }
    const float bar = width / float(shown);
    const auto sel = selected();
    for (std::size_t i = 0; i < shown; ++i) {
        const auto& f = frames_[frames_.size() - shown + i];
        SDL_Color c = frameColour(ms[i]);
        if (sel && f->index == sel->index && selectedIndex_ >= 0) c = {120, 170, 255, 255};
        fill(x + float(i) * bar, yOf(ms[i]), std::max(1.0f, bar - (bar > 3 ? 1.0f : 0.0f)), y + height - yOf(ms[i]), c);
    }
    text(x + 6, y + 4, format("last %.0f frames", double(shown)) + format(", 99%% under %.1f ms", p99), kSoft);
    const SDL_FPoint p{mouseX_, mouseY_};
    if (SDL_PointInRectFloat(&p, &graphRect_)) {
        const auto at = std::size_t(std::clamp((mouseX_ - x) / bar, 0.0f, float(shown - 1)));
        const auto& f = frames_[frames_.size() - shown + at];
        const std::string tip = "frame " + std::to_string(f->index) + format("  %.2f ms", f->millis()) + "  allocs " +
                                std::to_string(f->allocations) + "  " + bytes(double(f->allocatedBytes));
        fill(mouseX_ + 10, mouseY_ - 6, float(tip.size()) * kGlyph + 8, 14, {0, 0, 0, 220});
        text(mouseX_ + 14, mouseY_ - 3, tip, kText);
    }
    return y + height;
}

float ProfilerWindow::drawTimeline(float y, float height) {
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    const float x = 8, width = float(w) - 16;
    fill(x, y, width, height, kPanel);
    const auto f = selected();
    if (!f) return y + height;
    const double frameMs = f->millis();
    if (viewSpan_ <= 0) { viewStart_ = -frameMs * 0.02; viewSpan_ = frameMs * 1.04 + 0.01; }
    const float laneLabel = 140;
    timelineRect_ = {x + laneLabel, y + 18, width - laneLabel, height - 18};
    text(x + 6, y + 4, "frame " + std::to_string(f->index) + format("  %.2f ms  ", frameMs) +
                       std::to_string(f->samples.size()) + " samples   wheel zooms, drag pans, Shift+wheel scrolls lanes",
         kSoft);
    const double step = std::pow(10.0, std::floor(std::log10(std::max(viewSpan_ / 8.0, 1e-4))));
    for (double t = std::ceil(viewStart_ / step) * step; t < viewStart_ + viewSpan_; t += step) {
        const float px = timelineRect_.x + float((t - viewStart_) / viewSpan_) * timelineRect_.w;
        fill(px, timelineRect_.y, 1, timelineRect_.h, {40, 44, 52, 255});
        if (px > timelineRect_.x + 520) text(px + 2, y + 4, format(step < 0.01 ? "%.3f" : step < 1 ? "%.2f" : "%.0f", t), kSoft);
    }
    fill(timelineRect_.x + float((frameMs - viewStart_) / viewSpan_) * timelineRect_.w, timelineRect_.y, 1, timelineRect_.h, kAccent);
    // Lanes: the main thread, then every thread that ran in the frame, busiest first.
    const auto threads = pf::threads();
    std::map<std::uint16_t, std::size_t> running;
    for (const auto& s : f->samples) if (!s.waiting) ++running[s.thread];
    std::vector<std::uint16_t> lanes;
    for (std::uint16_t t = 0; t < threads.size(); ++t) if (threads[t].main) lanes.push_back(t);
    std::vector<std::pair<std::size_t, std::uint16_t>> busy;
    for (const auto& [t, n] : running) if (t >= threads.size() || !threads[t].main) busy.emplace_back(n, t);
    std::sort(busy.rbegin(), busy.rend());
    for (const auto& [n, t] : busy) lanes.push_back(t);
    const float row = 12;
    float ly = timelineRect_.y + 4 - laneScroll_;
    const pf::Bar* hover = nullptr;
    pf::Bar hovered;
    for (const auto thread : lanes) {
        const auto bars = pf::flame(*f, thread, 40);
        int depth = 0;
        for (const auto& b : bars) depth = std::max(depth, int(b.depth) + 1);
        const bool main = thread < threads.size() && threads[thread].main;
        const int rows = std::clamp(depth, 1, main ? 28 : 12);
        const float laneH = float(rows) * row + 8;
        if (ly > y + height) break;
        if (ly + laneH > timelineRect_.y) {
            fill(x, std::max(ly - 3, timelineRect_.y), width, 1, kGrid);
            if (ly >= timelineRect_.y)
                text(x + 6, ly + 2, fit(thread < threads.size() ? threads[thread].name : "?", laneLabel - 10), main ? kAccent : kText);
            const double share = f->samples.empty() ? 0 : double(running.count(thread) ? running[thread] : 0) *
                                                                  double(f->interval) * 1e-6;
            if (ly + 14 >= timelineRect_.y) text(x + 6, ly + 14, format("%.1f ms on CPU", share), kSoft);
            for (const auto& b : bars) {
                if (b.depth >= rows) continue;
                const double a = double(b.begin - f->begin) * 1e-6, e = double(b.end - f->begin) * 1e-6;
                if (e < viewStart_ || a > viewStart_ + viewSpan_) continue;
                const float px0 = timelineRect_.x + float(std::max(0.0, (a - viewStart_) / viewSpan_)) * timelineRect_.w;
                const float px1 = timelineRect_.x + float(std::min(1.0, (e - viewStart_) / viewSpan_)) * timelineRect_.w;
                const float by = ly + float(b.depth) * row;
                if (by < timelineRect_.y || by + row > y + height) continue;
                const float bw = std::max(1.0f, px1 - px0);
                fill(px0, by, bw, row - 1, b.waiting ? kWaiting : colourOf(b.name));
                if (bw > 24) text(px0 + 2, by + 2, fit(std::string(b.name) + format(" %.2f", e - a), bw - 4), {12, 12, 14, 255});
                if (mouseX_ >= px0 && mouseX_ < px0 + bw && mouseY_ >= by && mouseY_ < by + row - 1) {
                    hovered = b;
                    hover = &hovered;
                }
            }
        }
        ly += laneH;
    }
    if (hover) {
        const std::string tip = std::string(hover->name) + format("  %.3f ms", double(hover->end - hover->begin) * 1e-6);
        const float tw = float(tip.size()) * kGlyph + 8;
        const float tx = std::min(mouseX_ + 10, float(w) - tw - 4);
        fill(tx, mouseY_ + 12, tw, 14, {0, 0, 0, 230});
        text(tx + 4, mouseY_ + 15, tip, kText);
    }
    return y + height;
}

void ProfilerWindow::drawFunctions(float x, float y, float w, float h) {
    fill(x, y, w, h, kPanel);
    std::vector<std::shared_ptr<const pf::FrameRecord>> over;
    if (overSecond_) {
        std::int64_t ns = 0;
        for (auto it = frames_.rbegin(); it != frames_.rend() && ns < 1000000000; ++it) {
            over.push_back(*it);
            ns += (*it)->end - (*it)->begin;
        }
    } else if (const auto f = selected()) {
        over.push_back(f);
    }
    const auto threads = pf::threads();
    struct Row { const char* name = "?"; double self = 0, inclusive = 0; };
    std::unordered_map<std::uintptr_t, Row> rows;
    std::unordered_set<std::uintptr_t> seen;
    double total = 0, waiting = 0;
    for (const auto& f : over) {
        const double ms = double(f->interval) * 1e-6;
        for (const auto& s : f->samples) {
            const bool main = s.thread < threads.size() && threads[s.thread].main;
            if (!allThreads_ && !main) continue;
            // A waiting main thread keeps its stack: what the frame waits on
            // (the GPU's next image, a lock) is in the table too.
            if (s.waiting) waiting += ms;
            else total += ms;
            if (s.waiting && (!main || s.depth == 0)) continue;
            seen.clear();
            for (std::uint32_t k = 0; k < s.depth; ++k) {
                const auto sym = pf::symbolOf(f->frames[s.first + k]);
                auto& r = rows[sym.start];
                r.name = sym.name;
                if (k == 0) r.self += ms;
                if (seen.insert(sym.start).second) r.inclusive += ms;
            }
        }
    }
    std::vector<Row> list;
    list.reserve(rows.size());
    for (auto& [k, r] : rows) list.push_back(r);
    std::sort(list.begin(), list.end(), [&](const Row& a, const Row& b) { return bySelf_ ? a.self > b.self : a.inclusive > b.inclusive; });
    const double frames = double(std::max<std::size_t>(1, over.size()));
    text(x + 6, y + 5, std::string(overSecond_ ? "FUNCTIONS, LAST SECOND (ms a frame)" : "FUNCTIONS, THIS FRAME") +
                       (allThreads_ ? ", ALL THREADS" : ", MAIN THREAD") + (bySelf_ ? ", BY SELF" : ", BY INCLUSIVE") +
                       "   (click: frame/second)",
         kAccent);
    text(x + 6, y + 20, format("on CPU %.2f ms", total / frames) + format("   waiting %.2f ms", waiting / frames), kSoft);
    float ry = y + 36;
    text(x + w - 230, ry, "inclusive", kSoft);
    text(x + w - 130, ry, "self", kSoft);
    text(x + w - 60, ry, "%", kSoft);
    ry += 14;
    for (const auto& r : list) {
        if (ry + 12 > y + h) break;
        const double value = bySelf_ ? r.self : r.inclusive;
        if (value <= 0) break;
        fill(x + 4, ry - 1, 6, 9, colourOf(r.name));
        text(x + 14, ry, fit(r.name, w - 260), kText);
        text(x + w - 230, ry, format("%.3f", r.inclusive / frames), kText);
        text(x + w - 130, ry, format("%.3f", r.self / frames), r.self > 0 ? kText : kSoft);
        text(x + w - 60, ry, format("%.1f", total > 0 ? 100.0 * value / total : 0.0), kSoft);
        ry += 13;
    }
}

void ProfilerWindow::drawCounters(float x, float y, float width, float height) {
    fill(x, y, width, height, kPanel);
    text(x + 6, y + 5, "A FRAME (graph: the last 400)", kAccent);
    if (frames_.empty()) return;
    struct Series { const char* name; bool memory; std::vector<double> v; };
    const std::size_t shown = std::min<std::size_t>(frames_.size(), 400);
    const auto from = frames_.size() - shown;
    std::vector<Series> series;
    const auto make = [&](const char* name, bool memory, auto get) {
        Series s{name, memory, {}};
        for (std::size_t i = from; i < frames_.size(); ++i) s.v.push_back(get(*frames_[i]));
        series.push_back(std::move(s));
    };
    const auto threads = pf::threads();
    make("frame ms", false, [](const pf::FrameRecord& f) { return f.millis(); });
    make("main on CPU ms", false, [&](const pf::FrameRecord& f) {
        double ms = 0;
        for (const auto& s : f.samples)
            if (!s.waiting && s.thread < threads.size() && threads[s.thread].main) ms += double(f.interval) * 1e-6;
        return ms;
    });
    make("all threads on CPU ms", false, [](const pf::FrameRecord& f) {
        double ms = 0;
        for (const auto& s : f.samples) if (!s.waiting) ms += double(f.interval) * 1e-6;
        return ms;
    });
    make("allocations", false, [](const pf::FrameRecord& f) { return double(f.allocations); });
    make("allocated", true, [](const pf::FrameRecord& f) { return double(f.allocatedBytes); });
    make("frees", false, [](const pf::FrameRecord& f) { return double(f.frees); });
    make("heap live", true, [](const pf::FrameRecord& f) { return double(f.liveBytes); });
    make("resident", true, [](const pf::FrameRecord& f) { return double(f.residentBytes); });
    const auto sel = selected();
    std::size_t selAt = shown - 1;
    if (sel)
        for (std::size_t i = from; i < frames_.size(); ++i)
            if (frames_[i]->index == sel->index) selAt = i - from;
    float ry = y + 22;
    const float rowH = 34, graphX = x + 190, graphW = width - 198;
    for (const auto& s : series) {
        if (ry + rowH > y + height) break;
        double top = 0, sum = 0;
        for (const double v : s.v) { top = std::max(top, v); sum += v; }
        const double here = s.v.empty() ? 0 : s.v[std::min(selAt, s.v.size() - 1)];
        text(x + 6, ry + 2, s.name, kText);
        text(x + 6, ry + 13, s.memory ? bytes(here) : format(here >= 100 ? "%.0f" : "%.2f", here), kAccent);
        text(x + 6, ry + 24, s.memory ? "max " + bytes(top) : format("max %.1f", top) + format(" avg %.2f", sum / double(std::max<std::size_t>(1, s.v.size()))), kSoft);
        fill(graphX, ry, graphW, rowH - 3, {22, 24, 29, 255});
        if (top > 0) {
            const float bw = graphW / float(std::max<std::size_t>(1, s.v.size()));
            const SDL_Color c = colourOf(s.name);
            for (std::size_t i = 0; i < s.v.size(); ++i) {
                const float bh = float(s.v[i] / top) * (rowH - 4);
                if (bh > 0) fill(graphX + float(i) * bw, ry + rowH - 3 - bh, std::max(1.0f, bw), bh, i == selAt ? SDL_Color{255, 255, 255, 255} : c);
            }
        }
        ry += rowH;
    }
}

} // namespace ui
