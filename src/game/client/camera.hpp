#pragma once
// Compatibility only: projection and camera controls are an engine module.
#include "engine/camera/camera.hpp"
namespace client {
using engine::camera::Camera;
using engine::camera::kDefaultPixelsPerTile;
using engine::camera::kVerticalSquash;
using engine::camera::kMinPixelsPerTile;
using engine::camera::kMaxPixelsPerTile;
using engine::camera::kDefaultCameraYaw;
using engine::camera::kDefaultCameraPitch;
using engine::camera::kMinCameraPitch;
using engine::camera::kMaxCameraPitch;
using engine::camera::kSpriteDetailPixelsPerTile;
using engine::camera::kWorldViewPixelsPerTile;
}
