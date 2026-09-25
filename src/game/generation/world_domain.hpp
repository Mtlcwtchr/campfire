#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace generation {
struct WorldMapParams;

// Physical extent, never a macro-grid resolution or a render LOD.
class WorldDomain {
public:
    static constexpr std::int64_t kMaxExtent = 1000000000;
    WorldDomain(std::int64_t widthMetres, std::int64_t heightMetres);
    static WorldDomain fromLegacy(const WorldMapParams&);
    std::optional<std::array<std::int32_t,2>> legacyCells() const;
    std::int64_t widthMetres() const { return width_; }
    std::int64_t heightMetres() const { return height_; }
    std::uint64_t areaSquareMetres() const;
    bool contains(double x, double y) const; // half-open domain
    // Not clamped: halo queries may lie outside the domain.
    std::array<double,2> normalized(double x, double y) const;
    std::array<double,2> metres(double u, double v) const;
    std::array<double,2> gradientPerMetre(double dhdu, double dhdv) const;
    bool operator==(const WorldDomain&) const = default;
private:
    std::int64_t width_, height_;
};

struct PhysicalWorldSize {
    std::string_view name;
    std::int64_t sideMetres;
};
// The physical sides the runtime presets in kWorldSizes stand for, and the two
// lists have to agree - see the test that holds them together. They did not
// once: the client ran at a quarter of these while the descriptors named the
// full ones, because the H64 foundation could not hold a world this size.
inline constexpr std::array<PhysicalWorldSize,7> kScaleWorldSizes{{
    {"tiny",32400}, {"smaller",64800}, {"small",128520}, {"average",255960},
    {"medium",511920}, {"large",1023840}, {"giant",2048760}}};
WorldDomain scaleWorldPreset(std::string_view name = "medium");
} // namespace generation

