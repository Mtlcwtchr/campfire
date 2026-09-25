#include "game/generation/world_domain.hpp"
#include "game/generation/world_map_gen.hpp"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace generation {
WorldDomain::WorldDomain(std::int64_t widthMetres, std::int64_t heightMetres)
    : width_(widthMetres), height_(heightMetres) {
    if (width_<=0 || height_<=0 || width_>kMaxExtent || height_>kMaxExtent)
        throw std::invalid_argument("world extent must be in (0, 1000000000] metres");
}
WorldDomain WorldDomain::fromLegacy(const WorldMapParams& p) {
    return {std::int64_t(p.width)*kMetresPerCell, std::int64_t(p.height)*kMetresPerCell};
}
std::optional<std::array<std::int32_t,2>> WorldDomain::legacyCells() const {
    if (width_%kMetresPerCell || height_%kMetresPerCell ||
        width_/kMetresPerCell>std::numeric_limits<std::int32_t>::max() ||
        height_/kMetresPerCell>std::numeric_limits<std::int32_t>::max()) return std::nullopt;
    return std::array{std::int32_t(width_/kMetresPerCell),std::int32_t(height_/kMetresPerCell)};
}
std::uint64_t WorldDomain::areaSquareMetres() const {
    return std::uint64_t(width_)*std::uint64_t(height_);
}
bool WorldDomain::contains(double x, double y) const {
    return std::isfinite(x) && std::isfinite(y) && x>=0 && y>=0 && x<width_ && y<height_;
}
std::array<double,2> WorldDomain::normalized(double x, double y) const { return {x/width_,y/height_}; }
std::array<double,2> WorldDomain::metres(double u, double v) const { return {u*width_,v*height_}; }
std::array<double,2> WorldDomain::gradientPerMetre(double dhdu, double dhdv) const {
    return {dhdu/width_,dhdv/height_};
}
WorldDomain scaleWorldPreset(std::string_view name) {
    if (name=="med") name="medium";
    for (const auto& preset:kScaleWorldSizes)
        if (name==preset.name) return {preset.sideMetres,preset.sideMetres};
    throw std::invalid_argument("unknown physical world preset");
}
} // namespace generation

