#include "game/world/terrain_grid.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace world::terrain {

GridTopology makeGridTopology(std::uint16_t cells) {
    if (cells == 0) throw std::invalid_argument("terrain grid must contain cells");
    const std::size_t side = static_cast<std::size_t>(cells) + 1;
    const std::size_t vertices = side * side + 4 * side;
    if (vertices > std::numeric_limits<std::uint16_t>::max())
        throw std::invalid_argument("terrain grid does not fit uint16 indices");

    GridTopology out;
    out.cells = cells;
    out.vertices.reserve(vertices);
    out.indices.reserve(static_cast<std::size_t>(cells) * cells * 6 + 4 * cells * 6);
    for (std::uint16_t row = 0; row <= cells; ++row)
        for (std::uint16_t column = 0; column <= cells; ++column)
            out.vertices.push_back({column, row, 0, 0});

    const auto at = [side](std::uint16_t column, std::uint16_t row) {
        return static_cast<std::uint16_t>(static_cast<std::size_t>(row) * side + column);
    };
    for (std::uint16_t row = 0; row < cells; ++row)
        for (std::uint16_t column = 0; column < cells; ++column) {
            const auto top = at(column, row);
            const auto bottom = at(column, static_cast<std::uint16_t>(row + 1));
            out.indices.insert(out.indices.end(), {top, static_cast<std::uint16_t>(top + 1), bottom,
                                                   static_cast<std::uint16_t>(top + 1),
                                                   static_cast<std::uint16_t>(bottom + 1), bottom});
        }

    const auto skirt = [&](auto coordinate) {
        const auto first = static_cast<std::uint16_t>(out.vertices.size());
        for (std::uint16_t n = 0; n <= cells; ++n) {
            const auto [column, row] = coordinate(n);
            out.vertices.push_back({column, row, 1, 0});
        }
        for (std::uint16_t n = 0; n < cells; ++n) {
            const auto [column, row] = coordinate(n);
            const auto [nextColumn, nextRow] = coordinate(static_cast<std::uint16_t>(n + 1));
            const auto top = at(column, row);
            const auto next = at(nextColumn, nextRow);
            const auto low = static_cast<std::uint16_t>(first + n);
            out.indices.insert(out.indices.end(), {top, low, next, next,
                                                   low, static_cast<std::uint16_t>(low + 1)});
        }
    };
    skirt([&](std::uint16_t n) { return std::pair{n, std::uint16_t{0}}; });
    skirt([&](std::uint16_t n) { return std::pair{static_cast<std::uint16_t>(cells - n), cells}; });
    skirt([&](std::uint16_t n) { return std::pair{std::uint16_t{0}, static_cast<std::uint16_t>(cells - n)}; });
    skirt([&](std::uint16_t n) { return std::pair{cells, n}; });
    return out;
}

} // namespace world::terrain

