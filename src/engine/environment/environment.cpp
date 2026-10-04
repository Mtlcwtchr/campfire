#include "engine/environment/environment.hpp"

#include "engine/environment/random.hpp"

#include <atomic>
#include <cstdlib>
#include <string_view>
#include <mutex>

namespace engine::environment {

namespace fs = std::filesystem;

namespace {
std::atomic<std::uint64_t> nextGeneration{1};
std::mutex activeLock;
std::shared_ptr<const Environment> current;
std::atomic<std::uint64_t> currentGeneration{0};

fs::path contentRoot() {
    fs::path content = "content";
    std::error_code ec;
    for (int up = 0; up < 6 && !fs::exists(content, ec); ++up) content = ".." / content;
    return content;
}
} // namespace

std::shared_ptr<Environment> Environment::build(EnvironmentSetup setup, std::vector<EnvironmentProblem>* problems) {
    std::vector<CatalogueProblem> found;
    auto catalogue = Catalogue::load(setup.content, setup.classifier.get(), setup.models, &found);
    if (problems) for (auto& p : found) problems->push_back({p.file, p.what});
    auto env = build(std::move(setup), std::move(catalogue));
    std::vector<StyleProblem> style;
    std::error_code ec;
    if (fs::exists(env->setup_.style / "palettes.json", ec))
        env->palettes_ = StyleTable::load(env->setup_.style / "palettes.json", &style);
    if (fs::exists(env->setup_.style / "grades.json", ec))
        env->grades_ = StyleTable::load(env->setup_.style / "grades.json", &style);
    if (problems) for (auto& p : style) problems->push_back({"style/" + p.file, p.what});
    return env;
}

std::shared_ptr<Environment> Environment::build(EnvironmentSetup setup, std::shared_ptr<const Catalogue> catalogue) {
    auto env = std::make_shared<Environment>();
    env->setup_ = std::move(setup);
    env->catalogue_ = std::move(catalogue);
    env->generation_ = nextGeneration.fetch_add(1);
    auto zs = env->setup_.zones;
    if (zs.seed == 0) zs.seed = env->setup_.seed;
    // A classifier with no zone but "unclassified" classifies nothing: no
    // zone field, and nothing is sampled for it.
    if (env->setup_.fields && env->setup_.classifier && env->setup_.classifier->types().size() > 1)
    {
        env->zones_ = std::make_unique<ZoneField>(*env->setup_.fields, *env->setup_.classifier, zs);
        auto coarse = zs;
        const double k = 128.0 / zs.step;
        coarse.step = 128.0;
        coarse.pageMetres = 8192.0;
        coarse.alongMetres *= k * 0.75;
        coarse.acrossMetres *= k * 0.75;
        coarse.breakupMetres *= k * 0.5;
        env->coarseZones_ = std::make_unique<ZoneField>(*env->setup_.fields, *env->setup_.classifier, coarse);
    }
    PlannerContext ctx;
    ctx.catalogue = env->catalogue_;
    ctx.fields = env->setup_.fields.get();
    // Features are placed by the coarse zones: a feature is tens to hundreds of
    // metres, and its planning cell's neighbours would otherwise classify
    // kilometres of country at sixteen metres before one page could bake.
    ctx.zones = env->coarseZones_ ? env->coarseZones_.get() : env->zones_.get();
    ctx.height = env->setup_.height;
    ctx.climate = env->setup_.climate;
    ctx.drainage = env->setup_.drainage;
    ctx.seed = env->setup_.seed;
    ctx.removed = env->setup_.removed;
    ctx.painted = env->setup_.painted;
    env->planner_ = std::make_shared<FeaturePlanner>(std::move(ctx));
    env->features_ = std::make_shared<FeatureLayer>(env->planner_);
    if (env->catalogue_->movesGround() || env->writesMasks()) {
        std::uint64_t h = mix64(env->setup_.seed ^ 0xe7f1ULL) ^ mix64(env->setup_.gameVersion + 1) ^
                          mix64(env->setup_.removedVersion ^ 0x7e3dULL) ^
                          mix64(env->setup_.painted ? env->setup_.painted->fingerprint() : 0x5a17ULL);
        const auto add = [&](std::string_view text) {
            for (unsigned char c : text) h = mix64(h ^ c);
            h = mix64(h ^ 0xffULL);
        };
        for (const auto& r : env->catalogue_->recipes()) add(recipeToJson(r));
        for (const auto& m : env->catalogue_->masks()) add(m.name);
        for (const auto& z : env->catalogue_->zones()) add(z.name);
        for (const auto& row : env->coverTable())
            for (float v : row) h = mix64(h ^ std::uint64_t(std::int64_t(double(v) * 65536.0)));
        env->fingerprint_ = h ? h : 1;
    }
    return env;
}

bool Environment::writesMasks() const {
    if (zones_ || setup_.zoneMasks) return true;
    for (const auto& r : catalogue_->cover().rules)
        if (r.tier == CoverTier::Ground) return true;
    for (const auto& r : catalogue_->recipes())
        if (!r.masks.empty()) return true;
    return false;
}

PageMasks Environment::masks(double x0, double y0, double step, int side) const {
    // Pages read coarser than 32 m carry no masks: at that distance the ground's
    // wetness and moss are a few pixels and no grass is drawn, and classifying
    // the kilometres such a page covers would hold its bake up for seconds.
    if (step > 32.0) return PageMasks{x0, y0, step, side, {}, {}};
    const ZoneField* zones = zones_.get();
    return rasteriseMasks(features_.get(), zones, setup_.zoneMasks, setup_.height, x0, y0, step, side,
                          &catalogue_->cover(), setup_.fields.get(), setup_.seed);
}

std::vector<std::array<float, 4>> Environment::coverTable() const {
    return environment::coverTable(catalogue_->cover(), std::max<std::size_t>(1, catalogue_->zones().size()));
}

std::shared_ptr<const Environment> active() {
    std::lock_guard guard(activeLock);
    return current;
}

void setActive(std::shared_ptr<const Environment> environment) {
    std::lock_guard guard(activeLock);
    current = std::move(environment);
    currentGeneration.store(current ? current->generation() : 0);
}

std::uint64_t activeGeneration() { return currentGeneration.load(); }

// ASR_ENVIRONMENT_CONTENT and ASR_STYLE_CONTENT point a tool or a test at
// other content without touching the game's.
fs::path defaultContentDirectory() {
    if (const char* over = std::getenv("ASR_ENVIRONMENT_CONTENT"); over && *over) return over;
    return contentRoot() / "config" / "environment";
}
fs::path defaultStyleDirectory() {
    if (const char* over = std::getenv("ASR_STYLE_CONTENT"); over && *over) return over;
    return contentRoot() / "config" / "style";
}

} // namespace engine::environment
