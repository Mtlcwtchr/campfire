# GPU modules are engine infrastructure, not a side effect of building the game.
set(SDLSHADERCROSS_SHARED OFF CACHE BOOL "" FORCE)
set(SDLSHADERCROSS_STATIC ON CACHE BOOL "" FORCE)
set(SDLSHADERCROSS_CLI OFF CACHE BOOL "" FORCE)
set(SDLSHADERCROSS_VENDORED ON CACHE BOOL "" FORCE)
set(SDLSHADERCROSS_DXC ON CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3_shadercross
        GIT_REPOSITORY https://github.com/libsdl-org/SDL_shadercross.git
        GIT_TAG 1ff05bec573988a98ef9e0260b4da44f512b8367
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(SDL3_shadercross)

add_library(asr_engine_render STATIC
        src/engine/render/device.cpp
        src/engine/render/targets.cpp
        src/engine/render/draw_queue.cpp
        src/engine/render/render_pipeline.cpp
        src/engine/render/geometry/mesh_cache.cpp
        src/engine/render/geometry/instances.cpp
        src/engine/render/geometry/instanced.cpp
        src/engine/pipeline/calc_pipeline.cpp
        src/engine/pipeline/runner.cpp)
target_include_directories(asr_engine_render PUBLIC src)
target_link_libraries(asr_engine_render PUBLIC SDL3::SDL3-static SDL3_image::SDL3_image-static
        SDL3_shadercross::SDL3_shadercross-static PRIVATE asr_warnings)
add_library(Campfire::Render ALIAS asr_engine_render)

add_library(asr_impostors_gpu STATIC src/engine/render/impostor_gpu_cache.cpp)
target_include_directories(asr_impostors_gpu PUBLIC src)
target_link_libraries(asr_impostors_gpu PUBLIC asr_engine_render asr_impostors PRIVATE asr_warnings)
add_library(Campfire::ImpostorsGPU ALIAS asr_impostors_gpu)

add_library(asr_virtual_geometry_gpu STATIC
        src/engine/render/geometry/cluster_cull.cpp
        src/engine/render/geometry/mesh_root_cull.cpp
        src/engine/render/geometry/instance_root_cull.cpp
        src/engine/render/geometry/instance_hierarchy_cull.cpp
        src/engine/render/geometry/hiz_pyramid.cpp)
target_include_directories(asr_virtual_geometry_gpu PUBLIC src)
target_link_libraries(asr_virtual_geometry_gpu PUBLIC asr_engine_render asr_virtual_geometry
        asr_representation PRIVATE asr_warnings)
add_library(Campfire::VirtualGeometryGPU ALIAS asr_virtual_geometry_gpu)

add_library(asr_engine_viewport STATIC src/engine/render/dag_viewport.cpp)
target_include_directories(asr_engine_viewport PUBLIC src)
target_link_libraries(asr_engine_viewport PUBLIC asr_engine_render asr_camera asr_virtual_geometry
        PRIVATE asr_warnings)
add_library(Campfire::Viewport ALIAS asr_engine_viewport)
