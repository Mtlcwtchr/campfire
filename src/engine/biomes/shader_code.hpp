#pragma once
// The shader code and tables made from the terrain categories (registry.hpp).
//
// The config is not interpreted on every pixel. What a category is made of -
// which soils stand behind which classes, which noise kinds mix them, which
// texture layers, which rock on its slopes, which decal families - becomes
// code: a function per soil, a switch over the category ids, and only the
// noises and decals something uses are called. Every number - tints, shares,
// noise sizes, decal densities and colours, water colours - goes into a table
// texture instead, which a live edit refills without touching the compiler.
//
//   assets/shaders/terrain_layers.gen.hlsli   the texture array's constants
//   assets/shaders/terrain_biomes.gen.hlsli   the categories' code
//
// Both are ignored by git and remade from the config: at start when the
// config says something else than they do, from the editor when it saves,
// and from `worldtool biomes --emit`. The writer is deterministic (sorted by
// id, no time, no paths), so the same config is the same bytes and the
// shader cache's key (Device::shaderKey hashes the folder) does not move.
// Without them the shaders fall back to the engine's own ground
// (terrain_layers.hlsli, terrain_biomes_code.hlsli).
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "engine/biomes/registry.hpp"

namespace engine::biomes {

// The table: kTableColumns x kTableRows texels of four floats, in sections.
inline constexpr int kTableColumns = 16;
inline constexpr int kRowCategory = 0;     // + category id
inline constexpr int kRowSoil = 256;       // + soil index
inline constexpr int kRowWater = 512;      // + water biome id
inline constexpr int kRowDecal = 768;      // + decal index
inline constexpr int kRowForest = 1024;    // + forest biome id
inline constexpr int kRowFoliage = 1280;   // + foliage index
inline constexpr int kRowRock = 1536;      // + rock index
inline constexpr int kRowDecor = 1792;     // + decor biome id
inline constexpr int kTableRows = 2048;
inline constexpr std::size_t kShaderDecalsPerSet = 4;

struct ShaderCode {
    std::string layers;   // terrain_layers.gen.hlsli
    std::string biomes;   // terrain_biomes.gen.hlsli
};

[[nodiscard]] ShaderCode shaderCode(const Registry& registry);
// kTableColumns * kTableRows * 4 floats, row by row.
[[nodiscard]] std::vector<float> shaderTable(const Registry& registry);
// Of the structure: what the code depends on. Same key, same code.
[[nodiscard]] std::uint64_t structureKey(const Registry& registry);

inline constexpr const char* kLayersFile = "terrain_layers.gen.hlsli";
inline constexpr const char* kBiomesFile = "terrain_biomes.gen.hlsli";

struct EmitResult {
    bool ok = false;
    bool changed = false;               // a file was written: the shaders recompile once
    std::vector<std::string> problems;  // the validator's, when it refused
};
// Validates, then writes the two files into `shaders` where they differ from
// what is there. A registry the validator refuses writes nothing.
EmitResult emitShaderCode(const Registry& registry, const std::filesystem::path& shaders,
                          const std::filesystem::path& assets = {});
// The generated files the shaders last built with, kept beside the shader
// cache (<repo>/.cache/biomes): after a successful build, mark them good;
// after a failed one, put the last good ones back. Restore answers whether
// it changed anything.
void markShaderCodeGood(const std::filesystem::path& shaders);
bool restoreShaderCode(const std::filesystem::path& shaders);

// What a process does at start and on a live edit: the registry in `dir`
// read and validated, made active (active()), and - with `shaders` - its code
// written there where it changed. A registry the validator refuses is not
// made active and writes nothing; what was active stays. Problems go to
// `problems`. `structureChanged`: the code was rewritten (the shaders build
// again once).
struct StartUp {
    std::shared_ptr<const Registry> registry;   // the active one afterwards (may be null)
    bool loaded = false;                        // this call made a new one active
    bool structureChanged = false;
    std::vector<std::string> problems;
};
StartUp startUp(const std::filesystem::path& dir, const std::filesystem::path& shaders = {},
                const std::filesystem::path& assets = {});

// When the registry's files were last written, the newest of them: a watcher
// keeps the last it acted on and reloads when this moves past it.
std::filesystem::file_time_type registryWritten(const std::filesystem::path& dir);

} // namespace engine::biomes
