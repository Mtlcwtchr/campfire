#include "game/world/world_saves.hpp"

#include <algorithm>
#include <system_error>

#include "engine/world_store/world_root.hpp"

namespace world::saves {
namespace fs = std::filesystem;

fs::path pathOf(const std::string& utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string utf8Of(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

namespace {

std::uintmax_t sizeOf(const fs::path& directory) {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    return total;
}

SavedWorld describe(const fs::path& layoutFile) {
    SavedWorld world;
    world.layout = layoutFile;
    world.name = utf8Of(layoutFile.stem());
    world.root = engine::world_store::WorldRoot::forLayout(layoutFile);
    std::error_code ec;
    world.modified = fs::last_write_time(layoutFile, ec);
    if (const auto layout = generation::loadWorldLayout(layoutFile)) {
        world.regionsX = layout->regionsX;
        world.regionsY = layout->regionsY;
        world.seed = layout->seed;
        world.generatedRegions = std::int32_t(std::count_if(layout->regions.begin(), layout->regions.end(),
                                                            [](const auto& r) { return r.generated; }));
    } else {
        world.readable = false;
    }
    const fs::path manifest = world.root / engine::world_store::WorldRoot::kManifestName;
    if (fs::exists(manifest, ec)) {
        world.hasHistory = true;
        world.historyBytes = sizeOf(world.root);
        const auto saved = fs::last_write_time(manifest, ec);
        if (!ec && saved > world.modified) world.modified = saved;
    }
    return world;
}

bool reserved(char32_t c) {
    return c < 32 || c == 127 || c == U'/' || c == U'\\' || c == U':' || c == U'*' || c == U'?' ||
           c == U'"' || c == U'<' || c == U'>' || c == U'|';
}

} // namespace

std::vector<SavedWorld> list(const fs::path& directory) {
    std::vector<SavedWorld> worlds;
    std::error_code ec;
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".json") continue;
        // Anything else that keeps a json here (settings, presets) is not a world.
        const std::string stem = utf8Of(it->path().stem());
        if (stem.empty() || stem.front() == '.') continue;
        worlds.push_back(describe(it->path()));
    }
    std::sort(worlds.begin(), worlds.end(), [](const SavedWorld& a, const SavedWorld& b) {
        return a.modified != b.modified ? a.modified > b.modified : a.name < b.name;
    });
    return worlds;
}

std::string cleanName(const std::string& name) {
    std::string out;
    // Byte by byte: every byte of a multi-byte UTF-8 sequence is 0x80 or
    // above, so none of them can be one of the ASCII characters refused here.
    for (const unsigned char c : name)
        if (!reserved(c)) out.push_back(char(c));
    const auto edge = [](char c) { return c == ' ' || c == '.' || c == '\t'; };
    while (!out.empty() && edge(out.back())) out.pop_back();
    std::size_t first = 0;
    while (first < out.size() && edge(out[first])) ++first;
    out.erase(0, first);
    // Runs of spaces to one.
    std::string single;
    for (const char c : out)
        if (!(c == ' ' && !single.empty() && single.back() == ' ')) single.push_back(c);
    if (single.size() > 60) {
        // Back to the start of a codepoint, so a letter is never cut in half.
        std::size_t cut = 60;
        while (cut > 0 && (static_cast<unsigned char>(single[cut]) & 0xC0) == 0x80) --cut;
        single.resize(cut);
        while (!single.empty() && single.back() == ' ') single.pop_back();
    }
    return single;
}

std::string freeName(const fs::path& directory, const std::string& name) {
    std::error_code ec;
    const auto taken = [&](const std::string& candidate) {
        return fs::exists(directory / pathOf(candidate + ".json"), ec) ||
               fs::exists(directory / pathOf(candidate), ec);
    };
    if (!taken(name)) return name;
    for (int n = 2;; ++n) {
        const std::string candidate = name + " " + std::to_string(n);
        if (!taken(candidate)) return candidate;
    }
}

std::optional<SavedWorld> create(const fs::path& directory, const NewWorld& spec,
                                 const std::vector<generation::WorldPreset>& presets, std::string* error) {
    const auto fail = [&](std::string why) -> std::optional<SavedWorld> {
        if (error) *error = std::move(why);
        return std::nullopt;
    };
    const std::string name = cleanName(spec.name);
    if (name.empty()) return fail("Give the world a name.");
    if (presets.empty() && !spec.empty) return fail("No world presets to make it from.");
    const fs::path file = directory / pathOf(name + ".json");
    std::error_code ec;
    if (fs::exists(file, ec) || fs::exists(directory / pathOf(name), ec))
        return fail("A world called \"" + name + "\" already exists.");
    const auto preset = std::find_if(presets.begin(), presets.end(),
                                     [&](const auto& p) { return p.name == spec.preset; });
    const std::int32_t most = spec.empty ? kMaxEmptyRegions : kMaxGeneratedRegions;
    const std::int32_t across = std::clamp(spec.regionsX > 0 ? spec.regionsX : spec.regions, 1, most);
    const std::int32_t down = std::clamp(spec.regionsY > 0 ? spec.regionsY : spec.regions, 1, most);
    auto layout = generation::emptyLayout(across, down, spec.seed);
    if (spec.empty) {
        // Placed on its planet from the start, so what is made in it later
        // has a latitude of its own: sixty degrees north at the top, the
        // world spanning at least twenty-five degrees, true to scale when it
        // is big enough to be.
        const double heightKm = layout.heightMetres() / 1000.0;
        layout.latitude.fixed = true;
        layout.latitude.northDegrees = 60.0;
        layout.latitude.kmPerDegree = std::min(111.2, heightKm / 25.0);
    } else {
        generation::generateAll(layout, preset != presets.end() ? *preset : presets.front());
    }
    fs::create_directories(directory, ec);
    if (!generation::saveWorldLayout(layout, file)) return fail("Could not write " + utf8Of(file) + ".");
    return describe(file);
}

bool remove(const SavedWorld& world, std::string* error) {
    std::error_code ec;
    fs::remove_all(world.root, ec);
    if (ec) {
        if (error) *error = "Could not delete " + utf8Of(world.root) + ": " + ec.message();
        return false;
    }
    fs::remove(world.layout, ec);
    if (ec) {
        if (error) *error = "Could not delete " + utf8Of(world.layout) + ": " + ec.message();
        return false;
    }
    return true;
}

} // namespace world::saves

