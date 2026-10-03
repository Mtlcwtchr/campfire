# Reusable CPU modules. None may depend on game/, content, SDL or a window.
add_library(asr_camera STATIC ${CMAKE_CURRENT_LIST_DIR}/camera/camera.cpp)
target_include_directories(asr_camera PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(asr_camera PUBLIC asr_core PRIVATE asr_warnings)
add_library(Campfire::Camera ALIAS asr_camera)

add_library(asr_terrain_generator STATIC
        ${CMAKE_CURRENT_LIST_DIR}/terrain/plate_field.cpp
        ${CMAKE_CURRENT_LIST_DIR}/terrain/range_field.cpp
        ${CMAKE_CURRENT_LIST_DIR}/terrain/erosion_field.cpp
        ${CMAKE_CURRENT_LIST_DIR}/terrain/mountain_shape.cpp)
target_include_directories(asr_terrain_generator PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(asr_terrain_generator PUBLIC asr_core PRIVATE asr_warnings)
add_library(Campfire::Terrain ALIAS asr_terrain_generator)

add_library(asr_representation STATIC ${CMAKE_CURRENT_LIST_DIR}/render/representation_selector.cpp)
target_include_directories(asr_representation PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(asr_representation PUBLIC asr_camera PRIVATE asr_warnings)
add_library(Campfire::Representation ALIAS asr_representation)

find_package(Threads REQUIRED)
add_library(asr_impostors STATIC
        ${CMAKE_CURRENT_LIST_DIR}/render/recursive_impostor.cpp
        ${CMAKE_CURRENT_LIST_DIR}/render/impostor_cache.cpp
        ${CMAKE_CURRENT_LIST_DIR}/render/impostor_hierarchy.cpp)
target_include_directories(asr_impostors PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(asr_impostors PUBLIC asr_representation Threads::Threads PRIVATE asr_warnings)
add_library(Campfire::Impostors ALIAS asr_impostors)

add_library(asr_virtual_geometry STATIC
        ${CMAKE_CURRENT_LIST_DIR}/geometry/cluster_dag.cpp
        ${CMAKE_CURRENT_LIST_DIR}/geometry/cluster_asset.cpp
        ${CMAKE_CURRENT_LIST_DIR}/geometry/cluster_morph.cpp
        ${CMAKE_CURRENT_LIST_DIR}/geometry/instance_hierarchy.cpp
        ${CMAKE_CURRENT_LIST_DIR}/geometry/region_mass.cpp
        ${CMAKE_CURRENT_LIST_DIR}/geometry/quad_mass.cpp
        ${CMAKE_CURRENT_LIST_DIR}/render/geometry/page_residency.cpp)
target_include_directories(asr_virtual_geometry PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(asr_virtual_geometry PRIVATE asr_warnings)
add_library(Campfire::VirtualGeometry ALIAS asr_virtual_geometry)

# The world on disk: chunk keys, chunk files of typed compressed blocks, atomic
# writes and the world root's manifest. Knows nothing of what a block holds -
# the game's persistent delta is one user of it, derived caches another.
add_library(asr_world_store STATIC
        ${CMAKE_CURRENT_LIST_DIR}/world_store/chunk_key.cpp
        ${CMAKE_CURRENT_LIST_DIR}/world_store/codec.cpp
        ${CMAKE_CURRENT_LIST_DIR}/world_store/chunk_file.cpp
        ${CMAKE_CURRENT_LIST_DIR}/world_store/atomic_file.cpp
        ${CMAKE_CURRENT_LIST_DIR}/world_store/world_root.cpp)
target_include_directories(asr_world_store PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(asr_world_store PRIVATE ${zstd_SOURCE_DIR}/lib)
target_link_libraries(asr_world_store PUBLIC asr_core PRIVATE libzstd_static nlohmann_json asr_warnings)
add_library(Campfire::WorldStore ALIAS asr_world_store)

# The authored world (world_authoring_import_export_spec): 256 m rasters and
# stable-id vectors in 32 km source chunks, and the package importer/exporter.
find_package(ZLIB REQUIRED)
add_library(asr_world_source STATIC
        ${CMAKE_CURRENT_LIST_DIR}/world_source/png_io.cpp
        ${CMAKE_CURRENT_LIST_DIR}/world_source/schema.cpp
        ${CMAKE_CURRENT_LIST_DIR}/world_source/world_source.cpp
        ${CMAKE_CURRENT_LIST_DIR}/world_source/package_vectors.cpp
        ${CMAKE_CURRENT_LIST_DIR}/world_source/transfer.cpp
        ${CMAKE_CURRENT_LIST_DIR}/world_source/example_package.cpp
        ${CMAKE_CURRENT_LIST_DIR}/world_source/bake.cpp)
target_include_directories(asr_world_source PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(asr_world_source PUBLIC asr_world_store nlohmann_json PRIVATE ZLIB::ZLIB asr_warnings)
add_library(Campfire::WorldSource ALIAS asr_world_source)

# Terrain categories and the biomes of the control layers
# (doc/plan_ground_types_2026-10-01.md): the libraries and categories read
# from content/config/terrain, their validator, the shader code and tables
# made from them, the categorical field over the world, and the hand edits
# to the details they place.
add_library(asr_biomes STATIC
        ${CMAKE_CURRENT_LIST_DIR}/biomes/registry.cpp
        ${CMAKE_CURRENT_LIST_DIR}/biomes/shader_code.cpp
        ${CMAKE_CURRENT_LIST_DIR}/biomes/category_field.cpp
        ${CMAKE_CURRENT_LIST_DIR}/biomes/detail_edits.cpp)
target_include_directories(asr_biomes PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(asr_biomes PUBLIC asr_core nlohmann_json PRIVATE asr_warnings)
add_library(Campfire::Biomes ALIAS asr_biomes)

# The procedural environment (doc/plan_procedural_environment_2026-10-03.md):
# fields, zones, feature recipes and their terrain operations, the planner,
# historical drainage, scatter primitives, page masks, procedural feature
# meshes, style tables and asset metadata. The mechanism only: which zones,
# recipes and looks a world has is the game's.
add_library(asr_environment STATIC
        ${CMAKE_CURRENT_LIST_DIR}/environment/fields.cpp
        ${CMAKE_CURRENT_LIST_DIR}/environment/zones.cpp
        ${CMAKE_CURRENT_LIST_DIR}/environment/recipe_json.cpp
        ${CMAKE_CURRENT_LIST_DIR}/environment/terrain_ops.cpp
        ${CMAKE_CURRENT_LIST_DIR}/environment/drainage.cpp
        ${CMAKE_CURRENT_LIST_DIR}/environment/cover.cpp
        ${CMAKE_CURRENT_LIST_DIR}/environment/catalogue.cpp
        ${CMAKE_CURRENT_LIST_DIR}/environment/planner.cpp
        ${CMAKE_CURRENT_LIST_DIR}/environment/feature_layer.cpp
        ${CMAKE_CURRENT_LIST_DIR}/environment/masks.cpp
        ${CMAKE_CURRENT_LIST_DIR}/environment/scatter.cpp
        ${CMAKE_CURRENT_LIST_DIR}/environment/feature_mesh.cpp
        ${CMAKE_CURRENT_LIST_DIR}/environment/style.cpp
        ${CMAKE_CURRENT_LIST_DIR}/environment/asset_meta.cpp
        ${CMAKE_CURRENT_LIST_DIR}/environment/environment.cpp)
target_include_directories(asr_environment PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(asr_environment PUBLIC asr_core nlohmann_json PRIVATE asr_warnings)
add_library(Campfire::Environment ALIAS asr_environment)

# The character animator: poses, clips, blend spaces, state machines, layers,
# IK and procedural humanoid motion. Pure CPU, no dependencies.
add_library(asr_animation STATIC
        ${CMAKE_CURRENT_LIST_DIR}/animation/pose.cpp
        ${CMAKE_CURRENT_LIST_DIR}/animation/motion.cpp
        ${CMAKE_CURRENT_LIST_DIR}/animation/ik.cpp
        ${CMAKE_CURRENT_LIST_DIR}/animation/procedural.cpp
        ${CMAKE_CURRENT_LIST_DIR}/animation/animator.cpp)
target_include_directories(asr_animation PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(asr_animation PRIVATE asr_warnings)
add_library(Campfire::Animation ALIAS asr_animation)
