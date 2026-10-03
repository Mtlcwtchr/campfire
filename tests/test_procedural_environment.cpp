#include "framework.hpp"

#include "engine/environment/asset_meta.hpp"
#include "engine/environment/cover.hpp"
#include "engine/environment/drainage.hpp"
#include "engine/environment/environment.hpp"
#include "engine/environment/feature_mesh.hpp"
#include "engine/environment/scatter.hpp"
#include "engine/environment/style.hpp"
#include "engine/environment/terrain_ops.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>

// The procedural environment's mechanism (doc/plan_procedural_environment_2026-10-03.md):
// fields, zones, recipes, terrain operations, drainage, planning, masks,
// scatter, feature meshes, style tables and asset records - over a small
// synthetic world, with zones the test makes up. No game content.
namespace {
using namespace engine::environment;
namespace fs = std::filesystem;

// A valley running along y, sloping down towards -y, with a bump.
double testHeight(double x, double y) {
    return 0.02 * y + 0.0004 * x * x + 6.0 * std::exp(-((x - 300) * (x - 300) + (y - 200) * (y - 200)) / 5000.0);
}

class TestFields final : public FieldSource {
public:
    void sample(double x, double y, FieldSample& out) const override {
        deriveShape(testHeight, x, y, ShapeSettings{}, out);
        // Wet near the valley floor, dry up its sides.
        out.set(field::Wetness, float(std::clamp(1.0 - std::abs(x) / 150.0, 0.0, 1.0)));
        out.set(field::Canopy, 0.5f);
        out.set(field::Water, 0.0f);
    }
};

// Two zones and the unclassified one: wet floor and dry side.
class TestZones final : public ZoneClassifier {
public:
    TestZones() : types_{{"unclassified"}, {"wet_floor"}, {"dry_side"}} {}
    std::span<const ZoneType> types() const override { return types_; }
    void classify(const FieldSample& f, std::span<float> w, ZoneScalars& s) const override {
        const float wet = f[field::Wetness];
        w[0] = 0;
        w[1] = wet;
        w[2] = 1 - wet;
        s.wetness = wet;
        s.density = 0.5f;
    }
private:
    std::vector<ZoneType> types_;
};

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("asr_env_" + std::to_string(std::random_device{}()));
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
    void write(const std::string& name, const std::string& text) const {
        fs::create_directories((path / name).parent_path());
        std::ofstream(path / name) << text;
    }
};

ModelResolver testModels() {
    return [](std::string_view name) -> std::optional<std::uint32_t> {
        if (name == "rock_big") return 1;
        if (name == "rock_mid") return 2;
        if (name == "rock_small") return 3;
        if (name == "fern") return 4;
        return std::nullopt;
    };
}

FeatureRecipe raiseRecipe() {
    FeatureRecipe r;
    r.name = "mound";
    r.scale = FeatureScale::Local;
    r.placement.densityPerKm2 = 6;
    r.placement.minSpacing = 120;
    TerrainOp op;
    op.kind = TerrainOpKind::Raise;
    op.height = 5;
    op.radiusA = 20;
    op.radiusB = 20;
    op.edgeNoise = 0;
    op.blend = 4;
    r.terrain.push_back(op);
    return r;
}

struct World {
    std::shared_ptr<TestFields> fields = std::make_shared<TestFields>();
    std::shared_ptr<TestZones> zones = std::make_shared<TestZones>();
    std::shared_ptr<Environment> env;
    explicit World(std::vector<FeatureRecipe> recipes, std::vector<MaskChannel> masks = {}, CoverRules cover = {},
                   std::uint64_t seed = 42) {
        std::vector<CatalogueProblem> problems;
        auto cat = Catalogue::make(std::move(recipes), std::move(masks), std::move(cover), zones.get(), testModels(), &problems);
        EnvironmentSetup setup;
        setup.classifier = zones;
        setup.fields = fields;
        setup.height = testHeight;
        setup.models = testModels();
        setup.seed = seed;
        env = Environment::build(setup, cat);
    }
};

core::WorldPos at(double x, double y) { return {quantised(x), quantised(y)}; }

} // namespace

TEST(environment_shape_fields_of_a_plane) {
    FieldSample f;
    deriveShape([](double x, double) { return 0.1 * x; }, 10, 10, ShapeSettings{}, f);
    CHECK(std::abs(f[field::Slope] - 0.1f) < 1e-4f);
    // It falls towards -x: the aspect faces that way.
    CHECK(std::abs(std::abs(f[field::Aspect]) - 3.14159f) < 1e-3f);
    CHECK(std::abs(f[field::TpiSmall]) < 1e-4f);
    CHECK(f.has(field::Convergence));
}

TEST(environment_field_registry_adds_names_once) {
    const auto a = fieldId("test_field_custom", true);
    const auto b = fieldId("test_field_custom", true);
    CHECK(a && b && *a == *b);
    CHECK(fieldName(*a) == "test_field_custom");
    CHECK(fieldId("wetness") == std::optional<FieldId>(field::Wetness));
}

TEST(environment_zones_are_soft_and_seamless) {
    TestFields fields;
    TestZones zones;
    ZoneField zf(fields, zones, {});
    const auto mid = zf.at(75, 100);
    // Halfway up the valley side, both zones share the point.
    CHECK(mid.weights.of(1) > 0.2f && mid.weights.of(2) > 0.2f);
    const auto floor = zf.at(0, 100);
    CHECK(floor.type() == 1);
    // A point on a page border is the same number whichever grid answers.
    const auto a = zf.build(512 - 64, 0, 9, 9).at(512, 64);
    const auto b = zf.build(512, 0, 9, 9).at(512, 64);
    CHECK(a.weights.type[0] == b.weights.type[0]);
    CHECK(std::abs(a.weights.weight[0] - b.weights.weight[0]) < 1e-5f);
}

TEST(environment_recipe_json_round_trip_and_unknown_keys) {
    const std::string text = R"({
        "name": "dry_gully", "scale": "local", "age": "ancient",
        "placement": {"zones": ["wet_floor"], "source": "channel", "channel_classes": ["paleochannel"],
                      "density_per_km2": 3, "min_spacing": 200, "length": [120, 300], "fields": {"slope": [0, 0.5]}},
        "terrain": [{"op": "carve_profile", "outer_width": 36, "outer_depth": 5, "inner_width": 6, "inner_depth": 1.5}],
        "masks": [{"channel": "wetness", "shape": "bed", "value": 0.8}],
        "scatter": [{"primitive": "along_channel", "models": [["rock_small", 2], "rock_mid"], "count": [6, 12]}],
        "composition": {"negative_space": 10},
        "colour": "typo"
    })";
    std::vector<RecipeProblem> problems;
    auto r = parseRecipe(text, "gully.json", &problems);
    CHECK(r.has_value());
    CHECK(problems.size() == 1);   // "colour" is not a key
    CHECK(r->placement.source == PlacementSource::Channel);
    CHECK(r->terrain.size() == 1 && r->terrain[0].kind == TerrainOpKind::CarveProfile);
    CHECK(r->scatter[0].models.size() == 2);
    std::vector<RecipeProblem> again;
    auto back = parseRecipe(recipeToJson(*r), "back.json", &again);
    CHECK(back.has_value());
    CHECK(again.empty());
    CHECK(back->terrain[0].outerWidth == 36);
    CHECK(back->placement.lengthMax == 300);
}

TEST(environment_catalogue_reports_unknown_names) {
    auto r = raiseRecipe();
    r.placement.zones = {"wet_floor", "no_such_zone"};
    MaskWrite m;
    m.channel = "no_such_mask";
    r.masks.push_back(m);
    ScatterRule s;
    s.models = {{"rock_big", 1}, {"no_such_model", 1}};
    r.scatter.push_back(s);
    TestZones zones;
    std::vector<CatalogueProblem> problems;
    auto cat = Catalogue::make({r}, {{"wetness"}}, {}, &zones, testModels(), &problems);
    CHECK(problems.size() == 3);
    CHECK(cat->recipes()[0].placement.zoneIds.size() == 1);
    CHECK(cat->recipes()[0].masks.empty());
    CHECK(cat->recipes()[0].scatter[0].models.size() == 1);
    CHECK(cat->movesGround());
}

TEST(environment_spline_nearest) {
    FixedSpline s;
    s.points = {at(0, 0), at(100, 0), at(100, 100)};
    s.finish();
    CHECK(std::abs(s.length.toDouble() - 200) < 1e-3);
    const auto n = s.nearest(at(50, 10));
    CHECK(std::abs(n.distance.toDouble() - 10) < 1e-3);
    CHECK(n.side.toDouble() > 0);   // left of travel along +x
    CHECK(std::abs(n.along.toDouble() - 50) < 1e-3);
    const auto m = s.nearest(at(90, 50));
    CHECK(std::abs(m.along.toDouble() - 150) < 1e-3);
}

TEST(environment_ops_raise_carve_and_combine) {
    FeatureRecipe r = raiseRecipe();
    const auto ops = compileOps(r);
    FeatureInstance in;
    in.anchor = at(0, 0);
    OpTotals t;
    applyOps(ops, in, at(0, 0), core::kZero, 4, t);
    CHECK(std::abs(t.total().toDouble() - 5) < 0.05);
    OpTotals far;
    applyOps(ops, in, at(60, 0), core::kZero, 4, far);
    CHECK(far.total().raw == 0);
    // Two crossing carves are as deep as the deeper, not their sum.
    FeatureRecipe g;
    g.name = "g";
    TerrainOp carve;
    carve.kind = TerrainOpKind::CarveProfile;
    carve.outerWidth = 30;
    carve.outerDepth = 4;
    carve.innerWidth = 0;
    carve.innerDepth = 0;
    carve.taper = 0;
    g.terrain.push_back(carve);
    const auto cops = compileOps(g);
    FeatureInstance a, b;
    a.spline.points = {at(-100, 0), at(100, 0)};
    a.spline.finish();
    b.spline.points = {at(0, -100), at(0, 100)};
    b.spline.finish();
    OpTotals both;
    applyOps(cops, a, at(0, 0), core::kZero, 4, both);
    applyOps(cops, b, at(0, 0), core::kZero, 4, both);
    OpTotals one;
    applyOps(cops, a, at(0, 0), core::kZero, 4, one);
    CHECK(both.total() == one.total());
    CHECK(one.total().toDouble() < -2);
    // A valley narrower than the samples is not cut at that spacing.
    OpTotals coarse;
    applyOps(cops, a, at(0, 0), core::kZero, 64, coarse);
    CHECK(coarse.total().raw == 0);
}

TEST(environment_drainage_finds_the_valley) {
    DrainageSettings s;
    s.channelArea = 20000;
    const auto net = buildDrainage(testHeight, {}, -256, 0, 32, 64, s);
    CHECK(!net.reaches.empty());
    // The longest reach runs down the valley floor, near x = 0.
    const ChannelReach* longest = nullptr;
    for (const auto& r : net.reaches) if (!longest || r.length > longest->length) longest = &r;
    double meanX = 0;
    for (const auto& p : longest->points) meanX += p[0];
    meanX /= double(longest->points.size());
    CHECK(std::abs(meanX) < 48);
    CHECK(longest->points.front()[1] > longest->points.back()[1]);   // flows to -y
    // Classes from the ratio of today's flow to the past's.
    CHECK(classifyChannel(1e5, 1e5, s) == ChannelClass::SeasonalStream);
    CHECK(classifyChannel(1e5, 1e3, s) == ChannelClass::Paleochannel);
    CHECK(classifyChannel(1e7, 1e7, s) == ChannelClass::PermanentRiver);
}

TEST(environment_planner_is_deterministic_and_spaced) {
    World one({raiseRecipe()});
    World two({raiseRecipe()});
    std::vector<FeatureInstance> a, b;
    const core::WorldRect area{at(-2000, -2000), at(2000, 2000)};
    one.env->planner().instancesIn(area, a);
    two.env->planner().instancesIn(area, b);
    CHECK(!a.empty());
    CHECK(a.size() == b.size());
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        CHECK(a[i].id == b[i].id);
        CHECK(a[i].anchor == b[i].anchor);
    }
    for (std::size_t i = 0; i < a.size(); ++i)
        for (std::size_t j = i + 1; j < a.size(); ++j) {
            const double d = std::hypot((a[i].anchor.x - a[j].anchor.x).toDouble(), (a[i].anchor.y - a[j].anchor.y).toDouble());
            CHECK(d >= 120 - 1e-6 || a[i].id == a[j].id);
        }
    // A different seed is a different world.
    World other({raiseRecipe()}, {}, {}, 7);
    std::vector<FeatureInstance> c;
    other.env->planner().instancesIn(area, c);
    CHECK(c.empty() || c[0].anchor != a[0].anchor);
}

TEST(environment_feature_layer_moves_the_ground) {
    World w({raiseRecipe()});
    std::vector<FeatureInstance> list;
    w.env->planner().instancesIn({at(-2000, -2000), at(2000, 2000)}, list);
    CHECK(!list.empty());
    const auto& in = list.front();
    const auto d = w.env->features().at(in.anchor, core::kZero, 4);
    CHECK(d.toDouble() > 2);
    // Far from every instance, nothing.
    World none({});
    CHECK(none.env->features().empty());
    CHECK(none.env->features().at(at(5, 5), core::kZero, 4).raw == 0);
}

TEST(environment_masks_follow_features_and_zones) {
    auto r = raiseRecipe();
    MaskWrite m;
    m.channel = "rock";
    m.shape = MaskShape::Footprint;
    m.value = 1;
    r.masks.push_back(m);
    World w({r}, {{"wetness"}, {"rock"}});
    std::vector<FeatureInstance> list;
    w.env->planner().instancesIn({at(-2000, -2000), at(2000, 2000)}, list);
    CHECK(!list.empty());
    const double ax = list.front().anchor.x.toDouble(), ay = list.front().anchor.y.toDouble();
    const auto page = w.env->masks(ax - 8, ay - 8, 4, 5);
    CHECK(page.channel(2, 2, 1) > 0.9f);   // rock at the anchor
    CHECK(page.channel(2, 2, 0) == 0.0f);  // nothing wrote wetness
    CHECK(page.zones[(2 * 5 + 2) * 4] != 0);
}

TEST(environment_dress_rock_hierarchy_falls_in_size) {
    auto r = raiseRecipe();
    ScatterRule s;
    s.primitive = ScatterPrimitive::RockHierarchy;
    s.models = {{"rock_big", 1}, {"rock_mid", 1}, {"rock_small", 1}};
    s.radius = 30;
    s.minSpacing = 0.2;
    r.scatter.push_back(s);
    World w({r});
    std::vector<FeatureInstance> list;
    w.env->planner().instancesIn({at(-2000, -2000), at(2000, 2000)}, list);
    CHECK(!list.empty());
    std::vector<PlacedObject> objects, again;
    dress(w.env->catalogue(), list.front(), testHeight, objects);
    dress(w.env->catalogue(), list.front(), testHeight, again);
    CHECK(objects == again);
    CHECK(objects.size() > 10);
    double big = 0, small = 0;
    int nb = 0, ns = 0;
    for (const auto& o : objects) {
        if (o.model == 1) { big += o.scale; ++nb; }
        if (o.model == 3) { small += o.scale; ++ns; }
    }
    CHECK(nb > 0 && ns > 0);
    CHECK(big / nb > small / ns);
}

TEST(environment_cover_follows_zone_and_negative_space) {
    CoverRules rules;
    CoverRule fern;
    fern.zone = "wet_floor";
    fern.tier = CoverTier::Secondary;
    fern.models = {{"fern", 1}};
    fern.density = 4;
    fern.patchContrast = 0;
    rules.rules.push_back(fern);
    auto r = raiseRecipe();
    r.composition.negativeSpace = 25;
    World w({r}, {}, rules);
    CoverContext ctx;
    ctx.catalogue = &w.env->catalogue();
    ctx.fields = w.fields.get();
    ctx.zones = w.env->zones();
    ctx.features = &w.env->features();
    ctx.height = testHeight;
    ctx.ground = testHeight;
    ctx.seed = 3;
    std::vector<PlacedObject> wet, dry;
    cover(ctx, -40, 0, 40, 200, wet);
    cover(ctx, 200, 0, 280, 200, dry);
    CHECK(wet.size() > dry.size() * 3);
    std::vector<FeatureInstance> list;
    w.env->planner().instancesIn({at(-2000, -2000), at(2000, 2000)}, list);
    for (const auto& in : list) {
        if (in.secondary) continue;
        for (const auto& o : wet) {
            const double d = std::hypot(o.x - in.anchor.x.toDouble(), o.y - in.anchor.y.toDouble());
            // Clumps start outside the negative space; their spread may reach in a little.
            CHECK(d > 25 * in.scale.toDouble() - fern.clusterRadius - 0.01);
        }
    }
}

TEST(environment_form_meshes_are_closed_and_repeatable) {
    MeshRule rule;
    rule.form = ProceduralForm::Overhang;
    rule.length = 8;
    rule.depth = 2;
    rule.height = 3;
    const auto a = formMesh(rule, 99, 0.5f);
    const auto b = formMesh(rule, 99, 0.5f);
    CHECK(!a.empty());
    CHECK(a.indices == b.indices);
    CHECK(a.indices.size() % 3 == 0);
    // It leans out: the top reaches further to -y than the foot.
    CHECK(a.min[1] < -2.0f);
    CHECK(a.max[2] > 2.5f);
}

TEST(environment_style_tables_match_and_blend) {
    TempDir dir;
    dir.write("grades.json", R"({
        "rows": ["shadow", "highlight"], "default": "base", "blend_seconds": 2,
        "profiles": [
            {"name": "base", "rows": {"shadow": [0.1, 0.1, 0.2, 1], "highlight": [1, 0.9, 0.7, 1]}},
            {"name": "marsh", "match": {"zones": ["wet_floor"]}, "rows": {"shadow": [0, 0.2, 0.1, 1]}},
            {"name": "marsh_forest", "match": {"zones": ["wet_floor"], "categories": ["forest"]}, "rows": {}}
        ]})");
    std::vector<StyleProblem> problems;
    auto t = StyleTable::load(dir.path / "grades.json", &problems);
    CHECK(t != nullptr);
    CHECK(problems.empty());
    CHECK(t->match("forest", "wet_floor") == 2);
    CHECK(t->match("meadow", "wet_floor") == 1);
    CHECK(t->match("meadow", "dry_side") == 0);
    // A row left out is the default's.
    CHECK(t->entries()[1].rows[1][1] == 0.9f);
    GradeBlend blend(t);
    blend.step(1, 100);
    CHECK(blend.weights()[1] > 0.99f);
    CHECK(std::abs(blend.rows()[0][1] - 0.2f) < 0.01f);
    dir.write("id.cube", "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n");
    auto lut = loadCube(dir.path / "id.cube");
    CHECK(lut.has_value());
    const auto c = lut->apply({0.25f, 0.5f, 0.75f});
    CHECK(std::abs(c[0] - 0.25f) < 1e-5f && std::abs(c[2] - 0.75f) < 1e-5f);
}

TEST(environment_asset_records_are_validated) {
    TempDir dir;
    dir.write("rocks/asset_meta.json", R"({"assets": [
        {"name": "Boulder", "source": "https://example.org/b", "license": "CC0-1.0"},
        {"name": "Fern", "source": "x", "license": "CC-BY-4.0"},
        {"name": "Moss", "license": "who-knows"}]})");
    dir.write("Stump.meta.json", R"({"source": "kitbash", "license": "proprietary-owned"})");
    std::vector<AssetProblem> problems;
    const auto metas = loadAssetMetas(dir.path, &problems);
    CHECK(metas.size() == 4);
    for (const auto& m : metas) validateAssetMeta(m, problems);
    // Fern lacks its author; Moss lacks a source and has an unknown licence.
    CHECK(problems.size() == 3);
    const auto missing = unrecorded(metas, {"Boulder", "Stump", "Pine"});
    CHECK(missing.size() == 1 && missing[0] == "Pine");
}

TEST(environment_channel_recipe_follows_the_drainage) {
    FeatureRecipe g;
    g.name = "gully";
    g.scale = FeatureScale::Local;
    g.placement.source = PlacementSource::Channel;
    g.placement.densityPerKm2 = 400;
    g.placement.minSpacing = 40;
    g.placement.lengthMin = 60;
    g.placement.lengthMax = 160;
    g.placement.avoidWater = false;
    TerrainOp carve;
    carve.kind = TerrainOpKind::CarveProfile;
    carve.outerWidth = 24;
    carve.outerDepth = 3;
    g.terrain.push_back(carve);
    World w({g});
    std::vector<FeatureInstance> list;
    w.env->planner().instancesIn({at(-600, -600), at(600, 1200)}, list);
    CHECK(!list.empty());
    for (const auto& in : list) {
        CHECK(!in.spline.empty());
        // Gullies lie along the valley floor, not up its sides.
        CHECK(std::abs(in.anchor.x.toDouble()) < 160);
    }
    // And the ground is cut along them.
    const auto& in = list.front();
    const auto mid = in.spline.pointAt(in.spline.length / 2);
    CHECK(w.env->features().at(mid, core::kZero, 4).toDouble() < -1);
}
