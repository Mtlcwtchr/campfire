#pragma once
// The only terrain topology kept by the renderer. Every terrain node instances
// this unit grid; world position and height come from node data and textures.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace world::terrain {

struct GridVertex {
    std::uint16_t column = 0;
    std::uint16_t row = 0;
    // Skirt copies share x/y with a border vertex and are lowered in the shader.
    std::uint16_t skirt = 0;
    std::uint16_t reserved = 0;
};

struct GridTopology {
    std::uint16_t cells = 0;
    std::vector<GridVertex> vertices;
    std::vector<std::uint16_t> indices;

    [[nodiscard]] std::size_t surfaceVertexCount() const {
        const auto side = static_cast<std::size_t>(cells) + 1;
        return side * side;
    }
    [[nodiscard]] std::size_t triangleCount() const { return indices.size() / 3; }
    [[nodiscard]] std::size_t bytes() const {
        return vertices.size() * sizeof(GridVertex) + indices.size() * sizeof(std::uint16_t);
    }
};

// Builds once at renderer setup. Sixty-four cells fit comfortably in uint16
// indices even with four independent skirt strips.
GridTopology makeGridTopology(std::uint16_t cells = 64);

} // namespace world::terrain

