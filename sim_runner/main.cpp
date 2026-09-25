// Headless scenario runner (GDD 11).
//
//   sim_runner --ticks 40000 --seed 7 --status-every 2400
//
// Builds the world from a seed, runs it with no client, checks invariants as it
// goes, and prints the report GDD 14 asks for. This is the loop an agent uses to
// change a mechanic and see what it did to the community.

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "game/simulation/report.hpp"
#include "game/work/planner.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/simulation/world.hpp"

namespace {

struct Options {
    std::int64_t ticks = 24000;
    std::uint64_t seed = 1;
    std::string contentDir = "content";
    std::int64_t statusEvery = 0;
    bool checkInvariants = true;
    std::int32_t mapSize = 180;
    std::int32_t worldCells = 0;
    std::string ethnos;
    bool allCommunities = false;
    std::int32_t population = 24;
    bool quiet = false;
    std::int64_t explainAt = -1;
    std::string dumpMap;
    std::string dumpWorld;
};

void printUsage() {
    std::cout << "usage: sim_runner [options]\n"
              << "  --ticks N          how many ticks to simulate (default 24000)\n"
              << "  --days N           run N game days instead of raw ticks\n"
              << "  --seed N           world seed (default 1)\n"
              << "  --content DIR      content directory (default ./content)\n"
              << "  --status-every N   print a status line every N ticks\n"
              << "  --map N            map edge in tiles (default 180)\n"
              << "  --world N          world map edge in cells (default 2048 = 368 km)\n"
              << "  --all              simulate every community on the map, not just ours\n"
              << "  --ethnos NAME      play a different people (sumerian, achaean, yamna, northfolk)\n"
              << "  --pop N            starting community size (default 24)\n"
              << "  --no-invariants    skip the per-tick invariant check\n"
              << "  --explain N        dump the demand table and every person at tick N\n"
              << "  --quiet            print only the final report\n"
              << "  --dump-map FILE    write the map as a PPM: terrain, hill shaded\n"
              << "  --dump-world FILE  write the world map as a PPM, one pixel a cell\n";
}

// Content lives beside the source tree, not beside the binary, so a build in any
// directory still finds it.
std::filesystem::path resolveContent(const std::string& given) {
    std::filesystem::path p(given);
    if (std::filesystem::exists(p)) return p;
    for (const char* prefix : {"..", "../..", "../../..", "../../../.."}) {
        std::filesystem::path candidate = std::filesystem::path(prefix) / given;
        if (std::filesystem::exists(candidate)) return candidate;
    }
    return p;
}


// The map as a picture, so the lie of the land can be looked at without opening
// the client: terrain colour, hill shaded from the north-west the same way the
// renderer does it. Written as a PPM, which needs no library.
void dumpMap(const sim::World& w, const std::string& path) {
    const std::int32_t width = w.map().width();
    const std::int32_t height = w.map().height();
    const auto at = [&](std::int32_t x, std::int32_t y) -> const sim::Tile& {
        return w.map().at({std::clamp(x, 0, width - 1), std::clamp(y, 0, height - 1)});
    };
    struct Rgb { int r, g, b; };
    const auto colourOf = [&](sim::Terrain t) -> Rgb {
        switch (t) {
            case sim::Terrain::Water:  return {58, 104, 132};
            case sim::Terrain::Marsh:  return {96, 108, 62};
            case sim::Terrain::Sand:   return {222, 196, 148};
            case sim::Terrain::Dirt:   return {150, 112, 72};
            case sim::Terrain::Grass:  return {108, 132, 78};
            case sim::Terrain::Rock:   return {132, 128, 122};
            case sim::Terrain::Forest: return {58, 92, 56};
            default:                   return {120, 120, 120};
        }
    };

    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << width << " " << height << "\n255\n";
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            const sim::Tile& t = at(x, y);
            Rgb c = colourOf(t.terrain);
            const float dzdx = static_cast<float>(at(x + 1, y).elevation) -
                               static_cast<float>(at(x - 1, y).elevation);
            const float dzdy = static_cast<float>(at(x, y + 1).elevation) -
                               static_cast<float>(at(x, y - 1).elevation);
            const float k = std::clamp(1.0f + (dzdx + dzdy) * 0.038f, 0.72f, 1.24f);
            // What the community has made of it, so the shape of a field and the
            // line of a canal can be read off the picture.
            // Farm ground is coloured by its soil: rich is green, poor is red.
            // What irrigation is for is turning the second into the first.
            if (t.zoneMask != 0 && (t.tilled || t.crop.valid() || true)) {
                bool farm = false;
                for (const auto& z : w.zones())
                    if (z.alive && z.kind == sim::ZoneKind::Farm &&
                        (t.zoneMask & (1ull << z.id.value)))
                        farm = true;
                if (farm) {
                    const int f = std::clamp(static_cast<int>((t.fertility * 100).toInt()), 0, 100);
                    c = {200 - f, 60 + f, 40};
                }
            }
            if (t.building.valid()) {
                const auto& b = w.buildings()[t.building.value];
                if (b.alive) {
                    c = w.db().building(b.def).irrigationRadius > 0 ? Rgb{70, 190, 235}
                                                                    : Rgb{232, 228, 216};
                }
            }
            const auto shade = [&](int v) { return std::clamp(static_cast<int>(v * k), 0, 255); };
            const char rgb[3] = {static_cast<char>(shade(c.r)), static_cast<char>(shade(c.g)),
                                 static_cast<char>(shade(c.b))};
            out.write(rgb, 3);
        }
    }
}


// The world above the local map, one pixel to a cell: sea shaded by depth, land
// by how wet and how high it is, rivers, and a mark on every settled site. The
// same painting the client uses (D92), so a dump and the screen agree.
void dumpWorldMap(const generation::WorldMapData& world, const std::string& path) {
    struct Rgb { int r, g, b; };
    const auto mix = [](Rgb a, Rgb b, double t) {
        t = std::clamp(t, 0.0, 1.0);
        return Rgb{static_cast<int>(a.r + (b.r - a.r) * t), static_cast<int>(a.g + (b.g - a.g) * t),
                   static_cast<int>(a.b + (b.b - a.b) * t)};
    };
    const std::size_t count = world.cells.size();
    std::vector<Rgb> px(count, Rgb{0, 0, 0});

    // How far each sea cell is from land, grown outwards in one pass: depth by
    // distance from the shore is what gives a coast a shelf instead of a wall.
    std::vector<std::uint8_t> fromLand(count, 255);
    {
        std::vector<std::int32_t> wave;
        for (std::int32_t y = 0; y < world.height; ++y)
            for (std::int32_t x = 0; x < world.width; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * world.width + x;
                if (world.cells[i].sea) continue;
                fromLand[i] = 0;
                wave.push_back(static_cast<std::int32_t>(i));
            }
        std::size_t head = 0;
        while (head < wave.size()) {
            const std::int32_t index = wave[head++];
            const std::uint8_t step = fromLand[static_cast<std::size_t>(index)];
            if (step >= 20) continue;
            const core::TilePos p{index % world.width, index / world.width};
            for (int dir : core::kCardinalDirections) {
                const core::TilePos n = core::neighbour(p, dir);
                if (!world.inBounds(n)) continue;
                const std::size_t j = static_cast<std::size_t>(n.y) * world.width + n.x;
                if (fromLand[j] <= step + 1) continue;
                fromLand[j] = static_cast<std::uint8_t>(step + 1);
                wave.push_back(static_cast<std::int32_t>(j));
            }
        }
    }

    for (std::int32_t y = 0; y < world.height; ++y) {
        for (std::int32_t x = 0; x < world.width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * world.width + x;
            const auto& c = world.cells[i];
            Rgb out{};
            if (c.sea) {
                out = mix({62, 110, 146}, {20, 46, 84}, fromLand[i] / 14.0);
            } else {
                // Painted by country, the same palette the client uses (D96).
                const auto climate = [&]() -> Rgb {
                    switch (c.climate) {
                        case generation::Climate::Ice:             return {232, 236, 240};
                        case generation::Climate::Tundra:          return {150, 148, 128};
                        case generation::Climate::Taiga:           return {62, 88, 68};
                        case generation::Climate::TemperateForest: return {84, 118, 66};
                        case generation::Climate::Steppe:          return {164, 158, 96};
                        case generation::Climate::Mediterranean:   return {134, 142, 78};
                        case generation::Climate::Desert:          return {214, 190, 138};
                        case generation::Climate::Savanna:         return {186, 172, 96};
                        case generation::Climate::TropicalForest:  return {48, 104, 56};
                        case generation::Climate::Alpine:          return {146, 138, 128};
                        case generation::Climate::RiverValley:     return {112, 140, 74};
                        case generation::Climate::Delta:           return {96, 138, 70};
                        default:                                   return {104, 134, 76};
                    }
                };
                const double h = c.elevation / 255.0;
                out = mix(climate(), {150, 142, 132}, std::clamp((h - 0.55) * 1.8, 0.0, 1.0));
                out = mix(out, {240, 242, 246}, std::clamp((h - 0.82) * 4.0, 0.0, 1.0));
                if (c.river) out = mix(out, {72, 112, 152}, 0.75);
            }
            // Hill shaded over eight neighbours, like the client: two neighbours
            // light the ground from a pair of steps and it comes out faceted.
            if (!c.sea) {
                const auto height = [&](std::int32_t hx, std::int32_t hy) {
                    const core::TilePos q{std::clamp(hx, 0, world.width - 1),
                                          std::clamp(hy, 0, world.height - 1)};
                    const auto& o = world.at(q);
                    return static_cast<double>(o.sea ? 0 : o.elevation);
                };
                const double dzdx = (height(x + 1, y - 1) + 2 * height(x + 1, y) + height(x + 1, y + 1)) -
                                    (height(x - 1, y - 1) + 2 * height(x - 1, y) + height(x - 1, y + 1));
                const double dzdy = (height(x - 1, y + 1) + 2 * height(x, y + 1) + height(x + 1, y + 1)) -
                                    (height(x - 1, y - 1) + 2 * height(x, y - 1) + height(x + 1, y - 1));
                const double k = std::clamp(1.0 + (dzdx + dzdy) * 0.0045, 0.72, 1.26);
                out = {static_cast<int>(out.r * k), static_cast<int>(out.g * k),
                       static_cast<int>(out.b * k)};
            }
            px[i] = out;
        }
    }
    for (const auto& s : world.sites) {
        auto& p = px[static_cast<std::size_t>(s.cell.y) * world.width + s.cell.x];
        p = s.played ? Rgb{255, 240, 120} : Rgb{220, 90, 70};
    }
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << world.width << " " << world.height << "\n255\n";
    for (const auto& p : px) {
        const char rgb[3] = {static_cast<char>(std::clamp(p.r, 0, 255)),
                             static_cast<char>(std::clamp(p.g, 0, 255)),
                             static_cast<char>(std::clamp(p.b, 0, 255))};
        out.write(rgb, 3);
    }
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    std::int64_t days = -1;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::int64_t fallback) -> std::int64_t {
            return (i + 1 < argc) ? std::strtoll(argv[++i], nullptr, 10) : fallback;
        };
        if (a == "--ticks") opt.ticks = next(opt.ticks);
        else if (a == "--days") days = next(0);
        else if (a == "--seed") opt.seed = static_cast<std::uint64_t>(next(1));
        else if (a == "--content" && i + 1 < argc) opt.contentDir = argv[++i];
        else if (a == "--status-every") opt.statusEvery = next(0);
        else if (a == "--map") opt.mapSize = static_cast<std::int32_t>(next(opt.mapSize));
        else if (a == "--world") opt.worldCells = static_cast<std::int32_t>(next(opt.worldCells));
        else if (a == "--all") opt.allCommunities = true;
        else if (a == "--ethnos" && i + 1 < argc) opt.ethnos = argv[++i];
        else if (a == "--pop") opt.population = static_cast<std::int32_t>(next(opt.population));
        else if (a == "--explain") opt.explainAt = next(-1);
        else if (a == "--no-invariants") opt.checkInvariants = false;
        else if (a == "--quiet") opt.quiet = true;
        else if (a == "--dump-map" && i + 1 < argc) opt.dumpMap = argv[++i];
        else if (a == "--dump-world" && i + 1 < argc) opt.dumpWorld = argv[++i];
        else if (a == "--help" || a == "-h") { printUsage(); return 0; }
        else { std::cerr << "unknown option: " << a << "\n"; printUsage(); return 2; }
    }

    content::ContentDb db;
    const auto contentPath = resolveContent(opt.contentDir);
    if (!db.load(contentPath)) {
        std::cerr << "content failed to load from " << contentPath << ":\n";
        for (const auto& e : db.errors()) std::cerr << "  " << e << "\n";
        return 1;
    }
    for (const auto& warn : db.warnings()) std::cerr << "warning: " << warn << "\n";

    sim::WorldConfig cfg;
    cfg.seed = opt.seed;
    cfg.mapWidth = opt.mapSize;
    cfg.mapHeight = opt.mapSize;
    cfg.worldCells = opt.worldCells;
    cfg.startingPopulation = opt.population;
    // Which people is being played. Their country follows from them: the
    // generator puts a community where its way of living works (D100).
    if (!opt.ethnos.empty()) cfg.ethnos = opt.ethnos;

    sim::World world(db, cfg);
    if (days >= 0) opt.ticks = days * db.time().ticksPerDay();

    // Every community on the map, if asked (D95): they all start with the same
    // band on the same morning, so running them side by side is how the claim
    // "equal footing" gets checked - and how the ground itself gets tested,
    // because fifteen communities on fifteen different cells is fifteen
    // simultaneous runs of the whole game.
    std::vector<std::unique_ptr<sim::World>> neighbours;
    if (opt.allCommunities) {
        const generation::WorldMapData& country = world.worldMap();
        for (const auto& site : country.sites) {
            if (site.cell == world.localCell()) continue;
            sim::WorldConfig other = cfg;
            other.localCell = site.cell;
            other.ethnos = site.ethnos;
            other.sharedWorldMap = &country;
            other.startingPopulation = generation::kFoundingCommunity;
            neighbours.push_back(std::make_unique<sim::World>(db, other));
        }
    }

    if (!opt.quiet) {
        std::cout << "content: " << db.items().size() << " items, " << db.recipes().size()
                  << " recipes, " << db.buildings().size() << " buildings, "
                  << db.resourceNodes().size() << " resources\n";
        std::cout << "world:   " << cfg.mapWidth << "x" << cfg.mapHeight << " tiles, seed "
                  << cfg.seed << ", " << world.people().size() << " people, "
                  << world.nodes().size() << " natural features\n";
        std::cout << "time:    " << db.time().ticksPerDay() << " ticks/day, "
                  << db.time().ticksPerYear() << " ticks/year\n";

        // The country above the map, counted. A generator is only ever checked
        // by its census: thresholds tuned by eye on one seed have twice now
        // produced a world with no stone in it, or with no rivers.
        {
            const auto& wm = world.worldMap();
            std::int64_t sea = 0, river = 0, valley = 0;
            std::int32_t highest = 0;
            for (const auto& c : wm.cells) {
                if (c.sea) { ++sea; continue; }
                if (c.river) ++river;
                if (c.biome == generation::Biome::RiverValley) ++valley;
                highest = std::max(highest, static_cast<std::int32_t>(c.elevation));
            }
            const std::int64_t land = static_cast<std::int64_t>(wm.cells.size()) - sea;
            const std::int64_t km = std::int64_t(wm.width) * generation::kMetresPerCell / 1000;
            std::cout << "country: " << wm.width << "x" << wm.height << " cells = " << km << "x" << km
                      << " km, sea " << (sea * 100 / std::max<std::int64_t>(1, wm.cells.size()))
                      << "%, river cells " << river << " (" << (river * 1000 / std::max<std::int64_t>(1, land))
                      << "/1000 of land), valley " << (valley * 100 / std::max<std::int64_t>(1, land))
                      << "%, peak " << highest << "\n";

            // What countries the world has in it. This is what decides where a
            // people can be put, so a world that is nine tenths one climate is
            // a world with one kind of settlement in it.
            {
                std::array<std::int64_t, static_cast<std::size_t>(generation::Climate::Count)> byClimate{};
                for (const auto& c : wm.cells)
                    if (!c.sea) ++byClimate[static_cast<std::size_t>(c.climate)];
                std::cout << "climate:";
                for (std::size_t i = 0; i < byClimate.size(); ++i) {
                    const std::int64_t share = byClimate[i] * 100 / std::max<std::int64_t>(1, land);
                    if (share == 0) continue;
                    std::cout << " " << generation::climateName(static_cast<generation::Climate>(i))
                              << " " << share << "%";
                }
                std::cout << "\n";
            }

            // Who lives there. Every settlement is founded the same way - one
            // band of twenty-four, this spring - so what this counts is how many
            // neighbours there are and what country each of them drew.
            std::int64_t people = 0;
            for (const auto& s : wm.sites) people += s.population;
            std::cout << "peoples: " << wm.sites.size() << " communities, " << people
                      << " people in all\n";
            for (const auto& s : wm.sites) {
                std::cout << "   " << s.name;
                for (std::size_t pad = s.name.size(); pad < 12; ++pad) std::cout << ' ';
                std::cout << s.ethnos;
                for (std::size_t pad = s.ethnos.size(); pad < 10; ++pad) std::cout << ' ';
                std::cout << generation::climateName(s.climate) << ", "
                          << generation::livelihoodName(s.livelihood)
                          << (s.played ? "   <- ours" : "") << "\n";
            }
            std::cout << "\nplayed:  " << wm.playedCell.x << "," << wm.playedCell.y;
            {
                // The ground the player was handed, in the world's own terms.
                // A community that starves has usually been put somewhere the
                // map could have told us about.
                const auto& c = wm.at(world.localCell());
                std::cout << " - moisture " << static_cast<int>(c.moisture) << "/255, ground "
                          << static_cast<int>(c.fertility) << "/255, " << static_cast<int>(c.elevation) * 10
                          << " m up" << (c.river ? ", on a river" : ", no river");
            }
            std::cout << "\n";
        }

        // What the terrain actually offers, and how much of it is within reach of
        // the hearth. A chain that stalls for want of stone usually stalls because
        // there is no stone near enough to matter.
        const auto hearth = world.settlements().front().hearth;
        std::vector<std::int32_t> total(db.resourceNodes().size(), 0);
        std::vector<std::int32_t> near(db.resourceNodes().size(), 0);
        for (const auto& n : world.nodes()) {
            if (!n.alive) continue;
            ++total[n.def.value];
            if (core::chebyshev(n.tile, hearth) <= 40) ++near[n.def.value];
        }
        std::cout << "terrain: ";
        for (std::size_t i = 0; i < total.size(); ++i) {
            if (total[i] == 0) continue;
            std::cout << db.resourceNodes()[i].name << " " << total[i] << "(" << near[i] << " near) ";
        }
        // What the ground itself is, which is what decides whether there is
        // anywhere to farm, anywhere to graze and anywhere to drink. A map that
        // came out nine tenths marsh starves a community that has plenty of
        // everything growing on it.
        {
            std::array<std::int32_t, 10> byTerrain{};
            std::int32_t waterNear = 0;
            for (std::int32_t y = 0; y < world.map().height(); ++y)
                for (std::int32_t x = 0; x < world.map().width(); ++x) {
                    const sim::Tile& t = world.map().at({x, y});
                    const auto kind = static_cast<std::size_t>(t.terrain);
                    if (kind < byTerrain.size()) ++byTerrain[kind];
                    if (t.terrain == sim::Terrain::Water && core::chebyshev({x, y}, hearth) <= 40)
                        ++waterNear;
                }
            const std::int32_t all = world.map().width() * world.map().height();
            std::cout << "\nground:  ";
            for (std::size_t i = 0; i < byTerrain.size(); ++i) {
                if (byTerrain[i] == 0) continue;
                std::cout << sim::terrainName(static_cast<sim::Terrain>(i)) << " "
                          << (byTerrain[i] * 100 / std::max(1, all)) << "% ";
            }
            std::cout << "| water within 40 of the fire: " << waterNear;
        }
        std::cout << "\nhearth:  " << hearth.x << "," << hearth.y << "\n\n";
    }

    for (std::int64_t i = 0; i < opt.ticks; ++i) {
        world.tick();
        for (auto& other : neighbours) other->tick();

        if (opt.checkInvariants) {
            const auto problems = sim::checkInvariants(world);
            if (!problems.empty()) {
                std::cerr << "INVARIANT VIOLATION at tick " << world.tickCount() << ":\n";
                for (const auto& p : problems) std::cerr << "  " << p << "\n";
                std::cerr << sim::formatReport(world);
                return 3;
            }
        }
        if (opt.explainAt >= 0 && world.tickCount() == opt.explainAt)
            std::cout << sim::formatDecisionDump(world) << "\n";
        if (opt.statusEvery > 0 && world.tickCount() % opt.statusEvery == 0 && !opt.quiet)
            std::cout << sim::formatStatusLine(world) << "\n";

        if (world.report().population == 0) {
            std::cout << "\nthe community died out at tick " << world.tickCount() << "\n\n";
            break;
        }
    }

    // How the neighbours did. Same start, same day, different ground: this is
    // the table that says whether the world is worth living on in more than one
    // place, and it is the only view there is of the other communities without
    // opening the client.
    if (!neighbours.empty()) {
        struct Row {
            std::string name, land;
            std::int32_t alive, born, dead, built;
            std::int32_t water, wood, rock, grass;
        };
        std::vector<Row> rows;
        const auto readOff = [&](const sim::World& w) {
            Row row;
            for (const auto& s : w.worldMap().sites)
                if (s.cell == w.localCell()) {
                    row.name = s.name;
                    row.land = generation::climateName(s.climate);
                }
            row.alive = w.report().population;
            row.born = w.report().births;
            row.dead = w.report().deaths;
            row.built = 0;
            for (const auto& b : w.buildings())
                if (b.alive && b.state == sim::BuildState::Complete) ++row.built;
            // What ground they drew. Two communities on the same kind of map is
            // the thing to watch for: it means the local generator is not
            // listening to the world above it (D97).
            std::int32_t water = 0, wood = 0, rock = 0, grass = 0;
            for (std::int32_t y = 0; y < w.map().height(); ++y)
                for (std::int32_t x = 0; x < w.map().width(); ++x) {
                    switch (w.map().at({x, y}).terrain) {
                        case sim::Terrain::Water:  ++water; break;
                        case sim::Terrain::Forest: ++wood;  break;
                        case sim::Terrain::Rock:   ++rock;  break;
                        case sim::Terrain::Grass:  ++grass; break;
                        default: break;
                    }
                }
            const std::int32_t all = std::max(1, w.map().width() * w.map().height());
            row.water = water * 100 / all;
            row.wood = wood * 100 / all;
            row.rock = rock * 100 / all;
            row.grass = grass * 100 / all;
            return row;
        };
        rows.push_back(readOff(world));
        for (const auto& other : neighbours) rows.push_back(readOff(*other));

        std::int32_t living = 0, gone = 0;
        std::cout << "\n-- the other communities --\n";
        for (const auto& r : rows) {
            living += r.alive;
            if (r.alive == 0) ++gone;
            std::cout << "  " << r.name;
            for (std::size_t pad = r.name.size(); pad < 13; ++pad) std::cout << ' ';
            std::cout << r.land;
            for (std::size_t pad = r.land.size(); pad < 17; ++pad) std::cout << ' ';
            std::cout << "water " << r.water << "% wood " << r.wood << "% rock " << r.rock
                      << "% grass " << r.grass << "% | alive " << r.alive << ", born " << r.born
                      << ", died " << r.dead << ", built " << r.built << "\n";
        }
        std::cout << "  " << rows.size() << " communities, " << living << " people alive, " << gone
                  << " died out\n";
    }

    if (!opt.dumpMap.empty()) dumpMap(world, opt.dumpMap);
    // The world the community is actually living in, not a second one generated
    // from the same seed: with --world in play those are different maps, and the
    // picture has to be of the one whose marked site is where the game is.
    if (!opt.dumpWorld.empty()) dumpWorldMap(world.worldMap(), opt.dumpWorld);

    std::cout << "\n" << sim::formatReport(world);
    return 0;
}
