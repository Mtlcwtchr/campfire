#include "engine/biomes/shader_code.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>

namespace engine::biomes {
namespace fs = std::filesystem;
namespace {

constexpr double kPi = 3.14159265358979323846;

// A float literal, the same text on every machine.
std::string lit(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    std::string s = buf;
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}

std::uint64_t fnv(const std::string& text, std::uint64_t h = 0xcbf29ce484222325ull) {
    for (const unsigned char c : text) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

int layerOf(const Registry& r, const std::string& name) { return r.textureLayer(name).value_or(0); }

// The scree a category's slopes gather: its own, else its rock's.
std::string screeOf(const Registry& r, const Category& c) {
    if (!c.scree.empty()) return c.scree;
    if (c.slopeRock.empty()) return {};
    const auto k = r.rockIndex(c.slopeRock);
    return k ? r.rocks()[*k].scree : std::string();
}

bool handlesRock(const Registry& r, const Category& c) {
    return !c.soils[3].empty() || !c.slopeRock.empty() || !screeOf(r, c).empty();
}

std::vector<std::size_t> shaderDecalsOf(const Registry& r, const Weighted& list) {
    std::vector<std::size_t> out;
    for (const auto& [name, w] : list)
        if (const auto i = r.decalIndex(name); i && r.decals()[*i].kind != DecalKind::Instance &&
                                               out.size() < kShaderDecalsPerSet)
            out.push_back(*i);
    return out;
}

std::string layersCode(const Registry& r) {
    std::ostringstream o;
    o << "// Generated from content/config/terrain/layers.json by engine::biomes (worldtool biomes --emit).\n"
         "// Do not edit: it is remade whenever the config changes.\n"
         "#ifndef TERRAIN_LAYERS_GEN_HLSLI\n#define TERRAIN_LAYERS_GEN_HLSLI\n"
         "#define TERRAIN_LAYERS_GENERATED 1\n";
    const auto& layers = r.textureLayers();
    o << "#define GROUND_LAYERS " << layers.size() << "\n";
    for (std::size_t i = 0; i < layers.size(); ++i) {
        std::string name = layers[i].name;
        for (auto& c : name) c = (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') :
            ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
        o << "#define TERRAIN_LAYER_" << name << " " << i << "\n";
    }
    o << "static const float kLayerMetres[GROUND_LAYERS] = {";
    for (std::size_t i = 0; i < layers.size(); ++i) o << (i ? "," : "") << (i % 8 == 0 ? "\n    " : " ") << lit(layers[i].metres);
    o << "};\n#endif\n";
    return o.str();
}

std::string biomesCode(const Registry& r) {
    std::ostringstream o;
    o << "// Generated from content/config/terrain by engine::biomes (worldtool biomes --emit).\n"
         "// Do not edit: it is remade whenever the config's structure changes. Numbers are\n"
         "// not here - they are rows of the biome table (terrain_biomes.hlsli); this only\n"
         "// says which category takes which soil, rock, noise kind, layer and decal.\n"
         "#ifndef TERRAIN_BIOMES_GEN_HLSLI\n#define TERRAIN_BIOMES_GEN_HLSLI\n"
         "#define BIOMES_GENERATED 1\n";
    std::vector<const Category*> grounds, decalled;
    for (const auto& c : r.categories()) {
        if (c.id == 0) continue;
        bool soils = std::any_of(c.soils.begin(), c.soils.end(), [](const std::string& s) { return !s.empty(); });
        if (soils || handlesRock(r, c)) grounds.push_back(&c);
        if (!shaderDecalsOf(r, c.decals).empty()) decalled.push_back(&c);
    }
    std::vector<const DecorBiome*> decors;
    for (const auto& d : r.decorBiomes())
        if (!shaderDecalsOf(r, d.decals).empty()) decors.push_back(&d);
    std::vector<const ForestBiome*> floors;
    for (const auto& f : r.forestBiomes())
        if (!f.floorSoil.empty() && r.soilIndex(f.floorSoil)) floors.push_back(&f);

    // Only what something uses is compiled in.
    std::set<NoiseKind> kinds;
    std::set<DecalKind> families;
    std::set<std::size_t> strata;
    for (const auto* c : grounds) {
        for (const auto& s : c->soils)
            if (!s.empty()) if (const auto i = r.soilIndex(s); i && r.soils()[*i].layers.size() >= 2) kinds.insert(r.soils()[*i].noise.kind);
        if (const auto k = c->slopeRock.empty() ? std::nullopt : r.rockIndex(c->slopeRock); k && r.rocks()[*k].strataMetres > 0)
            strata.insert(*k);
    }
    for (const auto* c : decalled) for (const auto i : shaderDecalsOf(r, c->decals)) families.insert(r.decals()[i].kind);
    for (const auto* d : decors) for (const auto i : shaderDecalsOf(r, d->decals)) families.insert(r.decals()[i].kind);
    if (!grounds.empty()) o << "#define BIOME_ANY_GROUND 1\n";
    if (!decalled.empty() || !decors.empty()) o << "#define BIOME_ANY_DECALS 1\n";
    if (!floors.empty()) o << "#define BIOME_ANY_FLOOR 1\n";
    if (!strata.empty()) o << "#define BIOME_ANY_STRATA 1\n";
    for (const auto k : kinds) {
        std::string name = kNoiseKindNames[std::size_t(k)];
        std::transform(name.begin(), name.end(), name.begin(), [](char ch) { return char(std::toupper(ch)); });
        o << "#define BIOME_NOISE_" << name << " 1\n";
    }
    for (const auto f : families) {
        std::string name = kDecalKindNames[std::size_t(f)];
        std::transform(name.begin(), name.end(), name.begin(), [](char ch) { return char(std::toupper(ch)); });
        o << "#define BIOME_DECAL_" << name << " 1\n";
    }

    const auto soilOf = [&](const std::string& name) {
        // biomeSoilOf(row, noise kind, layers, l0, l1, l2)
        const auto i = r.soilIndex(name);
        if (!i) return std::string("noBiomeSoil()");
        const Soil& s = r.soils()[*i];
        const std::size_t n = std::min<std::size_t>(s.layers.size(), 3);
        const int l0 = n > 0 ? layerOf(r, s.layers[0].first) : 0;
        const int l1 = n > 1 ? layerOf(r, s.layers[1].first) : l0;
        const int l2 = n > 2 ? layerOf(r, s.layers[2].first) : l0;
        std::ostringstream c;
        c << "biomeSoilOf(BIOME_ROW_SOIL + " << *i << ", " << int(s.noise.kind) << ", " << n << ", " << l0 << ", " << l1
          << ", " << l2 << ")";
        return c.str();
    };

    o << "\n// Does the category draw any class differently from the engine's own?\n"
         "bool biomeCategoryChangesGround(int category)\n{\n";
    if (!grounds.empty()) {
        o << "    switch (category) {\n";
        for (const auto* c : grounds) o << "    case " << c->id << ":   // " << c->name << "\n";
        o << "        return true;\n    }\n";
    }
    o << "    return false;\n}\n";

    o << "\n// The soil a category puts behind a class (all but rock, which is a slope).\n"
         "// False: the class is the engine's own in this category.\n"
         "bool biomeClassSoil(int category, int cls, out BiomeSoil soil)\n{\n"
         "    soil = noBiomeSoil();\n";
    if (!grounds.empty()) {
        o << "    switch (category) {\n";
        for (const auto* c : grounds) {
            bool any = false;
            for (std::size_t k = 0; k < kClasses; ++k) any = any || (k != 3 && !c->soils[k].empty());
            if (!any) continue;
            o << "    case " << c->id << ":   // " << c->name << "\n";
            for (std::size_t k = 0; k < kClasses; ++k) {
                if (k == 3 || c->soils[k].empty() || !r.soilIndex(c->soils[k])) continue;
                o << "        if (cls == " << k << ") { soil = " << soilOf(c->soils[k]) << "; return true; }   // "
                  << kClassNames[k] << ": " << c->soils[k] << "\n";
            }
            o << "        break;\n";
        }
        o << "    }\n";
    }
    o << "    return false;\n}\n";

    o << "\n// A category's slopes: the rock class's soil, the face's layer and rock row,\n"
         "// the scree's layer. False: the engine's own cliffs.\n"
         "bool biomeCategorySlope(int category, out BiomeSlope slope)\n{\n"
         "    slope = noBiomeSlope();\n";
    bool anySlope = false;
    for (const auto* c : grounds) anySlope = anySlope || handlesRock(r, *c);
    if (anySlope) {
        o << "    switch (category) {\n";
        for (const auto* c : grounds) {
            if (!handlesRock(r, *c)) continue;
            int face = -1, rockRow = -1, scree = -1;
            bool hasStrata = false;
            if (const auto k = c->slopeRock.empty() ? std::nullopt : r.rockIndex(c->slopeRock)) {
                face = layerOf(r, r.rocks()[*k].layer);
                rockRow = kRowRock + int(*k);
                hasStrata = r.rocks()[*k].strataMetres > 0;
            }
            const auto screeName = screeOf(r, *c);
            if (const auto q = screeName.empty() ? std::nullopt : r.soilIndex(screeName); q && !r.soils()[*q].layers.empty())
                scree = layerOf(r, r.soils()[*q].layers[0].first);
            o << "    case " << c->id << ":   // " << c->name << ": " << (c->slopeRock.empty() ? "-" : c->slopeRock)
              << ", scree " << (screeName.empty() ? "-" : screeName) << "\n"
              << "        slope.soil = " << (c->soils[3].empty() ? std::string("noBiomeSoil()") : soilOf(c->soils[3])) << ";\n"
              << "        slope.face = " << face << "; slope.rockRow = " << rockRow << "; slope.scree = " << scree
              << "; slope.strata = " << (hasStrata ? "true" : "false") << ";\n"
              << "        return true;\n";
        }
        o << "    }\n";
    }
    o << "    return false;\n}\n";

    o << "\n// The forest floor's layer under a forest biome; -1: the engine's.\n"
         "int biomeForestFloor(int forest)\n{\n";
    if (!floors.empty()) {
        o << "    switch (forest) {\n";
        for (const auto* f : floors) {
            const Soil& s = r.soils()[*r.soilIndex(f->floorSoil)];
            o << "    case " << f->id << ": return " << (s.layers.empty() ? 7 : layerOf(r, s.layers[0].first))
              << ";   // " << f->name << ": " << f->floorSoil << "\n";
        }
        o << "    }\n";
    }
    o << "    return -1;\n}\n";

    // Decal sets: up to four (family, row) a set; the weights are the table's.
    const auto decalSet = [&](const std::vector<std::size_t>& list) {
        int rows[4] = {-1, -1, -1, -1}, fams[4] = {-1, -1, -1, -1};
        for (std::size_t k = 0; k < list.size() && k < 4; ++k) {
            rows[k] = kRowDecal + int(list[k]);
            fams[k] = int(r.decals()[list[k]].kind);
        }
        std::ostringstream c;
        c << "rows = int4(" << rows[0] << ", " << rows[1] << ", " << rows[2] << ", " << rows[3] << "); kinds = int4("
          << fams[0] << ", " << fams[1] << ", " << fams[2] << ", " << fams[3] << ");";
        std::string names;
        for (const auto i : list) names += (names.empty() ? "" : ", ") + r.decals()[i].name;
        return c.str() + "   // " + names;
    };
    o << "\n// The shader decals a category switches on: rows of the table and families\n"
         "// (0 speckle, 1 stain, 2 streak); -1 empty. Their weights: the category's\n"
         "// row, column 10.\n"
         "bool biomeCategoryDecalSet(int category, out int4 rows, out int4 kinds)\n{\n"
         "    rows = int4(-1, -1, -1, -1); kinds = int4(-1, -1, -1, -1);\n";
    if (!decalled.empty()) {
        o << "    switch (category) {\n";
        for (const auto* c : decalled)
            o << "    case " << c->id << ": " << decalSet(shaderDecalsOf(r, c->decals)) << " return true;   // " << c->name << "\n";
        o << "    }\n";
    }
    o << "    return false;\n}\n";
    o << "\n// The same for a decor biome; weights: its row of BIOME_ROW_DECOR, column 0.\n"
         "bool biomeDecorDecalSet(int decor, out int4 rows, out int4 kinds)\n{\n"
         "    rows = int4(-1, -1, -1, -1); kinds = int4(-1, -1, -1, -1);\n";
    if (!decors.empty()) {
        o << "    switch (decor) {\n";
        for (const auto* d : decors)
            o << "    case " << d->id << ": " << decalSet(shaderDecalsOf(r, d->decals)) << " return true;   // " << d->name << "\n";
        o << "    }\n";
    }
    o << "    return false;\n}\n#endif\n";
    return o.str();
}

void put(std::vector<float>& t, int row, int column, double x, double y, double z, double w) {
    if (row < 0 || row >= kTableRows || column < 0 || column >= kTableColumns) return;
    const std::size_t at = (std::size_t(row) * kTableColumns + std::size_t(column)) * 4;
    t[at] = float(x); t[at + 1] = float(y); t[at + 2] = float(z); t[at + 3] = float(w);
}

void putNoise(std::vector<float>& t, int row, int column, const Noise& n) {
    put(t, row, column, n.metres, n.contrast, n.warp, n.angle * kPi / 180.0);
}

bool writeIfDifferent(const fs::path& file, const std::string& text, bool& changed, std::string* why) {
    std::error_code ec;
    if (fs::exists(file, ec)) {
        std::ifstream in(file, std::ios::binary);
        const std::string have((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (have == text) return true;
    }
    const auto partial = fs::path(file.string() + ".partial");
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) { if (why) *why = "cannot write " + partial.string(); return false; }
    }
    fs::rename(partial, file, ec);
    if (ec) { if (why) *why = "cannot replace " + file.string() + ": " + ec.message(); return false; }
    changed = true;
    return true;
}

fs::path goodDirectory(const fs::path& shaders) {
    return fs::absolute(shaders).parent_path().parent_path() / ".cache" / "biomes";
}

} // namespace

ShaderCode shaderCode(const Registry& registry) { return {layersCode(registry), biomesCode(registry)}; }

std::uint64_t structureKey(const Registry& registry) {
    const auto code = shaderCode(registry);
    return fnv(code.biomes, fnv(code.layers));
}

std::vector<float> shaderTable(const Registry& r) {
    std::vector<float> t(std::size_t(kTableColumns) * kTableRows * 4, 0.0f);
    // Every category row starts as the default's: no tint, no change.
    for (int id = 0; id < 256; ++id) {
        const int row = kRowCategory + id;
        put(t, row, 0, 1, 1, 1, 1);
        put(t, row, 1, 0.45, 0, 0, 0);
        for (int k = 0; k < int(kClasses); ++k) put(t, row, 3 + k, 1, 1, 1, 0);
    }
    const auto idOf = [](const auto& list, const std::string& name) -> double {
        for (const auto& x : list)
            if (x.name == name) return double(x.id);
        return 0.0;
    };
    for (const auto& c : r.categories()) {
        if (c.id > 255) continue;
        const int row = kRowCategory + int(c.id);
        put(t, row, 0, c.tint[0], c.tint[1], c.tint[2], c.saturation);
        put(t, row, 1, c.steepFrom, 0, 0, 0);
        for (std::size_t k = 0; k < kClasses; ++k) {
            Rgb tint{1, 1, 1};
            if (!c.soils[k].empty())
                if (const auto i = r.soilIndex(c.soils[k])) tint = r.soils()[*i].tint;
            put(t, row, 3 + int(k), tint[0], tint[1], tint[2], 0);
        }
        double foliage = 0;
        if (c.groundFoliage == "none") foliage = -1;
        else if (const auto i = c.groundFoliage.empty() ? std::nullopt : r.foliageIndex(c.groundFoliage)) foliage = double(*i + 1);
        put(t, row, 9, idOf(r.forestBiomes(), c.forest), idOf(r.waterBiomes(), c.water), idOf(r.decorBiomes(), c.decor), foliage);
        // The shader decals' weights, in the order the code calls them.
        double w[kShaderDecalsPerSet]{};
        std::size_t k = 0;
        for (const auto& [name, weight] : c.decals)
            if (const auto i = r.decalIndex(name); i && r.decals()[*i].kind != DecalKind::Instance && k < kShaderDecalsPerSet)
                w[k++] = weight;
        put(t, row, 10, w[0], w[1], w[2], w[3]);
    }
    for (std::size_t i = 0; i < r.soils().size() && i < 256; ++i) {
        const Soil& s = r.soils()[i];
        const int row = kRowSoil + int(i);
        putNoise(t, row, 0, s.noise);
        const double s0 = s.layers.size() > 0 ? s.layers[0].second : 1.0;
        const double s1 = s.layers.size() > 1 ? s.layers[1].second : 0.0;
        const double s2 = s.layers.size() > 2 ? s.layers[2].second : 0.0;
        // `first` covers its share outright; `second` takes its share of what is left.
        put(t, row, 1, s0, s1, s1 < 1.0 ? s2 / (1.0 - s1) : 0.0, 0);
        put(t, row, 2, s.tint[0], s.tint[1], s.tint[2], 0);
    }
    for (const auto& w : r.waterBiomes()) {
        if (w.id > 255) continue;
        const int row = kRowWater + int(w.id);
        const Rgb colour = w.colour.value_or(Rgb{0, 0, 0});
        put(t, row, 0, colour[0], colour[1], colour[2], w.colour ? 1.0 : 0.0);
        put(t, row, 1, w.turbidity, w.scum, w.foam, w.emissive);
    }
    for (std::size_t i = 0; i < r.decals().size() && i < 256; ++i) {
        const Decal& d = r.decals()[i];
        const int row = kRowDecal + int(i);
        put(t, row, 0, d.colour[0], d.colour[1], d.colour[2], d.opacity);
        put(t, row, 1, d.cellMetres, d.density, d.sizeMetres[0], d.sizeMetres[1]);
        put(t, row, 2, d.clusterMetres, d.clusterShare, d.metal, d.rough);
        put(t, row, 3, d.slope[0], d.slope[1], d.underFoliage, d.edgeNoise);
        put(t, row, 4, d.fadeMetres[0], d.fadeMetres[1], d.emissive, d.nearWaterMetres);
        put(t, row, 5, d.angle * kPi / 180.0, d.alongWind ? 1.0 : 0.0,
            d.texture.empty() ? 0.0 : double(layerOf(r,d.texture) + 1), 0);
    }
    for (const auto& f : r.forestBiomes()) {
        if (f.id > 255) continue;
        const int row = kRowForest + int(f.id);
        put(t, row, 0, f.fertility, f.shrubDensity, f.clearingShare, f.edgeShrubs);
        putNoise(t, row, 1, f.floorNoise.value_or(Noise{}));
        // Its undergrowth: -1 none, 0 the engine's, else foliage index + 1.
        double under = 0;
        if (f.undergrowth == "none") under = -1;
        else if (const auto i = f.undergrowth.empty() ? std::nullopt : r.foliageIndex(f.undergrowth)) under = double(*i + 1);
        put(t, row, 2, under, 0, 0, 0);
    }
    for (std::size_t i = 0; i < r.foliage().size() && i < 256; ++i) {
        const Foliage& f = r.foliage()[i];
        const int row = kRowFoliage + int(i);
        put(t, row, 0, f.density, f.height, f.dryness, f.flowers);
        put(t, row, 1, f.tint[0], f.tint[1], f.tint[2], 0);
    }
    for (std::size_t i = 0; i < r.rocks().size() && i < 256; ++i) {
        const Rock& k = r.rocks()[i];
        const int row = kRowRock + int(i);
        put(t, row, 0, k.tint[0], k.tint[1], k.tint[2], 0);
        put(t, row, 1, k.strataMetres, k.strataTilt, 0, 0);
    }
    for (const auto& d : r.decorBiomes()) {
        if (d.id > 255) continue;
        double w[kShaderDecalsPerSet]{};
        std::size_t k = 0;
        for (const auto& [name, weight] : d.decals)
            if (const auto i = r.decalIndex(name); i && r.decals()[*i].kind != DecalKind::Instance && k < kShaderDecalsPerSet)
                w[k++] = weight;
        put(t, kRowDecor + int(d.id), 0, w[0], w[1], w[2], w[3]);
    }
    return t;
}

EmitResult emitShaderCode(const Registry& registry, const fs::path& shaders, const fs::path& assets) {
    EmitResult result;
    for (const auto& p : registry.validate(assets)) result.problems.push_back(p.file + ": " + p.what);
    if (!result.problems.empty()) return result;
    const auto code = shaderCode(registry);
    std::string why;
    if (!writeIfDifferent(shaders / kLayersFile, code.layers, result.changed, &why) ||
        !writeIfDifferent(shaders / kBiomesFile, code.biomes, result.changed, &why)) {
        result.problems.push_back(why);
        return result;
    }
    result.ok = true;
    return result;
}

void markShaderCodeGood(const fs::path& shaders) {
    std::error_code ec;
    const auto good = goodDirectory(shaders);
    fs::create_directories(good, ec);
    for (const char* name : {kLayersFile, kBiomesFile})
        if (fs::exists(shaders / name, ec)) fs::copy_file(shaders / name, good / name, fs::copy_options::overwrite_existing, ec);
}

bool restoreShaderCode(const fs::path& shaders) {
    std::error_code ec;
    const auto good = goodDirectory(shaders);
    bool changed = false;
    for (const char* name : {kLayersFile, kBiomesFile}) {
        if (!fs::exists(good / name, ec)) {
            // Never built well: without the file the shaders fall back to the engine's own.
            if (fs::exists(shaders / name, ec)) { fs::remove(shaders / name, ec); changed = true; }
            continue;
        }
        std::ifstream in(good / name, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string why;
        writeIfDifferent(shaders / name, text, changed, &why);
    }
    return changed;
}

StartUp startUp(const fs::path& dir, const fs::path& shaders, const fs::path& assets) {
    StartUp out;
    out.registry = active();
    std::error_code ec;
    if (!fs::exists(dir / "categories.json", ec)) {
        out.problems.push_back(dir.string() + ": no categories.json; every ground is the engine's own");
        return out;
    }
    std::vector<Problem> problems;
    auto registry = Registry::load(dir, &problems);
    for (const auto& p : registry->validate(assets)) problems.push_back(p);
    for (const auto& p : problems) out.problems.push_back(p.file + ": " + p.what);
    if (!problems.empty()) return out;
    setActive(registry);
    out.registry = registry;
    out.loaded = true;
    if (!shaders.empty()) {
        const auto emitted = emitShaderCode(*registry, shaders, assets);
        out.structureChanged = emitted.changed;
        for (const auto& p : emitted.problems) out.problems.push_back(p);
    }
    return out;
}

fs::file_time_type registryWritten(const fs::path& dir) {
    std::error_code ec;
    fs::file_time_type newest{};
    for (const auto& entry : fs::directory_iterator(dir, ec))
        if (entry.path().extension() == ".json") newest = std::max(newest, entry.last_write_time(ec));
    return newest;
}

} // namespace engine::biomes
