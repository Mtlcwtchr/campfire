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

