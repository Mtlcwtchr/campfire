#include "game/generation/world_sketch.hpp"

#include <algorithm>
#include <array>

namespace generation {

SketchMesh sketchMesh(const WorldLayout& layout) {
    SketchMesh mesh;
    const LayerMap& map = layout.layer(LayerId::Continents);
    const float texel = float(layerDef(LayerId::Continents).texelMetres);
    const std::int32_t perRegion = layerTexels(LayerId::Continents, kCellsPerRegion);
    const float shore = mesh.shore;
    for (std::int32_t ry = 0; ry < layout.regionsY; ++ry)
        for (std::int32_t rx = 0; rx < layout.regionsX; ++rx)
            if (layout.at(rx, ry).stage == RegionStage::Sketch && authored(layout, rx, ry)) ++mesh.sketchRegions;
    if (mesh.sketchRegions == 0 || map.empty()) return mesh;

    const auto sketchAt = [&](std::int32_t tx, std::int32_t ty) {
        const std::int32_t rx = tx / std::max(1, perRegion), ry = ty / std::max(1, perRegion);
        return layout.inBounds(rx, ry) && layout.at(rx, ry).stage == RegionStage::Sketch && authored(layout, rx, ry);
    };
    // The paint over the sea: nothing was made under a sketch, so what the
    // place is, is what was painted, times how much it covers.
    const auto valueAt = [&](std::int32_t tx, std::int32_t ty) {
        if (!map.inBounds(tx, ty) || !sketchAt(tx, ty)) return 0.0f;
        return map.value(tx, ty) * map.cover(tx, ty);
    };
    // The texel centre's position in the world.
    const auto at = [&](std::int32_t tx, std::int32_t ty) {
        return std::array<float, 2>{(float(tx) + 0.5f) * texel, (float(ty) + 0.5f) * texel};
    };

    for (std::int32_t tileY = 0; tileY < map.tilesY(); ++tileY)
        for (std::int32_t tileX = 0; tileX < map.tilesX(); ++tileX) {
            // This tile's quads reach one texel into the tiles to its right and
            // below, so it is wanted when any of the four holds paint.
            if (!map.tile(tileX, tileY) && !map.tile(tileX + 1, tileY) && !map.tile(tileX, tileY + 1) &&
                !map.tile(tileX + 1, tileY + 1))
                continue;
            // A tile's quads run to the first texel of the next tile, so tiles
            // join; a quad is kept when any corner of it is land.
            const std::int32_t x0 = tileX * kLayerTile, y0 = tileY * kLayerTile;
            const std::int32_t side = kLayerTile + 1;
            std::array<float, (kLayerTile + 1) * (kLayerTile + 1)> values{};
            bool any = false;
            for (std::int32_t j = 0; j < side; ++j)
                for (std::int32_t i = 0; i < side; ++i) {
                    const float v = valueAt(x0 + i, y0 + j);
                    values[std::size_t(j * side + i)] = v;
                    any |= v > shore;
                }
            if (!any) continue;
            // --- the top: the lattice, clipped at the shore by the renderer ---
            std::array<std::int32_t, (kLayerTile + 1) * (kLayerTile + 1)> vertexOf{};
            vertexOf.fill(-1);
            const auto vertex = [&](std::int32_t i, std::int32_t j) {
                auto& slot = vertexOf[std::size_t(j * side + i)];
                if (slot < 0) {
                    const auto p = at(x0 + i, y0 + j);
                    slot = std::int32_t(mesh.vertices.size());
                    mesh.vertices.push_back({p[0], p[1], 1.0f, values[std::size_t(j * side + i)]});
                }
                return std::uint32_t(slot);
            };
            for (std::int32_t j = 0; j + 1 < side; ++j)
                for (std::int32_t i = 0; i + 1 < side; ++i) {
                    const float a = values[std::size_t(j * side + i)], b = values[std::size_t(j * side + i + 1)];
                    const float c = values[std::size_t((j + 1) * side + i)], d = values[std::size_t((j + 1) * side + i + 1)];
                    if (std::max({a, b, c, d}) <= shore) continue;
                    const auto va = vertex(i, j), vb = vertex(i + 1, j), vc = vertex(i, j + 1), vd = vertex(i + 1, j + 1);
                    mesh.indices.insert(mesh.indices.end(), {va, vb, vd, va, vd, vc});
                    // --- the walls: marching squares on this quad -----------------
                    // Corners a b / c d; where an edge crosses the shore value.
                    const auto cross = [&](float va0, float va1, std::array<float, 2> p0, std::array<float, 2> p1) {
                        const float t = std::clamp((shore - va0) / (va1 - va0), 0.0f, 1.0f);
                        return std::array<float, 2>{p0[0] + (p1[0] - p0[0]) * t, p0[1] + (p1[1] - p0[1]) * t};
                    };
                    const auto pa = at(x0 + i, y0 + j), pb = at(x0 + i + 1, y0 + j);
                    const auto pc = at(x0 + i, y0 + j + 1), pd = at(x0 + i + 1, y0 + j + 1);
                    std::array<std::array<float, 2>, 4> hits{};
                    int count = 0;
                    if ((a > shore) != (b > shore)) hits[std::size_t(count++)] = cross(a, b, pa, pb);   // top
                    if ((b > shore) != (d > shore)) hits[std::size_t(count++)] = cross(b, d, pb, pd);   // right
                    if ((d > shore) != (c > shore)) hits[std::size_t(count++)] = cross(d, c, pd, pc);   // bottom
                    if ((c > shore) != (a > shore)) hits[std::size_t(count++)] = cross(c, a, pc, pa);   // left
                    // Two crossings: one wall. Four (a saddle): two, pairing
                    // each crossing with the next round the quad.
                    for (int k = 0; k + 1 < count; k += 2) {
                        const auto& p = hits[std::size_t(k)];
                        const auto& q = hits[std::size_t(k + 1)];
                        const auto base = std::uint32_t(mesh.vertices.size());
                        // The value on a wall is the shore's: never clipped.
                        mesh.vertices.push_back({p[0], p[1], 1.0f, 1023.0f});
                        mesh.vertices.push_back({q[0], q[1], 1.0f, 1023.0f});
                        mesh.vertices.push_back({p[0], p[1], 0.0f, 1023.0f});
                        mesh.vertices.push_back({q[0], q[1], 0.0f, 1023.0f});
                        mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 3, base, base + 3, base + 2});
                    }
                }
        }
    return mesh;
}

} // namespace generation

