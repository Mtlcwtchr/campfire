#include "engine/environment/environment.hpp"

#include <atomic>
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
    if (env->setup_.fields && env->setup_.classifier)
        env->zones_ = std::make_unique<ZoneField>(*env->setup_.fields, *env->setup_.classifier, zs);
    PlannerContext ctx;
    ctx.catalogue = env->catalogue_;
    ctx.fields = env->setup_.fields.get();
    ctx.zones = env->zones_.get();
    ctx.height = env->setup_.height;
    ctx.climate = env->setup_.climate;
    ctx.drainage = env->setup_.drainage;
    ctx.seed = env->setup_.seed;
    env->planner_ = std::make_shared<FeaturePlanner>(std::move(ctx));
    env->features_ = std::make_shared<FeatureLayer>(env->planner_);
    return env;
}

PageMasks Environment::masks(double x0, double y0, double step, int side) const {
    return rasteriseMasks(features_.get(), zones_.get(), setup_.zoneMasks, setup_.height, x0, y0, step, side);
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

fs::path defaultContentDirectory() { return contentRoot() / "config" / "environment"; }
fs::path defaultStyleDirectory() { return contentRoot() / "config" / "style"; }

} // namespace engine::environment
