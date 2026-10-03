#include "engine/environment/style.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace engine::environment {

namespace fs = std::filesystem;
using nlohmann::json;

std::shared_ptr<StyleTable> StyleTable::load(const fs::path& file, std::vector<StyleProblem>* problems) {
    const auto name = file.filename().string();
    const auto problem = [&](const std::string& what) { if (problems) problems->push_back({name, what}); };
    std::ifstream in(file);
    if (!in) { problem("cannot be read"); return nullptr; }
    std::stringstream text;
    text << in.rdbuf();
    json j = json::parse(text.str(), nullptr, false, true);
    if (j.is_discarded() || !j.is_object()) { problem("not a JSON object"); return nullptr; }
    auto t = std::make_shared<StyleTable>();
    t->directory_ = file.parent_path();
    if (!j.contains("rows") || !j["rows"].is_array()) { problem("needs \"rows\": the names of the rows, in order"); return nullptr; }
    for (const auto& r : j["rows"]) {
        if (!r.is_string()) { problem("a row name must be a string"); continue; }
        t->rowNames_.push_back(r.get<std::string>());
    }
    if (auto it = j.find("blend_seconds"); it != j.end() && it->is_number()) t->blendSeconds_ = std::max(0.0, it->get<double>());
    const json* list = j.contains("regions") ? &j["regions"] : j.contains("profiles") ? &j["profiles"] : nullptr;
    if (!list || !list->is_array() || list->empty()) { problem("needs \"regions\" or \"profiles\", a non-empty list"); return nullptr; }
    for (const auto& e : *list) {
        StyleEntry entry;
        if (!e.is_object() || !e.contains("name") || !e["name"].is_string()) { problem("every entry needs a name"); continue; }
        entry.name = e["name"].get<std::string>();
        entry.rows.assign(t->rowNames_.size(), Row{0, 0, 0, 0});
        if (auto m = e.find("match"); m != e.end() && m->is_object()) {
            for (const char* key : {"categories", "zones"}) {
                auto& out = std::string(key) == "categories" ? entry.categories : entry.zones;
                if (auto it = m->find(key); it != m->end() && it->is_array())
                    for (const auto& v : *it) if (v.is_string()) out.push_back(v.get<std::string>());
            }
        }
        if (auto rows = e.find("rows"); rows != e.end() && rows->is_object()) {
            for (auto r = rows->begin(); r != rows->end(); ++r) {
                const auto at = std::find(t->rowNames_.begin(), t->rowNames_.end(), r.key());
                if (at == t->rowNames_.end()) { problem(entry.name + ": unknown row \"" + r.key() + "\""); continue; }
                Row v{0, 0, 0, 0};
                if (r->is_number()) v[0] = r->get<float>();
                else if (r->is_array() && r->size() <= 4) for (std::size_t k = 0; k < r->size(); ++k) v[k] = (*r)[k].get<float>();
                else { problem(entry.name + "." + r.key() + ": a number or up to four"); continue; }
                entry.rows[std::size_t(at - t->rowNames_.begin())] = v;
            }
        }
        if (auto lut = e.find("lut"); lut != e.end() && lut->is_string()) entry.lut = lut->get<std::string>();
        t->entries_.push_back(std::move(entry));
    }
    if (t->entries_.empty()) { problem("no usable entries"); return nullptr; }
    if (auto d = j.find("default"); d != j.end() && d->is_string()) {
        const auto want = d->get<std::string>();
        auto at = std::find_if(t->entries_.begin(), t->entries_.end(), [&](const StyleEntry& e) { return e.name == want; });
        if (at == t->entries_.end()) problem("default \"" + want + "\" is not an entry");
        else t->default_ = std::size_t(at - t->entries_.begin());
    }
    // Rows an entry left out are the default's. Mark "left out" by reading the
    // file again rather than by a sentinel value a row could legitimately hold.
    const auto& fallback = t->entries_[t->default_].rows;
    for (std::size_t i = 0; i < list->size() && i < t->entries_.size(); ++i) {
        if (i == t->default_) continue;
        const auto& rows = (*list)[i].contains("rows") ? (*list)[i]["rows"] : json::object();
        for (std::size_t r = 0; r < t->rowNames_.size(); ++r)
            if (!rows.contains(t->rowNames_[r])) t->entries_[i].rows[r] = fallback[r];
    }
    return t;
}

std::optional<std::size_t> StyleTable::row(std::string_view name) const {
    for (std::size_t i = 0; i < rowNames_.size(); ++i)
        if (rowNames_[i] == name) return i;
    return std::nullopt;
}

std::size_t StyleTable::match(std::string_view category, std::string_view zone) const {
    const auto has = [](const std::vector<std::string>& list, std::string_view v) {
        return !v.empty() && std::find(list.begin(), list.end(), v) != list.end();
    };
    for (std::size_t i = 0; i < entries_.size(); ++i)
        if (has(entries_[i].zones, zone) && has(entries_[i].categories, category)) return i;
    for (std::size_t i = 0; i < entries_.size(); ++i)
        if (has(entries_[i].zones, zone) && entries_[i].categories.empty()) return i;
    for (std::size_t i = 0; i < entries_.size(); ++i)
        if (has(entries_[i].categories, category) && entries_[i].zones.empty()) return i;
    return default_;
}

std::vector<Row> StyleTable::packed() const {
    std::vector<Row> out;
    out.reserve(entries_.size() * rowNames_.size());
    for (const auto& e : entries_) out.insert(out.end(), e.rows.begin(), e.rows.end());
    return out;
}

std::optional<Lut3d> loadCube(const fs::path& file, std::string* problem) {
    std::ifstream in(file);
    if (!in) { if (problem) *problem = "cannot read " + file.string(); return std::nullopt; }
    Lut3d lut;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream words(line);
        std::string first;
        words >> first;
        if (first == "LUT_3D_SIZE") { words >> lut.size; continue; }
        if (first == "TITLE" || first == "DOMAIN_MIN" || first == "DOMAIN_MAX" || first == "LUT_1D_SIZE") {
            if (first == "LUT_1D_SIZE") { if (problem) *problem = "1D LUTs are not supported"; return std::nullopt; }
            continue;
        }
        std::array<float, 3> c{};
        try { c[0] = std::stof(first); } catch (...) { continue; }
        words >> c[1] >> c[2];
        lut.rgb.push_back(c);
    }
    if (lut.size < 2 || lut.rgb.size() != std::size_t(lut.size) * lut.size * lut.size) {
        if (problem) *problem = "LUT_3D_SIZE does not match the entries";
        return std::nullopt;
    }
    return lut;
}

std::array<float, 3> Lut3d::apply(std::array<float, 3> c) const {
    if (empty()) return c;
    const float m = float(size - 1);
    std::array<int, 3> i0{}, i1{};
    std::array<float, 3> f{};
    for (int k = 0; k < 3; ++k) {
        const float v = std::clamp(c[k], 0.0f, 1.0f) * m;
        i0[k] = std::min(int(v), size - 1);
        i1[k] = std::min(i0[k] + 1, size - 1);
        f[k] = v - float(i0[k]);
    }
    const auto at = [&](int r, int g, int b) { return rgb[(std::size_t(b) * size + g) * size + r]; };
    std::array<float, 3> out{};
    for (int k = 0; k < 3; ++k) {
        const float c00 = at(i0[0], i0[1], i0[2])[k] * (1 - f[0]) + at(i1[0], i0[1], i0[2])[k] * f[0];
        const float c10 = at(i0[0], i1[1], i0[2])[k] * (1 - f[0]) + at(i1[0], i1[1], i0[2])[k] * f[0];
        const float c01 = at(i0[0], i0[1], i1[2])[k] * (1 - f[0]) + at(i1[0], i0[1], i1[2])[k] * f[0];
        const float c11 = at(i0[0], i1[1], i1[2])[k] * (1 - f[0]) + at(i1[0], i1[1], i1[2])[k] * f[0];
        out[k] = (c00 * (1 - f[1]) + c10 * f[1]) * (1 - f[2]) + (c01 * (1 - f[1]) + c11 * f[1]) * f[2];
    }
    return out;
}

GradeBlend::GradeBlend(std::shared_ptr<const StyleTable> table) : table_(std::move(table)) {
    weights_.assign(table_ ? table_->entries().size() : 0, 0.0f);
    if (!weights_.empty()) weights_[table_->defaultEntry()] = 1;
}

void GradeBlend::snap(std::size_t target) {
    std::fill(weights_.begin(), weights_.end(), 0.0f);
    if (target < weights_.size()) weights_[target] = 1;
}

void GradeBlend::step(std::size_t target, double seconds) {
    if (weights_.empty() || target >= weights_.size()) return;
    const double span = table_->blendSeconds();
    if (span <= 0) { snap(target); return; }
    // Exponential ease: the same feel at any frame rate.
    const float k = float(1 - std::exp(-seconds * 3.0 / span));
    float sum = 0;
    for (std::size_t i = 0; i < weights_.size(); ++i) {
        weights_[i] += ((i == target ? 1.0f : 0.0f) - weights_[i]) * k;
        sum += weights_[i];
    }
    if (sum > 0) for (auto& w : weights_) w /= sum;
}

std::vector<Row> GradeBlend::rows() const {
    std::vector<Row> out(table_ ? table_->rowNames().size() : 0, Row{0, 0, 0, 0});
    if (!table_) return out;
    for (std::size_t e = 0; e < weights_.size(); ++e) {
        if (weights_[e] <= 0) continue;
        const auto& rows = table_->entries()[e].rows;
        for (std::size_t r = 0; r < out.size(); ++r)
            for (int k = 0; k < 4; ++k) out[r][k] += rows[r][k] * weights_[e];
    }
    return out;
}

GradeBlend::Pair GradeBlend::strongest() const {
    Pair p;
    float wa = -1, wb = -1;
    for (std::size_t i = 0; i < weights_.size(); ++i) {
        if (weights_[i] > wa) { wb = wa; p.b = p.a; wa = weights_[i]; p.a = i; }
        else if (weights_[i] > wb) { wb = weights_[i]; p.b = i; }
    }
    if (wb < 0) { p.b = p.a; wb = 0; }
    p.t = wa + wb > 0 ? wb / (wa + wb) : 0;
    return p;
}

} // namespace engine::environment
