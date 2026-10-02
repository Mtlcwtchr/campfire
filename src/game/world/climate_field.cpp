#include "game/world/climate_field.hpp"

#include <algorithm>
#include <cmath>
#include <thread>
#include <unordered_map>
#include <vector>

#include "engine/biomes/category_field.hpp"
#include "engine/biomes/registry.hpp"
#include "engine/core/hash.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/world/scene_scatter.hpp"

namespace world {
using core::Fixed;

void ClimateField::raise(const generation::WorldMapData& world, const HeightField& field) {
    if (world.width <= 0 || world.height <= 0) return;
    const std::int64_t metres = generation::kMetresPerCell;
    // One sample past the far edge, so a point on the last metre of the map
    // still has four corners to interpolate between.
    const auto samplesAt = [&](std::int64_t step) {
        return std::size_t((std::int64_t(world.width) * metres / step + 2) * (std::int64_t(world.height) * metres / step + 2));
    };
    metres_ = kFinestMetres;
    if (samplesAt(metres_) > kFineSamples) metres_ *= 2;
    while (samplesAt(metres_) > kSampleBudget) metres_ *= 2;
    wide_ = static_cast<std::int32_t>(world.width * metres / metres_) + 2;
    high_ = static_cast<std::int32_t>(world.height * metres / metres_) + 2;
    chunksWide_ = (wide_ + kChunk - 1) / kChunk;
    const std::int32_t chunksHigh = (high_ + kChunk - 1) / kChunk;
    // The categories: painted ones from the import, the rest named by the
    // registry's rules where there is land to name.
    const auto registry = engine::biomes::active();
    const auto* painted = world.categories.get();
    std::vector<std::pair<const engine::biomes::DeriveRule*, std::uint8_t>> rules;
    if (registry)
        for (const auto& rule : registry->deriveRules())
            if (const auto* c = registry->category(rule.category); c && c->id > 0 && c->id < 256)
                rules.emplace_back(&rule, std::uint8_t(c->id));
    const auto categoryOf = [&](std::int64_t x, std::int64_t y, const HeightField::SurfaceClimate& climate,
                                std::uint8_t* into) {
        engine::biomes::CategoryField::Ids ids{};
        if (painted) ids = painted->at(double(x), double(y));
        ids[4] = 0;   // the forest bias stays with the field, not the card
        if (ids[0] == 0 && !rules.empty()) {
            const auto cx = std::clamp<std::int64_t>(x / metres, 0, world.width - 1);
            const auto cy = std::clamp<std::int64_t>(y / metres, 0, world.height - 1);
            if (!world.cells[std::size_t(cy * world.width + cx)].sea) {
                const double desert = climate.desert.toDouble(), moisture = climate.environment[2].toDouble();
                const double temperature = climate.environment[0].toDouble();
                for (const auto& [rule, id] : rules)
                    if (rule->passes(desert, moisture, temperature)) {
                        ids[0] = id;
                        break;
                    }
            }
        }
        for (std::size_t k = 0; k < 4; ++k) into[kFirstCategory + k] = ids[k];
    };
    // Every sample on its own and a pure function of the map, so the rows go
    // wide across the machine: four million samples to a region, and this was
    // a second of the loading screen on one thread. The same bytes either way.
    const auto fillChunk = [&](const HeightField& local, std::int32_t cx, std::int32_t cy, std::uint8_t* out) {
        for (std::int32_t dy = 0; dy < kChunk; ++dy)
            for (std::int32_t dx = 0; dx < kChunk; ++dx) {
                const std::int32_t column = cx * kChunk + dx, row = cy * kChunk + dy;
                std::uint8_t* into = out + (static_cast<std::size_t>(dy) * kChunk + dx) * kChannels;
                if (column >= wide_ || row >= high_) { std::fill_n(into, kChannels, std::uint8_t{0}); continue; }
                const core::WorldPos at{Fixed::fromInt(std::int64_t(column) * metres_),
                                        Fixed::fromInt(std::int64_t(row) * metres_)};
                const HeightField::SurfaceClimate climate = local.surfaceClimateAt(at);
                const auto put = [&](std::size_t slot, Fixed unit) {
                    const Fixed stored = signedSlot(slot)
                                                 ? unit / Fixed::fromInt(kSignedSpan) + Fixed::ratio(1, 2)
                                                 : unit;
                    into[slot] = static_cast<std::uint8_t>(std::clamp<std::int64_t>(
                            (stored * Fixed::fromInt(255)).roundToInt(), 0, 255));
                };
                for (std::size_t i = 0; i < 4; ++i) put(i, climate.foliage[i]);
                put(4, climate.desert);
                for (std::size_t i = 0; i < 6; ++i) put(5 + i, climate.environment[i]);
                // A low-frequency representation of the SAME seeded scatter field.
                // It survives outside local object residency; no per-frame height queries.
                // No woodland (open sea, bare country): no forest to be dense.
                put(11, climate.woodland == core::kZero ? core::kZero
                        : climate.woodland * Fixed::fromDoubleForContent(decor::forestDensity(world.seed,
                                  double(column) * metres_, double(row) * metres_)));
                put(12, climate.woodland);
                categoryOf(std::int64_t(column) * metres_, std::int64_t(row) * metres_, climate, into);
            }
    };
    // A chunk row at a time: its chunks, the distinct ones among them, and
    // which of those each chunk is. Merged into the pool afterwards in row
    // order, so the pool is the same bytes however the rows were shared out.
    struct Row {
        std::vector<std::uint8_t> distinct;   // kChunkBytes each
        std::vector<std::uint32_t> which;     // per chunk of the row
    };
    std::vector<Row> rowsOut(static_cast<std::size_t>(chunksHigh));
    const auto chunkRow = [&](const HeightField& local, std::int32_t cy) {
        Row& row = rowsOut[static_cast<std::size_t>(cy)];
        std::vector<std::uint8_t> chunk(kChunkBytes);
        std::unordered_multimap<std::uint64_t, std::uint32_t> seen;
        row.which.reserve(static_cast<std::size_t>(chunksWide_));
        for (std::int32_t cx = 0; cx < chunksWide_; ++cx) {
            fillChunk(local, cx, cy, chunk.data());
            const auto hash = core::hashBytes(chunk.data(), kChunkBytes);
            std::uint32_t found = std::uint32_t(row.distinct.size() / kChunkBytes);
            const auto [first, last] = seen.equal_range(hash);
            for (auto it = first; it != last; ++it)
                if (std::equal(chunk.begin(), chunk.end(),
                               row.distinct.begin() + std::ptrdiff_t(std::size_t(it->second) * kChunkBytes))) {
                    found = it->second;
                    break;
                }
            if (found == row.distinct.size() / kChunkBytes) {
                row.distinct.insert(row.distinct.end(), chunk.begin(), chunk.end());
                seen.emplace(hash, found);
            }
            row.which.push_back(found);
        }
    };
    // One field shared by every thread, not a copy each: surfaceClimateAt reads
    // only the map and the immutable hybrid terrain and never touches the
    // field's mutable caches (block_, corners_, windCorners_, carver_). A copy
    // per thread was forty megabytes apiece for nothing.
    const std::int32_t threads = std::clamp<std::int32_t>(std::int32_t(std::thread::hardware_concurrency()), 1, 8);
    std::vector<std::thread> workers;
    for (std::int32_t t = 1; t < threads; ++t)
        workers.emplace_back([&, t] { for (std::int32_t cy = t; cy < chunksHigh; cy += threads) chunkRow(field, cy); });
    for (std::int32_t cy = 0; cy < chunksHigh; cy += threads) chunkRow(field, cy);
    for (auto& worker : workers) worker.join();

    // Into one pool, each distinct chunk once (found by a hash of its bytes).
    table_.assign(static_cast<std::size_t>(chunksWide_) * chunksHigh, 0);
    pool_.clear();
    std::unordered_multimap<std::uint64_t, std::uint32_t> held;
    for (std::int32_t cy = 0; cy < chunksHigh; ++cy) {
        Row& row = rowsOut[static_cast<std::size_t>(cy)];
        std::vector<std::uint32_t> placed(row.distinct.size() / kChunkBytes);
        for (std::size_t d = 0; d < placed.size(); ++d) {
            const auto* bytes = row.distinct.data() + d * kChunkBytes;
            const auto hash = core::hashBytes(bytes, kChunkBytes);
            std::uint32_t at = std::uint32_t(pool_.size() / kChunkBytes);
            const auto [first, last] = held.equal_range(hash);
            for (auto it = first; it != last; ++it)
                if (std::equal(bytes, bytes + kChunkBytes, pool_.begin() + std::ptrdiff_t(std::size_t(it->second) * kChunkBytes))) {
                    at = it->second;
                    break;
                }
            if (at == pool_.size() / kChunkBytes) {
                pool_.insert(pool_.end(), bytes, bytes + kChunkBytes);
                held.emplace(hash, at);
            }
            placed[d] = at;
        }
        for (std::int32_t cx = 0; cx < chunksWide_; ++cx)
            table_[static_cast<std::size_t>(cy) * chunksWide_ + cx] = placed[row.which[static_cast<std::size_t>(cx)]];
        row = {};
    }
    pool_.shrink_to_fit();
    anyCategory_ = false;
    for (std::size_t at = kFirstCategory; at < pool_.size() && !anyCategory_; at += kChannels)
        anyCategory_ = pool_[at] | pool_[at + 1] | pool_[at + 2] | pool_[at + 3];
}

std::vector<std::uint8_t> ClimateField::categoryPlane() const {
    std::vector<std::uint8_t> out;
    if (!ready()) return out;
    const auto texels = static_cast<std::size_t>(wide_) * high_;
    out.assign(texels * 4, 0);
    if (!anyCategory_) return out;
    for (std::int32_t row = 0; row < high_; ++row)
        for (std::int32_t column = 0; column < wide_; ++column) {
            const auto i = static_cast<std::size_t>(row) * wide_ + column;
            for (std::size_t c = 0; c < 4; ++c) out[i * 4 + c] = sample(column, row, kFirstCategory + c);
        }
    return out;
}

std::array<std::uint8_t, 4> ClimateField::categoriesAt(double x, double y) const {
    if (!ready() || !anyCategory_ || !std::isfinite(x + y)) return {};
    const auto column = std::clamp<std::int64_t>(std::llround(x / metres_), 0, wide_ - 1);
    const auto row = std::clamp<std::int64_t>(std::llround(y / metres_), 0, high_ - 1);
    std::array<std::uint8_t, 4> out{};
    for (std::size_t c = 0; c < 4; ++c) out[c] = sample(column, row, kFirstCategory + c);
    return out;
}

std::vector<std::uint8_t> ClimateField::plane(int which) const {
    std::vector<std::uint8_t> out;
    if (!ready() || which < 0 || which >= kPlanes) return out;
    const auto texels = static_cast<std::size_t>(wide_) * high_;
    out.assign(texels * 4, 0);
    for (std::int32_t row = 0; row < high_; ++row)
        for (std::int32_t column = 0; column < wide_; ++column) {
            const auto i = static_cast<std::size_t>(row) * wide_ + column;
            for (std::size_t c = 0; c < 4; ++c)
                out[i * 4 + c] = sample(column, row, static_cast<std::size_t>(which) * 4 + c);
        }
    return out;
}

Fixed ClimateField::channel(std::int64_t x, std::int64_t y, std::size_t slot) const {
    const auto column = std::clamp<std::int64_t>(x, 0, wide_ - 1);
    const auto row = std::clamp<std::int64_t>(y, 0, high_ - 1);
    const Fixed stored = Fixed::ratio(sample(column, row, slot), 255);
    return signedSlot(slot) ? (stored - Fixed::ratio(1, 2)) * Fixed::fromInt(kSignedSpan) : stored;
}

float ClimateField::forestCoverAt(double x,double y) const {
    if (!ready() || !std::isfinite(x+y) || x<0 || y<0 || x>double(wide_-1)*metres_ || y>double(high_-1)*metres_) return 0;
    x/=metres_;y/=metres_;
    const auto ix=std::int64_t(std::floor(x)),iy=std::int64_t(std::floor(y));
    const auto at=[&](auto a,auto b){return channel(a,b,11).toDouble();};
    return float(std::lerp(std::lerp(at(ix,iy),at(ix+1,iy),x-ix),
        std::lerp(at(ix,iy+1),at(ix+1,iy+1),x-ix),y-iy));
}

HeightField::SurfaceClimate ClimateField::at(core::WorldPos where) const {
    HeightField::SurfaceClimate out{};
    if (!ready()) {
        out.foliage[2] = core::kOne;   // temperate, where there is nothing to say
        return out;
    }
    const std::int64_t x = floorDiv(where.x.toInt(), std::int64_t(metres_));
    const std::int64_t y = floorDiv(where.y.toInt(), std::int64_t(metres_));
    const Fixed fx = Fixed::ratio(floorMod(where.x.toInt(), std::int64_t(metres_)), metres_);
    const Fixed fy = Fixed::ratio(floorMod(where.y.toInt(), std::int64_t(metres_)), metres_);
    const auto blended = [&](std::size_t slot) {
        const Fixed top = channel(x, y, slot) + (channel(x + 1, y, slot) - channel(x, y, slot)) * fx;
        const Fixed bottom =
                channel(x, y + 1, slot) + (channel(x + 1, y + 1, slot) - channel(x, y + 1, slot)) * fx;
        return top + (bottom - top) * fy;
    };
    for (std::size_t i = 0; i < 4; ++i) out.foliage[i] = blended(i);
    out.desert = blended(4);
    for (std::size_t i = 0; i < 6; ++i) out.environment[i] = blended(5 + i);
    out.woodland = blended(12);
    return out;
}

} // namespace world
