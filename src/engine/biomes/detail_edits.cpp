#include "engine/biomes/detail_edits.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>

#include <nlohmann/json.hpp>

namespace engine::biomes {
namespace fs = std::filesystem;
namespace {

std::int64_t floorDiv(std::int64_t a, std::int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

bool parseStem(const std::string& stem, std::int64_t& cx, std::int64_t& cy) {
    long long a = 0, b = 0;
    char tail = 0;
    if (std::sscanf(stem.c_str(), "%lld_%lld%c", &a, &b, &tail) != 2) return false;
    cx = a;
    cy = b;
    return true;
}

} // namespace

std::string DetailEdits::fileName(std::int64_t cx, std::int64_t cy) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%+05lld_%+05lld.json", static_cast<long long>(cx), static_cast<long long>(cy));
    return buf;
}

DetailEdits::Key DetailEdits::chunkOf(double x, double y) {
    return {floorDiv(std::int64_t(std::floor(x)), kChunkMetres), floorDiv(std::int64_t(std::floor(y)), kChunkMetres)};
}

DetailEdits::Chunk& DetailEdits::at(const Key& key) {
    dirty_.insert(key);
    ++revision_;
    return chunks_[key];
}

DetailEdits DetailEdits::load(const fs::path& sourceRoot, std::string* why) {
    DetailEdits out;
    std::error_code ec;
    const auto dir = sourceRoot / "details";
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".json") continue;
        std::int64_t cx = 0, cy = 0;
        if (!parseStem(entry.path().stem().string(), cx, cy)) continue;
        std::ifstream in(entry.path());
        const auto j = nlohmann::json::parse(in, nullptr, false);
        if (!j.is_object()) {
            if (why) *why = entry.path().string() + " is not a details file";
            continue;
        }
        Chunk c;
        for (const auto& p : j.value("pinned", nlohmann::json::array())) {
            PinnedDetail d;
            d.id = p.value("id", std::string());
            d.kind = p.value("kind", std::string("prop"));
            d.name = p.value("name", std::string());
            d.x = p.value("x", 0.0);
            d.y = p.value("y", 0.0);
            d.yaw = p.value("yaw", 0.0);
            d.scale = p.value("scale", 1.0);
            if (!d.id.empty() && !d.name.empty()) c.pinned.push_back(std::move(d));
        }
        for (const auto& r : j.value("removed", nlohmann::json::array()))
            if (r.is_string()) c.removed.insert(std::stoull(r.get<std::string>(), nullptr, 16));
        if (j.contains("density") && j["density"].is_object())
            for (const auto& [cell, v] : j["density"].items()) {
                int i = 0, k = 0;
                if (std::sscanf(cell.c_str(), "%d,%d", &i, &k) == 2 && v.is_number()) c.density[{i, k}] = v.get<float>();
            }
        c.nextPin = j.value("next_pin", std::uint32_t(c.pinned.size()));
        if (!c.empty()) out.chunks_[{cx, cy}] = std::move(c);
    }
    return out;
}

bool DetailEdits::save(const fs::path& sourceRoot, std::string* why) {
    std::error_code ec;
    const auto dir = sourceRoot / "details";
    for (const auto& key : dirty_) {
        const auto file = dir / fileName(key.first, key.second);
        const auto it = chunks_.find(key);
        if (it == chunks_.end() || it->second.empty()) {
            fs::remove(file, ec);
            if (it != chunks_.end()) chunks_.erase(it);
            continue;
        }
        fs::create_directories(dir, ec);
        const Chunk& c = it->second;
        nlohmann::json j = nlohmann::json::object();
        j["format"] = "campfire.details";
        j["version"] = 1;
        j["next_pin"] = c.nextPin;
        auto pinned = nlohmann::json::array();
        for (const auto& d : c.pinned)
            pinned.push_back({{"id", d.id}, {"kind", d.kind}, {"name", d.name}, {"x", d.x}, {"y", d.y},
                              {"yaw", d.yaw}, {"scale", d.scale}});
        j["pinned"] = pinned;
        auto removed = nlohmann::json::array();
        for (const auto id : c.removed) {
            char buf[24];
            std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(id));
            removed.push_back(buf);
        }
        j["removed"] = removed;
        j["density"] = nlohmann::json::object();
        for (const auto& [cell, v] : c.density) j["density"][std::to_string(cell.first) + "," + std::to_string(cell.second)] = v;
        const auto partial = fs::path(file.string() + ".partial");
        {
            std::ofstream out(partial, std::ios::trunc);
            out << j.dump(1);
            if (!out) { if (why) *why = "cannot write " + partial.string(); return false; }
        }
        fs::rename(partial, file, ec);
        if (ec) { if (why) *why = ec.message(); return false; }
    }
    dirty_.clear();
    return true;
}

std::string DetailEdits::pin(PinnedDetail detail) {
    const auto key = chunkOf(detail.x, detail.y);
    Chunk& c = at(key);
    detail.id = "pin:" + fileName(key.first, key.second).substr(0, 11) + ":" + std::to_string(c.nextPin++);
    c.pinned.push_back(detail);
    return detail.id;
}

bool DetailEdits::unpin(const std::string& id) {
    for (auto& [key, c] : chunks_) {
        const auto it = std::find_if(c.pinned.begin(), c.pinned.end(), [&](const PinnedDetail& d) { return d.id == id; });
        if (it == c.pinned.end()) continue;
        at(key).pinned.erase(it);
        return true;
    }
    return false;
}

void DetailEdits::remove(std::uint64_t derivedId, double x, double y) { at(chunkOf(x, y)).removed.insert(derivedId); }

void DetailEdits::restore(std::uint64_t derivedId, double x, double y) {
    const auto key = chunkOf(x, y);
    if (chunks_.count(key)) at(key).removed.erase(derivedId);
}

void DetailEdits::setDensity(double x, double y, double multiplier) {
    const auto key = chunkOf(x, y);
    const std::pair<std::int32_t, std::int32_t> cell{
            std::int32_t(std::floor((x - double(key.first * kChunkMetres)) / kDensityCell)),
            std::int32_t(std::floor((y - double(key.second * kChunkMetres)) / kDensityCell))};
    Chunk& c = at(key);
    if (std::fabs(multiplier - 1.0) < 1e-3) c.density.erase(cell);
    else c.density[cell] = float(std::clamp(multiplier, 0.0, 8.0));
}

void DetailEdits::brushDensity(double x, double y, double radius, double by) {
    if (!(radius > 0)) return;
    const double step = kDensityCell;
    for (double v = std::floor((y - radius) / step) * step; v <= y + radius; v += step)
        for (double u = std::floor((x - radius) / step) * step; u <= x + radius; u += step) {
            const double cx = u + step / 2, cy = v + step / 2;
            const double d = std::hypot(cx - x, cy - y) / radius;
            if (d >= 1) continue;
            const double fall = 1 - d * d * (3 - 2 * d);
            setDensity(cx, cy, density(cx, cy) * std::pow(by, fall));
        }
}

bool DetailEdits::removed(std::uint64_t derivedId, double x, double y) const {
    const auto it = chunks_.find(chunkOf(x, y));
    return it != chunks_.end() && it->second.removed.count(derivedId) > 0;
}

double DetailEdits::density(double x, double y) const {
    const auto key = chunkOf(x, y);
    const auto it = chunks_.find(key);
    if (it == chunks_.end()) return 1.0;
    const std::pair<std::int32_t, std::int32_t> cell{
            std::int32_t(std::floor((x - double(key.first * kChunkMetres)) / kDensityCell)),
            std::int32_t(std::floor((y - double(key.second * kChunkMetres)) / kDensityCell))};
    const auto d = it->second.density.find(cell);
    return d == it->second.density.end() ? 1.0 : double(d->second);
}

std::vector<PinnedDetail> DetailEdits::pinnedIn(double x0, double y0, double x1, double y1) const {
    std::vector<PinnedDetail> out;
    const auto low = chunkOf(x0, y0), high = chunkOf(x1 - 1e-6, y1 - 1e-6);
    for (auto it = chunks_.lower_bound({low.first, std::numeric_limits<std::int64_t>::min()});
         it != chunks_.end() && it->first.first <= high.first; ++it) {
        if (it->first.second < low.second || it->first.second > high.second) continue;
        for (const auto& d : it->second.pinned)
            if (d.x >= x0 && d.x < x1 && d.y >= y0 && d.y < y1) out.push_back(d);
    }
    return out;
}

std::uint64_t DetailEdits::removedFingerprint() const {
    std::uint64_t h = 0x9e3779b97f4a7c15ULL;
    const auto mix = [&](std::uint64_t v) {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h *= 0xbf58476d1ce4e5b9ULL;
    };
    // std::map and std::set: the order is the keys', the same every session.
    for (const auto& [key, chunk] : chunks_)
        for (const auto id : chunk.removed) mix(id);
    return h;
}

} // namespace engine::biomes
