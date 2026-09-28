cmake_minimum_required(VERSION 3.24)
project(campfire LANGUAGES CXX)

include(FetchContent)

# EnTT is header-only. Keep it as a fetched, pinned dependency so the
# simulation can use an ECS without coupling itself to SDL or the renderer.
FetchContent_Declare(EnTT
        GIT_REPOSITORY https://github.com/skypjack/entt.git
        GIT_TAG v3.15.0
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(EnTT)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ASR_BUILD_CLIENT "Build the SDL3 graphical client" ON)
option(ASR_BUILD_ENGINE_EDITOR "Build the standalone engine module editor" ${ASR_BUILD_CLIENT})
option(ASR_ENGINE_EDITOR_GPU "Use the engine GPU viewport (OFF keeps a DXC-free CPU editor)" ON)
option(ASR_BUILD_TESTS "Build the test suite" ON)
option(ASR_TEST_GPU "Register tests requiring a window system and GPU with CTest" OFF)
option(ASR_ENABLE_PROFILING "Legacy default for ASR_ENABLE_DIAGNOSTICS" OFF)
option(ASR_ENABLE_DIAGNOSTICS "Compile metrics, profiling UI, traces and diagnostic readbacks" ${ASR_ENABLE_PROFILING})
# Retain old source guards/tool configurations; the new master switch wins.
set(ASR_ENABLE_PROFILING ${ASR_ENABLE_DIAGNOSTICS})
option(ASR_ENABLE_FPS "Compile the lightweight wall-clock FPS display" ON)
# The frame counter stands on its own, and used to not: it was ANDed with the
# diagnostics switch, so with diagnostics off - which is how this is normally
# built - the one number anybody actually wants while flying around was compiled
# out along with the profiler. It is two doubles and a counter; it does not need
# the machinery it was chained to.
add_compile_definitions(ASR_ENABLE_PROFILING=$<BOOL:${ASR_ENABLE_PROFILING}>
        ASR_ENABLE_DIAGNOSTICS=$<BOOL:${ASR_ENABLE_DIAGNOSTICS}>
        ASR_ENABLE_FPS=$<BOOL:${ASR_ENABLE_FPS}>)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# Debug builds this engine at -Og, not -O0, and the reason is that -O0 does not
# produce a program anybody can look at.
#
# Nearly everything here is written to be free after inlining: every arithmetic
# operator on core::Fixed is a function, the surface a mesh is built from
# arrives as a std::function called millions of times a page, and the geometry
# is templates throughout. At -O0 none of that is folded, so the same frame
# costs several times what it costs when built to run - and the frame rate,
# which is the thing most often being judged, becomes a number about the
# compiler rather than about the game. A day went into "why is this thirty
# frames a second" that was only ever the profile.
#
# -Og keeps the debugger honest: it is the optimisation level clang documents
# for debugging, line tables stay usable and variables mostly survive. Asserts
# stay on, because NDEBUG is what a debug build is FOR - the slowness never was.
#
# Written only when it is still CMake's own default. Forcing it on every
# configure rewrites the cache every time, and a rewritten cache is a reason to
# reconfigure - which on this project means rebuilding the vendored shader
# compiler, half an hour of it, for a flag nobody changed.
if(CMAKE_CXX_FLAGS_DEBUG STREQUAL "-g")
    set(CMAKE_C_FLAGS_DEBUG "-Og -g" CACHE STRING "" FORCE)
    set(CMAKE_CXX_FLAGS_DEBUG "-Og -g" CACHE STRING "" FORCE)
endif()

add_library(asr_warnings INTERFACE)
target_compile_options(asr_warnings INTERFACE
        $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wno-unused-parameter>)

# Vendored single-header JSON (content is declarative text data, GDD 11).
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${CMAKE_SOURCE_DIR}/third_party)

# ---------------------------------------------------------------------------
# engine/core - fixed point, deterministic RNG, ids, calendar. No game concepts.
# ---------------------------------------------------------------------------
add_library(asr_core STATIC
        src/engine/core/fixed.cpp
        src/engine/core/geometry.cpp
        src/engine/core/time.cpp)
target_include_directories(asr_core PUBLIC src)
target_link_libraries(asr_core PRIVATE asr_warnings)
include(src/engine/modules.cmake)

# The entity system itself, and nothing else: registry, spatial cells, the
# snapshot boundary, deterministic batching, the worker command buffer. No game
# vocabulary, no rendering, no window. Header-only, so it is what it says it is.
add_library(asr_ecs INTERFACE)
target_include_directories(asr_ecs INTERFACE src)
target_link_libraries(asr_ecs INTERFACE asr_core EnTT::EnTT)

# Render systems that need no GPU: the ones that read entities and produce what
# a renderer will draw. Separate from asr_engine_render because deciding what a
# frame contains is not the same job as putting it on a card, and because this
# half has to build and be tested without a window.
add_library(asr_render_systems STATIC
        src/engine/render/systems/instance_gather.cpp
        src/engine/render/systems/level_select.cpp
        src/engine/render/systems/frustum_cull.cpp
        src/engine/render/systems/terrain_gather.cpp
        src/engine/render/systems/cluster_draws.cpp
        src/engine/render/systems/source_cluster_draws.cpp
        src/engine/render/systems/instance_hierarchy_select.cpp)
target_include_directories(asr_render_systems PUBLIC src)
target_link_libraries(asr_render_systems PUBLIC asr_ecs asr_geometry asr_representation PRIVATE asr_warnings)

# Reusable CPU geometry/reconstruction: no game, window or GPU dependency.
add_library(asr_geometry STATIC src/engine/geometry/smart_terrain_mesh.cpp)
target_include_directories(asr_geometry PUBLIC src)
target_link_libraries(asr_geometry PUBLIC asr_virtual_geometry PRIVATE asr_warnings)

# ---------------------------------------------------------------------------
# content - declarative definitions and their loader/validator.
# ---------------------------------------------------------------------------
add_library(asr_content STATIC
        src/game/content/defs.cpp
        src/game/content/content_db.cpp
        src/game/content/ground_materials.cpp)
target_include_directories(asr_content PUBLIC src)
target_link_libraries(asr_content PUBLIC asr_core PRIVATE nlohmann_json asr_warnings)

# ---------------------------------------------------------------------------
# generation - terrain, resources, hearth placement.
# ---------------------------------------------------------------------------
add_library(asr_generation STATIC
        src/game/generation/hybrid_terrain.cpp
        src/game/generation/terrain_foundation.cpp
        src/game/generation/local_map_gen.cpp
        src/game/generation/world_domain.cpp
        src/game/generation/world_scale_policy.cpp
        src/game/generation/world_macro.cpp
        src/game/generation/land_coverage.cpp
        src/game/generation/world_map_gen.cpp)
target_include_directories(asr_generation PUBLIC src)
target_compile_definitions(asr_generation PRIVATE ASR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(asr_generation PUBLIC asr_core asr_content asr_terrain_generator EnTT::EnTT
        PRIVATE nlohmann_json asr_warnings)

# ---------------------------------------------------------------------------
# world - the ground as geometry: the height field, chunk meshes, cliffs.
# Continuous world coordinates; chunks are storage and streaming, not borders.
# ---------------------------------------------------------------------------
add_library(asr_world STATIC
        src/game/world/world_builder.cpp
        src/game/world/surface_reconstruction.cpp
        src/game/world/scene_placement.cpp
        src/game/world/edit_layer.cpp
        src/game/world/terrain_brush.cpp
        src/game/world/height_field.cpp
        src/game/world/soil.cpp
        src/game/world/terrain_mesh.cpp
        src/game/world/terrain_grid.cpp
        src/game/world/terrain_lod.cpp
        src/game/world/terrain_config.cpp
        src/game/world/terrain_plan.cpp
        src/game/world/terrain_seams.cpp
        src/game/world/terrain_detail.cpp
        src/game/world/ring_mesh.cpp
        src/game/world/tile_mesh.cpp
        src/game/world/climate_field.cpp
        src/game/world/terrain_streaming/base_tile_baker.cpp
        src/game/world/terrain_streaming/baked_page_cache.cpp
        src/game/world/terrain_streaming/graph_carve.cpp
        src/game/world/terrain_streaming/hydrology_builder.cpp
        src/game/world/terrain_streaming/page_ground.cpp
        src/game/world/terrain_streaming/page_store.cpp
        src/game/world/terrain_streaming/hydrology_cache.cpp
        src/game/world/terrain_streaming/hydrology_graph.cpp
        src/game/world/terrain_streaming/water_tile.cpp
        src/game/world/terrain_streaming/tile_cache.cpp
        src/game/world/terrain_streaming/worker_pool.cpp
        src/game/world/terrain_streaming/worker_runtime.cpp
        src/game/world/spatial.cpp
        src/game/world/navmesh.cpp
        src/game/world/macro.cpp)
target_include_directories(asr_world PUBLIC src)
target_sources(asr_world PRIVATE src/game/world/scene_scatter.cpp)
target_sources(asr_world PRIVATE src/game/world/scene_entities.cpp)
target_sources(asr_world PRIVATE src/game/world/terrain_source.cpp)
target_sources(asr_world PRIVATE src/game/world/terrain_source_tree.cpp)
target_link_libraries(asr_world PUBLIC asr_core asr_generation asr_geometry asr_render_systems
        PRIVATE nlohmann_json asr_warnings)

# ---------------------------------------------------------------------------
# simulation - the headless world. Knows nothing about rendering or input.
# game/work is a separate work pipeline; it reads world/ECS state but is not an
# ECS system and is not part of the actor tick scheduler.
# ---------------------------------------------------------------------------
add_library(asr_simulation STATIC
        src/game/ai/planner.cpp
        src/game/ecs/render/extraction.cpp
        src/game/simulation/world.cpp
        src/game/simulation/farming.cpp
        src/game/simulation/livestock.cpp
        src/game/simulation/pathfinder.cpp
        src/game/simulation/inventory.cpp
        src/game/simulation/needs.cpp
        src/game/work/planner.cpp
        src/game/work/execution.cpp
        src/game/simulation/population.cpp
        src/game/simulation/zones.cpp
        src/game/simulation/report.cpp
        src/game/simulation/savegame.cpp)
target_include_directories(asr_simulation PUBLIC src)
target_link_libraries(asr_simulation PUBLIC asr_core asr_content asr_generation asr_world EnTT::EnTT
        PRIVATE asr_warnings)

# ---------------------------------------------------------------------------
# sim_runner - headless scenario runs (GDD 11: the agent must be able to build,
# run a scenario, check invariants and get a report with no graphical client).
# ---------------------------------------------------------------------------
add_executable(sim_runner sim_runner/main.cpp)
target_link_libraries(sim_runner PRIVATE asr_simulation asr_warnings)

# ---------------------------------------------------------------------------
# tools - content validator.
# ---------------------------------------------------------------------------
# terrain_lint - looks for the marks the generator leaves on its own work:
# steps that should be slopes, patterns that line up with the axes, rivers drawn
# as dashed lines, levels of detail that disagree.
add_executable(terrain_lint tools/terrain_lint.cpp)
target_link_libraries(terrain_lint PRIVATE asr_world asr_warnings)

add_executable(terrain_resolution_probe tools/terrain_resolution_probe.cpp)
target_link_libraries(terrain_resolution_probe PRIVATE asr_world nlohmann_json asr_warnings)

add_executable(terrain_subdivision_probe tools/terrain_subdivision_probe.cpp)
target_link_libraries(terrain_subdivision_probe PRIVATE asr_world nlohmann_json asr_warnings)

# lake_edge_probe - counts lake water that ends over lower ground (a lake
# hanging in the air) in the baked pages of every lake the graph holds.
add_executable(lake_edge_probe tools/lake_edge_probe.cpp)
target_link_libraries(lake_edge_probe PRIVATE asr_world asr_warnings)

add_executable(terrain_feature_probe tools/terrain_feature_probe.cpp)
target_link_libraries(terrain_feature_probe PRIVATE asr_world nlohmann_json asr_warnings)

add_executable(hybrid_terrain_probe tools/hybrid_terrain_probe.cpp)
target_link_libraries(hybrid_terrain_probe PRIVATE asr_world nlohmann_json asr_warnings)

add_executable(mountain_shape_probe tools/mountain_shape_probe.cpp)
target_link_libraries(mountain_shape_probe PRIVATE asr_world nlohmann_json asr_warnings)

add_executable(world_scale_policy_probe tools/world_scale_policy_probe.cpp)
target_link_libraries(world_scale_policy_probe PRIVATE asr_generation nlohmann_json asr_warnings)

add_executable(world_scale_probe tools/world_scale_probe.cpp)
target_link_libraries(world_scale_probe PRIVATE asr_world asr_generation nlohmann_json asr_warnings)

# scene_model_clusters - the offline half of virtual geometry for props:
# reads the meshes prepare_scene_models.py wrote and puts a cluster DAG beside
# each one. See doc/reports/vg_step_3.
add_executable(scene_model_clusters tools/scene_model_clusters.cpp)
target_link_libraries(scene_model_clusters PRIVATE asr_geometry nlohmann_json asr_warnings)

add_executable(content_validator tools/content_validator.cpp)
target_link_libraries(content_validator PRIVATE asr_content asr_simulation asr_warnings)

# ---------------------------------------------------------------------------
# client - SDL3 top-down renderer. Optional; the simulation never links it.
# ---------------------------------------------------------------------------
if(ASR_BUILD_CLIENT OR ASR_BUILD_ENGINE_EDITOR)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG release-3.2.8
            GIT_SHALLOW TRUE
            EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(SDL3)

    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_TESTS OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_SAMPLES OFF CACHE BOOL "" FORCE)
    # Packed RG16 depth must bypass platform premultiplication/color management.
    set(SDLIMAGE_BACKEND_STB ON CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3_image
            GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git
            GIT_TAG release-3.2.4
            GIT_SHALLOW TRUE
            EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(SDL3_image)

    add_library(asr_engine_gui STATIC src/engine/ui/ui.cpp src/engine/editor/module_panel.cpp)
    target_include_directories(asr_engine_gui PUBLIC src)
    target_link_libraries(asr_engine_gui PUBLIC asr_representation asr_terrain_generator
            asr_virtual_geometry SDL3::SDL3-static PRIVATE asr_warnings)
    add_library(Campfire::Gui ALIAS asr_engine_gui)
    if(ASR_BUILD_CLIENT OR (ASR_BUILD_ENGINE_EDITOR AND ASR_ENGINE_EDITOR_GPU))
        include(src/engine/gpu_modules.cmake)
    endif()
    if(ASR_BUILD_ENGINE_EDITOR)
        add_executable(campfire_engine_editor src/engine/editor/main.cpp)
        target_link_libraries(campfire_engine_editor PRIVATE asr_engine_gui
                SDL3_image::SDL3_image-static nlohmann_json asr_warnings)
        target_compile_definitions(campfire_engine_editor PRIVATE ASR_ENGINE_EDITOR_GPU=$<BOOL:${ASR_ENGINE_EDITOR_GPU}>)
        if(ASR_ENGINE_EDITOR_GPU)
            target_link_libraries(campfire_engine_editor PRIVATE asr_engine_viewport)
        endif()
    endif()
endif()

if(ASR_BUILD_CLIENT)
    # The editor is its own program: it opens no world and ticks nothing, so it
    # starts in a second rather than after a continent has been generated.
    add_executable(asr_editor
            src/game/editor/main.cpp
            src/game/ui/editor.cpp)
    target_link_libraries(asr_editor PRIVATE asr_simulation asr_engine_gui nlohmann_json SDL3::SDL3-static
            SDL3_image::SDL3_image-static asr_warnings)

    # This game's passes. They know what ground is made of and what grass is; the
    # engine does not, which is the whole point of the split.
    add_library(asr_game_render STATIC
            src/game/render/world_renderer.cpp
            src/game/render/pass_ids.cpp
            src/game/render/grass_cull.cpp
            src/game/render/passes/terrain_pass.cpp
            src/game/render/passes/foliage_pass.cpp
            src/game/render/passes/far_trees_pass.cpp
            src/game/render/passes/water_pass.cpp
            src/game/render/passes/weather_pass.cpp
            src/game/render/passes/sky_pass.cpp src/game/render/passes/frustum_pass.cpp
            src/game/render/passes/menu_pass.cpp
            src/game/render/passes/sprite_pass.cpp
            src/game/client/explore_menu.cpp src/game/client/graphics_panel.cpp
            src/game/render/calc/terrain_stream.cpp
            src/game/render/calc/terrain_collect.cpp
            src/game/render/climate_textures.cpp
            src/game/render/height_page_atlas.cpp
            src/game/render/height_page_stream.cpp
            src/game/render/gpu_terrain.cpp
            src/game/render/calc/sprite_queue.cpp)
    target_include_directories(asr_game_render PUBLIC src)
    target_sources(asr_game_render PRIVATE src/game/render/passes/scene_models_pass.cpp
            src/game/render/shadow_clipmap.cpp src/game/render/forest_proxy_cache.cpp
            src/game/render/forest_hierarchy.cpp src/game/render/graphics_settings.cpp)
    if(ASR_ENABLE_PROFILING)
        target_sources(asr_game_render PRIVATE src/game/render/world_renderer_diagnostics.cpp)
    endif()
    target_link_libraries(asr_game_render PUBLIC asr_engine_render asr_world asr_camera asr_virtual_geometry_gpu
            asr_impostors_gpu PRIVATE nlohmann_json asr_warnings)

    add_executable(asr_client
            src/game/client/main.cpp
            src/game/client/renderer.cpp
            src/game/client/selection.cpp
            src/game/client/controls.cpp
            src/game/client/newgame_screen.cpp
            src/game/client/world_dials.cpp
            src/game/client/explorer.cpp
            src/game/client/explore_view.cpp
            src/game/client/explore_bench.cpp
            src/game/ui/hud.cpp
            src/game/ui/editor.cpp
            src/game/ui/overlay.cpp
            src/game/ui/info_panel.cpp)
    target_link_libraries(asr_client PRIVATE asr_simulation asr_world asr_game_render asr_engine_gui nlohmann_json
            SDL3::SDL3-static SDL3_image::SDL3_image-static
            SDL3_shadercross::SDL3_shadercross-static nlohmann_json asr_warnings)
    if(ASR_ENABLE_PROFILING)
        target_sources(asr_client PRIVATE src/game/client/explore_diagnostics.cpp)
    endif()

    # asr_mesh_lab - UE Nanite's bake against our cluster DAG on the same meshes,
    # rendered on the CPU into PNGs and a table. No window, no GPU. Input comes
    # from tools/ue_nanite_reference.py (headless Unreal).
    add_executable(asr_mesh_lab tools/mesh_lab.cpp)
    target_link_libraries(asr_mesh_lab PRIVATE asr_geometry nlohmann_json
            SDL3::SDL3-static SDL3_image::SDL3_image-static asr_warnings)

    add_executable(asr_shader_check tools/shader_check.cpp)
    target_link_libraries(asr_shader_check PRIVATE SDL3::SDL3-static
            SDL3_shadercross::SDL3_shadercross-static asr_warnings)

    # Public branding changes without breaking existing CLion targets/scripts.
    function(campfire_binary target output)
        set_target_properties(${target} PROPERTIES OUTPUT_NAME "${output}")
        if(UNIX)
            add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E rm -f "$<TARGET_FILE_DIR:${target}>/${target}"
                    COMMAND "${CMAKE_COMMAND}" -E create_symlink "$<TARGET_FILE_NAME:${target}>"
                            "$<TARGET_FILE_DIR:${target}>/${target}"
                    VERBATIM)
        else()
            add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${target}>"
                            "$<TARGET_FILE_DIR:${target}>/${target}${CMAKE_EXECUTABLE_SUFFIX}"
                    VERBATIM)
        endif()
    endfunction()
    campfire_binary(asr_client campfire_client)
    campfire_binary(asr_editor campfire_editor)

    option(ASR_PREPARE_SCENE_MODELS "Prepare instanced nature models and impostors offline" ON)
    if(ASR_PREPARE_SCENE_MODELS)
        set(ASR_SCENE_PYTHON "" CACHE FILEPATH "Python with tools/scene_models_requirements.txt installed")
        if(NOT ASR_SCENE_PYTHON)
            if(EXISTS "${CMAKE_SOURCE_DIR}/.cache/terrain-reference-tools/bin/python")
                set(ASR_SCENE_PYTHON "${CMAKE_SOURCE_DIR}/.cache/terrain-reference-tools/bin/python")
            else()
                find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
                set(ASR_SCENE_PYTHON "${Python3_EXECUTABLE}")
            endif()
        endif()
        set(SCENE_MODEL_ARCHIVE "${CMAKE_SOURCE_DIR}/assets/models/Stylized Nature MegaKit[Standard].zip")
        set(SCENE_MODEL_OUTPUT "${CMAKE_SOURCE_DIR}/assets/generated/scene_models")
        file(GLOB SCENE_UE_MARKER CONFIGURE_DEPENDS "${SCENE_MODEL_OUTPUT}/.ue-imported")
        if(SCENE_UE_MARKER)
            if(NOT EXISTS "${SCENE_MODEL_OUTPUT}/manifest.json")
                message(FATAL_ERROR "Activated UE resources have no manifest; rerun install_ue_assets.py")
            endif()
            add_custom_target(asr_scene_models DEPENDS "${SCENE_MODEL_OUTPUT}/manifest.json")
        else()
            add_custom_command(OUTPUT "${SCENE_MODEL_OUTPUT}/manifest.json"
                    COMMAND "${ASR_SCENE_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/prepare_scene_models.py"
                            --archive "${SCENE_MODEL_ARCHIVE}" --output "${SCENE_MODEL_OUTPUT}"
                    DEPENDS "${SCENE_MODEL_ARCHIVE}" "${CMAKE_SOURCE_DIR}/tools/prepare_scene_models.py"
                            "${CMAKE_SOURCE_DIR}/tools/mesh_lod.py"
                            "${CMAKE_SOURCE_DIR}/tools/scene_models_requirements.txt"
                    COMMENT "Preparing shared nature meshes and eight-view impostors (numpy/Pillow required)"
                    VERBATIM)
            add_custom_target(asr_scene_models DEPENDS "${SCENE_MODEL_OUTPUT}/manifest.json")
        endif()
        # The renderer consumes the cluster sidecars as content, not as an
        # optional developer experiment. Keep the offline DAG build behind the
        # mesh preparation step and use a stamp because the tool writes one
        # sidecar per model plus the two spatial grove shells.
        set(SCENE_CLUSTER_STAMP "${SCENE_MODEL_OUTPUT}/.cluster_sidecars.stamp")
        add_custom_command(OUTPUT "${SCENE_CLUSTER_STAMP}"
                COMMAND "$<TARGET_FILE:scene_model_clusters>" "${SCENE_MODEL_OUTPUT}"
                COMMAND "${CMAKE_COMMAND}" -E touch "${SCENE_CLUSTER_STAMP}"
                DEPENDS asr_scene_models scene_model_clusters "${SCENE_MODEL_OUTPUT}/manifest.json"
                COMMENT "Building per-model geometry DAGs and instance canopy sidecars"
                VERBATIM)
        add_custom_target(asr_scene_geometry DEPENDS "${SCENE_CLUSTER_STAMP}")
        add_dependencies(asr_client asr_scene_geometry)
    endif()
endif()

# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------
if(ASR_BUILD_TESTS)
    enable_testing()
    if(TARGET asr_engine_viewport)
        add_executable(asr_engine_viewport_tests tests/main.cpp tests/test_dag_viewport_gpu.cpp)
        target_link_libraries(asr_engine_viewport_tests PRIVATE asr_engine_viewport asr_engine_gui asr_warnings)
        if(ASR_TEST_GPU)
            add_test(NAME engine_viewport_gpu COMMAND asr_engine_viewport_tests)
            set_tests_properties(engine_viewport_gpu PROPERTIES LABELS "gpu" TIMEOUT 60
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        endif()
    endif()
    add_executable(asr_engine_module_tests tests/main.cpp tests/test_engine_modules.cpp
            tests/test_recursive_impostor.cpp tests/test_impostor_hierarchy.cpp
            tests/test_camera.cpp tests/test_camera_matrix.cpp
            tests/test_plate_field.cpp tests/test_range_field.cpp tests/test_erosion_field.cpp)
    target_link_libraries(asr_engine_module_tests PRIVATE asr_representation asr_terrain_generator
            asr_virtual_geometry asr_impostors asr_warnings)
    add_test(NAME engine_modules COMMAND asr_engine_module_tests)
    set_tests_properties(engine_modules PROPERTIES TIMEOUT 45)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME external_profiler COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/tests/test_external_profiler.py")
        set_tests_properties(external_profiler PROPERTIES TIMEOUT 30)
        execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import numpy; import PIL"
                RESULT_VARIABLE ASR_DEPTH_TEST_DEPS OUTPUT_QUIET ERROR_QUIET)
        if(ASR_DEPTH_TEST_DEPS EQUAL 0)
            add_test(NAME impostor_depth_baker COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/tests/test_impostor_depth.py")
            set_tests_properties(impostor_depth_baker PROPERTIES TIMEOUT 30)
        endif()
    endif()
    if(ASR_BUILD_CLIENT AND ASR_PREPARE_SCENE_MODELS)
        add_test(NAME ue_asset_import COMMAND "${ASR_SCENE_PYTHON}"
                "${CMAKE_SOURCE_DIR}/tests/test_prepare_ue_assets.py")
        set_tests_properties(ue_asset_import PROPERTIES TIMEOUT 60)
    endif()
    add_executable(asr_smart_mesh_tests tests/main.cpp tests/test_smart_mesh.cpp)
    target_link_libraries(asr_smart_mesh_tests PRIVATE asr_geometry asr_warnings)
    if(ASR_BUILD_CLIENT)
        target_compile_definitions(asr_smart_mesh_tests PRIVATE ASR_SMART_RENDER_TESTS=1)
        target_link_libraries(asr_smart_mesh_tests PRIVATE asr_engine_render)
    endif()
    target_sources(asr_smart_mesh_tests PRIVATE tests/test_cluster_dag.cpp
            tests/test_instance_hierarchy.cpp tests/test_page_residency.cpp)
    target_sources(asr_smart_mesh_tests PRIVATE
            src/engine/render/systems/instance_hierarchy_select.cpp)
    target_sources(asr_smart_mesh_tests PRIVATE tests/test_quad_mass.cpp)
    target_sources(asr_smart_mesh_tests PRIVATE tests/test_region_mass.cpp)
    add_test(NAME smart_mesh_components COMMAND asr_smart_mesh_tests)
    set_tests_properties(smart_mesh_components PROPERTIES TIMEOUT 30)
    add_executable(asr_world_system_tests tests/main.cpp tests/test_world_system.cpp
            tests/test_scene_entities.cpp tests/test_terrain_brush.cpp)
    target_link_libraries(asr_world_system_tests PRIVATE asr_world asr_warnings)
    add_test(NAME world_system COMMAND asr_world_system_tests)
    set_tests_properties(world_system PROPERTIES TIMEOUT 30)
    add_executable(asr_scene_view_tests tests/main.cpp tests/test_scene_view.cpp
            tests/test_camera_matrix.cpp tests/test_scene_scatter.cpp tests/test_ecology_shadow.cpp)
    target_link_libraries(asr_scene_view_tests PRIVATE asr_world asr_camera asr_warnings)
    add_test(NAME scene_view COMMAND asr_scene_view_tests)
    set_tests_properties(scene_view PROPERTIES TIMEOUT 30)
    # The inspector lives in the client, so its tests only exist when the client
    # is built. The simulation tests never depend on it.
    if(ASR_BUILD_CLIENT)
        add_library(asr_inspector STATIC src/game/client/selection.cpp src/game/ui/info_panel.cpp)
        target_include_directories(asr_inspector PUBLIC src)
        target_link_libraries(asr_inspector PUBLIC asr_simulation SDL3::SDL3-static PRIVATE asr_warnings)
        set(ASR_INSPECTOR_TESTS tests/test_inspector.cpp)
        set(ASR_INSPECTOR_LIBS asr_inspector)
        # Streaming integration without a window/GPU. Use the same Explorer
        # and worker implementation as the client, not a test-only scheduler.
        add_executable(asr_ring_tests
                tests/main.cpp
                tests/test_instance_arena.cpp
                tests/test_ring_stream.cpp
                tests/test_environment_menu.cpp
                tests/test_draw_distance.cpp
                tests/test_environment_workers.cpp
                tests/test_weather.cpp
                tests/test_water_bodies.cpp
                src/game/client/explore_menu.cpp src/game/client/graphics_panel.cpp
                src/game/client/world_dials.cpp
                src/game/client/explorer.cpp
                src/game/client/renderer.cpp
                src/game/client/controls.cpp
                src/game/client/selection.cpp)
        target_link_libraries(asr_ring_tests PRIVATE asr_simulation asr_world asr_game_render asr_engine_gui
                nlohmann_json SDL3::SDL3-static SDL3_image::SDL3_image-static asr_warnings)
        add_test(NAME ring_stream COMMAND asr_ring_tests)

        # Explicit GPU readback check, not a dependency of headless CI runs.
        add_executable(asr_height_page_gpu_tests
                tests/main.cpp
                tests/test_cluster_cull_gpu.cpp
                tests/test_indirect_draw_gpu.cpp
                tests/test_height_page_atlas_gpu.cpp
                tests/test_slope_shadow_gpu.cpp tests/test_impostor_cache_gpu.cpp)
        if(ASR_ENABLE_DIAGNOSTICS)
            target_sources(asr_height_page_gpu_tests PRIVATE tests/test_grass_cull_gpu.cpp)
        endif()
        target_link_libraries(asr_height_page_gpu_tests PRIVATE asr_game_render asr_warnings)
        if(ASR_TEST_GPU)
            add_test(NAME height_page_gpu COMMAND asr_height_page_gpu_tests)
            set_tests_properties(height_page_gpu PROPERTIES LABELS "gpu" TIMEOUT 60)
        endif()
    endif()
    add_executable(asr_tests
            tests/main.cpp
            tests/test_camera.cpp
            tests/test_camera_matrix.cpp
            tests/test_fixed.cpp
            tests/test_determinism.cpp
            tests/test_content.cpp
            tests/test_pathfinding.cpp
            tests/test_worldgen.cpp
            tests/test_hybrid_terrain.cpp
            tests/test_medicine.cpp
            tests/test_simulation.cpp
            tests/test_savegame.cpp
            tests/test_world_mesh.cpp
            tests/test_environment.cpp
            tests/test_water_queries.cpp
            tests/test_natural_basins.cpp
            tests/test_soil.cpp
            tests/test_world_spatial.cpp
            tests/test_world_nav.cpp
            tests/test_terrain_streaming.cpp
            tests/test_terrain_adaptive.cpp
            tests/test_terrain_refinement.cpp
            tests/test_terrain_plan.cpp
            tests/test_terrain_lod.cpp
            tests/test_terrain_height_pages.cpp
            tests/test_tile_mesh.cpp
            tests/test_engine_clock.cpp
            tests/test_ecs.cpp
            ${ASR_INSPECTOR_TESTS})
    target_link_libraries(asr_tests PRIVATE asr_simulation asr_world asr_camera nlohmann_json asr_warnings
            ${ASR_INSPECTOR_LIBS})
    target_sources(asr_tests PRIVATE tests/test_terrain_config.cpp)
    target_sources(asr_tests PRIVATE tests/test_scene_scatter.cpp)
    target_sources(asr_tests PRIVATE tests/test_world_scale.cpp tests/test_land_coverage.cpp
            tests/test_terrain_source.cpp)
    add_test(NAME unit COMMAND asr_tests)

    add_executable(asr_terrain_tests
            tests/main.cpp
            tests/test_hybrid_terrain.cpp
            tests/test_terrain_adaptive.cpp
            tests/test_terrain_refinement.cpp
            tests/test_terrain_plan.cpp
            tests/test_terrain_lod.cpp
            tests/test_terrain_height_pages.cpp
            tests/test_terrain_cut.cpp
            tests/test_tile_mesh.cpp)
    target_link_libraries(asr_terrain_tests PRIVATE asr_world nlohmann_json asr_warnings)
    target_sources(asr_terrain_tests PRIVATE tests/test_terrain_config.cpp)
    target_sources(asr_terrain_tests PRIVATE tests/test_scene_scatter.cpp)
    target_sources(asr_terrain_tests PRIVATE tests/test_terrain_source_tree.cpp)
    target_sources(asr_terrain_tests PRIVATE tests/test_plate_field.cpp)
    target_sources(asr_terrain_tests PRIVATE tests/test_range_field.cpp)
    target_sources(asr_terrain_tests PRIVATE tests/test_erosion_field.cpp)
    target_sources(asr_terrain_tests PRIVATE tests/test_terrain_foundation.cpp)
    target_sources(asr_terrain_tests PRIVATE tests/test_natural_basins.cpp)
    target_sources(asr_terrain_tests PRIVATE tests/test_world_scale.cpp tests/test_land_coverage.cpp
            tests/test_terrain_source.cpp)
    add_test(NAME terrain_unit COMMAND asr_terrain_tests)
    set_tests_properties(terrain_unit PROPERTIES TIMEOUT 120)

    # Focused ECS suite. It deliberately excludes optional environment/render
    # tests so simulation component and scheduler changes can be validated while
    # those independent targets are being developed in parallel.
    add_executable(asr_ecs_tests
            tests/main.cpp
            tests/test_ecs.cpp)
    target_link_libraries(asr_ecs_tests PRIVATE asr_simulation asr_world nlohmann_json asr_warnings)
    add_test(NAME ecs_unit COMMAND asr_ecs_tests)

    # CPU render systems, on their own so they stay fast and windowless.
    add_executable(asr_render_system_tests
            tests/main.cpp
            tests/test_instance_gather.cpp
            tests/test_level_select.cpp
            tests/test_frustum_cull.cpp
            tests/test_horizon_cull.cpp
            tests/test_terrain_gather.cpp
            tests/test_cluster_draws.cpp
            tests/test_source_cluster_draws.cpp)
    target_link_libraries(asr_render_system_tests PRIVATE asr_render_systems asr_warnings)
    add_test(NAME render_systems COMMAND asr_render_system_tests)
endif()
