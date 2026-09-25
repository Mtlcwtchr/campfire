#include "game/client/world_dials.hpp"

#include <algorithm>

namespace client {
namespace {

std::string number(std::int64_t v) { return std::to_string(v); }

int stepFor(int value, int direction, int low, int high, int step) {
    return std::clamp(value + direction * step, low, high);
}

} // namespace

const std::vector<Dial>& dials() {
    static const std::vector<Dial> kDials = {
            {"seed", "the whole world is a function of this",
             [](const generation::WorldMapParams& p) { return number(static_cast<std::int64_t>(p.seed)); },
             [](generation::WorldMapParams& p, int d) {
                 p.seed = static_cast<std::uint64_t>(std::max<std::int64_t>(
                         1, static_cast<std::int64_t>(p.seed) + d));
             }},
            {"world size", "how far it is to the next people",
             [](const generation::WorldMapParams& p) {
                 const std::int64_t km = std::int64_t(p.width) * generation::kMetresPerCell / 1000;
                 std::string name = number(p.width);
                 for (const generation::WorldSize& size : generation::kWorldSizes)
                     if (size.cells == p.width) name = size.label;
                 return name + " (" + number(km) + " km across, " + number(km * km) + " km2)";
             },
             [](generation::WorldMapParams& p, int d) {
                 int at = 0;
                 for (std::size_t i = 0; i < generation::kWorldSizeCount; ++i)
                     if (generation::kWorldSizes[i].cells <= p.width) at = static_cast<int>(i);
                 at = std::clamp(at + d, 0, static_cast<int>(generation::kWorldSizeCount) - 1);
                 p.width = p.height = generation::kWorldSizes[at].cells;
             }},
            {"sea", "percent of the map under water",
             [](const generation::WorldMapParams& p) { return number(p.seaPercent) + "%"; },
             [](generation::WorldMapParams& p, int d) {
                 p.seaPercent = stepFor(p.seaPercent, d, 10, 90, 2);
             }},
            {"plates", "pieces the crust is broken into; 0 = by area",
             [](const generation::WorldMapParams& p) {
                 return p.plates == 0 ? std::string("by area") : number(p.plates);
             },
             [](generation::WorldMapParams& p, int d) { p.plates = stepFor(p.plates, d, 0, 120, 2); }},
            {"erosion", "passes of weather on the rock: 0 is raw tectonics",
             [](const generation::WorldMapParams& p) { return number(p.erosionPasses); },
             [](generation::WorldMapParams& p, int d) {
                 p.erosionPasses = stepFor(p.erosionPasses, d, 0, 24, 1);
             }},
            {"rainfall", "percent of the usual: the deserts move with it",
             [](const generation::WorldMapParams& p) { return number(p.rainfallPercent) + "%"; },
             [](generation::WorldMapParams& p, int d) {
                 p.rainfallPercent = stepFor(p.rainfallPercent, d, 20, 250, 5);
             }},
            {"neighbours", "communities settled on it; 0 = by area",
             [](const generation::WorldMapParams& p) {
                 return p.sites == 0 ? std::string("by area") : number(p.sites);
             },
             [](generation::WorldMapParams& p, int d) { p.sites = stepFor(p.sites, d, 0, 400, 1); }},
            {"spacing", "cells between any two of them",
             [](const generation::WorldMapParams& p) { return number(p.siteSpacing); },
             [](generation::WorldMapParams& p, int d) {
                 p.siteSpacing = stepFor(p.siteSpacing, d, 4, 400, 5);
             }},
    };
    return kDials;
}

} // namespace client
