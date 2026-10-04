#include "engine/environment/recipe.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

namespace engine::environment {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

struct Reader {
    const std::string& file;
    std::vector<RecipeProblem>* problems;
    std::string where;

    void problem(const std::string& what) const {
        if (problems) problems->push_back({file, where.empty() ? what : where + ": " + what});
    }
    // Every key of `j` must be one of `known`: a misspelt key that silently
    // falls back to its default is the worst kind of content bug.
    void keys(const json& j, std::initializer_list<const char*> known) const {
        for (auto it = j.begin(); it != j.end(); ++it) {
            const bool ok = std::any_of(known.begin(), known.end(), [&](const char* k) { return it.key() == k; });
            if (!ok) problem("unknown key \"" + it.key() + "\"");
        }
    }
    template <class T>
    void number(const json& j, const char* key, T& out) const {
        auto it = j.find(key);
        if (it == j.end()) return;
        if (!it->is_number()) { problem(std::string(key) + " must be a number"); return; }
        out = it->get<T>();
    }
    void flag(const json& j, const char* key, bool& out) const {
        auto it = j.find(key);
        if (it == j.end()) return;
        if (!it->is_boolean()) { problem(std::string(key) + " must be true or false"); return; }
        out = it->get<bool>();
    }
    void text(const json& j, const char* key, std::string& out) const {
        auto it = j.find(key);
        if (it == j.end()) return;
        if (!it->is_string()) { problem(std::string(key) + " must be a string"); return; }
        out = it->get<std::string>();
    }
    template <class E, std::size_t N>
    void choice(const json& j, const char* key, E& out, const char* const (&names)[N]) const {
        auto it = j.find(key);
        if (it == j.end()) return;
        if (it->is_string()) {
            const auto s = it->get<std::string>();
            for (std::size_t i = 0; i < N; ++i)
                if (s == names[i]) { out = E(i); return; }
        }
        std::string all;
        for (std::size_t i = 0; i < N; ++i) all += (i ? ", " : "") + std::string(names[i]);
        problem(std::string(key) + " must be one of " + all);
    }
    void names(const json& j, const char* key, std::vector<std::string>& out) const {
        auto it = j.find(key);
        if (it == j.end()) return;
        if (it->is_string()) { out.push_back(it->get<std::string>()); return; }
        if (!it->is_array()) { problem(std::string(key) + " must be a name or a list of names"); return; }
        for (const auto& v : *it) {
            if (v.is_string()) out.push_back(v.get<std::string>());
            else problem(std::string(key) + " holds a non-string");
        }
    }
    // [min, max] or a single number for both.
    void span(const json& j, const char* key, double& lo, double& hi) const {
        auto it = j.find(key);
        if (it == j.end()) return;
        if (it->is_number()) { lo = hi = it->get<double>(); return; }
        if (it->is_array() && it->size() == 2 && (*it)[0].is_number() && (*it)[1].is_number()) {
            lo = (*it)[0].get<double>(); hi = (*it)[1].get<double>();
            return;
        }
        problem(std::string(key) + " must be a number or [min, max]");
    }
    void span(const json& j, const char* key, int& lo, int& hi) const {
        double a = lo, b = hi;
        span(j, key, a, b);
        lo = int(std::lround(a)); hi = int(std::lround(b));
    }
};

std::vector<ModelChoice> readModels(const Reader& r, const json& j) {
    std::vector<ModelChoice> out;
    auto it = j.find("models");
    if (it == j.end()) it = j.find("model");
    if (it == j.end()) return out;
    const auto one = [&](const json& v) {
        if (v.is_string()) out.push_back({v.get<std::string>(), 1.0});
        else if (v.is_array() && v.size() == 2 && v[0].is_string() && v[1].is_number())
            out.push_back({v[0].get<std::string>(), v[1].get<double>()});
        else r.problem("a model is a name or [name, weight]");
    };
    if (it->is_array() && !(it->size() == 2 && (*it)[0].is_string() && (*it)[1].is_number())) {
        for (const auto& v : *it) one(v);
    } else if (it->is_object()) {
        for (auto m = it->begin(); m != it->end(); ++m) {
            if (m->is_number()) out.push_back({m.key(), m->get<double>()});
            else r.problem("model weights must be numbers");
        }
    } else {
        one(*it);
    }
    return out;
}

void readPlacement(Reader r, const json& j, Placement& p) {
    r.where += ".placement";
    r.keys(j, {"zones", "exclude_zones", "min_zone_weight", "fields", "source", "channel_classes",
               "density_per_km2", "min_spacing", "clearance", "chance", "length", "scale", "align", "avoid_water",
               "painted", "away_from_painted"});
    r.names(j, "zones", p.zones);
    r.names(j, "exclude_zones", p.excludeZones);
    r.number(j, "min_zone_weight", p.minZoneWeight);
    if (auto it = j.find("fields"); it != j.end()) {
        if (!it->is_object()) r.problem("fields must be {name: [min, max]}");
        else for (auto f = it->begin(); f != it->end(); ++f) {
            FieldRange range;
            range.name = f.key();
            double lo = range.min, hi = range.max;
            if (f->is_array() && f->size() == 2) {
                if ((*f)[0].is_number()) lo = (*f)[0].get<double>();
                if ((*f)[1].is_number()) hi = (*f)[1].get<double>();
            } else {
                r.problem("field " + f.key() + " must be [min, max] (null for open)");
            }
            range.min = float(lo); range.max = float(hi);
            if (auto id = fieldId(range.name)) range.field = *id;
            else r.problem("unknown field \"" + range.name + "\"");
            p.fields.push_back(range);
        }
    }
    r.choice(j, "source", p.source, kSourceNames);
    if (auto it = j.find("channel_classes"); it != j.end()) {
        std::vector<std::string> names;
        r.names(j, "channel_classes", names);
        for (const auto& n : names) {
            auto at = std::find_if(std::begin(kChannelClassNames), std::end(kChannelClassNames),
                                   [&](const char* c) { return n == c; });
            if (at == std::end(kChannelClassNames)) r.problem("unknown channel class \"" + n + "\"");
            else p.channelClasses.push_back(ChannelClass(at - std::begin(kChannelClassNames)));
        }
    }
    r.number(j, "density_per_km2", p.densityPerKm2);
    r.number(j, "min_spacing", p.minSpacing);
    r.number(j, "clearance", p.clearance);
    r.number(j, "chance", p.chance);
    r.span(j, "length", p.lengthMin, p.lengthMax);
    r.span(j, "scale", p.scaleMin, p.scaleMax);
    r.choice(j, "align", p.align, kAlignNames);
    r.flag(j, "avoid_water", p.avoidWater);
    r.names(j, "painted", p.painted);
    r.flag(j, "away_from_painted", p.awayFromPainted);
    if (p.source == PlacementSource::Painted && p.painted.empty())
        r.problem("source painted needs \"painted\": the legend names it stands on");
}

TerrainOp readOp(Reader r, const json& j) {
    TerrainOp op;
    r.keys(j, {"op", "outer_width", "outer_depth", "inner_width", "inner_depth", "water_width", "asymmetry",
               "taper", "height", "width", "breaks", "amphitheatre", "radius", "exponent", "edge_noise",
               "harden", "terrace_step", "terrace_sharpness", "blend"});
    r.choice(j, "op", op.kind, kTerrainOpNames);
    if (!j.contains("op")) r.problem("a terrain operation needs \"op\"");
    r.number(j, "outer_width", op.outerWidth);
    r.number(j, "outer_depth", op.outerDepth);
    r.number(j, "inner_width", op.innerWidth);
    r.number(j, "inner_depth", op.innerDepth);
    r.number(j, "water_width", op.waterWidth);
    r.number(j, "asymmetry", op.asymmetry);
    r.number(j, "taper", op.taper);
    r.number(j, "height", op.height);
    r.number(j, "width", op.width);
    r.number(j, "breaks", op.breaks);
    r.number(j, "amphitheatre", op.amphitheatre);
    r.span(j, "radius", op.radiusA, op.radiusB);
    r.number(j, "exponent", op.exponent);
    r.number(j, "edge_noise", op.edgeNoise);
    r.flag(j, "harden", op.harden);
    r.number(j, "terrace_step", op.terraceStep);
    r.number(j, "terrace_sharpness", op.terraceSharpness);
    r.number(j, "blend", op.blend);
    return op;
}

MaskWrite readMask(Reader r, const json& j) {
    MaskWrite m;
    r.keys(j, {"channel", "shape", "mode", "value", "radius", "falloff", "length", "spread"});
    r.text(j, "channel", m.channel);
    if (m.channel.empty()) r.problem("a mask write needs \"channel\"");
    r.choice(j, "shape", m.shape, kMaskShapeNames);
    r.choice(j, "mode", m.mode, kMaskModeNames);
    r.number(j, "value", m.value);
    r.number(j, "radius", m.radius);
    r.number(j, "falloff", m.falloff);
    r.number(j, "length", m.length);
    r.number(j, "spread", m.spread);
    return m;
}

ScatterRule readScatter(Reader r, const json& j) {
    ScatterRule s;
    r.keys(j, {"primitive", "models", "model", "count", "radius", "spread", "scale", "size_falloff", "sink",
               "align_to_ground", "min_spacing", "tag"});
    r.choice(j, "primitive", s.primitive, kScatterPrimitiveNames);
    if (!j.contains("primitive")) r.problem("a scatter rule needs \"primitive\"");
    s.models = readModels(r, j);
    r.span(j, "count", s.countMin, s.countMax);
    r.number(j, "radius", s.radius);
    r.number(j, "spread", s.spread);
    r.span(j, "scale", s.scaleMin, s.scaleMax);
    r.number(j, "size_falloff", s.sizeFalloff);
    r.number(j, "sink", s.sink);
    r.flag(j, "align_to_ground", s.alignToGround);
    r.number(j, "min_spacing", s.minSpacing);
    r.text(j, "tag", s.tag);
    return s;
}

MeshRule readMesh(Reader r, const json& j) {
    MeshRule m;
    r.keys(j, {"model", "form", "attach", "count", "spacing", "offset", "sink", "scale", "align_to_normal",
               "length", "depth", "height", "roughness"});
    if (j.contains("model")) {
        auto models = readModels(r, j);
        if (!models.empty()) m.model = models.front();
    }
    r.choice(j, "form", m.form, kProceduralFormNames);
    if (m.model.name.empty() && m.form == ProceduralForm::None) r.problem("a mesh rule needs \"model\" or \"form\"");
    r.choice(j, "attach", m.attach, kMeshAttachNames);
    r.number(j, "count", m.count);
    r.number(j, "spacing", m.spacing);
    r.number(j, "offset", m.offset);
    r.number(j, "sink", m.sink);
    r.span(j, "scale", m.scaleMin, m.scaleMax);
    r.flag(j, "align_to_normal", m.alignToNormal);
    r.number(j, "length", m.length);
    r.number(j, "depth", m.depth);
    r.number(j, "height", m.height);
    r.number(j, "roughness", m.roughness);
    return m;
}

void readComposition(Reader r, const json& j, Composition& c) {
    r.where += ".composition";
    r.keys(j, {"negative_space", "elongation", "reveal_width", "reveal_length", "secondaries",
               "secondary_radius", "secondary_scale"});
    r.number(j, "negative_space", c.negativeSpace);
    r.number(j, "elongation", c.elongation);
    r.number(j, "reveal_width", c.revealWidth);
    r.number(j, "reveal_length", c.revealLength);
    r.number(j, "secondaries", c.secondaries);
    r.number(j, "secondary_radius", c.secondaryRadius);
    r.number(j, "secondary_scale", c.secondaryScale);
}

template <class T, class F>
void list(const Reader& r, const json& j, const char* key, std::vector<T>& out, F read) {
    auto it = j.find(key);
    if (it == j.end()) return;
    if (!it->is_array()) { r.problem(std::string(key) + " must be a list"); return; }
    for (std::size_t i = 0; i < it->size(); ++i) {
        Reader item = r;
        item.where += "." + std::string(key) + "[" + std::to_string(i) + "]";
        if (!(*it)[i].is_object()) { item.problem("must be an object"); continue; }
        out.push_back(read(item, (*it)[i]));
    }
}

std::optional<FeatureRecipe> readRecipe(const json& j, const std::string& file, std::vector<RecipeProblem>* problems) {
    Reader r{file, problems, {}};
    if (!j.is_object()) { r.problem("a recipe must be an object"); return std::nullopt; }
    FeatureRecipe recipe;
    recipe.file = file;
    r.text(j, "name", recipe.name);
    if (recipe.name.empty()) { r.problem("a recipe needs \"name\""); return std::nullopt; }
    r.where = recipe.name;
    r.keys(j, {"name", "scale", "age", "placement", "terrain", "meshes", "materials", "masks", "scatter",
               "composition", "comment"});
    r.choice(j, "scale", recipe.scale, kScaleNames);
    r.choice(j, "age", recipe.age, kAgeNames);
    if (auto it = j.find("placement"); it != j.end() && it->is_object()) readPlacement(r, *it, recipe.placement);
    list(r, j, "terrain", recipe.terrain, readOp);
    list(r, j, "meshes", recipe.meshes, readMesh);
    list(r, j, "masks", recipe.masks, readMask);
    list(r, j, "materials", recipe.masks, readMask);
    list(r, j, "scatter", recipe.scatter, readScatter);
    if (auto it = j.find("composition"); it != j.end() && it->is_object()) readComposition(r, *it, recipe.composition);
    return recipe;
}

} // namespace

double FeatureRecipe::reach() const {
    double reach = 0;
    for (const auto& op : terrain) {
        switch (op.kind) {
            case TerrainOpKind::CarveProfile: reach = std::max(reach, op.outerWidth * 0.5 + op.taper); break;
            case TerrainOpKind::Step: reach = std::max(reach, op.width + op.amphitheatre + op.blend); break;
            default: reach = std::max(reach, std::max(op.radiusA, op.radiusB) * (1 + op.edgeNoise) + op.blend); break;
        }
    }
    for (const auto& m : masks) reach = std::max(reach, m.radius + m.falloff + (m.shape == MaskShape::Fan || m.shape == MaskShape::Corridor ? m.length : 0.0));
    for (const auto& s : scatter) reach = std::max(reach, s.radius * std::max(1.0, composition.elongation));
    for (const auto& m : meshes) reach = std::max(reach, std::abs(m.offset) + m.length);
    reach = std::max(reach, composition.negativeSpace);
    reach = std::max(reach, composition.revealLength);
    if (composition.secondaries > 0) reach += composition.secondaryRadius;
    return reach;
}

std::optional<FeatureRecipe> parseRecipe(const std::string& text, const std::string& file,
                                         std::vector<RecipeProblem>* problems) {
    json j = json::parse(text, nullptr, false, true);
    if (j.is_discarded()) {
        if (problems) problems->push_back({file, "not valid JSON"});
        return std::nullopt;
    }
    return readRecipe(j, file, problems);
}

std::vector<FeatureRecipe> loadRecipes(const fs::path& dir, std::vector<RecipeProblem>* problems) {
    std::vector<FeatureRecipe> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().extension() == ".json") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    std::set<std::string> seen;
    for (const auto& path : files) {
        std::ifstream in(path);
        std::stringstream buffer;
        buffer << in.rdbuf();
        const auto name = path.lexically_relative(dir).generic_string();
        json j = json::parse(buffer.str(), nullptr, false, true);
        if (j.is_discarded()) {
            if (problems) problems->push_back({name, "not valid JSON"});
            continue;
        }
        const json* items = &j;
        if (j.is_object() && j.contains("recipes")) items = &j["recipes"];
        const auto take = [&](const json& item) {
            if (auto recipe = readRecipe(item, name, problems)) {
                if (!seen.insert(recipe->name).second) {
                    if (problems) problems->push_back({name, "recipe \"" + recipe->name + "\" is defined twice"});
                    return;
                }
                out.push_back(std::move(*recipe));
            }
        };
        if (items->is_array()) for (const auto& item : *items) take(item);
        else take(*items);
    }
    return out;
}

std::string recipeToJson(const FeatureRecipe& r) {
    json j;
    j["name"] = r.name;
    j["scale"] = kScaleNames[int(r.scale)];
    j["age"] = kAgeNames[int(r.age)];
    auto& p = j["placement"];
    p["zones"] = r.placement.zones;
    p["exclude_zones"] = r.placement.excludeZones;
    p["min_zone_weight"] = r.placement.minZoneWeight;
    for (const auto& f : r.placement.fields) p["fields"][f.name] = {f.min, f.max};
    p["source"] = kSourceNames[int(r.placement.source)];
    for (auto c : r.placement.channelClasses) p["channel_classes"].push_back(kChannelClassNames[int(c)]);
    p["density_per_km2"] = r.placement.densityPerKm2;
    p["min_spacing"] = r.placement.minSpacing;
    p["clearance"] = r.placement.clearance;
    p["chance"] = r.placement.chance;
    p["length"] = {r.placement.lengthMin, r.placement.lengthMax};
    p["scale"] = {r.placement.scaleMin, r.placement.scaleMax};
    p["align"] = kAlignNames[int(r.placement.align)];
    p["avoid_water"] = r.placement.avoidWater;
    if (!r.placement.painted.empty()) p["painted"] = r.placement.painted;
    if (r.placement.awayFromPainted) p["away_from_painted"] = true;
    for (const auto& op : r.terrain) {
        json o;
        o["op"] = kTerrainOpNames[int(op.kind)];
        o["outer_width"] = op.outerWidth; o["outer_depth"] = op.outerDepth;
        o["inner_width"] = op.innerWidth; o["inner_depth"] = op.innerDepth;
        o["water_width"] = op.waterWidth; o["asymmetry"] = op.asymmetry; o["taper"] = op.taper;
        o["height"] = op.height; o["width"] = op.width; o["breaks"] = op.breaks; o["amphitheatre"] = op.amphitheatre;
        o["radius"] = {op.radiusA, op.radiusB}; o["exponent"] = op.exponent; o["edge_noise"] = op.edgeNoise;
        o["harden"] = op.harden; o["terrace_step"] = op.terraceStep; o["terrace_sharpness"] = op.terraceSharpness;
        o["blend"] = op.blend;
        j["terrain"].push_back(o);
    }
    for (const auto& m : r.masks) {
        j["masks"].push_back({{"channel", m.channel}, {"shape", kMaskShapeNames[int(m.shape)]},
                              {"mode", kMaskModeNames[int(m.mode)]}, {"value", m.value}, {"radius", m.radius},
                              {"falloff", m.falloff}, {"length", m.length}, {"spread", m.spread}});
    }
    const auto models = [](const std::vector<ModelChoice>& ms) {
        json out = json::array();
        for (const auto& m : ms) out.push_back({m.name, m.weight});
        return out;
    };
    for (const auto& s : r.scatter) {
        j["scatter"].push_back({{"primitive", kScatterPrimitiveNames[int(s.primitive)]}, {"models", models(s.models)},
                                {"count", {s.countMin, s.countMax}}, {"radius", s.radius}, {"spread", s.spread},
                                {"scale", {s.scaleMin, s.scaleMax}}, {"size_falloff", s.sizeFalloff},
                                {"sink", s.sink}, {"align_to_ground", s.alignToGround},
                                {"min_spacing", s.minSpacing}, {"tag", s.tag}});
    }
    for (const auto& m : r.meshes) {
        json o{{"form", kProceduralFormNames[int(m.form)]}, {"attach", kMeshAttachNames[int(m.attach)]},
               {"count", m.count}, {"spacing", m.spacing}, {"offset", m.offset}, {"sink", m.sink},
               {"scale", {m.scaleMin, m.scaleMax}}, {"align_to_normal", m.alignToNormal},
               {"length", m.length}, {"depth", m.depth}, {"height", m.height}, {"roughness", m.roughness}};
        if (!m.model.name.empty()) o["model"] = json::array({m.model.name, m.model.weight});
        j["meshes"].push_back(o);
    }
    const auto& c = r.composition;
    j["composition"] = {{"negative_space", c.negativeSpace}, {"elongation", c.elongation},
                        {"reveal_width", c.revealWidth}, {"reveal_length", c.revealLength},
                        {"secondaries", c.secondaries}, {"secondary_radius", c.secondaryRadius},
                        {"secondary_scale", c.secondaryScale}};
    return j.dump(2);
}

} // namespace engine::environment
