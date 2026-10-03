#include "engine/environment/catalogue.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace engine::environment {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::vector<MaskChannel> loadMasks(const fs::path& file, std::vector<CatalogueProblem>* problems) {
    std::vector<MaskChannel> out;
    std::error_code ec;
    if (!fs::exists(file, ec)) return out;
    std::ifstream in(file);
    std::stringstream text;
    text << in.rdbuf();
    const auto problem = [&](const std::string& what) { if (problems) problems->push_back({"masks.json", what}); };
    json j = json::parse(text.str(), nullptr, false, true);
    if (j.is_discarded() || !j.is_object() || !j.contains("channels") || !j["channels"].is_array()) {
        problem("expected {\"channels\": [...]}");
        return out;
    }
    for (const auto& c : j["channels"]) {
        MaskChannel m;
        if (c.is_string()) m.name = c.get<std::string>();
        else if (c.is_object() && c.contains("name") && c["name"].is_string()) {
            m.name = c["name"].get<std::string>();
            if (c.contains("colour") && c["colour"].is_array() && c["colour"].size() == 3)
                for (int k = 0; k < 3; ++k) m.debugColour[k] = c["colour"][k].get<float>();
        } else {
            problem("a channel is a name or {\"name\": ..., \"colour\": [r, g, b]}");
            continue;
        }
        if (std::any_of(out.begin(), out.end(), [&](const MaskChannel& o) { return o.name == m.name; })) {
            problem("channel \"" + m.name + "\" twice");
            continue;
        }
        out.push_back(m);
    }
    if (out.size() > kMaskChannels) {
        problem("at most " + std::to_string(kMaskChannels) + " channels; the rest are dropped");
        out.resize(kMaskChannels);
    }
    return out;
}

bool needsSpline(const FeatureRecipe& r) {
    for (const auto& op : r.terrain)
        if (op.kind == TerrainOpKind::CarveProfile || op.kind == TerrainOpKind::Step) return true;
    for (const auto& m : r.meshes)
        if (m.attach == MeshAttach::Spline || m.attach == MeshAttach::Edge) return true;
    for (const auto& s : r.scatter)
        if (s.primitive == ScatterPrimitive::AlongChannel || s.primitive == ScatterPrimitive::Edge) return true;
    return r.placement.source == PlacementSource::Channel;
}

} // namespace

std::shared_ptr<Catalogue> Catalogue::load(const fs::path& dir, const ZoneClassifier* classifier,
                                           const ModelResolver& models, std::vector<CatalogueProblem>* problems) {
    std::vector<RecipeProblem> recipeProblems;
    auto recipes = loadRecipes(dir / "recipes", &recipeProblems);
    if (problems) for (auto& p : recipeProblems) problems->push_back({"recipes/" + p.file, p.what});
    auto masks = loadMasks(dir / "masks.json", problems);
    std::vector<CoverProblem> coverProblems;
    auto cover = loadCover(dir / "cover.json", &coverProblems);
    if (problems) for (auto& p : coverProblems) problems->push_back({p.file, p.what});
    return make(std::move(recipes), std::move(masks), std::move(cover), classifier, models, problems);
}

std::shared_ptr<Catalogue> Catalogue::make(std::vector<FeatureRecipe> recipes, std::vector<MaskChannel> masks,
                                           CoverRules cover, const ZoneClassifier* classifier,
                                           const ModelResolver& models, std::vector<CatalogueProblem>* problems) {
    auto c = std::make_shared<Catalogue>();
    c->recipes_ = std::move(recipes);
    c->masks_ = std::move(masks);
    c->cover_ = std::move(cover);
    c->classifier_ = classifier;
    c->resolve(classifier, models, problems);
    return c;
}

std::span<const ZoneType> Catalogue::zones() const {
    return classifier_ ? classifier_->types() : std::span<const ZoneType>();
}

std::optional<ZoneTypeId> Catalogue::zoneId(std::string_view name) const {
    const auto types = zones();
    for (std::size_t i = 0; i < types.size(); ++i)
        if (types[i].name == name) return ZoneTypeId(i);
    return std::nullopt;
}

std::optional<std::uint8_t> Catalogue::maskId(std::string_view name) const {
    for (std::size_t i = 0; i < masks_.size(); ++i)
        if (masks_[i].name == name) return std::uint8_t(i);
    return std::nullopt;
}

std::optional<std::uint32_t> Catalogue::recipeId(std::string_view name) const {
    for (std::size_t i = 0; i < recipes_.size(); ++i)
        if (recipes_[i].name == name) return std::uint32_t(i);
    return std::nullopt;
}

void Catalogue::resolve(const ZoneClassifier* classifier, const ModelResolver& models,
                        std::vector<CatalogueProblem>* problems) {
    (void)classifier;
    const auto problem = [&](const std::string& file, const std::string& what) {
        if (problems) problems->push_back({file, what});
    };
    const auto resolveModels = [&](std::vector<ModelChoice>& list, const std::string& file, const std::string& where) {
        std::vector<ModelChoice> kept;
        for (auto& m : list) {
            std::optional<std::uint32_t> id;
            if (models) id = models(m.name);
            if (!id) { problem(file, where + ": unknown model \"" + m.name + "\""); continue; }
            m.id = *id;
            if (m.weight > 0) kept.push_back(m);
        }
        list = std::move(kept);
    };
    for (std::uint32_t i = 0; i < recipes_.size(); ++i) {
        auto& r = recipes_[i];
        const auto& file = r.file;
        const std::string& w = r.name;
        auto& p = r.placement;
        p.zoneIds.clear();
        for (const auto& z : p.zones) {
            if (auto id = zoneId(z)) p.zoneIds.push_back(*id);
            else problem(file, w + ": unknown zone \"" + z + "\"");
        }
        if (!p.zones.empty() && p.zoneIds.empty())
            problem(file, w + ": none of its zones exist, so it is never placed");
        p.excludeZoneIds.clear();
        for (const auto& z : p.excludeZones) {
            if (auto id = zoneId(z)) p.excludeZoneIds.push_back(*id);
            else problem(file, w + ": unknown zone \"" + z + "\"");
        }
        std::vector<MaskWrite> masks;
        for (auto& m : r.masks) {
            if (auto id = maskId(m.channel)) { m.channelId = *id; masks.push_back(m); }
            else problem(file, w + ": unknown mask channel \"" + m.channel + "\"");
        }
        r.masks = std::move(masks);
        for (std::size_t s = 0; s < r.scatter.size(); ++s)
            resolveModels(r.scatter[s].models, file, w + ".scatter[" + std::to_string(s) + "]");
        for (std::size_t m = 0; m < r.meshes.size(); ++m) {
            auto& mesh = r.meshes[m];
            if (mesh.model.name.empty()) continue;
            std::vector<ModelChoice> one{mesh.model};
            resolveModels(one, file, w + ".meshes[" + std::to_string(m) + "]");
            if (one.empty()) mesh.model = {};
            else mesh.model = one.front();
        }
        // Shapes that make no sense together.
        if (p.densityPerKm2 <= 0) problem(file, w + ": density_per_km2 must be above 0");
        if (p.minSpacing < 0) problem(file, w + ": min_spacing must not be negative");
        if (p.lengthMax < p.lengthMin) problem(file, w + ": length is [min, max]");
        if (p.scaleMax < p.scaleMin || p.scaleMin <= 0) problem(file, w + ": scale is [min, max], above 0");
        if (needsSpline(r) && p.source == PlacementSource::Point && p.align == Align::None)
            problem(file, w + ": a spline feature placed at points needs an alignment (contour, slope, flow, wind or random)");
        for (const auto& op : r.terrain) {
            if (op.kind == TerrainOpKind::CarveProfile && op.innerWidth > op.outerWidth)
                problem(file, w + ": carve_profile inner_width is wider than outer_width");
            if (op.kind == TerrainOpKind::CarveProfile && op.waterWidth > op.innerWidth)
                problem(file, w + ": carve_profile water_width is wider than inner_width");
        }
        const double density = p.densityPerKm2 * (kPlanningCell[int(r.scale)] / 1000.0) * (kPlanningCell[int(r.scale)] / 1000.0);
        if (density > 4096) problem(file, w + ": more than 4096 candidates a planning cell; lower density or raise scale");
        ops_.push_back(compileOps(r));
        reach_.push_back(r.reach() * p.scaleMax + (needsSpline(r) ? p.lengthMax : 0.0));
        maxReach_ = std::max(maxReach_, reach_.back());
        if (!r.terrain.empty()) movesGround_ = true;
        byScale_[int(r.scale)].push_back(i);
    }
    for (std::size_t i = 0; i < cover_.rules.size(); ++i) {
        auto& rule = cover_.rules[i];
        const std::string where = "cover.json rules[" + std::to_string(i) + "]";
        if (!rule.anyZone) {
            if (auto id = zoneId(rule.zone)) rule.zoneId = *id;
            else problem("cover.json", where + ": unknown zone \"" + rule.zone + "\"");
        }
        std::vector<CoverModifier> kept;
        for (auto& m : rule.modifiers) {
            if (m.source == CoverModifier::Source::Mask) {
                if (auto id = maskId(m.name)) m.index = *id;
                else { problem("cover.json", where + ": unknown mask channel \"" + m.name + "\""); continue; }
            }
            kept.push_back(m);
        }
        rule.modifiers = std::move(kept);
        if (rule.tier == CoverTier::Secondary) resolveModels(rule.models, "cover.json", where);
    }
}

} // namespace engine::environment
