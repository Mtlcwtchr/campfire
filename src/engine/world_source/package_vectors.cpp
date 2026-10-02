#include "engine/world_source/package_vectors.hpp"

#include <algorithm>

#include "engine/world_store/atomic_file.hpp"

namespace engine::world_source {
namespace {

bool fail(std::string* why, std::string text) {
    if (why) *why = std::move(text);
    return false;
}

std::optional<nlohmann::json> readJson(const std::filesystem::path& file, std::string* why) {
    const auto bytes = world_store::readFileBytes(file, why);
    if (!bytes) return std::nullopt;
    auto j = nlohmann::json::parse(bytes->begin(), bytes->end(), nullptr, false);
    if (j.is_discarded()) { fail(why, file.string() + ": not valid JSON"); return std::nullopt; }
    return j;
}

bool writeJson(const std::filesystem::path& file, const nlohmann::json& j, std::string* why) {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    return world_store::writeFileAtomic(file, j.dump(1), why);
}

std::optional<Ring> ringFrom(const nlohmann::json& j, std::string* why, const std::string& id) {
    Ring r;
    if (!j.contains("points") || !j["points"].is_array()) { fail(why, id + ": a ring has no points"); return std::nullopt; }
    for (const auto& p : j["points"]) {
        if (!p.is_array() || p.size() < 2 || !p[0].is_number() || !p[1].is_number()) {
            fail(why, id + ": a point is not [x, y]");
            return std::nullopt;
        }
        r.points.push_back({p[0].get<double>(), p[1].get<double>()});
    }
    if (j.contains("values")) {
        for (const auto& [name, values] : j["values"].items()) {
            auto v = values.get<std::vector<double>>();
            if (v.size() != r.points.size()) { fail(why, id + ": values." + name + " is not one per vertex"); return std::nullopt; }
            r.values[name] = std::move(v);
        }
    }
    return r;
}

nlohmann::json ringJson(const Ring& r) {
    nlohmann::json j{{"points", r.points}};
    if (!r.values.empty()) j["values"] = r.values;
    return j;
}

} // namespace

std::optional<std::vector<Feature>> readVectorFile(const std::filesystem::path& file, const std::string& kind,
                                                   std::string* why) {
    const auto j = readJson(file, why);
    if (!j) return std::nullopt;
    if (j->value("version", 1) > 1) { fail(why, file.string() + ": from a newer version"); return std::nullopt; }
    std::vector<Feature> out;
    for (const auto& fj : j->value("features", nlohmann::json::array())) {
        Feature f;
        f.kind = kind;
        f.id = fj.value("id", std::string());
        if (f.id.empty()) { fail(why, file.string() + ": a feature has no stable id"); return std::nullopt; }
        const auto geometry = fj.value("geometry", std::string("line"));
        if (geometry == "line") f.geometry = Geometry::Line;
        else if (geometry == "polygon") f.geometry = Geometry::Polygon;
        else if (geometry == "point") f.geometry = Geometry::Point;
        else { fail(why, f.id + ": unknown geometry " + geometry); return std::nullopt; }
        if (fj.contains("rings")) {
            for (const auto& rj : fj["rings"]) {
                auto r = ringFrom(rj, why, f.id);
                if (!r) return std::nullopt;
                f.rings.push_back(std::move(*r));
            }
        } else {
            auto r = ringFrom(fj, why, f.id);
            if (!r) return std::nullopt;
            f.rings.push_back(std::move(*r));
        }
        if (f.rings.empty() || f.rings[0].points.empty()) { fail(why, f.id + ": no points"); return std::nullopt; }
        if (f.geometry == Geometry::Line && f.rings[0].points.size() < 2) { fail(why, f.id + ": a line needs two points"); return std::nullopt; }
        if (f.geometry == Geometry::Polygon)
            for (const auto& r : f.rings)
                if (r.points.size() < 3) { fail(why, f.id + ": a polygon ring needs three points"); return std::nullopt; }
        f.properties = fj.value("properties", nlohmann::json::object());
        out.push_back(std::move(f));
    }
    return out;
}

bool writeVectorFile(const std::filesystem::path& file, const std::string& kind, const std::vector<Feature>& features,
                     std::string* why) {
    nlohmann::json j{{"version", 1}, {"kind", kind}, {"features", nlohmann::json::array()}};
    auto sorted = features;
    std::sort(sorted.begin(), sorted.end(), [](const Feature& a, const Feature& b) { return a.id < b.id; });
    for (const auto& f : sorted) {
        nlohmann::json fj{{"id", f.id}, {"geometry", geometryName(f.geometry)}, {"properties", f.properties}};
        if (f.rings.size() == 1 && f.geometry != Geometry::Polygon) {
            fj["points"] = f.rings[0].points;
            if (!f.rings[0].values.empty()) fj["values"] = f.rings[0].values;
        } else {
            fj["rings"] = nlohmann::json::array();
            for (const auto& r : f.rings) fj["rings"].push_back(ringJson(r));
        }
        j["features"].push_back(fj);
    }
    return writeJson(file, j, why);
}

std::optional<std::vector<Feature>> readPoiFile(const std::filesystem::path& file, std::string* why) {
    const auto j = readJson(file, why);
    if (!j) return std::nullopt;
    std::vector<Feature> out;
    for (const auto& pj : j->value("poi", nlohmann::json::array())) {
        Feature f;
        f.kind = "poi";
        f.geometry = Geometry::Point;
        f.id = pj.value("id", std::string());
        if (f.id.empty()) { fail(why, file.string() + ": a point of interest has no stable id"); return std::nullopt; }
        const auto position = pj.value("position", std::vector<double>{});
        if (position.size() < 2) { fail(why, f.id + ": position must be [x, y] or [x, y, z]"); return std::nullopt; }
        f.rings.push_back({{{position[0], position[1]}}, {}});
        f.properties = {{"type", pj.value("type", std::string())},
                        {"z", position.size() > 2 ? position[2] : 0.0},
                        {"yaw_deg", pj.value("yaw_deg", 0.0)},
                        {"scale", pj.value("scale", 1.0)},
                        {"metadata", pj.value("metadata", nlohmann::json::object())}};
        out.push_back(std::move(f));
    }
    return out;
}

bool writePoiFile(const std::filesystem::path& file, const std::vector<Feature>& poi, std::string* why) {
    nlohmann::json j{{"version", 1}, {"poi", nlohmann::json::array()}};
    auto sorted = poi;
    std::sort(sorted.begin(), sorted.end(), [](const Feature& a, const Feature& b) { return a.id < b.id; });
    for (const auto& f : sorted) {
        const auto& p = f.rings.at(0).points.at(0);
        j["poi"].push_back({{"id", f.id}, {"type", f.properties.value("type", std::string())},
                            {"position", {p[0], p[1], f.properties.value("z", 0.0)}},
                            {"yaw_deg", f.properties.value("yaw_deg", 0.0)},
                            {"scale", f.properties.value("scale", 1.0)},
                            {"metadata", f.properties.value("metadata", nlohmann::json::object())}});
    }
    return writeJson(file, j, why);
}

} // namespace engine::world_source
