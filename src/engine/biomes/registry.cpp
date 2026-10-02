#include "engine/biomes/registry.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>

#include <nlohmann/json.hpp>

namespace engine::biomes {
namespace fs = std::filesystem;
using json = nlohmann::json;

double Curve::at(double x) const {
    if (points.empty()) return x;
    if (x <= points.front().first) return points.front().second;
    for (std::size_t i = 1; i < points.size(); ++i)
        if (x <= points[i].first) {
            const auto [x0, y0] = points[i - 1];
            const auto [x1, y1] = points[i];
            return x1 > x0 ? y0 + (y1 - y0) * (x - x0) / (x1 - x0) : y1;
        }
    return points.back().second;
}

bool Category::changesGround() const {
    if (!slopeRock.empty() || !scree.empty() || !groundFoliage.empty() || !decals.empty()) return true;
    if (std::any_of(soils.begin(), soils.end(), [](const std::string& s) { return !s.empty(); })) return true;
    return tint != Rgb{1, 1, 1} || saturation != 1.0;
}

// Reads the files into a registry, collecting what is wrong with them.
struct RegistryReader {
    Registry& r;
    std::vector<Problem>& problems;
    std::string file;   // the one being read

    void problem(std::string what) { problems.push_back({file, std::move(what)}); }

    json read(const fs::path& path, const char* key) {
        file = path.filename().string();
        std::error_code ec;
        if (!fs::exists(path, ec)) return json::array();
        r.newest_ = std::max(r.newest_, fs::last_write_time(path, ec));
        std::ifstream in(path);
        auto j = json::parse(in, nullptr, false, true);
        if (j.is_discarded()) { problem("is not valid JSON"); return json::array(); }
        if (j.is_object() && j.contains(key)) j = j[key];
        if (!j.is_array()) { problem(std::string("has no \"") + key + "\" list"); return json::array(); }
        return j;
    }

    template <class T>
    T number(const json& j, const char* key, T fallback) {
        if (!j.contains(key)) return fallback;
        if (!j[key].is_number()) { problem(std::string("\"") + key + "\" is not a number"); return fallback; }
        return j[key].get<T>();
    }
    std::string text(const json& j, const char* key, std::string fallback = {}) {
        if (!j.contains(key)) return fallback;
        if (!j[key].is_string()) { problem(std::string("\"") + key + "\" is not a string"); return fallback; }
        return j[key].get<std::string>();
    }
    Rgb rgb(const json& j, const char* key, Rgb fallback) {
        if (!j.contains(key)) return fallback;
        const auto& a = j[key];
        if (!a.is_array() || a.size() != 3 || !a[0].is_number() || !a[1].is_number() || !a[2].is_number()) {
            problem(std::string("\"") + key + "\" is not three numbers");
            return fallback;
        }
        return {a[0].get<double>(), a[1].get<double>(), a[2].get<double>()};
    }
    std::array<double, 2> pair(const json& j, const char* key, std::array<double, 2> fallback) {
        if (!j.contains(key)) return fallback;
        const auto& a = j[key];
        if (!a.is_array() || a.size() != 2 || !a[0].is_number() || !a[1].is_number()) {
            problem(std::string("\"") + key + "\" is not two numbers");
            return fallback;
        }
        return {a[0].get<double>(), a[1].get<double>()};
    }
    // [["name", 0.5], ...] or {"name": 0.5, ...}
    Weighted weighted(const json& j, const char* key) {
        Weighted out;
        if (!j.contains(key)) return out;
        const auto& a = j[key];
        if (a.is_object()) {
            for (const auto& [name, v] : a.items()) {
                if (!v.is_number()) { problem(std::string("\"") + key + "\": " + name + " is not a number"); continue; }
                out.emplace_back(name, v.get<double>());
            }
            return out;
        }
        if (!a.is_array()) { problem(std::string("\"") + key + "\" is not a list"); return out; }
        for (const auto& e : a) {
            if (e.is_array() && e.size() == 2 && e[0].is_string() && e[1].is_number())
                out.emplace_back(e[0].get<std::string>(), e[1].get<double>());
            else if (e.is_string())
                out.emplace_back(e.get<std::string>(), 1.0);
            else
                problem(std::string("\"") + key + "\" entries are [name, number]");
        }
        return out;
    }

    Noise noiseObject(const json& j) {
        Noise n;
        n.name = text(j, "name");
        const auto kind = text(j, "kind", "fbm");
        const auto it = std::find(std::begin(kNoiseKindNames), std::end(kNoiseKindNames), kind);
        if (it == std::end(kNoiseKindNames)) problem("noise kind \"" + kind + "\" is not fbm, cellular, streaks or ridged");
        else n.kind = NoiseKind(it - std::begin(kNoiseKindNames));
        n.metres = number(j, "metres", 8.0);
        n.contrast = number(j, "contrast", 0.5);
        n.warp = number(j, "warp", 0.0);
        n.angle = number(j, "angle", 0.0);
        return n;
    }
    // A noise inline, or the name of one in noises.json.
    std::optional<Noise> noise(const json& j, const char* key) {
        if (!j.contains(key)) return std::nullopt;
        const auto& v = j[key];
        if (v.is_string()) {
            if (auto named = r.noiseNamed(v.get<std::string>())) return named;
            problem("noise \"" + v.get<std::string>() + "\" is not in noises.json");
            return std::nullopt;
        }
        if (v.is_object()) return noiseObject(v);
        problem(std::string("\"") + key + "\" is neither a noise nor a noise's name");
        return std::nullopt;
    }

    std::uint32_t id(const json& j) {
        if (!j.contains("id")) { problem(text(j, "name") + ": no \"id\""); return 0; }
        if (!j["id"].is_number_integer() || j["id"].get<long long>() < 0 || j["id"].get<long long>() > 255) {
            problem(text(j, "name") + ": \"id\" must be a whole number 0..255");
            return 0;
        }
        return j["id"].get<std::uint32_t>();
    }
};

namespace {

// Objects merge key by key, all else replaces.
json overlay(json base, const json& over) {
    if (!base.is_object() || !over.is_object()) return over;
    for (const auto& [k, v] : over.items()) {
        if (base.contains(k) && base[k].is_object() && v.is_object()) base[k] = overlay(base[k], v);
        else base[k] = v;
    }
    return base;
}

template <class T>
std::optional<std::size_t> indexNamed(const std::vector<T>& list, const std::string& name) {
    for (std::size_t i = 0; i < list.size(); ++i)
        if (list[i].name == name) return i;
    return std::nullopt;
}

template <class T>
const T* byId(const std::vector<T>& list, std::uint32_t id) {
    for (const auto& t : list)
        if (t.id == id) return &t;
    return nullptr;
}

std::mutex activeGuard;
std::shared_ptr<const Registry> activeRegistry;
std::uint64_t activeCount = 0;

} // namespace

std::shared_ptr<Registry> Registry::builtIn() {
    auto r = std::make_shared<Registry>();
    Category none;
    none.name = "default";
    r->categories_.push_back(none);
    return r;
}

std::shared_ptr<Registry> Registry::load(const fs::path& dir, std::vector<Problem>* problemsOut) {
    std::vector<Problem> scratch;
    std::vector<Problem>& problems = problemsOut ? *problemsOut : scratch;
    auto out = std::make_shared<Registry>();
    Registry& r = *out;
    r.dir_ = dir;
    RegistryReader rd{r, problems, {}};

    for (const auto& e : rd.read(dir / "layers.json", "layers")) {
        TextureLayer l;
        l.name = rd.text(e, "name");
        l.metres = rd.number(e, "metres", 2.0);
        l.role = rd.text(e, "role");
        r.layers_.push_back(l);
    }
    for (const auto& e : rd.read(dir / "noises.json", "noises")) r.noises_.push_back(rd.noiseObject(e));
    for (const auto& e : rd.read(dir / "soils.json", "soils")) {
        Soil s;
        s.name = rd.text(e, "name");
        s.layers = rd.weighted(e, "layers");
        s.noise = rd.noise(e, "noise").value_or(Noise{});
        s.tint = rd.rgb(e, "tint", s.tint);
        r.soils_.push_back(s);
    }
    for (const auto& e : rd.read(dir / "rocks.json", "rocks")) {
        Rock k;
        k.name = rd.text(e, "name");
        k.layer = rd.text(e, "layer");
        if (e.contains("strata") && e["strata"].is_object()) {
            k.strataMetres = rd.number(e["strata"], "metres", 0.0);
            k.strataTilt = rd.number(e["strata"], "tilt", 0.0);
        }
        k.scree = rd.text(e, "scree");
        k.tint = rd.rgb(e, "tint", k.tint);
        r.rocks_.push_back(k);
    }
    for (const auto& e : rd.read(dir / "foliage.json", "foliage")) {
        Foliage f;
        f.name = rd.text(e, "name");
        f.density = rd.number(e, "density", 1.0);
        f.height = rd.number(e, "height", 1.0);
        f.dryness = rd.number(e, "dryness", 0.0);
        f.tint = rd.rgb(e, "tint", f.tint);
        f.flowers = rd.number(e, "flowers", 1.0);
        r.foliage_.push_back(f);
    }
    for (const auto& e : rd.read(dir / "plants.json", "plants")) {
        Plant p;
        p.name = rd.text(e, "name");
        p.model = rd.text(e, "model");
        const auto h = rd.pair(e, "height", {1.0, 1.0});
        p.heightMin = h[0];
        p.heightMax = h[1];
        p.tint = rd.rgb(e, "tint", p.tint);
        r.plants_.push_back(p);
    }
    for (const auto& e : rd.read(dir / "props.json", "props")) {
        Prop p;
        p.name = rd.text(e, "name");
        p.model = rd.text(e, "model");
        const auto s = rd.pair(e, "scale", {1.0, 1.0});
        p.scaleMin = s[0];
        p.scaleMax = s[1];
        r.props_.push_back(p);
    }
    for (const auto& e : rd.read(dir / "decals.json", "decals")) {
        Decal d;
        d.name = rd.text(e, "name");
        const auto kind = rd.text(e, "kind", "speckle");
        const auto it = std::find(std::begin(kDecalKindNames), std::end(kDecalKindNames), kind);
        if (it == std::end(kDecalKindNames)) rd.problem(d.name + ": kind \"" + kind + "\" is not speckle, stain, streak or instance");
        else d.kind = DecalKind(it - std::begin(kDecalKindNames));
        d.cellMetres = rd.number(e, "cell_m", d.cellMetres);
        d.density = rd.number(e, "density", d.density);
        d.sizeMetres = rd.pair(e, "size_m", d.sizeMetres);
        if (e.contains("cluster") && e["cluster"].is_object()) {
            d.clusterMetres = rd.number(e["cluster"], "cell_m", 0.0);
            d.clusterShare = rd.number(e["cluster"], "share", 1.0);
        }
        d.colour = rd.rgb(e, "colour", d.colour);
        d.opacity = rd.number(e, "opacity", d.opacity);
        d.emissive = rd.number(e, "emissive", 0.0);
        d.metal = rd.number(e, "metal", 0.0);
        d.rough = rd.number(e, "rough", d.rough);
        d.slope = rd.pair(e, "slope", d.slope);
        d.underFoliage = rd.number(e, "under_foliage", d.underFoliage);
        d.fadeMetres = rd.pair(e, "fade_m", d.fadeMetres);
        d.edgeNoise = rd.number(e, "edge_noise", d.edgeNoise);
        d.nearWaterMetres = rd.number(e, "near_water_m", 0.0);
        d.alongWind = rd.text(e, "along") == "wind";
        d.angle = rd.number(e, "angle", 0.0);
        d.model = rd.text(e, "model");
        r.decals_.push_back(d);
    }
    for (const auto& e : rd.read(dir / "water_biomes.json", "water_biomes")) {
        WaterBiome w;
        w.name = rd.text(e, "name");
        w.id = rd.id(e);
        if (e.contains("colour")) w.colour = rd.rgb(e, "colour", {0, 0, 0});
        w.turbidity = rd.number(e, "turbidity", -1.0);
        w.scum = rd.number(e, "scum", 0.0);
        w.foam = rd.number(e, "foam", 1.0);
        w.emissive = rd.number(e, "emissive", 0.0);
        r.waters_.push_back(w);
    }
    for (const auto& e : rd.read(dir / "forest_biomes.json", "forest_biomes")) {
        ForestBiome f;
        f.name = rd.text(e, "name");
        f.id = rd.id(e);
        f.trees = rd.weighted(e, "trees");
        f.shrubs = rd.weighted(e, "shrubs");
        f.shrubDensity = rd.number(e, "shrub_density", f.shrubDensity);
        f.undergrowth = rd.text(e, "undergrowth");
        if (e.contains("floor") && e["floor"].is_object()) {
            f.floorSoil = rd.text(e["floor"], "soil");
            f.floorNoise = rd.noise(e["floor"], "noise");
        }
        f.props = rd.weighted(e, "props");
        f.fertility = rd.number(e, "fertility", 1.0);
        if (e.contains("density") && e["density"].is_object()) {
            f.density.by = rd.text(e["density"], "by");
            if (e["density"].contains("curve") && e["density"]["curve"].is_array()) {
                for (const auto& p : e["density"]["curve"]) {
                    if (p.is_array() && p.size() == 2 && p[0].is_number() && p[1].is_number())
                        f.density.points.emplace_back(p[0].get<double>(), p[1].get<double>());
                    else
                        rd.problem(f.name + ": a density curve point is [x, y]");
                }
            }
        }
        if (e.contains("clearings") && e["clearings"].is_object()) {
            f.clearings = rd.noise(e["clearings"], "noise");
            f.clearingShare = rd.number(e["clearings"], "share", 0.0);
        }
        if (e.contains("edge") && e["edge"].is_object()) {
            f.edgeShrubs = rd.number(e["edge"], "shrubs", 1.0);
            f.edgeMetres = rd.number(e["edge"], "metres", 0.0);
        }
        r.forests_.push_back(f);
    }
    for (const auto& e : rd.read(dir / "decor_biomes.json", "decor_biomes")) {
        DecorBiome d;
        d.name = rd.text(e, "name");
        d.id = rd.id(e);
        d.props = rd.weighted(e, "props");
        d.decals = rd.weighted(e, "decals");
        r.decors_.push_back(d);
    }

    // Categories: the raw entries first, then each over its parent.
    const auto categoriesFile = dir / "categories.json";
    std::map<std::string, json> raw;
    std::vector<std::string> order;
    {
        rd.file = "categories.json";
        std::error_code ec;
        json whole = json::object();
        if (fs::exists(categoriesFile, ec)) {
            r.newest_ = std::max(r.newest_, fs::last_write_time(categoriesFile, ec));
            std::ifstream in(categoriesFile);
            whole = json::parse(in, nullptr, false, true);
            if (whole.is_discarded()) { rd.problem("is not valid JSON"); whole = json::object(); }
        }
        const json list = whole.is_object() && whole.contains("categories") ? whole["categories"]
                          : whole.is_array()                                ? whole
                                                                            : json::array();
        for (const auto& e : list) {
            if (!e.is_object() || !e.contains("name") || !e["name"].is_string()) { rd.problem("a category without a name"); continue; }
            const auto name = e["name"].get<std::string>();
            if (raw.count(name)) { rd.problem("category \"" + name + "\" is written twice"); continue; }
            raw[name] = e;
            order.push_back(name);
            if (e.contains("inherit") && e["inherit"].is_string()) r.inherits_[name] = e["inherit"].get<std::string>();
        }
        if (whole.is_object() && whole.contains("derive") && whole["derive"].is_array())
            for (const auto& e : whole["derive"]) {
                DeriveRule rule;
                rule.category = rd.text(e, "category");
                rule.desertAbove = rd.number(e, "desert_above", rule.desertAbove);
                rule.moistureAbove = rd.number(e, "moisture_above", rule.moistureAbove);
                rule.moistureBelow = rd.number(e, "moisture_below", rule.moistureBelow);
                rule.temperatureBelow = rd.number(e, "temperature_below", rule.temperatureBelow);
                rule.temperatureAbove = rd.number(e, "temperature_above", rule.temperatureAbove);
                r.rules_.push_back(rule);
            }
    }
    if (!raw.count("default")) {
        raw["default"] = json{{"name", "default"}, {"id", 0}};
        order.insert(order.begin(), "default");
    }
    std::map<std::string, json> merged;
    std::function<const json*(const std::string&, std::set<std::string>&)> resolveJson =
            [&](const std::string& name, std::set<std::string>& path) -> const json* {
        if (auto it = merged.find(name); it != merged.end()) return &it->second;
        const auto it = raw.find(name);
        if (it == raw.end()) return nullptr;
        json self = it->second;
        json result = self;
        if (self.contains("inherit") && self["inherit"].is_string()) {
            const auto parent = self["inherit"].get<std::string>();
            if (path.count(parent)) {
                r.resolveProblems_.push_back({"categories.json", "inheritance cycle through \"" + name + "\""});
            } else if (!raw.count(parent)) {
                r.resolveProblems_.push_back({"categories.json", "\"" + name + "\" inherits \"" + parent + "\", which is not a category"});
            } else {
                path.insert(name);
                if (const json* base = resolveJson(parent, path)) {
                    json under = *base;
                    under.erase("id");
                    under.erase("name");
                    under.erase("inherit");
                    result = overlay(under, self);
                }
                path.erase(name);
            }
        }
        return &(merged[name] = result);
    };
    rd.file = "categories.json";
    for (const auto& name : order) {
        std::set<std::string> path;
        const json* jp = resolveJson(name, path);
        if (!jp) continue;
        const json& e = *jp;
        Category c;
        c.name = name;
        c.id = name == "default" && !e.contains("id") ? 0 : rd.id(e);
        c.inherit = rd.text(e, "inherit");
        if (e.contains("soils") && e["soils"].is_object())
            for (const auto& [cls, soil] : e["soils"].items()) {
                const auto at = std::find(std::begin(kClassNames), std::end(kClassNames), cls);
                if (at == std::end(kClassNames)) { rd.problem(name + ": soils name \"" + cls + "\", not a material class"); continue; }
                if (!soil.is_string()) { rd.problem(name + ": soils." + cls + " is not a soil's name"); continue; }
                c.soils[std::size_t(at - std::begin(kClassNames))] = soil.get<std::string>();
            }
        if (e.contains("slopes") && e["slopes"].is_object()) {
            c.slopeRock = rd.text(e["slopes"], "rock");
            c.scree = rd.text(e["slopes"], "scree");
            c.steepFrom = rd.number(e["slopes"], "steep_from", c.steepFrom);
        }
        c.groundFoliage = rd.text(e, "ground_foliage");
        c.decals = rd.weighted(e, "decals");
        if (e.contains("defaults") && e["defaults"].is_object()) {
            c.forest = rd.text(e["defaults"], "forest");
            c.water = rd.text(e["defaults"], "water");
            c.decor = rd.text(e["defaults"], "decor");
        }
        if (e.contains("look") && e["look"].is_object()) {
            c.tint = rd.rgb(e["look"], "tint", c.tint);
            c.saturation = rd.number(e["look"], "saturation", c.saturation);
        }
        if (e.contains("climate") && e["climate"].is_object()) {
            const auto& k = e["climate"];
            c.climateZone = rd.text(k, "zone");
            if (k.contains("warmth")) c.warmth = rd.number(k, "warmth", 0.55);
            if (k.contains("fertility")) c.fertility = rd.number(k, "fertility", 0.5);
        }
        if (e.contains("controls") && e["controls"].is_object()) {
            const auto& k = e["controls"];
            const auto opt = [&](const char* key) -> std::optional<double> {
                if (!k.contains(key)) return std::nullopt;
                return rd.number(k, key, 0.0);
            };
            c.controls.moisture = opt("moisture");
            c.controls.forest = opt("forest");
            c.controls.mountain = opt("mountain");
            c.controls.erosion = opt("erosion");
        }
        r.categories_.push_back(c);
    }
    std::sort(r.categories_.begin(), r.categories_.end(), [](const Category& a, const Category& b) { return a.id < b.id; });
    return out;
}

std::vector<Problem> Registry::validate(const fs::path& assets) const {
    std::vector<Problem> out = resolveProblems_;
    const auto bad = [&](const char* file, std::string what) { out.push_back({file, std::move(what)}); };
    const auto uniqueNames = [&](const char* file, const auto& list) {
        std::set<std::string> seen;
        for (const auto& t : list) {
            if (t.name.empty()) bad(file, "an entry has no name");
            else if (!seen.insert(t.name).second) bad(file, "\"" + t.name + "\" is named twice");
        }
    };
    const auto uniqueIds = [&](const char* file, const auto& list) {
        std::map<std::uint32_t, std::string> seen;
        for (const auto& t : list) {
            if (t.id > 255) bad(file, t.name + ": id " + std::to_string(t.id) + " is past 255");
            const auto [it, fresh] = seen.emplace(t.id, t.name);
            if (!fresh) bad(file, "id " + std::to_string(t.id) + " is both \"" + it->second + "\" and \"" + t.name + "\"");
        }
    };
    uniqueNames("layers.json", layers_);
    uniqueNames("noises.json", noises_);
    uniqueNames("soils.json", soils_);
    uniqueNames("rocks.json", rocks_);
    uniqueNames("foliage.json", foliage_);
    uniqueNames("plants.json", plants_);
    uniqueNames("props.json", props_);
    uniqueNames("decals.json", decals_);
    uniqueNames("categories.json", categories_);
    uniqueNames("forest_biomes.json", forests_);
    uniqueNames("water_biomes.json", waters_);
    uniqueNames("decor_biomes.json", decors_);
    uniqueIds("categories.json", categories_);
    uniqueIds("forest_biomes.json", forests_);
    uniqueIds("water_biomes.json", waters_);
    uniqueIds("decor_biomes.json", decors_);
    for (const auto* list : {&forests_}) for (const auto& t : *list) if (t.id == 0) bad("forest_biomes.json", t.name + ": id 0 is \"as the ground says\"");
    for (const auto& t : waters_) if (t.id == 0) bad("water_biomes.json", t.name + ": id 0 is \"as the ground says\"");
    for (const auto& t : decors_) if (t.id == 0) bad("decor_biomes.json", t.name + ": id 0 is \"as the ground says\"");
    if (const auto* d = category(0u); !d || d->name != "default") bad("categories.json", "id 0 must be the category \"default\"");

    const auto layerKnown = [&](const char* file, const std::string& who, const std::string& layer) {
        if (!textureLayer(layer)) { bad(file, who + ": texture layer \"" + layer + "\" is not in layers.json"); return; }
        if (!assets.empty()) {
            std::error_code ec;
            const auto dir = assets / "terrain" / "ph" / layer;
            if (!fs::exists(dir / ("ph_" + layer + "_albedo.png"), ec))
                bad(file, who + ": texture layer \"" + layer + "\" has no packed albedo in " + dir.string());
        }
    };
    const auto sharesSum = [&](const char* file, const std::string& who, const Weighted& w) {
        if (w.empty()) return;
        double total = 0;
        for (const auto& [n, s] : w) {
            if (!(s >= 0)) bad(file, who + ": the share of \"" + n + "\" is negative");
            total += s;
        }
        if (std::fabs(total - 1.0) > 1e-3) bad(file, who + ": shares sum to " + std::to_string(total) + ", not 1");
    };
    for (const auto& l : layers_)
        if (!(l.metres > 0)) bad("layers.json", l.name + ": metres must be positive");
    if (layers_.size() > 64) bad("layers.json", "more than 64 texture layers");
    for (std::size_t i = 0; i < std::size(kBuiltInLayers); ++i)
        if (i >= layers_.size() || layers_[i].name != kBuiltInLayers[i])
            bad("layers.json", "layer " + std::to_string(i) + " must be the engine's own \"" + kBuiltInLayers[i] + "\"");
    for (const auto& s : soils_) {
        if (s.layers.empty() || s.layers.size() > 3) bad("soils.json", s.name + ": a soil is one to three layers");
        for (const auto& [l, share] : s.layers) layerKnown("soils.json", s.name, l);
        sharesSum("soils.json", s.name, s.layers);
        if (!(s.noise.metres > 0)) bad("soils.json", s.name + ": noise metres must be positive");
    }
    for (const auto& n : noises_)
        if (!(n.metres > 0)) bad("noises.json", n.name + ": metres must be positive");
    for (const auto& k : rocks_) {
        layerKnown("rocks.json", k.name, k.layer);
        if (!k.scree.empty() && !soilIndex(k.scree)) bad("rocks.json", k.name + ": scree \"" + k.scree + "\" is not a soil");
    }
    for (const auto& p : plants_)
        if (p.model.empty()) bad("plants.json", p.name + ": no model");
    for (const auto& p : props_)
        if (p.model.empty()) bad("props.json", p.name + ": no model");
    for (const auto& d : decals_) {
        if (d.kind == DecalKind::Instance && d.model.empty()) bad("decals.json", d.name + ": an instance decal needs a model");
        if (!(d.cellMetres > 0)) bad("decals.json", d.name + ": cell_m must be positive");
        if (d.sizeMetres[0] > d.sizeMetres[1]) bad("decals.json", d.name + ": size_m is [smallest, largest]");
    }
    const auto foliageKnown = [&](const char* file, const std::string& who, const std::string& f) {
        if (!f.empty() && f != "none" && !foliageIndex(f)) bad(file, who + ": foliage \"" + f + "\" is not in foliage.json");
    };
    for (const auto& f : forests_) {
        sharesSum("forest_biomes.json", f.name + " trees", f.trees);
        sharesSum("forest_biomes.json", f.name + " shrubs", f.shrubs);
        for (const auto& [p, s] : f.trees) if (!plantIndex(p)) bad("forest_biomes.json", f.name + ": tree \"" + p + "\" is not a plant");
        for (const auto& [p, s] : f.shrubs) if (!plantIndex(p)) bad("forest_biomes.json", f.name + ": shrub \"" + p + "\" is not a plant");
        for (const auto& [p, s] : f.props) if (!propIndex(p)) bad("forest_biomes.json", f.name + ": prop \"" + p + "\" is not in props.json");
        foliageKnown("forest_biomes.json", f.name, f.undergrowth);
        if (!f.floorSoil.empty() && !soilIndex(f.floorSoil)) bad("forest_biomes.json", f.name + ": floor soil \"" + f.floorSoil + "\" is not a soil");
        if (f.clearingShare < 0 || f.clearingShare > 1) bad("forest_biomes.json", f.name + ": clearings share is 0..1");
    }
    for (const auto& d : decors_) {
        for (const auto& [p, s] : d.props) if (!propIndex(p)) bad("decor_biomes.json", d.name + ": prop \"" + p + "\" is not in props.json");
        for (const auto& [p, s] : d.decals) if (!decalIndex(p)) bad("decor_biomes.json", d.name + ": decal \"" + p + "\" is not in decals.json");
    }
    for (const auto& c : categories_) {
        for (std::size_t k = 0; k < kClasses; ++k)
            if (!c.soils[k].empty() && !soilIndex(c.soils[k]))
                bad("categories.json", c.name + ": soils." + kClassNames[k] + " \"" + c.soils[k] + "\" is not a soil");
        if (!c.slopeRock.empty() && !rockIndex(c.slopeRock)) bad("categories.json", c.name + ": slopes.rock \"" + c.slopeRock + "\" is not a rock");
        if (!c.scree.empty() && !soilIndex(c.scree)) bad("categories.json", c.name + ": slopes.scree \"" + c.scree + "\" is not a soil");
        foliageKnown("categories.json", c.name, c.groundFoliage);
        for (const auto& [d, w] : c.decals) {
            const auto i = decalIndex(d);
            if (!i) bad("categories.json", c.name + ": decal \"" + d + "\" is not in decals.json");
        }
        std::size_t shaderDecals = 0;
        for (const auto& [d, w] : c.decals)
            if (const auto i = decalIndex(d); i && decals_[*i].kind != DecalKind::Instance) ++shaderDecals;
        if (shaderDecals > 4) bad("categories.json", c.name + ": more than four shader decals");
        if (!c.forest.empty() && !indexNamed(forests_, c.forest)) bad("categories.json", c.name + ": defaults.forest \"" + c.forest + "\" is not a forest biome");
        if (!c.water.empty() && !indexNamed(waters_, c.water)) bad("categories.json", c.name + ": defaults.water \"" + c.water + "\" is not a water biome");
        if (!c.decor.empty() && !indexNamed(decors_, c.decor)) bad("categories.json", c.name + ": defaults.decor \"" + c.decor + "\" is not a decor biome");
        for (const auto& v : {c.controls.moisture, c.controls.forest, c.controls.mountain, c.controls.erosion})
            if (v && (*v < 0 || *v > 1)) bad("categories.json", c.name + ": controls are 0..1");
        if (c.saturation < 0) bad("categories.json", c.name + ": saturation is not negative");
        static const std::set<std::string> zones{"steppe", "taiga", "temperate_forest", "tropical_forest", "mediterranean",
                                                 "savanna", "tundra", "alpine", "desert", "ice"};
        if (!c.climateZone.empty() && !zones.count(c.climateZone))
            bad("categories.json", c.name + ": climate zone \"" + c.climateZone + "\" is not one the world knows");
        for (const auto& v : {c.warmth, c.fertility})
            if (v && (*v < 0 || *v > 1)) bad("categories.json", c.name + ": climate warmth and fertility are 0..1");
    }
    for (const auto& rule : rules_) {
        if (!category(rule.category)) bad("categories.json", "derive: \"" + rule.category + "\" is not a category");
        if (rule.unconditional()) bad("categories.json", "derive: the rule for \"" + rule.category + "\" has no condition and would take all the land");
    }
    return out;
}

std::optional<int> Registry::textureLayer(const std::string& name) const {
    for (std::size_t i = 0; i < layers_.size(); ++i)
        if (layers_[i].name == name) return int(i);
    return std::nullopt;
}

const Category* Registry::category(std::uint32_t id) const { return byId(categories_, id); }
const Category* Registry::category(const std::string& name) const {
    const auto i = indexNamed(categories_, name);
    return i ? &categories_[*i] : nullptr;
}
const ForestBiome* Registry::forestBiome(std::uint32_t id) const { return byId(forests_, id); }
const WaterBiome* Registry::waterBiome(std::uint32_t id) const { return byId(waters_, id); }
const DecorBiome* Registry::decorBiome(std::uint32_t id) const { return byId(decors_, id); }
std::optional<std::size_t> Registry::soilIndex(const std::string& n) const { return indexNamed(soils_, n); }
std::optional<std::size_t> Registry::rockIndex(const std::string& n) const { return indexNamed(rocks_, n); }
std::optional<std::size_t> Registry::foliageIndex(const std::string& n) const { return indexNamed(foliage_, n); }
std::optional<std::size_t> Registry::plantIndex(const std::string& n) const { return indexNamed(plants_, n); }
std::optional<std::size_t> Registry::propIndex(const std::string& n) const { return indexNamed(props_, n); }
std::optional<std::size_t> Registry::decalIndex(const std::string& n) const { return indexNamed(decals_, n); }
std::optional<Noise> Registry::noiseNamed(const std::string& n) const {
    const auto i = indexNamed(noises_, n);
    if (!i) return std::nullopt;
    return noises_[*i];
}

std::map<std::string, std::uint32_t> Registry::legend(Layer layer) const {
    std::map<std::string, std::uint32_t> out;
    const auto take = [&](const auto& list) { for (const auto& t : list) out[t.name] = t.id; };
    switch (layer) {
        case Layer::Ground: take(categories_); break;
        case Layer::Forest: take(forests_); break;
        case Layer::Water: take(waters_); break;
        case Layer::Decor: take(decors_); break;
    }
    return out;
}

std::uint32_t Registry::resolve(Layer layer, std::uint32_t categoryId, std::uint32_t layerId) const {
    if (layer == Layer::Ground) return categoryId;
    if (layerId != 0) return layerId;
    const Category* c = category(categoryId);
    if (!c) return 0;
    const auto idOf = [](const auto& list, const std::string& name) -> std::uint32_t {
        if (name.empty()) return 0;
        const auto i = indexNamed(list, name);
        return i ? list[*i].id : 0;
    };
    switch (layer) {
        case Layer::Forest: return idOf(forests_, c->forest);
        case Layer::Water: return idOf(waters_, c->water);
        case Layer::Decor: return idOf(decors_, c->decor);
        default: return 0;
    }
}

std::optional<double> Registry::control(std::uint32_t categoryId, const std::string& channel) const {
    const Category* c = category(categoryId);
    if (!c) return std::nullopt;
    if (channel == "moisture" || channel == "moisture_bias") return c->controls.moisture;
    if (channel == "forest" || channel == "forest_bias") return c->controls.forest;
    if (channel == "mountain" || channel == "mountain_strength") return c->controls.mountain;
    if (channel == "erosion" || channel == "erosion_strength") return c->controls.erosion;
    return std::nullopt;
}

std::shared_ptr<const Registry> active() {
    const std::lock_guard<std::mutex> lock(activeGuard);
    return activeRegistry;
}

void setActive(std::shared_ptr<const Registry> registry) {
    const std::lock_guard<std::mutex> lock(activeGuard);
    activeRegistry = std::move(registry);
    ++activeCount;
}

std::uint64_t activeGeneration() {
    const std::lock_guard<std::mutex> lock(activeGuard);
    return activeCount;
}

fs::path defaultDirectory() {
    fs::path content = "content";
    std::error_code ec;
    for (int up = 0; up < 6 && !fs::exists(content, ec); ++up) content = ".." / content;
    return content / "config" / "terrain";
}

} // namespace engine::biomes
