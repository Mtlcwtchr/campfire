#pragma once
#include <array>
#include <optional>
// Client controls/menu adapter. World ownership and rendering live in game subsystems.

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "engine/pipeline/runner.hpp"
#include "engine/render/device.hpp"
#include "engine/render/geometry/mesh_cache.hpp"
#include "game/render/climate_textures.hpp"
#include "game/client/camera.hpp"
#include "game/content/ground_materials.hpp"
#include "game/render/calc/sprite_queue.hpp"
#include "engine/core/time.hpp"
#include "game/render/world_renderer.hpp"

namespace engine { class RenderPipeline; }
namespace game { class TerrainCollectPass; class SpritePass; class SceneModelsPass; class FoliagePass; }

namespace client {

class Explorer;
class ExploreMenu;

struct ExploreViewOptions {
    Camera::Mode cameraMode = Camera::Mode::Map;
    int gridMode = 0;
    // Draw the objects as edges from the first frame. The Shift+M toggle does
    // the same thing, but a shot never sees a keystroke - and a wireframe you
    // can only reach by hand is a wireframe you cannot put in a report.
    bool objectMesh = false;
    double yaw = 0.7853981633974483;
    double pitch = 0.4636476090008061;
    double heightOffset = 0;
    // Initial draw distance / fog in metres (--draw-distance); 0 keeps the menu's.
    double drawDistance = 0;
    bool fog = true;         // --no-fog
    // --scene-view-back METRES: freeze culling at the start camera and draw
    // from that far behind (and above) it.
    double sceneViewBack = 0;
    int graphicsTab = -1;    // --graphics-panel N: open the settings window on tab N
    bool iceVisible = true;
    // No window: frames go to an offscreen target of this size (--headless).
    int headlessWidth = 0, headlessHeight = 0;
    // Fly the scripted route and print frame times (--bench N frames a segment).
    int benchFrames = 0;
    int benchLoadFrames = 900;
    std::string benchJson;
    // --world-file FILE: open a world made in the editor (its regions, not the
    // seed and size above). --edit: start in Edit mode. --edit-layer NAME: on
    // that layer ("continents", "ranges", "hills", "sea", "weathering",
    // "rain"; "regions" is the default).
    std::string worldFile;
    bool editing = false;
    std::string editLayer;
    // --dig X,Y[,RADIUS[,METRES]]: once the ground has settled, dig (negative
    // metres, the default -12) or raise it there through the world delta,
    // exactly as a tool would, and let a shot wait for the ground to settle
    // again. A probe of the live path: nothing is saved in a session that digs.
    std::string dig;
    // --eye X,Y,ABOVE: a free camera standing exactly there, ABOVE metres over
    // the ground under it (yaw and pitch as given). For shots a person would
    // take on foot, which the orbit placement cannot aim.
    std::string eye;
    // --graphics-file FILE: shots and benches use these settings instead of
    // the defaults (the saved graphics.json is ASR_SHOT_USER_GRAPHICS=1).
    std::string graphicsFile;
    // --shot-list FILE: several pictures from one launch, one per line:
    //   PATH eye X Y ABOVE YAW PITCH     on foot, ABOVE metres over the ground
    //   PATH orbit X Y ZOOM YAW PITCH    third person round X,Y
    // Each waits for the streaming to settle after the camera moves. One world
    // raised and one renderer built instead of one per picture.
    std::string shotList;
    // --clean: no developer strip or progress panel, the picture only.
    bool clean = false;
};

class ExploreView {
public:
    explicit ExploreView(game::WorldRenderer& renderer) : renderer_(renderer) {}
    bool open(SDL_Window* window, const std::filesystem::path& assets, world::WorldSystem& world,
              const Camera& camera, ExploreMenu& menu, int headlessWidth = 0, int headlessHeight = 0);
    bool draw(const Camera& camera);
    void fog(bool on) { settings_.fog = on; }
    // Scene view: decisions from `frozen`, pixels from the camera draw() gets.
    void cull(const Camera* frozen) { settings_.cull = frozen; }
    // The marks laid on the ground (editor_overlay.hlsli): the world editor's
    // unless somebody else's are given - the game client's brushes.
    using EditorMarks = std::array<std::array<float, 4>, engine::kSceneEditorVectors>;
    void marks(std::optional<EditorMarks> marks) { marks_ = marks; }
    // The weather drawn at all: rain and snow in the air, wet and snowed-on
    // ground, the season's tint. Off, the world is shown in plain daylight -
    // what the editor wants, where rain across the view is only in the way.
    void weather(bool on) { weather_ = on; }
#if ASR_ENABLE_PROFILING
    bool compareGrassCulling(const Camera& camera, const std::string& path);
#endif
private:
    game::WorldRenderer& renderer_;
    std::uint64_t sketchShown_ = 0;   // the editor's sketch revision on the card, 0 none
    ExploreMenu* menu_ = nullptr;
    const world::WorldSystem* source_ = nullptr;
    game::WorldRenderSettings settings_;
    std::optional<EditorMarks> marks_;
    bool weather_ = true;
};

// Opens the world, shows it, and returns when the window is closed. With
// `shotPath` it draws one settled frame into that file and returns instead,
// which is how the terrain gets reviewed without a person at the keyboard.
int runExploreMode(SDL_Window* window, std::uint64_t seed, std::int32_t worldCells,
                   const std::filesystem::path& assets, const std::string& shotPath,
                   double startZoom = 0, const std::string& startAt = {}, bool measuring = false,
                   bool closeUp = false, bool tracing = false, int shotFrame = 0,
                   bool showMenu = false, int crowd = 0,
                   // Which instant of the shader clock a picture is taken at.
                   // Held, so two pictures of a moving thing are comparable -
                   // and settable, because otherwise everything that moves can
                   // only be reviewed at nought seconds, which for wind is the
                   // one moment it is doing nothing.
                   double shotTime = 0.0, const std::string& mapName = "none",
                   double weatherDay = 0.0, int weatherPreset = 0,
                   const core::TimeConfig& calendar = {},
                   const std::array<float, 4>& seasons = {18.0f, 32.0f, 21.0f, 9.0f},
                   const ExploreViewOptions& options = {});

} // namespace client
