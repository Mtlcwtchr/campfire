#pragma once
// Style tables (doc/plan_procedural_environment_2026-10-03.md, part H).
//
// The look of a world is the game's: what a shadow is tinted, how far a stone
// is pulled towards warm or cool, how coarse the print is. The engine does not
// know any of that. It knows that a game describes its look as named rows of
// four numbers, grouped into regions (for surfaces) and profiles (for the
// finished picture), that a region or profile applies where some categories
// or zones are, and that the shader the game writes reads the rows by index.
//
//   content/config/style/palettes.json   surface regions, read by styleSurface()
//   content/config/style/grades.json     picture profiles, read by stylePicture()
//
//   { "rows": ["shadow_tint", "highlight_tint", ...],     the row order the shader reads
//     "default": "temperate",
//     "blend_seconds": 4,                                 grades only
//     "regions"|"profiles": [
//       { "name": "temperate", "match": {"categories": [...], "zones": [...]},
//         "rows": {"shadow_tint": [r, g, b, a], ...},
//         "lut": "luts/temperate.cube" }                   grades only, optional
//     ] }
//
// A row a region leaves out is the default region's row; the default's own
// missing rows are zero.
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::environment {

using Row = std::array<float, 4>;

struct StyleEntry {
    std::string name;
    std::vector<std::string> categories, zones;
    std::vector<Row> rows;      // in the table's row order
    std::string lut;            // grades: path relative to the file's directory, or empty
};

struct StyleProblem {
    std::string file;
    std::string what;
};

class StyleTable {
public:
    static std::shared_ptr<StyleTable> load(const std::filesystem::path& file, std::vector<StyleProblem>* problems);

    [[nodiscard]] const std::vector<std::string>& rowNames() const { return rowNames_; }
    [[nodiscard]] std::optional<std::size_t> row(std::string_view name) const;
    [[nodiscard]] const std::vector<StyleEntry>& entries() const { return entries_; }
    [[nodiscard]] std::size_t defaultEntry() const { return default_; }
    [[nodiscard]] double blendSeconds() const { return blendSeconds_; }
    [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }

    // The entry for ground of this category in this zone: one that names both,
    // then one that names the zone, then the category, then the default.
    [[nodiscard]] std::size_t match(std::string_view category, std::string_view zone) const;

    // Every entry's rows, entry-major: what is uploaded as the style table.
    [[nodiscard]] std::vector<Row> packed() const;

private:
    std::vector<std::string> rowNames_;
    std::vector<StyleEntry> entries_;
    std::size_t default_ = 0;
    double blendSeconds_ = 4;
    std::filesystem::path directory_;
};

// A 3D colour lookup from an Adobe/Resolve .cube file.
struct Lut3d {
    int size = 0;
    std::vector<std::array<float, 3>> rgb;   // size^3, red fastest
    [[nodiscard]] bool empty() const { return size == 0; }
    [[nodiscard]] std::array<float, 3> apply(std::array<float, 3> c) const;
};
std::optional<Lut3d> loadCube(const std::filesystem::path& file, std::string* problem = nullptr);

// Moving between grading profiles as the camera crosses the world: the
// weights ease towards the profile the camera stands in over blend_seconds.
class GradeBlend {
public:
    explicit GradeBlend(std::shared_ptr<const StyleTable> table);
    void step(std::size_t target, double seconds);
    void snap(std::size_t target);
    // The rows of the blend, and the two strongest profiles for their LUTs.
    [[nodiscard]] std::vector<Row> rows() const;
    struct Pair { std::size_t a = 0, b = 0; float t = 0; };
    [[nodiscard]] Pair strongest() const;
    [[nodiscard]] const std::vector<float>& weights() const { return weights_; }

private:
    std::shared_ptr<const StyleTable> table_;
    std::vector<float> weights_;
};

} // namespace engine::environment
