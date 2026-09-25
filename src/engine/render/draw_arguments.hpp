#pragma once
// The five numbers a card reads to issue one draw.
//
// Its own header, depending on nothing, because both ends of the indirect path
// need it and neither should drag the other in: a compute shader's C++ side
// writes these, and a plain CPU plan that never touches a device writes the
// same thing. A second declaration of the same layout is how the two come to
// disagree by a field.
#include <cstdint>

namespace engine {

// Must match DrawArguments in the shaders, and SDL_GPUIndexedIndirectDrawCommand.
struct DrawArguments {
    std::uint32_t indexCount = 0;
    std::uint32_t instanceCount = 0;
    std::uint32_t firstIndex = 0;
    std::int32_t vertexOffset = 0;
    std::uint32_t firstInstance = 0;
};
static_assert(sizeof(DrawArguments) == 20, "the card reads this layout");

} // namespace engine
