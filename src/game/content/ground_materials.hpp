#pragma once
// What the ground is made of, as data.
//
// In content rather than beside the renderer, and that is a dependency
// direction rather than a filing preference: the editor reads and writes this
// table and must not link a graphics device to do it, while the renderer reads
// it and must not reach into a tool. Content sits below both.
//
// One table for both ends of it: the bakery turns `set`, `variant`, `tint` and
// `keepColour` into a texture, and the renderer reads the rest every frame to
// decide how one material meets the next. They are one decision - how this
// stuff looks - and split across two files they drift apart.
//
// Editable while the game is running: the numbers the renderer reads go into
// the scene each frame, so moving one moves the ground in the next frame. What
// the bakery reads does not - that needs the texture baking again - and the
// difference is written on the screen where they are edited.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace content {

struct GroundMaterial {
    std::string name;
    // For the bakery.
    std::string set, variant;
    float tint[3]{1, 1, 1};
    float keepColour = 1.0f;
    // For the renderer, every frame.
    float metresPerTurn = 24.0f;   // ground one turn of the texture covers
    float blendWidth = 0.34f;      // how wide the border with a neighbour is
    float tear = 0.30f;            // how far noise tears that border about
    float tearMetres = 3.0f;       // how big the tears are
};

// The six the ground blends between, in the order their weights come in the
// vertex - which is the order the shader indexes its layers. Anything after
// them in the file is baked but not blended.
inline constexpr std::size_t kBlendedMaterials = 6;

// Reads the table as it is, with no substitute. False, and `into` untouched,
// if the file is not one: a tool that was handed the wrong file has to say so
// rather than show somebody else's numbers, and a tool watching a file for
// changes has to be able to tell a half-written file from a table of defaults.
bool readGroundMaterials(const std::filesystem::path& file, std::vector<GroundMaterial>& into);

// Reads the table. An unreadable or missing file gives back the built-in
// defaults rather than nothing: a game that will not start because somebody
// mistyped a number in a look-and-feel file is worse than a game that looks
// wrong and says so.
std::vector<GroundMaterial> loadGroundMaterials(const std::filesystem::path& file);

// Writes it back, keeping a copy of what was there first as FILE.bak. Returns
// false if it could not be written, which is worth telling the person who just
// pressed save.
bool saveGroundMaterials(const std::filesystem::path& file,
                         const std::vector<GroundMaterial>& materials);

} // namespace content
