// Builds the cluster DAG sidecar for every scene model that has solid geometry.
//
// Offline, because it is: a build takes tens of milliseconds per model and the
// answer never changes until the model does. Run it after
// tools/prepare_scene_models.py and before the renderer wants clusters:
//
//     ./build/scene_model_clusters assets/generated/scene_models
//
// Full-source CPU reference assets, including foliage and imported seams:
//     ./build/scene_model_clusters --source assets/generated/scene_models
// Writes .source.clusters, never overwriting the legacy renderer's sidecars.
// Only the finest source indices are used; no shells or thinned chain levels.
//
// A model with no sidecar simply has no clusters, and the renderer carries on
// with the discrete chain, so running this is an addition and never a
// prerequisite.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <array>
#include <map>
#include <span>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "engine/geometry/cluster_asset.hpp"
#include "engine/geometry/smart_mesh.hpp"
#include "engine/render/level_of_detail.hpp"
#include <nlohmann/json.hpp>

namespace {
// The vertex tools/prepare_scene_models.py writes: position, normal, uv,
// colour, texture layer.
constexpr std::size_t kVertexFloats = 12;

struct Mesh {
    std::vector<float> positions;          // three per vertex
    std::vector<float> layers;             // the texture layer of each vertex
    std::vector<std::uint64_t> attributes; // exact non-position vertex attributes
    std::vector<std::uint64_t> materials;  // hard material identity for seams
    std::vector<std::uint32_t> indices;    // every level of the chain
    std::vector<engine::IndexRange> levels;
    std::uint32_t vertices = 0;
};

bool read(const std::filesystem::path& file, Mesh& into, std::string& why) {
    std::ifstream input(file, std::ios::binary);
    if (!input) { why = "cannot open"; return false; }
    char magic[4]{};
    std::uint32_t vertices = 0, levels = 0;
    input.read(magic, 4);
    input.read(reinterpret_cast<char*>(&vertices), 4);
    input.read(reinterpret_cast<char*>(&levels), 4);
    if (!input || std::memcmp(magic, "SCM2", 4) != 0) { why = "not an SCM2 mesh"; return false; }
    if (vertices == 0 || vertices > 4000000u || levels == 0 || levels > 8) {
        why = "implausible header";
        return false;
    }
    std::vector<std::uint32_t> perLevel(levels);
    for (auto& count : perLevel) input.read(reinterpret_cast<char*>(&count), 4);
    if (!input || perLevel[0] == 0 || perLevel[0] % 3) { why = "bad level table"; return false; }

    std::vector<float> all(std::size_t(vertices) * kVertexFloats);
    input.read(reinterpret_cast<char*>(all.data()), std::streamsize(all.size() * 4));
    if (!input) { why = "truncated vertices"; return false; }
    into.vertices = vertices;
    into.positions.resize(std::size_t(vertices) * 3);
    into.layers.resize(vertices);
    into.attributes.resize(vertices);
    into.materials.resize(vertices);
    for (std::uint32_t v = 0; v < vertices; ++v) {
        for (int axis = 0; axis < 3; ++axis)
            into.positions[std::size_t(v) * 3 + axis] = all[std::size_t(v) * kVertexFloats + axis];
        into.layers[v] = all[std::size_t(v) * kVertexFloats + 11];
        std::uint32_t materialBits = 0;
        std::memcpy(&materialBits, &into.layers[v], sizeof(materialBits));
        into.materials[v] = materialBits;
        // Hash the exact normal/UV/colour/layer payload. Position is hashed by
        // the geometry builder itself, so this key only prevents an attribute
        // seam from being welded to an arbitrary neighbouring vertex.
        std::uint64_t key = 1469598103934665603ull;
        for (std::size_t field = 3; field < kVertexFloats; ++field) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &all[std::size_t(v) * kVertexFloats + field], sizeof(bits));
            if (bits == 0x80000000u) bits = 0;
            key = (key ^ bits) * 1099511628211ull;
        }
        into.attributes[v] = key;
    }

    // Every level. The DAG is built from the finest - the coarser ones are a
    // different answer to the same question - but each level's alpha cards are
    // carried out, because a coarse level's cards are the ones the offline
    // thinning already reduced for the distance that level is drawn at.
    std::uint32_t total = 0;
    for (const auto count : perLevel) {
        into.levels.push_back({total, count});
        total += count;
    }
    into.indices.resize(total);
    input.read(reinterpret_cast<char*>(into.indices.data()), std::streamsize(total * 4));
    if (!input) { why = "truncated indices"; return false; }
    for (const auto index : into.indices)
        if (index >= vertices) { why = "an index past the vertices"; return false; }
    return true;
}
}

// What the two representations cost at the same size on screen.
//
// The chain answers with a whole level; the DAG answers with a cut, which takes
// the coarse part of a model coarse and the part that does not simplify well
// fine. Comparing them at one allowance is the only honest comparison: triangles
// alone say nothing without the error they were measured at.
void distanceTable(const std::string& name, const engine::geometry::ClusterAsset& asset,
                   const std::vector<float>& errors, const std::vector<std::size_t>& chain,
                   double extent) {
    if (errors.empty() || chain.empty() || !(extent > 0)) return;
    // A 1080p frame at sixty degrees across: 960 / tan(30) pixels per radian.
    constexpr double kFocal = 1662.0;
    std::cout << "    on screen   distance   chain       cut    drawn  saved" << char(10);
    for (const double pixels : {1200.0, 600.0, 300.0, 150.0, 75.0, 40.0, 25.0}) {
        const auto level = engine::render::levelFor(errors, pixels, extent);
        const double allowance = engine::render::kLevelPixelError * extent / pixels;
        std::size_t cut = 0;
        std::vector<std::uint32_t> selected;
        if (engine::geometry::cutAtHierarchy(asset.clusters, allowance, selected)) {
            for (const auto id : selected) cut += asset.clusters[id].indices.count / 3;
        } else {
            for (const auto& cluster : asset.clusters)
                if (double(cluster.error) <= allowance && double(cluster.parentError) > allowance)
                    cut += cluster.indices.count / 3;
        }
        if (level < asset.cardLevels.size()) cut += asset.cardLevels[level].count / 3;
        const auto whole = chain[std::min(level, chain.size() - 1)];
        // What the frame actually draws: whichever is cheaper at the SAME
        // allowance, which is the rule planDraws applies.
        const auto drawn = std::min(cut, whole);
        std::printf("    %6.0f px  %7.0f m  %6zu    %6zu   %6zu   %+.0f%%%c", pixels,
                    extent * kFocal / pixels, whole, cut, drawn,
                    whole ? 100.0 * (double(drawn) / double(whole) - 1.0) : 0.0, 10);
    }
    (void)name;
}

// The grove, as the thing it actually is: nine trees standing together, merged
// into one surface and clustered.
//
// Not a new kind of object with its own asset and its own code path - the same
// merge that turns a crown into a surface, with nine trees in the input instead
// of one and a cell a grove wide instead of a leaf. The layout is the one
// prepare_scene_models.py bakes its grove pictures from, so the shell stands
// where the picture stood.
// Which texture the shell of a model should wear.
//
// The cards' own layer when it has cards, because at the distance a shell is
// drawn a tree reads as its leaves - and asking the whole mesh instead gives
// the TRUNK's answer, since a tree has twice as much bark as leaves by triangle
// count. The crown came out in bark that way, which is exactly what it looks
// like. The most common layer otherwise, for a model whose foliage is solid
// geometry or which has no foliage at all.
// Which material the crown shell wears.
//
// One rule for every model, and it used to be two. Where a model had alpha
// cards the layer was taken from their corners; where it had none - a conifer
// whose needles are modelled rather than billboarded - it fell back to counting
// every index, which picks whatever has the most TRIANGLES. On a pine that is
// the trunk and the branches, because tubes are expensive and needles are
// cheap, so the crown came out wearing bark. That is the "crown still looks
// like trunks" you can see from any distance.
//
// A crown is the part of a tree that is high and far from its axis. Weighting
// each vertex by how far out and how far up it sits answers the question for
// both kinds of model at once: cards are out and up by construction, and so are
// needles, while bark is low and central however much of it there is.
float shellLayer(const Mesh& mesh) {
    if (mesh.levels.empty()) return 0;
    const auto& finest = mesh.levels.front();
    if (finest.count == 0) return 0;

    // Where there are cards, they ARE the foliage and there is nothing to work
    // out: whoever authored the model already said which material the leaves
    // are by putting them on billboards. Their corners answer directly, and
    // they answer better than any geometric rule can - on a broadleaf the outer
    // twigs reach further than the leaf cards do, so a rule about what is
    // outermost picks the bark.
    std::vector<std::array<std::uint32_t, 4>> corners;
    engine::geometry::cardsOf(mesh.positions,
                              std::span<const std::uint32_t>(mesh.indices).subspan(finest.first,
                                                                                   finest.count),
                              &corners);
    if (!corners.empty()) {
        std::map<int, std::size_t> seen;
        for (const auto& card : corners)
            for (const auto v : card)
                if (v < mesh.layers.size()) ++seen[int(mesh.layers[v])];
        float best = 0;
        std::size_t most = 0;
        for (const auto& [value, count] : seen)
            if (count > most) { most = count; best = float(value); }
        return best;
    }

    double lowest = 1e30, highest = -1e30, axisX = 0, axisY = 0;
    for (std::uint32_t i = 0; i < finest.count; ++i) {
        const auto v = mesh.indices[finest.first + i];
        lowest = std::min(lowest, double(mesh.positions[v * 3 + 2]));
        highest = std::max(highest, double(mesh.positions[v * 3 + 2]));
        axisX += mesh.positions[v * 3];
        axisY += mesh.positions[v * 3 + 1];
    }
    axisX /= double(finest.count);
    axisY /= double(finest.count);
    double reach = 1e-6;
    for (std::uint32_t i = 0; i < finest.count; ++i) {
        const auto v = mesh.indices[finest.first + i];
        const double dx = mesh.positions[v * 3] - axisX, dy = mesh.positions[v * 3 + 1] - axisY;
        reach = std::max(reach, std::sqrt(dx * dx + dy * dy));
    }
    const double rise = std::max(1e-6, highest - lowest);
    std::map<int, double> score;
    for (std::uint32_t i = 0; i < finest.count; ++i) {
        const auto v = mesh.indices[finest.first + i];
        if (v >= mesh.layers.size()) continue;
        const double dx = mesh.positions[v * 3] - axisX, dy = mesh.positions[v * 3 + 1] - axisY;
        const double out = std::sqrt(dx * dx + dy * dy) / reach;
        const double up = (double(mesh.positions[v * 3 + 2]) - lowest) / rise;
        // Raised to a high power on purpose: what the shell wears is what is on
        // its OUTSIDE, and a tree's branches reach out and up as surely as its
        // foliage does. Under a plain product the bark wins on a conifer simply
        // by having more geometry. At the sixth power only the outermost tenth
        // of the radius counts for anything, which is the surface a shell is
        // wrapped around.
        const double skin = out * out * out * out * out * out;
        score[int(mesh.layers[v])] += skin * up;
    }
    float best = 0;
    double most = -1;
    for (const auto& [value, weight] : score) {
        if (weight > most) { most = weight; best = float(value); }
    }
    return best;
}

bool buildGrove(const std::filesystem::path& root, const std::string& name, const Mesh& mesh,
                double width, float layer) {
    if (mesh.levels.empty() || !(width > 0)) return false;
    const auto& finest = mesh.levels.front();
    std::vector<float> positions;
    std::vector<float> layers;
    std::vector<std::uint32_t> indices;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            const double ox = x * 7.5 + (((y % 2) + 2) % 2) * 1.3, oy = y * 7.5;
            const double factor = 0.86 + (((x + 2 * y) % 5 + 5) % 5) * 0.06;
            const auto base = std::uint32_t(positions.size() / 3);
            for (std::uint32_t v = 0; v < mesh.vertices; ++v) {
                positions.push_back(float(mesh.positions[std::size_t(v) * 3] * factor + ox));
                positions.push_back(float(mesh.positions[std::size_t(v) * 3 + 1] * factor + oy));
                positions.push_back(float(mesh.positions[std::size_t(v) * 3 + 2] * factor));
                layers.push_back(mesh.layers[v]);
            }
            for (std::uint32_t i = 0; i < finest.count; ++i)
                indices.push_back(base + mesh.indices[finest.first + i]);
        }

    engine::geometry::ClusterAsset grove;
    grove.sourceVertices = std::uint32_t(positions.size() / 3);
    grove.sourceTriangles = std::uint32_t(indices.size() / 3);
    const engine::IndexRange one[]{{0, std::uint32_t(indices.size())}};
    // A cell a sixteenth of the grove across: at the distance a grove is drawn,
    // one tree of nine is not a thing a viewer resolves.
    grove.crownLayer = layer;
    engine::geometry::buildCrown(grove, positions, indices, one, width / 16.0, layers);
    if (grove.crownClusters.empty()) return false;

    std::size_t finestTriangles = 0, coarsest = 0;
    for (const auto& cluster : grove.crownClusters) {
        if (cluster.level == 0) finestTriangles += cluster.indices.count / 3;
        if (cluster.level + 1 == grove.crownLevels) coarsest += cluster.indices.count / 3;
    }
    std::cout << name << "-grove          nine trees = " << indices.size() / 3 << " tri -> shell "
              << finestTriangles << " -> " << coarsest << " over " << grove.crownLevels
              << " levels, cell " << grove.crownCellMetres << " m" << char(10);

    const auto bytes = engine::geometry::encodeClusters(grove);
    std::string why;
    if (engine::geometry::decodeClusters(bytes, why).crownClusters.size() !=
        grove.crownClusters.size()) {
        std::cerr << name << "-grove: what was written does not read back: " << why << char(10);
        return false;
    }
    std::ofstream out(root / (name + "-grove.clusters"), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    return bool(out);
}

bool buildSource(const std::filesystem::path& file, const Mesh& mesh) {
    const auto& finest = mesh.levels.front();
    const auto source = std::span<const std::uint32_t>(mesh.indices).subspan(finest.first, finest.count);
    const auto asset = engine::geometry::buildSourceClusterAsset(mesh.positions, source,
                                                                 engine::geometry::naniteProfile());
    if (asset.empty()) {
        std::cerr << file.filename() << ": cannot build full source hierarchy\n";
        return false;
    }
    // Connectivity, winding, source attribute indices and triangle multiplicity
    // must all survive. Vertex counts alone cannot detect a rewired leaf card.
    using Face = std::array<std::uint32_t, 3>;
    std::vector<Face> original, restored;
    for (std::size_t i = 0; i < source.size(); i += 3)
        original.push_back({source[i], source[i+1], source[i+2]});
    for (const auto& cluster : asset.clusters)
        if (cluster.level == 0)
            for (std::uint32_t i = 0; i < cluster.indices.count; i += 3) {
                const auto at = cluster.indices.first + i;
                restored.push_back({asset.indices[at], asset.indices[at+1], asset.indices[at+2]});
            }
    std::sort(original.begin(), original.end());
    std::sort(restored.begin(), restored.end());
    if (original != restored) {
        std::cerr << file.filename() << ": source triangles changed during clustering\n";
        return false;
    }
    const auto bytes = engine::geometry::encodeClusters(asset);
    std::string why;
    const auto back = engine::geometry::decodeClusters(bytes, why);
    if (!why.empty() || back.empty() || back.indices != asset.indices ||
        back.clusters.size() != asset.clusters.size()) {
        std::cerr << file.filename() << ": source hierarchy did not round-trip: " << why << '\n';
        return false;
    }
    // Selection must use the same stored intervals after loading the asset.
    for (std::size_t i = 0; i < asset.clusters.size(); ++i) {
        const auto& a = asset.clusters[i];
        const auto& b = back.clusters[i];
        if (a.error != b.error || a.parentError != b.parentError ||
            a.indices.first != b.indices.first || a.indices.count != b.indices.count) {
            std::cerr << file.filename() << ": source selection metadata changed\n";
            return false;
        }
    }
    auto outputPath = file;
    outputPath.replace_extension(".source.clusters");
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    output.close();
    if (!output) {
        std::cerr << "cannot write " << outputPath << '\n';
        return false;
    }
    std::cout << file.stem().string() << ": source " << asset.sourceTriangles << " tri, "
              << asset.clusters.size() << " clusters, " << asset.levels << " levels\n";
    for (double allowance : {0.0, 0.001, 0.01, 0.1, 1.0, 10.0}) {
        std::size_t triangles = 0;
        std::vector<std::uint32_t> selected;
        if (engine::geometry::cutAtHierarchy(back.clusters, allowance, selected)) {
            for (const auto id : selected) triangles += back.clusters[id].indices.count / 3;
        } else {
            for (const auto& c : back.clusters)
                if (c.error <= allowance && allowance < c.parentError)
                    triangles += c.indices.count / 3;
        }
        std::cout << "    error <= " << allowance << " m: " << triangles << " tri\n";
    }
    return true;
}

int main(int argc, char** argv) {
    std::filesystem::path root = "assets/generated/scene_models";
    bool sourceMode = false, haveRoot = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--source") sourceMode = true;
        else if (!argument.starts_with("-") && !haveRoot) { root = argument; haveRoot = true; }
        else {
            std::cerr << "usage: scene_model_clusters [--source] [scene_model_directory]\n";
            return argument == "--help" ? 0 : 2;
        }
    }
    std::error_code code;
    if (!std::filesystem::is_directory(root, code)) {
        std::cerr << "no scene model directory at " << root
                  << "\nrun tools/prepare_scene_models.py first\n";
        return 2;
    }
    std::vector<std::filesystem::path> meshes;
    for (const auto& entry : std::filesystem::directory_iterator(root))
        if (entry.path().extension() == ".mesh") meshes.push_back(entry.path());
    std::sort(meshes.begin(), meshes.end());
    if (meshes.empty()) {
        std::cerr << "no .mesh files in " << root << '\n';
        return 2;
    }

    int failures = 0;
    if (sourceMode) std::cout << "Full source geometry: no card thinning, impostors or shells\n";
    else std::cout << "model                  model  solid  cards  clusters  levels  in all  dominated\n";
    for (const auto& file : meshes) {
        Mesh mesh;
        std::string why;
        std::size_t shellCoarsest = 0, shellFinest = 0, chainCoarsest = 0;
        if (!read(file, mesh, why)) {
            std::cerr << file.filename().string() << ": " << why << '\n';
            ++failures;
            continue;
        }
        if (sourceMode) {
            if (!buildSource(file, mesh)) ++failures;
            continue;
        }
        auto asset =
                engine::geometry::buildClusterAsset(mesh.positions, mesh.indices, mesh.levels,
                                                    engine::geometry::naniteProfile(),
                                                    mesh.attributes, mesh.materials);
        if (!mesh.levels.empty()) chainCoarsest = mesh.levels.back().count / 3;
        // The shell, at a cell a twenty-fourth of the model across. It stands in
        // for the whole model at distance, so the scale that matters is the
        // model's, not a leaf's - a leaf only sets a floor under it.
        {
            double low[3]{1e30, 1e30, 1e30}, high[3]{-1e30, -1e30, -1e30};
            for (std::uint32_t v = 0; v < mesh.vertices; ++v)
                for (int axis = 0; axis < 3; ++axis) {
                    const double value = mesh.positions[std::size_t(v) * 3 + axis];
                    low[axis] = std::min(low[axis], value);
                    high[axis] = std::max(high[axis], value);
                }
            double extent = 0;
            for (int axis = 0; axis < 3; ++axis) extent = std::max(extent, high[axis] - low[axis]);
            // Coarsened until the shell is no finer than what it stands in for.
            //
            // A cell of a twenty-fourth of the model is right for a tree, whose
            // foliage is thousands of separate leaves, and badly wrong for a
            // rock: a three-hundred-and-forty-triangle rock came out as a shell
            // of four and a half THOUSAND. That is thirteen times the memory to
            // say what the model already said, and its coarse levels are then
            // violent simplifications of a surface that never needed the detail
            // - which is what makes a simplified shell look torn rather than
            // smooth.
            //
            // The shell's triangle count goes as the square of the cell, so
            // doubling it quarters the shell. A few attempts is always enough.
            double cell = std::max(0.05, extent / 24.0);
            for (int attempt = 0; attempt < 4; ++attempt) {
                asset.crownClusters.clear();
                asset.crownIndices.clear();
                asset.crownPositions.clear();
                asset.crownNormals.clear();
                asset.crownCoverage.clear();
                asset.crownLevels = 0;
                engine::geometry::buildCrown(asset, mesh.positions, mesh.indices, mesh.levels, cell,
                                             mesh.layers);
                std::size_t at0 = 0;
                for (const auto& cluster : asset.crownClusters)
                    if (cluster.level == 0) at0 += cluster.indices.count / 3;
                if (at0 <= std::size_t(asset.sourceTriangles) || asset.crownClusters.empty()) break;
                cell *= 2.0;
            }
            // A shell is only worth shipping if it is cheaper than the chain
            // where it would be used - at the coarse end. A three-hundred-
            // triangle rock merges into nine thousand and simplifies to more
            // than its own coarsest level; that is a shell nobody will ever
            // draw, and carrying it is a megabyte of nothing.
            std::size_t coarsest = 0, finest = 0;
            for (const auto& cluster : asset.crownClusters) {
                if (cluster.level + 1 == asset.crownLevels) coarsest += cluster.indices.count / 3;
                if (cluster.level == 0) finest += cluster.indices.count / 3;
            }
            shellCoarsest = coarsest;
            shellFinest = finest;
            asset.crownLayer = shellLayer(mesh);
        }
        const auto name = file.stem().string();
        if (asset.empty()) {
            // Not a failure. A model with neither solid geometry nor enough
            // cards to make a crown has nothing to cluster, and saying so is
            // the right answer.
            std::cout << name << std::string(name.size() < 21 ? 21 - name.size() : 1, ' ')
                      << asset.sourceTriangles << "  nothing to cluster - no sidecar written\n";
            continue;
        }
        const auto bytes = engine::geometry::encodeClusters(asset);
        std::string check;
        const auto back = engine::geometry::decodeClusters(bytes, check);
        if (back.clusters.size() != asset.clusters.size() || back.indices != asset.indices) {
            std::cerr << name << ": what was written does not read back: " << check << '\n';
            ++failures;
            continue;
        }
        // A shell is dropped when it wins NOWHERE, and it used to be dropped
        // when it lost at the coarse end.
        //
        // Those are very different tests, and the second one threw away the
        // thing the shell is for. A tree crown's coarse levels have to beat a
        // handful of billboards, which is hard; its FINE levels replace the
        // whole model with less than half the triangles and look like foliage
        // instead of like four squashed photographs of a tree, which is the
        // entire point of building one. Losing the far end is not a reason to
        // give up the near end - the draw planner already picks the cheapest of
        // the three at every distance, and dropping the asset is what takes that
        // choice away from it.
        //
        // What is genuinely worth dropping is the shell that is dearer than the
        // chain when far and dearer than the model itself when near: a
        // three-hundred-triangle rock that merges into nine thousand. Nobody
        // will ever draw that one, and carrying it is a megabyte of nothing.
        const bool losesFar = chainCoarsest && shellCoarsest >= chainCoarsest;
        const bool losesNear = shellFinest >= asset.sourceTriangles;
        //
        // A model with no cards has no chain to lose to, and that must not read
        // as passing: a three-hundred-triangle rock whose shell comes out at
        // nine thousand has to go whether or not there is a chain to compare it
        // against. Losing when near is the necessary half of the test; losing
        // when far only matters where there is something out there to lose to.
        if (!asset.crownClusters.empty() && losesNear && (chainCoarsest == 0 || losesFar)) {
            std::cout << "    " << name << ": shell dropped, " << shellCoarsest
                      << " tri at its coarsest against the chain's " << chainCoarsest << " and "
                      << shellFinest << " at its finest against the model's "
                      << asset.sourceTriangles << char(10);
            asset.crownClusters.clear();
            asset.crownIndices.clear();
            asset.crownPositions.clear();
            asset.crownNormals.clear();
            asset.crownCoverage.clear();
            asset.crownLevels = 0;
        }
        if (asset.empty()) {
            std::cout << name << std::string(name.size() < 21 ? 21 - name.size() : 1, ' ')
                      << asset.sourceTriangles << "  nothing worth clustering\n";
            continue;
        }

        const auto out = root / (name + ".clusters");
        std::ofstream output(out, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!output) {
            std::cerr << name << ": could not write " << out << '\n';
            ++failures;
            continue;
        }
        std::size_t finest = 0;
        for (const auto& cluster : asset.clusters)
            if (cluster.level == 0) finest += cluster.indices.count / 3;
        std::cout << name << std::string(name.size() < 21 ? 21 - name.size() : 1, ' ')
                  << asset.sourceTriangles << "  " << finest << "  " << asset.cardTriangles << "  "
                  << asset.clusters.size() << "  " << asset.levels << "  "
                  << asset.indices.size() / 3 << "  " << asset.dominated << '\n';
        for (std::uint32_t level = 0; level < asset.levels; ++level) {
            std::size_t triangles = 0, count = 0;
            float error = 0;
            for (const auto& cluster : asset.clusters)
                if (cluster.level == level) {
                    triangles += cluster.indices.count / 3;
                    ++count;
                    error = std::max(error, cluster.error);
                }
            std::cout << "    level " << level << ": " << triangles << " tri in " << count
                      << " clusters, error " << error << '\n';
        }
        if (!asset.crownClusters.empty()) {
            // Blob or canopy, as a number.
            //
            // A crown built as one filled lump is a shrub: whatever its
            // triangle count, its surface is the smallest that can wrap its
            // own volume. A canopy has boughs and sky between them, so it
            // carries far more surface for the same volume. The isoperimetric
            // ratio says which it is - one for a sphere, and higher the more
            // lobed and open the shape - and it is the only way to tell the
            // two apart without looking, since the triangle counts of a lump
            // and a canopy are much the same.
            double area = 0, sixVolume = 0;
            for (std::size_t i = 0; i + 2 < asset.crownIndices.size(); i += 3) {
                const auto at = [&](std::size_t k) {
                    const auto v = asset.crownIndices[i + k] * 3;
                    return std::array<double, 3>{asset.crownPositions[v],
                                                 asset.crownPositions[v + 1],
                                                 asset.crownPositions[v + 2]};
                };
                const auto a = at(0), b = at(1), c = at(2);
                const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
                const double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
                const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz,
                             nz = ux * vy - uy * vx;
                area += std::sqrt(nx * nx + ny * ny + nz * nz) * 0.5;
                sixVolume += a[0] * nx + a[1] * ny + a[2] * nz;   // divergence theorem
            }
            // How much of the shell is NOT wearing the foliage layer - the
            // trunk, in other words. Nought means the whole crown is painted
            // with leaves, trunk included, which is a bush.
            std::size_t bark = 0;
            for (const float layer : asset.crownLayers)
                if (std::abs(layer - asset.crownLayer) > 0.5f) ++bark;
            const double barkShare =
                    asset.crownLayers.empty()
                            ? 0.0
                            : double(bark) / double(asset.crownLayers.size());
            const double volume = std::abs(sixVolume) / 6.0;
            const double sphere = volume > 0 ? std::cbrt(36.0 * 3.14159265358979 * volume * volume)
                                             : 0.0;
            std::cout << "    crown: " << asset.crownIndices.size() / 3 << " tri in "
                      << asset.crownClusters.size() << " clusters over " << asset.crownLevels
                      << " levels, cell " << asset.crownCellMetres << " m, layer "
                      << asset.crownLayer << ", bark share " << barkShare << ", openness "
                      << (sphere > 0 ? area / sphere : 0.0) << char(10);
            for (std::uint32_t level = 0; level < asset.crownLevels; ++level) {
                std::size_t triangles = 0;
                float error = 0;
                for (const auto& cluster : asset.crownClusters)
                    if (cluster.level == level) {
                        triangles += cluster.indices.count / 3;
                        error = std::max(error, cluster.error);
                    }
                std::cout << "      crown level " << level << ": " << triangles << " tri, error "
                          << error << char(10);
            }
        }
        for (std::size_t level = 0; level < asset.cardLevels.size(); ++level) {
            if (!asset.cardLevels[level].count) continue;
            std::cout << "    chain level " << level << ": " << asset.cardLevels[level].count / 3
                      << " card triangles\n";
        }
        // The chain beside it, read from the manifest the mesh was written with.
        {
            std::ifstream manifest(root / "manifest.json");
            if (manifest) {
                nlohmann::json content;
                manifest >> content;
                for (const auto& m : content.at("models"))
                    if (m.at("name") == name) {
                        std::vector<float> errors;
                        std::vector<std::size_t> chain;
                        for (const auto& level : m.at("levels")) {
                            errors.push_back(level.at("error_m").get<float>());
                            chain.push_back(level.at("triangles").get<std::size_t>());
                        }
                        distanceTable(name, asset, errors, chain,
                                      std::max(m.at("width").get<double>(),
                                               m.at("height").get<double>()));
                    }
            }
        }

    }
    if (sourceMode) return failures ? 1 : 0;
    // The groves, from the manifest: which model each is made of and how wide.
    {
        std::ifstream manifest(root / "manifest.json");
        if (manifest) {
            nlohmann::json content;
            manifest >> content;
            if (content.contains("groves"))
                for (const auto& grove : content.at("groves")) {
                    const auto which = grove.at("model").get<std::size_t>();
                    if (which >= content.at("models").size()) continue;
                    const auto name = content.at("models")[which].at("name").get<std::string>();
                    Mesh mesh;
                    std::string why;
                    if (!read(root / (name + ".mesh"), mesh, why)) continue;
                    if (!buildGrove(root, name, mesh, grove.at("width").get<double>(),
                                    shellLayer(mesh)))
                        std::cerr << name << "-grove: no shell built" << char(10);
                }
        }
    }
    return failures ? 1 : 0;
}
